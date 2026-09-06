// SPDX-License-Identifier: GPL-2.0
/*
 * baseband_guard.c — CP (Shannon modem) auto-recovery policy for Exynos7885
 *
 * The vendor ss310ap state machine detects CP crashes (watchdog/fail IRQs,
 * STATE_CRASH_*) but ships NO recovery policy: after a crash the kernel sits
 * in CRASH_EXIT/CRASH_WATCHDOG and waits for RIL/userspace to act. If RIL is
 * dead or wedged, the baseband stays down until a manual reboot.
 *
 * This driver adds the missing policy, entirely in-kernel:
 *   - hooks the existing modem_event notifier chain (RESET/EXIT/BOOTING/
 *     ONLINE/WATCHDOG)
 *   - on a crash event, arms a delayed_work watchdog
 *   - if ONLINE does not arrive within the window, runs the same recovery
 *     sequence RIL uses (modem_off -> modem_on -> boot_on, mirroring
 *     IOCTL_MODEM_OFF/ON handlers in modem_io_device.c)
 *   - rate-limits recoveries; after max attempts, gives up (counter + state
 *     readable via sysfs). Optional warm-reboot escalation is DEFAULT OFF.
 *
 * Sysfs: /sys/devices/virtual/baseband_guard/{enable,recoveries,crashes,
 *        last_event,boot_window_ms, max_attempts, reboot_on_dead}
 *
 * Config: CONFIG_MODEM_BASEBAND_GUARD (depends SEC_MODEM_SS310AP)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/suspend.h>
#include <linux/modem_notifier.h>
#include "modem_prj.h"
#include "modem_utils.h"

extern struct modem_ctl *ss310ap_get_modem_ctl_ext(void);

#define GUARD_NAME			"baseband_guard"
#define DEFAULT_BOOT_WINDOW_MS		45000	/* rild boot is ~10-30s, allow 45 */
#define DEFAULT_MAX_ATTEMPTS		3	/* then give up; rild can still act */
#define CRASH_SETTLE_MS		2000	/* let dump/notifiers finish first */
#define WDOG_INHIBIT			10

static bool enable = true;
static unsigned int boot_window_ms = DEFAULT_BOOT_WINDOW_MS;
static unsigned int max_attempts = DEFAULT_MAX_ATTEMPTS;
static bool reboot_on_dead;			/* default off: never surprise-reboot */
static unsigned int crashes;
static unsigned int recoveries;
static unsigned int consecutive_failures;
static unsigned long last_event;
static ktime_t last_event_time;

struct bb_guard {
	struct modem_ctl __rcu *mc;
	struct notifier_block nb;
	struct delayed_work watchdog_work;
	struct work_struct recovery_work;
	spinlock_t lock;
	bool watchdog_armed;
	bool recovering;
};

static struct bb_guard g;
static struct device *guard_dev;

static const char *event_str(unsigned long evt)
{
	switch (evt) {
	case MODEM_EVENT_RESET:	return "RESET";
	case MODEM_EVENT_EXIT:	return "EXIT";
	case MODEM_EVENT_BOOTING:	return "BOOTING";
	case MODEM_EVENT_ONLINE:	return "ONLINE";
	case MODEM_EVENT_WATCHDOG:	return "WATCHDOG";
	default:			return "UNKNOWN";
	}
}static bool crash_event(unsigned long evt)
{
	return evt == MODEM_EVENT_WATCHDOG || evt == MODEM_EVENT_EXIT;
}

/* Runs the same sequence RIL issues via ioctl on the boot device:
 * IOCTL_MODEM_OFF -> IOCTL_MODEM_ON -> IOCTL_MODEM_BOOT_ON
 * (modem_io_device.c). We hold no spinlocks here — it runs on the
 * system freeable workqueue with sleeps, like the ioctl path.
 */
static void bb_recovery_work(struct work_struct *ws)
{
	struct bb_guard *b = container_of(ws, struct bb_guard, recovery_work);
	struct modem_ctl *mc;
	int ret;

	rcu_read_lock();
	mc = rcu_dereference(b->mc);
	rcu_read_unlock();
	if (!mc || !mc->ops.modem_off || !mc->ops.modem_on) {
		pr_err(GUARD_NAME ": modem_ctl or ops not available, abort\n");
		return;
	}

	pr_info(GUARD_NAME ": recovery #%u starting (state=%s)\n",
		recoveries + 1, mc_state(mc));

	ret = mc->ops.modem_off(mc);
	if (ret)
		pr_err(GUARD_NAME ": modem_off -> %d (continuing)\n", ret);
	msleep(100);

	ret = mc->ops.modem_on(mc);
	if (ret) {
		pr_err(GUARD_NAME ": modem_on -> %d\n", ret);
		goto failed;
	}
	msleep(300);

	if (mc->ops.modem_boot_on) {
		ret = mc->ops.modem_boot_on(mc);
		if (ret) {
			pr_err(GUARD_NAME ": modem_boot_on -> %d\n", ret);
			goto failed;
		}
	}

	recoveries++;
	pr_info(GUARD_NAME ": recovery #%u dispatched, waiting for ONLINE "
		"(window %ums)\n", recoveries, boot_window_ms);

	/* re-arm the ONLINE watchdog for this recovery cycle */
	spin_lock(&b->lock);
	b->watchdog_armed = true;
	spin_unlock(&b->lock);
	schedule_delayed_work(&b->watchdog_work,
			msecs_to_jiffies(boot_window_ms));
	return;

failed:
	consecutive_failures++;
	if (reboot_on_dead &&
	    consecutive_failures >= max_attempts) {
		pr_emerg(GUARD_NAME ": %u consecutive failed recoveries, "
			"reboot_on_dead=1 -> warm reboot\n",
			consecutive_failures);
		orderly_reboot();
	} else if (consecutive_failures >= max_attempts) {
		pr_err(GUARD_NAME ": %u consecutive failed recoveries, "
			"giving up (rild may still recover manually)\n",
			consecutive_failures);
	}
}

