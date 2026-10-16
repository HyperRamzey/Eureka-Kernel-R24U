// SPDX-License-Identifier: GPL-2.0-only
/*
 * ntsync.c - Kernel driver for NT synchronization primitives
 *
 * Copyright (C) 2024 Elizabeth Figura <zfigura@codeweavers.com>
 */

#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#define NTSYNC_NAME	"ntsync"

#ifdef CONFIG_COMPAT
/*
 * compat_ptr_ioctl() is newer than this tree; open-code the equivalent
 * on top of arch/arm64's compat_ptr().  All ntsync ioctls take
 * pointer-transparent 32-bit arguments, so the compat handler is
 * identical to the native one.
 */
static long ntsync_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long parm)
{
	if (!file->f_op->unlocked_ioctl)
		return -ENOTTY;
	return file->f_op->unlocked_ioctl(file, cmd,
					  (unsigned long)compat_ptr(parm));
}
#endif

static int ntsync_char_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static int ntsync_char_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long ntsync_char_ioctl(struct file *file, unsigned int cmd,
			      unsigned long parm)
{
	switch (cmd) {
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations ntsync_fops = {
	.owner		= THIS_MODULE,
	.open		= ntsync_char_open,
	.release	= ntsync_char_release,
	.unlocked_ioctl	= ntsync_char_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ntsync_compat_ioctl,
#endif
	.llseek		= no_llseek,
};

static struct miscdevice ntsync_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= NTSYNC_NAME,
	.fops		= &ntsync_fops,
};

module_driver(ntsync_misc, misc_register, misc_deregister);

MODULE_AUTHOR("Elizabeth Figura <zfigura@codeweavers.com>");
MODULE_DESCRIPTION("Kernel driver for NT synchronization primitives");
MODULE_LICENSE("GPL");
