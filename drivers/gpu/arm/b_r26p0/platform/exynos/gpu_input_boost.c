// SPDX-License-Identifier: GPL-2.0
/*
 * Touch input boost for the Exynos Mali platform DVFS.
 *
 * Raises the GPU floor clock (dvfs min lock) as soon as the user touches
 * the screen, so the first frames after a touch never wait for the
 * ~30 ms DVFS polling cycle to notice the load spike. The lock is a
 * dedicated INPUT_BOOST_LOCK client of the standard gpu_dvfs_clock_lock()
 * machinery, so it composes correctly with TMU/sysfs/pmqos locks.
 *
 * Boost frequency and duration come from the DT properties
 * "gpu_input_boost_freq" and "gpu_input_boost_duration_ms" (mali node),
 * with safe defaults of 845 MHz and 500 ms.
 */

#define pr_fmt(fmt) "gpu_input_boost: " fmt

#include <linux/battery_saver.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <mali_kbase.h>
#include "mali_kbase_platform.h"
#include "gpu_dvfs_handler.h"

extern struct kbase_device *pkbdev;

#define DEFAULT_BOOST_FREQ_KHZ		845000
#define DEFAULT_BOOST_DURATION_MS	500

static struct workqueue_struct *gpu_boost_wq;
static struct work_struct boost_work;
static struct delayed_work unboost_work;
static bool screen_awake;
static bool boost_active;

static struct exynos_context *gpu_boost_get_platform(void)
{
	struct exynos_context *platform;

	if (!pkbdev)
		return NULL;

	platform = (struct exynos_context *)pkbdev->platform_context;
	if (!platform || !platform->dvfs_status)
		return NULL;

	return platform;
}

static void gpu_input_boost_work(struct work_struct *work)
{
	struct exynos_context *platform = gpu_boost_get_platform();
	int freq, duration;

	if (!platform || is_battery_saver_on())
		return;

	freq = platform->input_boost_freq > 0 ?
		platform->input_boost_freq : DEFAULT_BOOST_FREQ_KHZ;
	duration = platform->input_boost_duration_ms > 0 ?
		platform->input_boost_duration_ms : DEFAULT_BOOST_DURATION_MS;

	/* No-op if the requested floor isn't a valid DVFS level */
	if (gpu_dvfs_get_level(freq) < 0) {
		pr_warn_once("boost freq %d kHz is not in the DVFS table\n",
			     freq);
		return;
	}

	gpu_dvfs_clock_lock(GPU_DVFS_MIN_LOCK, INPUT_BOOST_LOCK, freq);
	boost_active = true;
	mod_delayed_work(gpu_boost_wq, &unboost_work,
			 msecs_to_jiffies(duration));
}

static void gpu_input_unboost_work(struct work_struct *work)
{
	if (!gpu_boost_get_platform())
		return;

	gpu_dvfs_clock_lock(GPU_DVFS_MIN_UNLOCK, INPUT_BOOST_LOCK, 0);
	boost_active = false;
}

static void gpu_input_boost_kick(void)
{
	if (!screen_awake)
		return;

	/*
	 * Cheap path: if the boost is already held, only extend the
	 * unboost deadline instead of running the full lock path again.
	 */
	if (boost_active) {
		struct exynos_context *platform = gpu_boost_get_platform();
		int duration = DEFAULT_BOOST_DURATION_MS;

		if (platform && platform->input_boost_duration_ms > 0)
			duration = platform->input_boost_duration_ms;

		mod_delayed_work(gpu_boost_wq, &unboost_work,
				 msecs_to_jiffies(duration));
		return;
	}

	queue_work(gpu_boost_wq, &boost_work);
}

static void gpu_input_boost_cancel(void)
{
	cancel_work_sync(&boost_work);
	if (cancel_delayed_work_sync(&unboost_work) || boost_active)
		queue_work(gpu_boost_wq, &unboost_work.work);
}

static int gpu_input_boost_fb_cb(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	screen_awake = *blank == FB_BLANK_UNBLANK;
	if (screen_awake) {
		/* Wake boost: floor the GPU clock while the screen turns on */
		boost_active = false;
		queue_work(gpu_boost_wq, &boost_work);
	} else {
		gpu_input_boost_cancel();
	}

	return NOTIFY_OK;
}

static struct notifier_block gpu_input_boost_fb_notif = {
	.notifier_call = gpu_input_boost_fb_cb,
	.priority = INT_MAX,
};

static void gpu_input_boost_event(struct input_handle *handle,
				  unsigned int type, unsigned int code,
				  int value)
{
	gpu_input_boost_kick();
}

static int gpu_input_boost_connect(struct input_handler *handler,
				   struct input_dev *dev,
				   const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "gpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void gpu_input_boost_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id gpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	{ }
};

static struct input_handler gpu_input_boost_handler = {
	.event		= gpu_input_boost_event,
	.connect	= gpu_input_boost_connect,
	.disconnect	= gpu_input_boost_disconnect,
	.name		= "gpu_input_boost_handler",
	.id_table	= gpu_input_boost_ids
};

static int __init gpu_input_boost_init(void)
{
	int ret;

	gpu_boost_wq = alloc_workqueue("gpu_input_boost_wq", WQ_HIGHPRI, 0);
	if (!gpu_boost_wq)
		return -ENOMEM;

	INIT_WORK(&boost_work, gpu_input_boost_work);
	INIT_DELAYED_WORK(&unboost_work, gpu_input_unboost_work);

	ret = input_register_handler(&gpu_input_boost_handler);
	if (ret) {
		pr_err("failed to register input handler: %d\n", ret);
		destroy_workqueue(gpu_boost_wq);
		return ret;
	}

	ret = fb_register_client(&gpu_input_boost_fb_notif);
	if (ret)
		pr_err("failed to register fb notifier: %d\n", ret);

	pr_info("touch input boost registered\n");
	return 0;
}
late_initcall(gpu_input_boost_init);