/* Watchdog: if we get here, ONLINE did not arrive in the window. */
static void bb_watchdog_work(struct work_struct *ws)
{
	struct bb_guard *b =
		container_of(to_delayed_work(ws), struct bb_guard, watchdog_work);
	struct modem_ctl *mc;
	unsigned long flags;
	bool online = false;

	rcu_read_lock();
	mc = rcu_dereference(b->mc);
	rcu_read_unlock();

	if (mc)
		online = (mc->phone_state == STATE_ONLINE);

	if (online) {
		pr_info(GUARD_NAME ": ONLINE confirmed before watchdog expiry\n");
		spin_lock_irqsave(&b->lock, flags);
		b->watchdog_armed = false;
		consecutive_failures = 0;
		spin_unlock_irqrestore(&b->lock, flags);
		return;
	}

	if (consecutive_failures >= max_attempts) {
		if (reboot_on_dead) {
			pr_emerg(GUARD_NAME ": CP dead after %u attempts, "
				"reboot_on_dead=1 -> warm reboot\n",
				consecutive_failures);
			orderly_reboot();
		} else {
			pr_err(GUARD_NAME ": CP not ONLINE after %u attempts, "
				"guard giving up (reboot_on_dead=0)\n",
				consecutive_failures);
		}
		return;
	}

	pr_info(GUARD_NAME ": CP not ONLINE within %ums -> schedule recovery\n",
		boot_window_ms);
	spin_lock_irqsave(&b->lock, flags);
	b->watchdog_armed =false;
	spin_unlock_irqrestore(&b->lock, flags);
	schedule_work(&b->recovery_work);
}

