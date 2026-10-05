cmd_/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o := /root/toolchains/bin/clang -Wp,-MD,/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/.psi.o.d -nostdinc -isystem /root/toolchains/lib/clang/24/include -I/root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include -Iarch/arm64/include/generated/uapi -Iarch/arm64/include/generated  -I/root/rom/crdroid16/kernel/samsung/exynos7885/include -Iinclude -I/root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi -Iarch/arm64/include/generated/uapi -I/root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi -Iinclude/generated/uapi -include /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kconfig.h   -I/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched -D__KERNEL__ -Qunused-arguments -mlittle-endian -Wall -Wundef -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -Werror-implicit-function-declaration -Wno-format-security -std=gnu89 -fno-PIE -DANDROID_VERSION=110000 -DANDROID_MAJOR_VERSION=r -march=armv8-a+crc+sha2+aes -mtune=cortex-a53 -mcpu=cortex-a53+crypto+crc+sha2+aes -mllvm -polly -mllvm -polly-run-inliner -mllvm -polly-isl-arg=--no-schedule-serialize-sccs -mllvm -polly-ast-use-context -mllvm -polly-detect-keep-going -mllvm -polly-vectorizer=stripmine -mllvm -polly-invariant-load-hoisting --target=aarch64-linux-gnu --gcc-toolchain=/usr --prefix=/usr/bin/aarch64-linux-gnu- -fuse-ld=lld -mno-implicit-float -DCONFIG_AS_LSE=1 -fno-pic -fno-asynchronous-unwind-tables -fno-pic -Wno-asm-operand-widths -fno-delete-null-pointer-checks -Wno-frame-address -Wno-format-truncation -Wno-format-overflow -Wno-int-in-bool-context -Wno-address-of-packed-member -Wno-attribute-alias -Wno-address-of-packed-member -O3 -fno-signed-zeros -fassociative-math -fno-trapping-math -freciprocal-math -fno-math-errno --param=allow-store-data-races=0 -DCC_HAVE_ASM_GOTO -fstack-protector-strong -Wno-format-invalid-specifier -Wno-gnu -Wno-duplicate-decl-specifier -Wno-tautological-compare -mno-global-merge -Wno-unused-but-set-variable -Wno-attribute-alias -Wno-unused-const-variable -fno-omit-frame-pointer -fno-optimize-sibling-calls -ffunction-sections -fdata-sections -flto=thin -fvisibility=default -fsplit-lto-unit -Wdeclaration-after-statement -Wno-pointer-sign -Wno-array-bounds -fno-strict-overflow -fno-merge-all-constants -fno-stack-check -Werror=implicit-int -Werror=strict-prototypes -Werror=date-time -Werror=incompatible-pointer-types -Wno-initializer-overrides -Wno-unused-value -Wno-format -Wno-sign-compare -Wno-format-zero-length -Wno-uninitialized -Wno-pointer-to-enum-cast -Wno-unaligned-access -Wno-cast-function-type-strict    -D"KBUILD_STR(s)=$(pound)s" -D"KBUILD_BASENAME=KBUILD_STR(psi)"  -D"KBUILD_MODNAME=KBUILD_STR(psi)" -c -o /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/.tmp_psi.o /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.c

source_/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o := /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.c

