/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *      http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/io.h>
#include <linux/gpio.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/input.h>
#endif
#include <linux/sec_ext.h>
#include "../../battery_v2/include/sec_battery.h"
#include <linux/sec_batt.h>

#include <asm/cacheflush.h>
#include <asm/system_misc.h>

#include <soc/samsung/exynos-pmu.h>
#include <soc/samsung/acpm_ipc_ctrl.h>

#ifdef CONFIG_SEC_DEBUG
#include <linux/sec_debug.h>
#endif

#if defined(CONFIG_SEC_ABC)
#include <linux/sti/abc_common.h>
#endif
/* function ptr for original arm_pm_restart */
void (*mach_restart)(enum reboot_mode mode, const char *cmd);
EXPORT_SYMBOL(mach_restart);

/* INFORM2 */
enum sec_power_flags {
	SEC_POWER_OFF = 0x0,
	SEC_POWER_RESET = 0x12345678,
};
/* INFORM3 */
#define SEC_RESET_REASON_PREFIX 0x12345670
#define SEC_RESET_SET_PREFIX    0xabc00000
enum sec_reset_reason {
	SEC_RESET_REASON_UNKNOWN   = (SEC_RESET_REASON_PREFIX | 0x0),
	SEC_RESET_REASON_DOWNLOAD  = (SEC_RESET_REASON_PREFIX | 0x1),
	SEC_RESET_REASON_UPLOAD    = (SEC_RESET_REASON_PREFIX | 0x2),
	SEC_RESET_REASON_CHARGING  = (SEC_RESET_REASON_PREFIX | 0x3),
	SEC_RESET_REASON_RECOVERY  = (SEC_RESET_REASON_PREFIX | 0x4),
	SEC_RESET_REASON_FOTA      = (SEC_RESET_REASON_PREFIX | 0x5),
	SEC_RESET_REASON_FOTA_BL   = (SEC_RESET_REASON_PREFIX | 0x6), /* update bootloader */
	SEC_RESET_REASON_SECURE    = (SEC_RESET_REASON_PREFIX | 0x7), /* image secure check fail */
	SEC_RESET_REASON_FWUP      = (SEC_RESET_REASON_PREFIX | 0x9), /* emergency firmware update */
	SEC_RESET_REASON_EM_FUSE   = (SEC_RESET_REASON_PREFIX | 0xa), /* EMC market fuse */
	SEC_RESET_REASON_BOOTLOADER   = (SEC_RESET_REASON_PREFIX | 0xd), /* go to download mode */
#ifdef CONFIG_MUIC_S2MU005
	SEC_RESET_REASON_MUIC_1K   = (SEC_RESET_REASON_PREFIX | 0xe), /* setting for muic 1k */
#endif
#ifdef CONFIG_SEC_PERIPHERAL_SECURE_CHK
	SEC_RESET_REASON_CROSS_FAIL   = (SEC_RESET_REASON_PREFIX | 0xf), /* setting for muic 1k */
#endif
	SEC_RESET_REASON_EMERGENCY = 0x0,
	SEC_RESET_SET_FORCE_UPLOAD = (SEC_RESET_SET_PREFIX | 0x40000),
	SEC_RESET_SET_DEBUG        = (SEC_RESET_SET_PREFIX | 0xd0000),
	SEC_RESET_SET_SWSEL        = (SEC_RESET_SET_PREFIX | 0xe0000),
	SEC_RESET_SET_SUD          = (SEC_RESET_SET_PREFIX | 0xf0000),
	SEC_RESET_CP_DBGMEM        = (SEC_RESET_SET_PREFIX | 0x50000), /* cpmem_on: CP RAM logging */
#if defined(CONFIG_SEC_ABC)
	SEC_RESET_USER_DRAM_TEST   = (SEC_RESET_SET_PREFIX | 0x60000), /* USER DRAM TEST */
#endif
};

/*
 * lpm_off - do not convert a poweroff-on-charger into an LP-charging entry.
 *
 * Default 0, i.e. stock behaviour: a poweroff with a charger attached writes
 * SEC_POWER_OFF to INFORM2 and the bootloader shows its charging screen. That
 * is deliberate upstream, and it is also a trap, because a hard reset cannot
 * clear INFORM2 - see the comment at the branch below.
 *
 * Set lpm_off=1 to make the poweroff real instead. The phone then powers off
 * properly and the next power-on boots Android, at the cost of not charging
 * while "off".
 */