static int bb_event_notifier(struct notifier_block *nb, unsigned long evt,
			     void *data)
{
	struct bb_guard *b = container_of(nb, struct bb_guard, nb);
	unsigned long flags;
	bool was_armed;

	if (!enable)
		return NOTIFY_DONE;

	if (data && !b->mc)
		rcu_assign_pointer(b->mc, (struct modem_ctl *)data);

	spin_lock_irqsave(&b->lock, flags);
	was_armed = b->watchdog_armed;
	spin_unlock_irqrestore(&b->lock, flags);

	last_event = evt;
	last_event_time = ktime_get();

	if (evt == MODEM_EVENT_ONLINE) {
		spin_lock_irqsave(&b->lock, flags);
		b->watchdog_armed = false;
		consecutive_failures = 0;
		spin_unlock_irqrestore(&b->lock, flags);
		cancel_delayed_work_sync(&b->watchdog_work);
		pr_info(GUARD_NAME ": ONLINE received, watchdog cancelled "
			"(was_armed=%d)\n", was_armed);
		return NOTIFY_OK;
	}

	if (crash_event(evt)) {
		crashes++;
		pr_info(GUARD_NAME ": crash event %s (#%u) -> settle %dms, "
			"arm watchdog %ums\n", event_str(evt), crashes,
			CRASH_SETTLE_MS, boot_window_ms);
		schedule_delayed_work(&b->watchdog_work,
				msecs_to_jiffies(CRASH_SETTLE_MS) +
				msecs_to_jiffies(boot_window_ms));
		spin_lock_irqsave(&b->lock, flags);
		b->watchdog_armed = true;
		spin_unlock_irqrestore(&b->lock, flags);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

/* ----------------- sysfs interface ----------------- */
static ssize_t enable_show(struct device *d, struct device_attribute *a,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", enable);
}
static ssize_t enable_store(struct device *d, struct device_attribute *a,
			    const char *buf, size_t cnt)
{
	int ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	return cnt;
}
static DEVICE_ATTR_RW(enable);

static ssize_t recoveries_show(struct device *d, struct device_attribute *a,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", recoveries);
}
static DEVICE_ATTR_RO(recoveries);

static ssize_t crashes_show(struct device *d, struct device_attribute *a,
			    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", crashes);
}
static DEVICE_ATTR_RO(crashes);

static ssize_t last_event_show(struct device *d, struct device_attribute *a,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", event_str(last_event));
}
static DEVICE_ATTR_RO(last_event);

static ssize_t boot_window_ms_show(struct device *d,
				    struct device_attribute *a, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", boot_window_ms);
}
static ssize_t boot_window_ms_store(struct device *d,
				     struct device_attribute *a,
				     const char *buf, size_t cnt)
{
	unsigned int v;
	int ret = kstrtouint(buf, 10, &v);
	if (ret)
		return ret;
	if (v < 1000)
		return -EINVAL;
	boot_window_ms = v;
	return cnt;
}
static DEVICE_ATTR_RW(boot_window_ms);

static ssize_t max_attempts_show(struct device *d,
				 struct device_attribute *a, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", max_attempts);
}
static ssize_t max_attempts_store(struct device *d,
				  struct device_attribute *a,
				  const char *buf, size_t cnt)
{
	unsigned int v;
	int ret = kstrtouint(buf, 10, &v);
	if (ret)
		return ret;
	if (v < 1)
		return -EINVAL;
	max_attempts = v;
	return cnt;
}
static DEVICE_ATTR_RW(max_attempts);

static ssize_t reboot_on_dead_show(struct device *d,
				    struct device_attribute *a, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", reboot_on_dead);
}
static ssize_t reboot_on_dead_store(struct device *d,
				    struct device_attribute *a,
				    const char *buf, size_t cnt)
{
	int ret = kstrtobool(buf, &reboot_on_dead);
	if (ret)
		return ret;
	return cnt;
}
static DEVICE_ATTR_RW(reboot_on_dead);

static ssize_t state_show(struct device *d, struct device_attribute *a,
			 char *buf)
{
	struct bb_guard *b = dev_get_drvdata(d);
	struct modem_ctl *mc;
	const char *s = "no-modem";

	mc = ss310ap_get_modem_ctl_ext();
	if (mc)
		s = mc_state(mc);

	return snprintf(buf, PAGE_SIZE,
			"cp_state=%s crashes=%u recoveries=%u "
			"consecutive_failures=%u watchdog_armed=%d "
			"last_event=%s\n",
			s, crashes, recoveries, consecutive_failures,
			b->watchdog_armed, event_str(last_event));
}
static DEVICE_ATTR_RO(state);

static struct attribute *bb_guard_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_recoveries.attr,
	&dev_attr_crashes.attr,
	&dev_attr_last_event.attr,
	&dev_attr_boot_window_ms.attr,
	&dev_attr_max_attempts.attr,
	&dev_attr_reboot_on_dead.attr,
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bb_guard);

static struct class bb_guard_class = {
	.name = GUARD_NAME,
	.dev_groups = bb_guard_groups,
};

static int __init bb_guard_init(void)
{
	struct modem_ctl *mc;
	int ret;

	ret = class_register(&bb_guard_class);
	if (ret) {
		pr_err(GUARD_NAME ": class_register failed (%d)\n", ret);
		return ret;
	}

	guard_dev = device_create(&bb_guard_class, NULL, 0, &g, GUARD_NAME);
	if (IS_ERR(guard_dev)) {
		ret = PTR_ERR(guard_dev);
		pr_err(GUARD_NAME ": device_create failed (%d)\n", ret);
		class_unregister(&bb_guard_class);
		return ret;
	}

	spin_lock_init(&g.lock);
	INIT_DELAYED_WORK(&g.watchdog_work, bb_watchdog_work);
	INIT_WORK(&g.recovery_work, bb_recovery_work);

	/* modem_notify_event() passes NULL as chain data; get the modem_ctl
	 * via the exported ss310ap getter (set after modem probe). */
	mc = ss310ap_get_modem_ctl_ext();
	if (mc)
		rcu_assign_pointer(g.mc, mc);
	else
		pr_warn(GUARD_NAME ": modem_ctl not probed yet; "
			"will start passive\n");

	g.nb.notifier_call = bb_event_notifier;
	g.nb.priority = 0;

	ret = register_modem_event_notifier(&g.nb);
	if (ret) {
		pr_err(GUARD_NAME ": register_modem_event_notifier failed "
			"(%d)\n", ret);
		device_destroy(&bb_guard_class, 0);
		class_unregister(&bb_guard_class);
		return ret;
	}

	pr_info(GUARD_NAME ": armed (window=%ums, max_attempts=%u, "
		"reboot_on_dead=%d)\n", boot_window_ms, max_attempts,
		reboot_on_dead);
	return 0;
}

static void __exit bb_guard_exit(void)
{
	/* No unregister_modem_event_notifier in this vendor tree: the notifier
	 * chain has no unregister API, so this module is built-in only. */
	cancel_delayed_work_sync(&g.watchdog_work);
	cancel_work_sync(&g.recovery_work);
	device_destroy(&bb_guard_class, 0);
	class_unregister(&bb_guard_class);
	pr_info(GUARD_NAME ": unloaded\n");
}

module_init(bb_guard_init);
module_exit(bb_guard_exit);

MODULE_DESCRIPTION("Exynos7885 ss310ap CP auto-recovery policy (baseband guard)");
MODULE_LICENSE("GPL v2");
