# Eureka-Kernel R24U + KernelSU (Neutron Clang 24, vendor-proven recipe)

Custom kernel for Samsung Galaxy Exynos7885 devices, based on
[Eureka-Kernel](https://github.com/eurekadevelopment/Eureka-Kernel) R24U with:

- **Linux 4.4.302-p6** — the vendor's final 4.4.x source release for this SoC,
  built unmodified (no additional patch sets).
- **KernelSU (rsuntk fork, RKSU v3.2.2-10-legacy / KSU v32473)** — kernel-level
  root with manual hooks (execveat/faccessat/stat/vfs_read/input/selinux)
  pre-integrated, plus the `path_umount` backport (Linux 5.9) so KSU module
  unmount works on 4.4. Device-verified on SM-A307FN: boots, root works,
  module system works.
- **Proven recipe**: vendor `full/` defconfig built with Neutron Clang 24 using
  **vendor flag style** (`CC=clang ... LD=ld.lld`, no `LLVM=1`).

## Supported devices

| Device | Model | Codename |
| --- | --- | --- |
| Galaxy A10 | SM-A105F | a10 |
| Galaxy A20 | SM-A205F | a20 |
| Galaxy A20e | SM-A202F | a20e |
| Galaxy A30 | SM-A305F | a30 |
| Galaxy A30s | SM-A307FN | a30s |
| Galaxy A40 | SM-A405FN | a40 |
| Galaxy M20 | SM-M205F | m20 |
| Galaxy A2 Core | SM-A260F | jackpotlte |

## Installation

1. Unlock bootloader, install a custom recovery (TWRP) for your device.
2. Download `Eureka-R24U-KSU_<device>.zip` from [Releases](../../releases).
3. Boot into recovery, flash the zip (AnyKernel3 — flashes boot + dtb + the
   DM-verity-patched dtbo where applicable; m20/jackpotlte are kernel-only).
4. First boot: install the **RKSU manager** APK
   ([rsuntk/KernelSU releases](https://github.com/rsuntk/KernelSU/releases),
   v3.2.2-10-legacy or newer) and grant root per app.

> KernelSU is compiled in (`CONFIG_KSU=y`, version 32473); root is granted per-app
> through the manager. The rsuntk manager pairs with this kernel (min supported
> kernel version 32377).

## Compilation

Requirements: Linux x86_64 (Debian 13 / Ubuntu 24.04+), ~50 GB disk, ~32 GB RAM.

### Toolchain

[Neutron Clang](https://github.com/Neutron-Toolchains/clang-build-catalogue)
(24.0.0git: clang + ld.lld + llvm-*), plus GNU cross binutils for `CROSS_COMPILE`
helper tools:

```sh
sudo apt install git bc bison flex build-essential cpio zstd \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
  gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi device-tree-compiler
```

### Build one device (proven recipe)

```sh
export TC=/path/to/neutron-clang   # dir with bin/ lib/
export PATH="$TC/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib"
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export CLANG_TRIPLE=aarch64-linux-gnu-
export ANDROID_MAJOR_VERSION=r
export KBUILD_BUILD_USER=you
export LOCALVERSION=-R24U-KSU
echo > .scmversion                     # keep utsrelease under 64 chars

make O=out ARCH=arm64 ANDROID_MAJOR_VERSION=r full/exynos7885-a30s_defconfig
make O=out -j8 ARCH=arm64 ANDROID_MAJOR_VERSION=r \
    CC=clang HOSTCC=clang HOSTCXX=clang++ \
    LD=ld.lld AR=llvm-ar NM=llvm-nm STRIP=llvm-strip \
    OBJDUMP=llvm-objdump LLVM_DIS=llvm-dis
```

Outputs: `out/arch/arm64/boot/{Image,dtb.img}` (dtbo is regenerated from the
stock device dtbo — see below).

Or use the helper: `tools_eureka/build_device.sh a30s`.

### Build configuration (what the shipped release uses)

- defconfig: `full/exynos7885-<dev>_defconfig` → `CONFIG_KSU=y`,
  `CONFIG_KSU_MANUAL_HOOK=y`, `CONFIG_KSU_FEATURE_ADBROOT=y`, vendor O3 + ThinLTO
- Toolchain: Neutron Clang 24; `LLVM=1` is **not** used (the lld-native LTO link
  path hangs this SoC at the bootlogo — see "Boot fixes" below)

### Boot fixes baked into the packaging (proven on SM-A307FN)

1. **DTBO DM-verity patch** — the stock dtbo keeps the AVB `firmware` node
   (`parts = "vbmeta,boot,...,dtbo"`), so the bootloader re-verifies the bootimg
   and any custom kernel hangs at the bootlogo. `tools_eureka/dtbo_gen.py`
   rebuilds the dtbo with the `firmware` node stripped and
   `customs=(hw_rev, hw_rev_end)` per entry — byte-identical to the original
   R24U release dtbo. Devices without a stock dtbo (m20, jackpotlte) ship
   kernel-only zips.
2. **No `LLVM=1`** — the lld-native ThinLTO link path hangs this SoC; vendor
   flag style (`ld.lld` + gold-plugin LTO) boots.

## Source layout / credits

- Base: Eureka-Kernel R24U (eurekadevelopment) — Samsung Exynos7885 BSP,
  OneUI/Q/R/S support
- Root: [rsuntk/KernelSU](https://github.com/rsuntk/KernelSU) (RKSU, non-GKI
  4.4-supported fork)
- Packaging: AnyKernel3 + `tools_eureka/` build helpers

Branch `R24U`: R24U sources → rsuntk KSU port + path_umount → build tools.