/*
 * Default must be 0, and that matters: an early_param handler only runs when
 * the parameter is actually present on the command line. Initialising this to
 * -1 would leave it at -1 for a normal boot, and !lpm_off on -1 is false, so
 * the gate would read as "lpm_off set" and silently make the *new* behaviour
 * the default - the opposite of what is intended, and invisible until someone
 * noticed they could no longer get the charging screen at all.
 *
 * The early_param alone is not sufficient on this device. The kernel cmdline
 * is composed by the bootloader (androidboot.bootloader=, androidboot.serialno=,
 * androidboot.warranty_bit=, androidboot.hmac_mismatch=, androidboot.odin_download=
 * and androidboot.dtbo_idx=3 are all bootloader-supplied), and the DT copy under
 * /proc/device-tree/chosen/bootargs comes from the AVB-signed dtbo prebuilt, which
 * cannot be re-signed. So lpm_off=1 is not deliverable through bootargs here, and
 * an early_param-only version of this change would be unreachable in practice.
 *
 * Hence the sysfs control below, which is the only way to set it on this device:
 *
 *     echo 1 > /sys/kernel/lpm_off      # poweroff stays a poweroff
 *     echo 0 > /sys/kernel/lpm_off      # stock charging mode on poweroff
 *
 * It is deliberately runtime-writable rather than build-time. Whether losing
 * "power off while plugged in and charge overnight" is worth it is the owner's
 * call per boot, not a compile-time decision baked into a shipped kernel.
 */
static int lpm_off;
static DEFINE_MUTEX(lpm_off_lock);
static struct kobject *lpm_off_kobj;

static int __init lpm_off_setup(char *str)
{
	int v;

	if (!get_option(&str, &v) || v < 0 || v > 1)
		v = 0;

	mutex_lock(&lpm_off_lock);
	lpm_off = v;
	mutex_unlock(&lpm_off_lock);
	return 1;
}
early_param("lpm_off", lpm_off_setup);

static ssize_t lpm_off_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int v;

	mutex_lock(&lpm_off_lock);
	v = lpm_off;
	mutex_unlock(&lpm_off_lock);
	return sysfs_emit(buf, "%d\n", v);
}

static ssize_t lpm_off_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int v, ret;

	ret = kstrtoint(buf, 10, &v);
	if (ret || v < 0 || v > 1)
		return -EINVAL;

	mutex_lock(&lpm_off_lock);
	lpm_off = v;
	mutex_unlock(&lpm_off_lock);

	pr_emerg("%s: lpm_off=%d (%s)\n", __func__, v,
			v ? "poweroff on charger stays a real poweroff" :
			     "stock: poweroff on charger enters charging mode");
	return count;
}

static struct kobj_attribute lpm_off_attr =
	__ATTR(lpm_off, 0644, lpm_off_show, lpm_off_store);

static int __init lpm_off_sysfs_init(void)
{
	int error;

	lpm_off_kobj = kobject_create_and_add("lpm_off", kernel_kobj);
	if (!lpm_off_kobj)
		return -ENOMEM;

	/* sysfs_create_file, not kobject_add_attr: the latter is not declared
	 * by the includes this file carries, and sec_resume_suspend_debug.c
	 * already establishes sysfs_create_file as the working pattern for a
	 * kobject hung off kernel_kobj in this tree. */
	error = sysfs_create_file(lpm_off_kobj, &lpm_off_attr.attr);
	if (error) {
		pr_err("%s: cannot create /sys/kernel/lpm_off\n", __func__);
		kobject_put(lpm_off_kobj);
		lpm_off_kobj = NULL;
		return error;
	}

	pr_info("%s: /sys/kernel/lpm_off = %d\n", __func__, lpm_off);
	return 0;
}
late_initcall(lpm_off_sysfs_init);

