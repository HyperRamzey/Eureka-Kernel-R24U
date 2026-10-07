// SPDX-License-Identifier: GPL-2.0
/*
 * CPU input boost for exynos7885 (EAS + schedutil).
 *
 * Conceptually based on Sultan Alsawaf's cpu_input_boost: raises the
 * cpufreq floor of both clusters for a short, re-triggerable window when
 * touch input arrives, so schedutil doesn't have to discover the load
 * ramp on its own (which on this platform costs several frames).
 *
 * Boost/unboost is done by temporarily overriding policy->min and
 * re-evaluating with cpufreq_update_policy(); the user's floor is
 * restored from policy->user_policy.min afterwards.
 *
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/battery_saver.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <asm/topology.h>

#define BOOST_DURATION_MS	CONFIG_CPU_INPUT_BOOST_DURATION_MS
#define WAKE_BOOST_DURATION_MS	CONFIG_CPU_INPUT_BOOST_WAKE_DURATION_MS
#define LITTLE_BOOST_FREQ	CONFIG_CPU_INPUT_BOOST_LITTLE_FREQ
#define BIG_BOOST_FREQ		CONFIG_CPU_INPUT_BOOST_BIG_FREQ

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct input_boost;
	struct delayed_work input_unboost;
	struct work_struct max_boost;
	struct delayed_work max_unboost;
	struct notifier_block fb_notif;
	struct notifier_block cpu_notif;
	bool screen_awake;
};

static struct boost_drv *boost_drv_g __read_mostly;

/* Per-policy boosted floors, indexed by policy->cpu */
static unsigned int boosted_min[NR_CPUS] __read_mostly;

static unsigned int get_boost_freq(struct cpufreq_policy *policy)
{
	/*
	 * exynos7885: cluster 0 = 6x Cortex-A53 (little), cluster 1 = 2x
	 * Cortex-A73 (big). Pick the floor by cluster id so this survives
	 * any policy->cpu numbering.
	 */
	return cpu_topology[policy->cpu].cluster_id ?
		BIG_BOOST_FREQ : LITTLE_BOOST_FREQ;
}

static void __cpu_input_boost(struct boost_drv *b, bool wake)
{
	unsigned int duration = wake ? WAKE_BOOST_DURATION_MS : BOOST_DURATION_MS;
	unsigned int cpu;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		unsigned int boost_freq;

		if (!policy)
			continue;

		boost_freq = get_boost_freq(policy);
		if (!boost_freq || boosted_min[cpu] >= boost_freq) {
			cpufreq_cpu_put(policy);
			continue;
		}

		boosted_min[cpu] = boost_freq;
		policy->min = max(boost_freq, policy->user_policy.min);
		policy->min = min(policy->min, policy->max);
		cpufreq_update_policy(policy->cpu);
		cpufreq_cpu_put(policy);
	}
	put_online_cpus();

	mod_delayed_work(b->wq,
		wake ? &b->max_unboost : &b->input_unboost,
		msecs_to_jiffies(duration));
}

static void __cpu_input_unboost(struct boost_drv *b)
{
	unsigned int cpu;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (boosted_min[cpu]) {
			boosted_min[cpu] = 0;
			policy->min = policy->user_policy.min;
			cpufreq_update_policy(policy->cpu);
		}
		cpufreq_cpu_put(policy);
	}
	put_online_cpus();
}

static void cpu_input_boost_work(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), input_boost);

	__cpu_input_boost(b, false);
}

static void cpu_input_unboost_work(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	__cpu_input_unboost(b);
}

static void cpu_max_boost_work(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), max_boost);

	__cpu_input_boost(b, true);
}

static void cpu_max_unboost_work(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	__cpu_input_unboost(b);
}

static int cpu_boost_fb_cb(struct notifier_block *nb,
			   unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	b->screen_awake = *blank == FB_BLANK_UNBLANK;
	if (b->screen_awake) {
		/* Boost while the screen turns on (wake/unlock path) */
		if (!is_battery_saver_on())
			queue_work(b->wq, &b->max_boost);
	} else {
		/* Drop all boosts when the screen turns off */
		cancel_work_sync(&b->input_boost);
		cancel_delayed_work_sync(&b->input_unboost);
		cancel_work_sync(&b->max_boost);
		cancel_delayed_work_sync(&b->max_unboost);
		__cpu_input_unboost(b);
	}

	return NOTIFY_OK;
}

/*
 * A CPU may come online while a boost is active (or a policy may be
 * created late). Re-apply the active floor to the new policy.
 */
static int cpu_boost_cpu_cb(struct notifier_block *nb,
			    unsigned long action, void *hcpu)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	unsigned int cpu = (unsigned long)hcpu;

	if (action != CPU_ONLINE && action != CPU_ONLINE_FROZEN)
		return NOTIFY_OK;

	if (!delayed_work_pending(&b->input_unboost) &&
	    !delayed_work_pending(&b->max_unboost))
		return NOTIFY_OK;

	if (!b->screen_awake || is_battery_saver_on())
		return NOTIFY_OK;

	__cpu_input_boost(b, delayed_work_pending(&b->max_unboost));

	return NOTIFY_OK;
}

static void cpu_input_boost_event(struct input_handle *handle,
				  unsigned int type, unsigned int code,
				  int value)
{
	struct boost_drv *b = boost_drv_g;

	if (!b || !b->screen_awake || is_battery_saver_on())
		return;

	queue_work(b->wq, &b->input_boost);
}

static int cpu_input_boost_connect(struct input_handler *handler,
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
	handle->name = "cpu_input_boost_handle";

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

static void cpu_input_boost_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
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
	/* Keypad (hardware keys) */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_handler = {
	.event		= cpu_input_boost_event,
	.connect	= cpu_input_boost_connect,
	.disconnect	= cpu_input_boost_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->wq = alloc_workqueue("cpu_input_boost_wq", WQ_HIGHPRI, 0);
	if (!b->wq) {
		ret = -ENOMEM;
		goto free_b;
	}

	INIT_WORK(&b->input_boost, cpu_input_boost_work);
	INIT_DELAYED_WORK(&b->input_unboost, cpu_input_unboost_work);
	INIT_WORK(&b->max_boost, cpu_max_boost_work);
	INIT_DELAYED_WORK(&b->max_unboost, cpu_max_unboost_work);

	ret = input_register_handler(&cpu_input_boost_handler);
	if (ret) {
		pr_err("failed to register input handler: %d\n", ret);
		goto destroy_wq;
	}

	b->fb_notif.notifier_call = cpu_boost_fb_cb;
	b->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		pr_err("failed to register fb notifier: %d\n", ret);
		goto unregister_handler;
	}

	b->cpu_notif.notifier_call = cpu_boost_cpu_cb;
	b->cpu_notif.priority = INT_MAX - 1;
	ret = register_cpu_notifier(&b->cpu_notif);
	if (ret)
		pr_err("failed to register cpu notifier: %d\n", ret);

	boost_drv_g = b;
	pr_info("touch input boost registered (little %u kHz, big %u kHz, %u ms)\n",
		LITTLE_BOOST_FREQ, BIG_BOOST_FREQ, BOOST_DURATION_MS);
	return 0;

unregister_handler:
	input_unregister_handler(&cpu_input_boost_handler);
destroy_wq:
	destroy_workqueue(b->wq);
free_b:
	kfree(b);
	return ret;
}
late_initcall(cpu_input_boost_init);