deps_/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o := \
    $(wildcard include/config/psi/default/disabled.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/seq_file.h \
    $(wildcard include/config/user/ns.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/types.h \
    $(wildcard include/config/have/uid16.h) \
    $(wildcard include/config/uid16.h) \
    $(wildcard include/config/lbdaf.h) \
    $(wildcard include/config/arch/dma/addr/t/64bit.h) \
    $(wildcard include/config/phys/addr/t/64bit.h) \
    $(wildcard include/config/64bit.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/types.h \
  arch/arm64/include/generated/asm/types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/int-ll64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/int-ll64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/bitsperlong.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitsperlong.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/bitsperlong.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/posix_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/stddef.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/stddef.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/compiler.h \
    $(wildcard include/config/sparse/rcu/pointer.h) \
    $(wildcard include/config/trace/branch/profiling.h) \
    $(wildcard include/config/profile/all/branches.h) \
    $(wildcard include/config/kasan.h) \
    $(wildcard include/config/enable/must/check.h) \
    $(wildcard include/config/enable/warn/deprecated.h) \
    $(wildcard include/config/kprobes.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/compiler-gcc.h \
    $(wildcard include/config/arch/supports/optimized/inlining.h) \
    $(wildcard include/config/optimize/inlining.h) \
    $(wildcard include/config/arm64.h) \
    $(wildcard include/config/gcov/kernel.h) \
    $(wildcard include/config/arch/use/builtin/bswap.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/compiler-clang.h \
    $(wildcard include/config/lto/clang.h) \
    $(wildcard include/config/ftrace/mcount/record.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kasan-checks.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/posix_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/posix_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/string.h \
    $(wildcard include/config/binary/printf.h) \
  /root/toolchains/lib/clang/24/include/stdarg.h \
  /root/toolchains/lib/clang/24/include/__stdarg_header_macro.h \
  /root/toolchains/lib/clang/24/include/__stdarg___gnuc_va_list.h \
  /root/toolchains/lib/clang/24/include/__stdarg_va_list.h \
  /root/toolchains/lib/clang/24/include/__stdarg_va_arg.h \
  /root/toolchains/lib/clang/24/include/__stdarg___va_copy.h \
  /root/toolchains/lib/clang/24/include/__stdarg_va_copy.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/string.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/string.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bug.h \
    $(wildcard include/config/generic/bug.h) \
    $(wildcard include/config/bug/on/data/corruption.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/bug.h \
    $(wildcard include/config/debug/bugverbose.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/brk-imm.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bug.h \
    $(wildcard include/config/bug.h) \
    $(wildcard include/config/generic/bug/relative/pointers.h) \
    $(wildcard include/config/smp.h) \
    $(wildcard include/config/preempt/rt/base.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kernel.h \
    $(wildcard include/config/preempt/voluntary.h) \
    $(wildcard include/config/debug/atomic/sleep.h) \
    $(wildcard include/config/mmu.h) \
    $(wildcard include/config/prove/locking.h) \
    $(wildcard include/config/panic/timeout.h) \
    $(wildcard include/config/tracing.h) \
    $(wildcard include/config/disable/trace/printk.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/linkage.h \
    $(wildcard include/config/uh/rkp.h) \
    $(wildcard include/config/rkp/kdp.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/stringify.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/export.h \
    $(wildcard include/config/have/underscore/symbol/prefix.h) \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/modversions.h) \
    $(wildcard include/config/unused/symbols.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/linkage.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bitops.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bits.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/bitops.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/barrier.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/builtin-__ffs.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/builtin-ffs.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/builtin-__fls.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/builtin-fls.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/ffz.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/fls64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/find.h \
    $(wildcard include/config/generic/find/first/bit.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/sched.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/hweight.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/arch_hweight.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/const_hweight.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/lock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/non-atomic.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/bitops/le.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/byteorder.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/byteorder/little_endian.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/byteorder/little_endian.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/swab.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/swab.h \
  arch/arm64/include/generated/asm/swab.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/swab.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/byteorder/generic.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/log2.h \
    $(wildcard include/config/arch/has/ilog2/u32.h) \
    $(wildcard include/config/arch/has/ilog2/u64.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/typecheck.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/printk.h \
    $(wildcard include/config/sec/debug/auto/summary.h) \
    $(wildcard include/config/message/loglevel/default.h) \
    $(wildcard include/config/early/printk.h) \
    $(wildcard include/config/printk.h) \
    $(wildcard include/config/dynamic/debug.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/init.h \
    $(wildcard include/config/broken/rodata.h) \
    $(wildcard include/config/debug/rodata.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kern_levels.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cache.h \
    $(wildcard include/config/arch/has/cache/line/size.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/kernel.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/sysinfo.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/cache.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/cachetype.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/cputype.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/sysreg.h \
    $(wildcard include/config/arm64/4k/pages.h) \
    $(wildcard include/config/arm64/16k/pages.h) \
    $(wildcard include/config/arm64/64k/pages.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/opcodes.h \
    $(wildcard include/config/cpu/big/endian.h) \
    $(wildcard include/config/cpu/endian/be8.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/../../arm/include/asm/opcodes.h \
    $(wildcard include/config/cpu/endian/be32.h) \
    $(wildcard include/config/thumb2/kernel.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/dynamic_debug.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/errno.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/errno.h \
  arch/arm64/include/generated/asm/errno.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/errno.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/errno-base.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/mutex.h \
    $(wildcard include/config/debug/lock/alloc.h) \
    $(wildcard include/config/preempt/rt/full.h) \
    $(wildcard include/config/debug/mutexes.h) \
    $(wildcard include/config/mutex/spin/on/owner.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/current.h \
    $(wildcard include/config/thread/info/in/task.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/thread_info.h \
    $(wildcard include/config/preempt/lazy.h) \
    $(wildcard include/config/have/arch/within/stack/frames.h) \
    $(wildcard include/config/hardened/usercopy.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/restart_block.h \
    $(wildcard include/config/compat.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/thread_info.h \
    $(wildcard include/config/arm64/sw/ttbr0/pan.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/stack_pointer.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/list.h \
    $(wildcard include/config/debug/list.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/poison.h \
    $(wildcard include/config/illegal/pointer/value.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/const.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/spinlock_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/spinlock_types_raw.h \
    $(wildcard include/config/generic/lockbreak.h) \
    $(wildcard include/config/debug/spinlock.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/spinlock_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/lockdep.h \
    $(wildcard include/config/lockdep.h) \
    $(wildcard include/config/lock/stat.h) \
    $(wildcard include/config/trace/irqflags.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/spinlock_types_nort.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rwlock_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/atomic.h \
    $(wildcard include/config/generic/atomic64.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/atomic.h \
    $(wildcard include/config/arm64/lse/atomics.h) \
    $(wildcard include/config/as/lse.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/lse.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/atomic_ll_sc.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/cmpxchg.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/atomic-long.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/processor.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/alternative.h \
    $(wildcard include/config/arm64/uao.h) \
    $(wildcard include/config/foo.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/cpufeature.h \
    $(wildcard include/config/hotplug/cpu.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/hwcap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/hwcap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/insn.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/fpsimd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/ptrace.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/ptrace.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/hw_breakpoint.h \
    $(wildcard include/config/sec/kwatcher.h) \
    $(wildcard include/config/have/hw/breakpoint.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/pgtable-hwdef.h \
    $(wildcard include/config/pgtable/levels.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/osq_lock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cpumask.h \
    $(wildcard include/config/cpumask/offstack.h) \
    $(wildcard include/config/sched/hmp.h) \
    $(wildcard include/config/debug/per/cpu/maps.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/threads.h \
    $(wildcard include/config/nr/cpus.h) \
    $(wildcard include/config/base/small.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bitmap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/nodemask.h \
    $(wildcard include/config/highmem.h) \
    $(wildcard include/config/movable/node.h) \
    $(wildcard include/config/numa.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/numa.h \
    $(wildcard include/config/nodes/shift.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/fs.h \
    $(wildcard include/config/crypto/fips.h) \
    $(wildcard include/config/sdp.h) \
    $(wildcard include/config/sysfs.h) \
    $(wildcard include/config/fs/posix/acl.h) \
    $(wildcard include/config/security.h) \
    $(wildcard include/config/cgroup/writeback.h) \
    $(wildcard include/config/ima.h) \
    $(wildcard include/config/fsnotify.h) \
    $(wildcard include/config/fs/encryption.h) \
    $(wildcard include/config/preempt.h) \
    $(wildcard include/config/epoll.h) \
    $(wildcard include/config/five/pa/feature.h) \
    $(wildcard include/config/proca.h) \
    $(wildcard include/config/file/locking.h) \
    $(wildcard include/config/ext4crypt/sdp.h) \
    $(wildcard include/config/quota.h) \
    $(wildcard include/config/fs/dax.h) \
    $(wildcard include/config/block.h) \
    $(wildcard include/config/migration.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/wait.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/spinlock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/preempt.h \
    $(wildcard include/config/preempt/count.h) \
    $(wildcard include/config/debug/preempt.h) \
    $(wildcard include/config/preempt/tracer.h) \
    $(wildcard include/config/preempt/notifiers.h) \
  arch/arm64/include/generated/asm/preempt.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/preempt.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/irqflags.h \
    $(wildcard include/config/irqsoff/tracer.h) \
    $(wildcard include/config/trace/irqflags/support.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/irqflags.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/exynos-ss.h \
    $(wildcard include/config/exynos/snapshot.h) \
    $(wildcard include/config/exynos/dramtest.h) \
    $(wildcard include/config/exynos/snapshot/acpm.h) \
    $(wildcard include/config/exynos/snapshot/regulator.h) \
    $(wildcard include/config/exynos/snapshot/thermal.h) \
    $(wildcard include/config/exynos/snapshot/minimized/mode.h) \
    $(wildcard include/config/exynos/snapshot/irq/disabled.h) \
    $(wildcard include/config/exynos/snapshot/hrtimer.h) \
    $(wildcard include/config/exynos/snapshot/i2c.h) \
    $(wildcard include/config/exynos/snapshot/spi.h) \
    $(wildcard include/config/exynos/snapshot/reg.h) \
    $(wildcard include/config/exynos/snapshot/spinlock.h) \
    $(wildcard include/config/exynos/snapshot/clk.h) \
    $(wildcard include/config/exynos/snapshot/pmu.h) \
    $(wildcard include/config/exynos/snapshot/freq.h) \
    $(wildcard include/config/exynos/snapshot/dm.h) \
    $(wildcard include/config/exynos/snapshot/irq/exit.h) \
    $(wildcard include/config/exynos/snapshot/pstore.h) \
    $(wildcard include/config/exynos/snapshot/sfrdump.h) \
    $(wildcard include/config/sec/debug.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bottom_half.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/spinlock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rwlock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/spinlock_api_smp.h \
    $(wildcard include/config/inline/spin/lock.h) \
    $(wildcard include/config/inline/spin/lock/bh.h) \
    $(wildcard include/config/inline/spin/lock/irq.h) \
    $(wildcard include/config/inline/spin/lock/irqsave.h) \
    $(wildcard include/config/inline/spin/trylock.h) \
    $(wildcard include/config/inline/spin/trylock/bh.h) \
    $(wildcard include/config/uninline/spin/unlock.h) \
    $(wildcard include/config/inline/spin/unlock/bh.h) \
    $(wildcard include/config/inline/spin/unlock/irq.h) \
    $(wildcard include/config/inline/spin/unlock/irqrestore.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rwlock_api_smp.h \
    $(wildcard include/config/inline/read/lock.h) \
    $(wildcard include/config/inline/write/lock.h) \
    $(wildcard include/config/inline/read/lock/bh.h) \
    $(wildcard include/config/inline/write/lock/bh.h) \
    $(wildcard include/config/inline/read/lock/irq.h) \
    $(wildcard include/config/inline/write/lock/irq.h) \
    $(wildcard include/config/inline/read/lock/irqsave.h) \
    $(wildcard include/config/inline/write/lock/irqsave.h) \
    $(wildcard include/config/inline/read/trylock.h) \
    $(wildcard include/config/inline/write/trylock.h) \
    $(wildcard include/config/inline/read/unlock.h) \
    $(wildcard include/config/inline/write/unlock.h) \
    $(wildcard include/config/inline/read/unlock/bh.h) \
    $(wildcard include/config/inline/write/unlock/bh.h) \
    $(wildcard include/config/inline/read/unlock/irq.h) \
    $(wildcard include/config/inline/write/unlock/irq.h) \
    $(wildcard include/config/inline/read/unlock/irqrestore.h) \
    $(wildcard include/config/inline/write/unlock/irqrestore.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/wait.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kdev_t.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/kdev_t.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/dcache.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rculist.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rcupdate.h \
    $(wildcard include/config/tiny/rcu.h) \
    $(wildcard include/config/tree/rcu.h) \
    $(wildcard include/config/preempt/rcu.h) \
    $(wildcard include/config/rcu/trace.h) \
    $(wildcard include/config/rcu/stall/common.h) \
    $(wildcard include/config/no/hz/full.h) \
    $(wildcard include/config/rcu/nocb/cpu.h) \
    $(wildcard include/config/tasks/rcu.h) \
    $(wildcard include/config/debug/objects/rcu/head.h) \
    $(wildcard include/config/prove/rcu.h) \
    $(wildcard include/config/rcu/boost.h) \
    $(wildcard include/config/rcu/nocb/cpu/all.h) \
    $(wildcard include/config/no/hz/full/sysidle.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/seqlock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/completion.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/swait.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/debugobjects.h \
    $(wildcard include/config/debug/objects.h) \
    $(wildcard include/config/debug/objects/free.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ktime.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/time.h \
    $(wildcard include/config/arch/uses/gettimeoffset.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/math64.h \
    $(wildcard include/config/arch/supports/int128.h) \
  arch/arm64/include/generated/asm/div64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/div64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/time64.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/time.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/jiffies.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/timex.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/timex.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/param.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/param.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/param.h \
    $(wildcard include/config/hz.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/param.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/timex.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/arch_timer.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/clocksource/arm_arch_timer.h \
    $(wildcard include/config/arm/arch/timer.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/timecounter.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/timex.h \
  include/generated/timeconst.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/timekeeping.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rcutree.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rculist_bl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/list_bl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bit_spinlock.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/lockref.h \
    $(wildcard include/config/arch/use/cmpxchg/lockref.h) \
  include/generated/bounds.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/path.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/stat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/stat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/stat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/stat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/compat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched.h \
    $(wildcard include/config/cpu/quiet.h) \
    $(wildcard include/config/no/hz/common.h) \
    $(wildcard include/config/sched/debug.h) \
    $(wildcard include/config/lockup/detector.h) \
    $(wildcard include/config/detect/hung/task.h) \
    $(wildcard include/config/core/dump/default/elf/headers.h) \
    $(wildcard include/config/virt/cpu/accounting/native.h) \
    $(wildcard include/config/sched/autogroup.h) \
    $(wildcard include/config/bsd/process/acct.h) \
    $(wildcard include/config/taskstats.h) \
    $(wildcard include/config/audit.h) \
    $(wildcard include/config/inotify/user.h) \
    $(wildcard include/config/fanotify.h) \
    $(wildcard include/config/posix/mqueue.h) \
    $(wildcard include/config/keys.h) \
    $(wildcard include/config/perf/events.h) \
    $(wildcard include/config/bpf/syscall.h) \
    $(wildcard include/config/sched/info.h) \
    $(wildcard include/config/task/delay/acct.h) \
    $(wildcard include/config/schedstats.h) \
    $(wildcard include/config/sched/smt.h) \
    $(wildcard include/config/sched/mc.h) \
    $(wildcard include/config/sched/skip/core/selection/mask.h) \
    $(wildcard include/config/sched/hmp/selective/boost/with/nitp.h) \
    $(wildcard include/config/sched/walt.h) \
    $(wildcard include/config/fair/group/sched.h) \
    $(wildcard include/config/rt/group/sched.h) \
    $(wildcard include/config/five.h) \
    $(wildcard include/config/sched/use/fluid/rt.h) \
    $(wildcard include/config/cgroup/sched.h) \
    $(wildcard include/config/blk/dev/io/trace.h) \
    $(wildcard include/config/memcg.h) \
    $(wildcard include/config/memcg/kmem.h) \
    $(wildcard include/config/compat/brk.h) \
    $(wildcard include/config/cgroups.h) \
    $(wildcard include/config/cc/stackprotector.h) \
    $(wildcard include/config/cpu/freq/times.h) \
    $(wildcard include/config/virt/cpu/accounting/gen.h) \
    $(wildcard include/config/psi.h) \
    $(wildcard include/config/sysvipc.h) \
    $(wildcard include/config/auditsyscall.h) \
    $(wildcard include/config/rt/mutexes.h) \
    $(wildcard include/config/task/xacct.h) \
    $(wildcard include/config/cpusets.h) \
    $(wildcard include/config/futex.h) \
    $(wildcard include/config/numa/balancing.h) \
    $(wildcard include/config/arch/want/batched/unmap/tlb/flush.h) \
    $(wildcard include/config/fault/injection.h) \
    $(wildcard include/config/latencytop.h) \
    $(wildcard include/config/function/graph/tracer.h) \
    $(wildcard include/config/wakeup/latency/hist.h) \
    $(wildcard include/config/missed/timer/offsets/hist.h) \
    $(wildcard include/config/kcov.h) \
    $(wildcard include/config/uprobes.h) \
    $(wildcard include/config/bcache.h) \
    $(wildcard include/config/x86/32.h) \
    $(wildcard include/config/arch/wants/dynamic/task/struct.h) \
    $(wildcard include/config/trace/task/usage.h) \
    $(wildcard include/config/have/unstable/sched/clock.h) \
    $(wildcard include/config/irq/time/accounting.h) \
    $(wildcard include/config/proc/fs.h) \
    $(wildcard include/config/stack/growsup.h) \
    $(wildcard include/config/have/copy/thread/tls.h) \
    $(wildcard include/config/have/exit/thread.h) \
    $(wildcard include/config/debug/stack/usage.h) \
    $(wildcard include/config/preemption.h) \
    $(wildcard include/config/cpu/freq.h) \
    $(wildcard include/config/sched/hp/event.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/sched.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched/prio.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/capability.h \
    $(wildcard include/config/multiuser.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/capability.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/plist.h \
    $(wildcard include/config/debug/pi/list.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rbtree.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/mm_types.h \
    $(wildcard include/config/split/ptlock/cpus.h) \
    $(wildcard include/config/arch/enable/split/pmd/ptlock.h) \
    $(wildcard include/config/have/cmpxchg/double.h) \
    $(wildcard include/config/have/aligned/struct/page.h) \
    $(wildcard include/config/transparent/hugepage.h) \
    $(wildcard include/config/kmemcheck.h) \
    $(wildcard include/config/userfaultfd.h) \
    $(wildcard include/config/uksm.h) \
    $(wildcard include/config/aio.h) \
    $(wildcard include/config/mmu/notifier.h) \
    $(wildcard include/config/compaction.h) \
    $(wildcard include/config/x86/intel/mpx.h) \
    $(wildcard include/config/hugetlb/page.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/auxvec.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/auxvec.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/auxvec.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rwsem.h \
    $(wildcard include/config/rwsem/spin/on/owner.h) \
    $(wildcard include/config/rwsem/generic/spinlock.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/err.h \
  arch/arm64/include/generated/asm/rwsem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/rwsem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/uprobes.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/page-flags-layout.h \
    $(wildcard include/config/sparsemem.h) \
    $(wildcard include/config/sparsemem/vmemmap.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/sparsemem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/workqueue.h \
    $(wildcard include/config/debug/objects/work.h) \
    $(wildcard include/config/freezer.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/timer.h \
    $(wildcard include/config/timer/stats.h) \
    $(wildcard include/config/debug/objects/timers.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sysctl.h \
    $(wildcard include/config/sysctl.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/sysctl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/page.h \
    $(wildcard include/config/have/arch/pfn/valid.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/pgtable-types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/pgtable-nopud.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/memory.h \
    $(wildcard include/config/arm64/va/bits.h) \
    $(wildcard include/config/blk/dev/initrd.h) \
  arch/arm64/include/generated/asm/sizes.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/sizes.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sizes.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/mmdebug.h \
    $(wildcard include/config/debug/vm.h) \
    $(wildcard include/config/debug/virtual.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/memory_model.h \
    $(wildcard include/config/flatmem.h) \
    $(wildcard include/config/discontigmem.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pfn.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/getorder.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/mmu.h \
    $(wildcard include/config/unmap/kernel/at/el0.h) \
    $(wildcard include/config/harden/branch/predictor.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/percpu.h \
    $(wildcard include/config/need/per/cpu/embed/first/chunk.h) \
    $(wildcard include/config/need/per/cpu/page/first/chunk.h) \
    $(wildcard include/config/have/setup/per/cpu/area.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/smp.h \
    $(wildcard include/config/up/late/init.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/llist.h \
    $(wildcard include/config/arch/have/nmi/safe/cmpxchg.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/smp.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/percpu.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/percpu.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/percpu-defs.h \
    $(wildcard include/config/page/table/isolation.h) \
    $(wildcard include/config/debug/force/weak/per/cpu.h) \
  arch/arm64/include/generated/asm/kmap_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/kmap_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cputime.h \
  arch/arm64/include/generated/asm/cputime.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/cputime.h \
    $(wildcard include/config/virt/cpu/accounting.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/cputime_jiffies.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/sem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ipc.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/uidgid.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/highuid.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/ipc.h \
  arch/arm64/include/generated/asm/ipcbuf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/ipcbuf.h \
  arch/arm64/include/generated/asm/sembuf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/sembuf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/shm.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/shm.h \
  arch/arm64/include/generated/asm/shmbuf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/shmbuf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/shmparam.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/shmparam.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/signal.h \
    $(wildcard include/config/old/sigaction.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/signal.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/signal.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/signal.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/signal.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/signal-defs.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/sigcontext.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/siginfo.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/siginfo.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/siginfo.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pid.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/topology.h \
    $(wildcard include/config/use/percpu/numa/node/id.h) \
    $(wildcard include/config/have/memoryless/nodes.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/mmzone.h \
    $(wildcard include/config/force/max/zoneorder.h) \
    $(wildcard include/config/cma.h) \
    $(wildcard include/config/memory/isolation.h) \
    $(wildcard include/config/zsmalloc.h) \
    $(wildcard include/config/zone/dma.h) \
    $(wildcard include/config/zone/dma32.h) \
    $(wildcard include/config/zone/device.h) \
    $(wildcard include/config/memory/hotplug.h) \
    $(wildcard include/config/flat/node/mem/map.h) \
    $(wildcard include/config/page/extension.h) \
    $(wildcard include/config/no/bootmem.h) \
    $(wildcard include/config/deferred/struct/page/init.h) \
    $(wildcard include/config/have/memory/present.h) \
    $(wildcard include/config/need/node/memmap/size.h) \
    $(wildcard include/config/have/memblock/node/map.h) \
    $(wildcard include/config/need/multiple/nodes.h) \
    $(wildcard include/config/have/arch/early/pfn/to/nid.h) \
    $(wildcard include/config/sparsemem/extreme.h) \
    $(wildcard include/config/holes/in/zone.h) \
    $(wildcard include/config/arch/has/holes/memorymodel.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pageblock-flags.h \
    $(wildcard include/config/hugetlb/page/size/variable.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/memory_hotplug.h \
    $(wildcard include/config/memory/hotremove.h) \
    $(wildcard include/config/have/arch/nodedata/extension.h) \
    $(wildcard include/config/have/bootmem/info/node.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/notifier.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/srcu.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/topology.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/topology.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/proportions.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/percpu_counter.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/gfp.h \
    $(wildcard include/config/pm/sleep.h) \
    $(wildcard include/config/hpa.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/seccomp.h \
    $(wildcard include/config/seccomp.h) \
    $(wildcard include/config/have/arch/seccomp/filter.h) \
    $(wildcard include/config/seccomp/filter.h) \
    $(wildcard include/config/checkpoint/restore.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/seccomp.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/seccomp.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/unistd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/unistd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/unistd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/unistd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/seccomp.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/unistd.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rtmutex.h \
    $(wildcard include/config/debug/rt/mutexes.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/resource.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/resource.h \
  arch/arm64/include/generated/asm/resource.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/resource.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/resource.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/hrtimer.h \
    $(wildcard include/config/high/res/timers.h) \
    $(wildcard include/config/time/low/res.h) \
    $(wildcard include/config/timerfd.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/timerqueue.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kcov.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/kcov.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/task_io_accounting.h \
    $(wildcard include/config/task/io/accounting.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/latencytop.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cred.h \
    $(wildcard include/config/debug/credentials.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/key.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/assoc_array.h \
    $(wildcard include/config/associative/array.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/selinux.h \
    $(wildcard include/config/security/selinux.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/magic.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cgroup-defs.h \
    $(wildcard include/config/sock/cgroup/data.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/limits.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/idr.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/percpu-refcount.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/percpu-rwsem.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rcu_sync.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/swork.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/bpf-cgroup.h \
    $(wildcard include/config/cgroup/bpf.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/jump_label.h \
    $(wildcard include/config/jump/label.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/bpf.h \
    $(wildcard include/config/efficient/unaligned/access.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/bpf_common.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cgroup_subsys.h \
    $(wildcard include/config/cgroup/cpuacct.h) \
    $(wildcard include/config/cgroup/schedtune.h) \
    $(wildcard include/config/blk/cgroup.h) \
    $(wildcard include/config/cgroup/device.h) \
    $(wildcard include/config/cgroup/freezer.h) \
    $(wildcard include/config/cgroup/net/classid.h) \
    $(wildcard include/config/cgroup/perf.h) \
    $(wildcard include/config/cgroup/net/prio.h) \
    $(wildcard include/config/cgroup/hugetlb.h) \
    $(wildcard include/config/cgroup/pids.h) \
    $(wildcard include/config/cgroup/debug.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/stat.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/list_lru.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/shrinker.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/radix-tree.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/semaphore.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/fcntl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/fcntl.h \
    $(wildcard include/config/five/debug.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/uapi/asm/fcntl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/fcntl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/fiemap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/migrate_mode.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/blk_types.h \
    $(wildcard include/config/blk/dev/integrity.h) \
    $(wildcard include/config/journal/data/tag.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/fs.h \
    $(wildcard include/config/epm.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/ioctl.h \
  arch/arm64/include/generated/asm/ioctl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/ioctl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/ioctl.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/quota.h \
    $(wildcard include/config/quota/netlink/interface.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/dqblk_xfs.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/dqblk_v1.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/dqblk_v2.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/dqblk_qtree.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/projid.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/quota.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/nfs_fs_i.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/proc_fs.h \
    $(wildcard include/config/proc/uid.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/uaccess.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/uaccess.h \
    $(wildcard include/config/arm64/pan.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/kernel-pgtable.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/pgtable.h \
    $(wildcard include/config/arm64/hw/afdbm.h) \
    $(wildcard include/config/tima/lkmauth.h) \
    $(wildcard include/config/tima/lkmauth/code/prot.h) \
    $(wildcard include/config/have/rcu/table/free.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/proc-fns.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/fixmap.h \
    $(wildcard include/config/uh.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/boot.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/fixmap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/pgtable.h \
    $(wildcard include/config/have/arch/soft/dirty.h) \
    $(wildcard include/config/have/arch/huge/vmap.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/compiler.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/module.h \
    $(wildcard include/config/module/sig.h) \
    $(wildcard include/config/modules/tree/lookup.h) \
    $(wildcard include/config/kallsyms.h) \
    $(wildcard include/config/tracepoints.h) \
    $(wildcard include/config/event/tracing.h) \
    $(wildcard include/config/livepatch.h) \
    $(wildcard include/config/module/unload.h) \
    $(wildcard include/config/constructors.h) \
    $(wildcard include/config/debug/set/module/ronx.h) \
    $(wildcard include/config/retpoline.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kmod.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/elf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/elf.h \
    $(wildcard include/config/vdso32.h) \
  arch/arm64/include/generated/asm/user.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/user.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/elf.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/elf-em.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kobject.h \
    $(wildcard include/config/uevent/helper.h) \
    $(wildcard include/config/debug/kobject/release.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sysfs.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kernfs.h \
    $(wildcard include/config/kernfs.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kobject_ns.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kref.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/moduleparam.h \
    $(wildcard include/config/alpha.h) \
    $(wildcard include/config/ia64.h) \
    $(wildcard include/config/ppc64.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/rbtree_latch.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/module.h \
    $(wildcard include/config/arm64/module/plts.h) \
    $(wildcard include/config/randomize/base.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/module.h \
    $(wildcard include/config/have/mod/arch/specific.h) \
    $(wildcard include/config/modules/use/elf/rel.h) \
    $(wildcard include/config/modules/use/elf/rela.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kthread.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/delay.h \
  arch/arm64/include/generated/asm/delay.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/delay.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ctype.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/file.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/poll.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/poll.h \
  arch/arm64/include/generated/asm/poll.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/asm-generic/poll.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/psi.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/psi_types.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/sched.h \
    $(wildcard include/config/cfs/bandwidth.h) \
    $(wildcard include/config/irq/work.h) \
    $(wildcard include/config/paravirt.h) \
    $(wildcard include/config/paravirt/time/accounting.h) \
    $(wildcard include/config/sched/hrtick.h) \
    $(wildcard include/config/cpu/idle.h) \
    $(wildcard include/config/hmp/frequency/invariant/scale.h) \
    $(wildcard include/config/cpu/freq/schedutil/perfstat/trigger.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched/sysctl.h \
    $(wildcard include/config/sched/tune.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched/rt.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched/smt.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/sched/deadline.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/stop_machine.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cpu.h \
    $(wildcard include/config/pm/sleep/smp.h) \
    $(wildcard include/config/arch/has/cpu/finalize/init.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/node.h \
    $(wildcard include/config/memory/hotplug/sparse.h) \
    $(wildcard include/config/hugetlbfs.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/device.h \
    $(wildcard include/config/debug/devres.h) \
    $(wildcard include/config/generic/msi/irq/domain.h) \
    $(wildcard include/config/pinctrl.h) \
    $(wildcard include/config/generic/msi/irq.h) \
    $(wildcard include/config/dma/cma.h) \
    $(wildcard include/config/of.h) \
    $(wildcard include/config/devtmpfs.h) \
    $(wildcard include/config/sysfs/deprecated.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ioport.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/klist.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pinctrl/devinfo.h \
    $(wildcard include/config/pm.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pinctrl/consumer.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pinctrl/pinctrl-state.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pm.h \
    $(wildcard include/config/vt/console/sleep.h) \
    $(wildcard include/config/pm/clk.h) \
    $(wildcard include/config/pm/generic/domains.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ratelimit.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/device.h \
    $(wildcard include/config/iommu/api.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/pm_wakeup.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/irq_work.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/irq_work.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/tick.h \
    $(wildcard include/config/generic/clockevents.h) \
    $(wildcard include/config/suspend.h) \
    $(wildcard include/config/tick/oneshot.h) \
    $(wildcard include/config/generic/clockevents/broadcast.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/clockchips.h \
    $(wildcard include/config/arch/has/tick/broadcast.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/clocksource.h \
    $(wildcard include/config/arch/clocksource/data.h) \
    $(wildcard include/config/clocksource/watchdog.h) \
    $(wildcard include/config/clksrc/probe.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/arch/arm64/include/asm/io.h \
  arch/arm64/include/generated/asm/early_ioremap.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/early_ioremap.h \
    $(wildcard include/config/generic/early/ioremap.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/xen/xen.h \
    $(wildcard include/config/xen.h) \
    $(wildcard include/config/xen/dom0.h) \
    $(wildcard include/config/xen/pvh.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/io.h \
    $(wildcard include/config/generic/iomap.h) \
    $(wildcard include/config/has/ioport/map.h) \
    $(wildcard include/config/virt/to/bus.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/asm-generic/pci_iomap.h \
    $(wildcard include/config/pci.h) \
    $(wildcard include/config/no/generic/pci/ioport/map.h) \
    $(wildcard include/config/generic/pci/iomap.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/vmalloc.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/context_tracking_state.h \
    $(wildcard include/config/context/tracking.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/static_key.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/slab.h \
    $(wildcard include/config/debug/slab.h) \
    $(wildcard include/config/failslab.h) \
    $(wildcard include/config/have/hardened/usercopy/allocator.h) \
    $(wildcard include/config/slab.h) \
    $(wildcard include/config/slub.h) \
    $(wildcard include/config/slob.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kmemleak.h \
    $(wildcard include/config/debug/kmemleak.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/kasan.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/cpupri.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/cpudeadline.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/cpuacct.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/cgroup.h \
    $(wildcard include/config/cgroup/data.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/cgroupstats.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/uapi/linux/taskstats.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/nsproxy.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/ns_common.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/include/linux/user_namespace.h \
    $(wildcard include/config/persistent/keyrings.h) \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/stats.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/auto_group.h \
  /root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/features.h \
    $(wildcard include/config/default/use/energy/aware.h) \

/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o: $(deps_/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o)

$(deps_/root/rom/crdroid16/kernel/samsung/exynos7885/kernel/sched/psi.o):
