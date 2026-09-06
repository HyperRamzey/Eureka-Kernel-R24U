// SPDX-License-Identifier: GPL-2.0
/*
 * asv_margin.c — runtime ASV voltage-margin sysfs interface for Exynos7885
 *
 * The cal-if layer already ships cal_dfs_set_volt_margin() -> ACPM MARGIN_REQ,
 * which tells the ACPM firmware to add a margin (uV) to every voltage of a
 * DVFS domain. The vendor kernel only uses it at boot (fvmap early params
 * "all=", "mif=", "int=", "big=", "lit=") — there is no runtime interface.
 *
 * This driver exposes it at runtime:
 *   /sys/class/asv_margin/asv_margin/<domain>/margin   (rw, uV)
 *   /sys/class/asv_margin/asv_margin/<domain>/max      (ro)
 *   /sys/class/asv_margin/asv_margin/<domain>/min      (ro)
 *
 * Domains (ids from include/dt-bindings/clock/exynos8895.h, ACPM_VCLK_TYPE
 * base 0x0B040000, verified against exynos7885.dtsi cal-id entries):
 *   cpucl0 = BIG cluster   (sibling-cpus 6-7)
 *   cpucl1 = LITTLE cluster (sibling-cpus 0-5)
 *   g3d, mif, int
 *
 * A margin is a SHIFT, not an absolute voltage: writing 25000 adds +25mV to
 * every OPP voltage of the domain; writing 0 restores the stock ECT/ASV
 * table. Positive = stability margin, negative = undervolt (user's
 * responsibility). Clamp: [-100000, +100000] uV.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <soc/samsung/cal-if.h>

#define MARGIN_MAX_UV		100000
#define MARGIN_MIN_UV		-100000

/* ACPM DVFS domain ids (see header comment for provenance) */
#define ACPM_DVFS_MIF_ID	0x0B040000
#define ACPM_DVFS_INT_ID	0x0B040001
#define ACPM_DVFS_CPUCL0_ID	0x0B040002
#define ACPM_DVFS_CPUCL1_ID	0x0B040003
#define ACPM_DVFS_G3D_ID	0x0B040004

struct asv_dom {
	const char	*name;
	unsigned int	cal_id;
	int		margin_uv;
	struct device	*dev;
};

static struct asv_dom doms[] = {
	{ "cpucl0", ACPM_DVFS_CPUCL0_ID, 0, NULL }, /* BIG   6-7 */
	{ "cpucl1", ACPM_DVFS_CPUCL1_ID, 0, NULL }, /* LITTLE 0-5 */
	{ "g3d",    ACPM_DVFS_G3D_ID,    0, NULL },
	{ "mif",    ACPM_DVFS_MIF_ID,    0, NULL },
	{ "int",    ACPM_DVFS_INT_ID,    0, NULL },
};

#define NDoms	(sizeof(doms) / sizeof(doms[0]))

static struct class asv_margin_class = {
	.name = "asv_margin",
};

static ssize_t margin_show(struct device *dev, struct device_attribute *a,
			   char *buf)
{
	struct asv_dom *d = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", d->margin_uv);
}

static ssize_t margin_store(struct device *dev, struct device_attribute *a,
			    const char *buf, size_t cnt)
{
	struct asv_dom *d = dev_get_drvdata(dev);
	int v, ret;

	ret = kstrtoint(buf, 10, &v);
	if (ret)
		return ret;
	if (v > MARGIN_MAX_UV || v < MARGIN_MIN_UV)
		return -EINVAL;

	cal_dfs_set_volt_margin(d->cal_id, v);
	d->margin_uv = v;
	return cnt;
}
static DEVICE_ATTR_RW(margin);

static ssize_t max_show(struct device *dev, struct device_attribute *a,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", MARGIN_MAX_UV);
}
static DEVICE_ATTR_RO(max);

static ssize_t min_show(struct device *dev, struct device_attribute *a,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", MARGIN_MIN_UV);
}
static DEVICE_ATTR_RO(min);

static umode_t asv_margin_is_visible(struct kobject *kobj,
				      struct attribute *attr, int idx)
{
	return attr->mode;
}

static struct attribute *asv_margin_attrs[] = {
	&dev_attr_margin.attr,
	&dev_attr_max.attr,
	&dev_attr_min.attr,
	NULL,
};

static const struct attribute_group asv_margin_group = {
	.attrs		= asv_margin_attrs,
	.is_visible	= asv_margin_is_visible,
};

static const struct attribute_group *asv_margin_groups[] = {
	&asv_margin_group,
	NULL,
};

static int __init asv_margin_init(void)
{
	int i, ret;

	asv_margin_class.dev_groups = asv_margin_groups;

	ret = class_register(&asv_margin_class);
	if (ret) {
		pr_err("asv_margin: class_register failed (%d)\n", ret);
		return ret;
	}

	for (i = 0; i < NDoms; i++) {
		doms[i].dev = device_create(&asv_margin_class, NULL, 0,
					    &doms[i], "%s", doms[i].name);
		if (IS_ERR(doms[i].dev)) {
			ret = PTR_ERR(doms[i].dev);
			pr_err("asv_margin: device_create(%s) failed (%d)\n",
				doms[i].name, ret);
			goto err;
		}
	}

	pr_info("asv_margin: %zu domains live under /sys/class/asv_margin "
		"(margin rw in uV, clamp [%d,%d], 0 = stock ECT)\n",
		NDoms, MARGIN_MIN_UV, MARGIN_MAX_UV);
	return 0;
err:
	while (--i >= 0)
		device_destroy(&asv_margin_class, doms[i].dev ? doms[i].dev->devt : 0);
	class_unregister(&asv_margin_class);
	return ret;
}

static void __exit asv_margin_exit(void)
{
	int i;
	for (i = 0; i < NDoms; i++)
		if (doms[i].dev)
			device_destroy(&asv_margin_class, doms[i].dev->devt);
	class_unregister(&asv_margin_class);
}

module_init(asv_margin_init);
module_exit(asv_margin_exit);

MODULE_DESCRIPTION("Exynos7885 runtime ASV voltage margin sysfs (cal_dfs_set_volt_margin)");
MODULE_LICENSE("GPL v2");
