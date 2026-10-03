#
# arch/arm64/boot/Makefile
#
# This file is included by the global makefile so that you can add your own
# architecture-specific flags and dependencies.
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 2012, ARM Ltd.
# Author: Will Deacon <will.deacon@arm.com>
#
# Based on the ia64 boot/Makefile.
#

include $(srctree)/arch/arm64/boot/dts/Makefile

targets := Image dtbo.img dtb.img

#DTB_NAMES := $(subst $\",,$(CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE_NAMES))
#ifneq ($(DTB_NAMES),)
#DTB_LIST := $(addsuffix .dtb,$(DTB_NAMES))
#else
#DTB_LIST := $(dtb-y) $(dtbo-y)
#endif
#DTB_OBJS := $(addprefix $(obj)/dts/,$(DTB_LIST))

DTB_OBJS := $(shell find $(obj)/dts/ -name \*.dtb)
DTBO_OBJS := $(shell find $(obj)/dts/ -name \*.dtbo)

# Patched: see the note in hm_mod/finish_stale_image_fix.py.
#
# Was:
#     $(obj)/Image: vmlinux FORCE
#             $(call if_changed,objcopy)
#
# if_changed re-runs only when the RECIPE TEXT changes, so a newly linked
# vmlinux left the previous Image in place and m bootimage packed that stale
# Image. That shipped a kernel one build behind: openat2 was present in
# System.map and in vmlinux yet absent from the running kernel (NR 437 answered
# ENOSYS). Depend on vmlinux itself and drop FORCE so the objcopy runs exactly
# when vmlinux is newer.
#
# $(OBJCOPY) is supplied by the device BoardConfig through
# TARGET_KERNEL_ADDITIONAL_FLAGS, alongside CLANG_TRIPLE/CROSS_COMPILE/STRIP.
# It has to be supplied that way: this ROM builds without LLVM=1, so the
# in-tree default is $(CROSS_COMPILE)objcopy, which is not on PATH here.
# This file does NOT include scripts/Makefile.lib, so quiet_cmd_objcopy and
# cmd_objcopy are UNDEFINED here. Using them expanded the recipe to the bare
# word "OBJCOPY", which the shell then tried to execute:
#     make[2]: OBJCOPY: No such file or directory      (Error 127)
# Define them locally, with the same flags scripts/Makefile.lib uses, so the
# rule is self-contained. $(OBJCOPY) is the variable kernel/Makefile defines
# (it resolves to /root/toolchains/bin/llvm-objcopy on this build host).

# The objcopy is written out explicitly for two reasons, both observed here:
#
#  1. if_changed re-runs only when the RECIPE TEXT changes, so a newly linked
#     vmlinux left the previous Image in place and m bootimage packed that stale
#     Image. openat2 was present in System.map and in vmlinux yet absent from
#     the running kernel (NR 437 answered ENOSYS).
#
#  2. quiet_cmd_objcopy is a PROGRESS MESSAGE, meaningful only as an argument to
#     $(call if_changed,...). As a standalone recipe line it expands to the bare
#     word "OBJCOPY", which the shell tries to execute:
#         make[2]: OBJCOPY: No such file or directory      (Error 127)
#     Hence the @echo, which must stay prefixed with @.
#
# $(OBJCOPY) is defined by kernel/Makefile and resolves to the llvm binutils on
# this host; see hm_mod/patch_kernel_binutils.py.
$(obj)/Image: vmlinux
	@echo "  OBJCOPY  $@"
	$(OBJCOPY) $(OBJCOPYFLAGS) $(OBJCOPYFLAGS_$(@F)) $< $@

$(obj)/Image.bz2: $(obj)/Image FORCE
	$(call if_changed,bzip2)

$(obj)/Image-dtb: $(obj)/Image $(DTB_OBJS) FORCE
	$(call if_changed,cat)

$(obj)/Image.gz: $(obj)/Image FORCE
	$(call if_changed,gzip)

$(obj)/Image.lz4: $(obj)/Image FORCE
	$(call if_changed,lz4)

$(obj)/Image.lzma: $(obj)/Image FORCE
	$(call if_changed,lzma)

$(obj)/Image.lzo: $(obj)/Image FORCE
	$(call if_changed,lzo)

$(obj)/Image.gz-dtb: $(obj)/Image.gz $(DTB_OBJS) FORCE
	$(call if_changed,cat)

$(obj)/dtbo.img: $(DTBO_OBJS) FORCE
	$(call if_changed,mkdtimg)

$(obj)/dtb.img: $(DTB_OBJS) FORCE
	$(call if_changed,mkdtimg)

install: $(obj)/Image
	$(CONFIG_SHELL) $(srctree)/$(src)/install.sh $(KERNELRELEASE) \
	$(obj)/Image System.map "$(INSTALL_PATH)"

zinstall: $(obj)/Image.gz
	$(CONFIG_SHELL) $(srctree)/$(src)/install.sh $(KERNELRELEASE) \
	$(obj)/Image.gz System.map "$(INSTALL_PATH)"