static void sec_power_off(void)
{
	int poweroff_try = 0;
	union power_supply_propval ac_val, usb_val, wpc_val, water_val;

#ifdef CONFIG_OF
	int powerkey_gpio = -1;
	struct device_node *np, *pp;

	np = of_find_node_by_path("/gpio_keys");
	if (!np)
		return;
	for_each_child_of_node(np, pp) {
		uint keycode = 0;
		if (!of_find_property(pp, "gpios", NULL))
			continue;
		of_property_read_u32(pp, "linux,code", &keycode);
		if (keycode == KEY_POWER) {
			pr_info("%s: <%u>\n", __func__,  keycode);
			powerkey_gpio = of_get_gpio(pp, 0);
			break;
		}
	}
	of_node_put(np);

	if (!gpio_is_valid(powerkey_gpio)) {
		pr_err("Couldn't find power key node\n");
		return;
	}
#else
	int powerkey_gpio = GPIO_nPOWER;
#endif

	local_irq_disable();

	psy_do_property("ac", get, POWER_SUPPLY_PROP_ONLINE, ac_val);
	psy_do_property("ac", get, (enum power_supply_property) POWER_SUPPLY_EXT_PROP_WATER_DETECT, water_val);
	psy_do_property("usb", get, POWER_SUPPLY_PROP_ONLINE, usb_val);
	psy_do_property("wireless", get, POWER_SUPPLY_PROP_ONLINE, wpc_val);
	pr_info("[%s] AC[%d], USB[%d], WPC[%d], WATER[%d]\n",
			__func__, ac_val.intval, usb_val.intval, wpc_val.intval, water_val.intval);

	while (1) {
		bool charger_online = ac_val.intval || water_val.intval ||
					usb_val.intval || wpc_val.intval;
		bool enter_lpm;

		/*
		 * Check reboot charging.
		 *
		 * lpm_off opts out of the charger-connected LP-charging branch
		 * ONLY. poweroff_try >= 5 is a different clause: it is the sole
		 * escape when the PS_HOLD poweroff sequence below keeps failing,
		 * and folding it under the lpm_off gate turned "poweroff fails
		 * five times" into an infinite loop with IRQs off. Keep it
		 * reachable regardless of lpm_off.
		 */
#ifdef CONFIG_SAMSUNG_BATTERY
		enter_lpm = charger_online && !lpcharge && !lpm_off;
#else
		enter_lpm = charger_online && !lpm_off;
#endif
		if (enter_lpm || (poweroff_try >= 5)) {
			pr_emerg("%s: charger connected or power off failed(%d), reboot!\n", __func__, poweroff_try);
#ifdef CONFIG_SEC_DEBUG
			sec_debug_reboot_handler();
#endif
			/*
			 * To enter LP charging.
			 *
			 * This is what strands the phone on the charging screen.
			 * INFORM2 is a one-register handshake with the bootloader:
			 *
			 *   sec_reboot()   -> SEC_POWER_RESET  "LPM mode prevention"
			 *   sec_power_off()-> SEC_POWER_OFF    "enter LP charging"
			 *
			 * A hard reset runs neither function. So once INFORM2 holds
			 * SEC_POWER_OFF it still holds it, the bootloader reads the
			 * same value on the next power-on, and shows the charging
			 * screen again. Every hard reset re-enters the trap, and the
			 * only thing that clears INFORM2 is an OS-initiated reboot,
			 * which goes through sec_reboot() and writes SEC_POWER_RESET.
			 *
			 * With lpm_off the poweroff request is honoured instead of
			 * being converted into an LP-charging entry: INFORM2 is
			 * explicitly set to SEC_POWER_RESET and the phone really
			 * powers off, so the next power-on boots normally.
			 *
			 * The cost is the stock behaviour of "plug in, power off,
			 * charge overnight". The trap costs more than that does:
			 * there is no escape from it short of an OS reboot, and the
			 * screen gives no indication of that.
			 */
			exynos_pmu_write(EXYNOS_PMU_INFORM2,
					lpm_off ? SEC_POWER_RESET : SEC_POWER_OFF);

			flush_cache_all();
			mach_restart(REBOOT_SOFT, "sw reset");

			pr_emerg("%s: waiting for reboot\n", __func__);
			while (1)
				;
		}

		/* wait for power button release */
		if (gpio_get_value(powerkey_gpio)) {
			exynos_acpm_reboot();

#ifdef CONFIG_SEC_DEBUG
			/* Clear magic code in power off */
			pr_emerg("%s: Clear magic code in power off!\n", __func__);
			sec_debug_reboot_handler();
			flush_cache_all();
#endif
			pr_emerg("%s: set PS_HOLD low\n", __func__);
			exynos_pmu_update(EXYNOS_PMU_PS_HOLD_CONTROL, 0x1<<8, 0x0);

			++poweroff_try;
			pr_emerg
			    ("%s: Should not reach here! (poweroff_try:%d)\n",
			     __func__, poweroff_try);
		} else {
		/* if power button is not released, wait and check TA again */
			pr_info("%s: PowerButton is not released.\n", __func__);
		}
		mdelay(1000);
	}
}

static void sec_reboot(enum reboot_mode reboot_mode, const char *cmd)
{
	local_irq_disable();

	pr_emerg("%s (%d, %s)\n", __func__, reboot_mode, cmd ? cmd : "(null)");

	/* LPM mode prevention */
	exynos_pmu_write(EXYNOS_PMU_INFORM2, SEC_POWER_RESET);

	if (cmd) {
		unsigned long value;
		if (!strcmp(cmd, "fota"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_FOTA);
		else if (!strcmp(cmd, "fota_bl"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_FOTA_BL);
		else if (!strncmp(cmd, "recovery", 8)) {
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_RECOVERY);
			//sec_debug_recovery_reboot();
		}
		else if (!strcmp(cmd, "download"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_DOWNLOAD);
		else if (!strcmp(cmd, "bootloader"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_BOOTLOADER);
		else if (!strcmp(cmd, "upload"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_UPLOAD);
		else if (!strcmp(cmd, "secure"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_SECURE);
		else if (!strcmp(cmd, "fwup"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_FWUP);
		else if (!strcmp(cmd, "em_mode_force_user"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_EM_FUSE);
#ifdef CONFIG_MUIC_S2MU005
		else if (!strcmp(cmd, "muic_1k"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_MUIC_1K);
#endif
#ifdef CONFIG_SEC_PERIPHERAL_SECURE_CHK
		else if (!strcmp(cmd, "cross_fail"))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_CROSS_FAIL);
#endif
#if defined(CONFIG_SEC_ABC)
		else if (!strcmp(cmd, "user_dram_test") && sec_abc_get_enabled())
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_USER_DRAM_TEST);
#endif
		else if (!strncmp(cmd, "emergency", 9))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_EMERGENCY);
		else if (!strncmp(cmd, "debug", 5) && !kstrtoul(cmd + 5, 0, &value))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_SET_DEBUG | value);
#if defined(CONFIG_SEC_DEBUG_SUPPORT_FORCE_UPLOAD)
		else if (!strncmp(cmd, "forceupload", 11) && !kstrtoul(cmd + 11, 0, &value))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_SET_FORCE_UPLOAD | value);
#endif
		else if (!strncmp(cmd, "swsel", 5) && !kstrtoul(cmd + 5, 0, &value))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_SET_SWSEL | value);
		else if (!strncmp(cmd, "sud", 3) && !kstrtoul(cmd + 3, 0, &value))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_SET_SUD | value);
		else if (!strncmp(cmd, "cpmem_on", 8))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_CP_DBGMEM | 0x1);
		else if (!strncmp(cmd, "cpmem_off", 9))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_CP_DBGMEM | 0x2);
		else if (!strncmp(cmd, "mbsmem_on", 9))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_CP_DBGMEM | 0x1);
		else if (!strncmp(cmd, "mbsmem_off", 10))
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_CP_DBGMEM | 0x2);
		else if (!strncmp(cmd, "panic", 5)) {
			/*
			 * This line is intentionally blanked because the INFORM3 is used for upload cause
			 * in sec_debug_set_upload_cause() only in case of  panic() .
			 */
		} else
			exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_UNKNOWN);
	} else {
		exynos_pmu_write(EXYNOS_PMU_INFORM3, SEC_RESET_REASON_UNKNOWN);
	}

	flush_cache_all();
	mach_restart(REBOOT_SOFT, "sw reset");

	pr_emerg("%s: waiting for reboot\n", __func__);
	while (1)
		;
}

static int __init sec_reboot_init(void)
{
	mach_restart = arm_pm_restart;
	pm_power_off = sec_power_off;
	arm_pm_restart = sec_reboot;
	return 0;
}

subsys_initcall(sec_reboot_init);
