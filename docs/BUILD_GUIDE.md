# Building Eureka-Kernel-R24U — modern guide

Applies to the crDroid 16 (Android 16) Samsung A30s kernel
`4.4.302-p6-Eureka_rom-crdroid16`, branch `rom-crdroid16`.

Last updated 2026-10-19 with kernel **#249** (release tag `kernel-20261019`).

There are two build paths. Use the first one for anything that ships to a
device:

| Path | Use it for | Produces |
| --- | --- | --- |
| **ROM tree (modern, recommended)** | release kernels and full ROMs | `boot.img`, `dtb.img`, `dtbo.img` exactly as the ROM ships them |
| Standalone `R24U` branch (legacy) | the old v1.x line / non-ROM builds | an AnyKernel3 zip from `tools_eureka/build_device.sh` |

Every kernel published since `kernel-20261007` was built with the ROM-tree
path. The legacy path is kept at the end of this guide for reference.

---

## 1. Prerequisites

- WSL2 Debian with ~32 GB RAM and ~200 GB free (the full `m bacon` is heavy;
  a kernel-only `m bootimage` needs far less).
- Neutron clang 24 at `/root/toolchains` (`/root/toolchains/bin/clang
  --version` must print 24.x).
- Debian packages:

```bash
sudo apt update
sudo apt install -y git ccache bc bison flex libssl-dev python3 python3-pip \
  zip unzip curl wget cpio rsync build-essential gcc-aarch64-linux-gnu \
  crossbuild-essential-armhf zstd lz4
```

Tree layout used below:

```
/root/rom/crdroid16                              # crDroid 16 ROM tree
/root/rom/crdroid16/kernel/samsung/exynos7885    # this kernel (remote: fork)
/root/rom/crdroid16/device/samsung/a30s          # device tree
/root/rom/crdroid16/device/samsung/exynos7885-common
/root/toolchains                                 # Neutron clang 24
```

The kernel fork is wired into the ROM's `local_manifest`; check it with:

```bash
cd /root/rom/crdroid16/kernel/samsung/exynos7885
git remote -v          # fork -> HyperRamzey/Eureka-Kernel-R24U
git branch --show-current   # rom-crdroid16
git status --short
```

---

## 2. Build in the ROM tree

### 2.1 Enter the build environment

```bash
cd /root/rom/crdroid16
export PATH=/root/rom/crdroid16/build/soong:/root/rom/crdroid16/out/host/linux-x86/bin:$PATH
source build/envsetup.sh
breakfast lineage_a30s-bp4a-userdebug
```

**Never pipe `breakfast`.** It is a shell function; a pipe runs it in a
subshell, so `TARGET_RELEASE` never reaches the parent and the build dies in
`release_config.mk` with "No release config set". A trailing `2>&1` is fine.

### 2.2 Kernel only (fast iteration)

```bash
m bootimage -j8
```

Incremental builds take 4–8 minutes. This rebuilds the kernel image and
re-packs `boot.img`; it does **not** touch system/vendor.

Outputs:

```
out/target/product/a30s/boot.img
out/target/product/a30s/dtb.img
out/target/product/a30s/dtbo.img
out/target/product/a30s/obj/KERNEL_OBJ/arch/arm64/boot/Image
out/target/product/a30s/kernel                  # a copy of Image
out/target/product/a30s/obj/KERNEL_OBJ/System.map
```

### 2.3 Full ROM

```bash
m bacon -j8
```

This rebuilds the kernel, system, vendor, and the flashable
`crDroidAndroid-16.0-YYYYMMDD-a30s-v12.12.zip`. It is the only way to guarantee
that the kernel in the zip matches the kernel you tested.

For an unattended build with logging, use the host helper
`hm_mod/rebuild2.sh` (WSL path `/mnt/g/projects/Derp/hm_mod/rebuild2.sh`); it
sources `envsetup.sh`, runs `breakfast`, then `m bacon -j8`, and logs to
`/root/port49/logs/rebuild_oct3.log`.

---

## 3. Toolchain wiring (why it is pinned)

`device/samsung/exynos7885-common/BoardConfigCommon.mk` pins the kernel
toolchain and the LLVM binutils:

```make
TARGET_KERNEL_SOURCE := kernel/samsung/exynos7885
TARGET_KERNEL_CONFIG := full/exynos7885-a30s_defconfig
TARGET_KERNEL_NO_GCC := true
TARGET_KERNEL_CLANG_PATH := /root/toolchains
TARGET_KERNEL_ADDITIONAL_FLAGS := CLANG_TRIPLE=aarch64-linux-gnu-
TARGET_KERNEL_ADDITIONAL_FLAGS += CROSS_COMPILE=aarch64-linux-gnu-
TARGET_KERNEL_ADDITIONAL_FLAGS += OBJCOPY=$(TARGET_KERNEL_CLANG_PATH)/bin/llvm-objcopy
TARGET_KERNEL_ADDITIONAL_FLAGS += OBJDUMP=$(TARGET_KERNEL_CLANG_PATH)/bin/llvm-objdump
TARGET_KERNEL_ADDITIONAL_FLAGS += NM=$(TARGET_KERNEL_CLANG_PATH)/bin/llvm-nm
TARGET_KERNEL_ADDITIONAL_FLAGS += AR=$(TARGET_KERNEL_CLANG_PATH)/bin/llvm-ar
TARGET_KERNEL_ADDITIONAL_FLAGS += STRIP=$(TARGET_KERNEL_CLANG_PATH)/bin/llvm-strip
```

Two traps this solves:

1. `TARGET_KERNEL_NO_GCC=true` makes crDroid's `kernel.mk` skip its LLVM
   branch, so the kernel Makefile falls back to `$(CROSS_COMPILE)objcopy`.
   `arch/arm64/boot/Makefile` really runs it now; without the explicit
   `OBJCOPY=` the build dies with `OBJCOPY: No such file or directory`
   (Error 127).
2. `CLANG_TRIPLE`/`CROSS_COMPILE` must be passed explicitly or the kernel
   builds **host x86 objects for arm64 output**.

Host tools (`fixdep`) are pinned to the ROM's stock clang in
`vendor/lineage/config/BoardConfigKernel.mk` — Neutron-built host binaries
segfault against the glibc-2.17 sysroot (`Error 139`).

---

## 4. Verify a build before publishing

```bash
K=/root/rom/crdroid16
P=$K/out/target/product/a30s

# 1. the ROM's copy and the kernel build output must be identical
md5sum $P/obj/KERNEL_OBJ/arch/arm64/boot/Image $P/kernel

# 2. banner (version, #build, compiler, date)
strings $P/obj/KERNEL_OBJ/arch/arm64/boot/Image | grep -m1 'Linux version'

# 3. the symbol you changed is really in the image
grep -c ist40xx_report_gesture_key $P/obj/KERNEL_OBJ/System.map

# 4. boot.img exists and has the expected partition size limit
ls -l $P/boot.img      # must be <= 37,748,736 B (mmcblk0p15)
```

`#N` is a build counter and increments on every rebuild, even with identical
sources. Match a running device to a build by **build date**, not by `#N`.

Kernel `.o` files are LLVM bitcode; `strings` on them shows metadata, not
string literals. Use `Image`, `vmlinux`, or the running device.

---

## 5. Package and publish a kernel release

`tools_eureka/publish_release.py` does the whole staging + upload step. It
needs a GitHub token at `/root/.github_token` (or `GITHUB_TOKEN` in the
environment) with `repo` scope on the fork.

### 5.1 Run it

From the kernel tree, after a successful build:

```bash
python3 tools_eureka/publish_release.py \
  --tag kernel-20261019 --version 249 --date 2026-10-19 \
  --boot   /root/rom/crdroid16/out/target/product/a30s/boot.img \
  --image  /root/rom/crdroid16/out/target/product/a30s/obj/KERNEL_OBJ/arch/arm64/boot/Image \
  --dtb    /root/rom/crdroid16/out/target/product/a30s/dtb.img \
  --dtbo   /root/rom/crdroid16/out/target/product/a30s/dtbo.img \
  --ak-dir kernel_zip/anykernel \
  --notes-file krel_20261019/NOTES_FINAL.md
```

It writes a staging directory `krel_YYYYMMDD/` (next to the script unless
`--stage-dir` is given) containing:

```
boot.img  boot.img.sha256
dtb.img   dtb.img.sha256
dtbo.img  dtbo.img.sha256
Eureka-Kernel-a30s-YYYYMMDD.zip
Eureka-Kernel-a30s-YYYYMMDD.zip.sha256
NOTES_FINAL.md
```

and then creates (or reuses) the GitHub release, uploading every asset.
`--replace` re-uploads an existing asset; `--update-body` replaces the release
body on an existing tag; `--no-upload` stages only.

### 5.2 Release contract

- Tag: `kernel-YYYYMMDD`.
- Title: `Eureka Kernel #N (YYYY-MM-DD)`.
- Assets (all with `.sha256` sidecars): `boot.img`, `dtb.img`, `dtbo.img`,
  `Eureka-Kernel-a30s-YYYYMMDD.zip`.
- The AK3 zip is built from `kernel_zip/anykernel` with the built `Image`
  dropped in; it flashes `boot` on `mmcblk0p15`
  (`/dev/block/platform/13500000.dwmmc0/by-name/boot`).
- The release body is the notes file; include the full banner, source commit,
  toolchain, boot image size, the change list, verification, an asset md5
  table, install steps, and provenance.

### 5.3 Verify the published release

```bash
gh release view kernel-20261019 -R HyperRamzey/Eureka-Kernel-R24U
gh release download kernel-20261019 -R HyperRamzey/Eureka-Kernel-R24U -p '*.sha256' -D /tmp/krel
cd /tmp/krel && sha256sum -c *.sha256
```

`sha256sum -c` must pass for all four assets. The release URL must answer 200
and the zip must pass `unzip -t`.

### 5.4 Full-ROM releases (ROM side)

The kernel release is separate from the full-ROM release. The ROM/OTA chain
uses the host-side tools in the Derp repo (`/mnt/g/projects/Derp/hm_mod/`):

- `mk_release.py` — creates/refreshes the ROM release and uploads assets;
- `make_ota_json.py` — regenerates and pushes the OTA feed
  (`a30s.json` on the `ota` branch of `HyperRamzey/device_samsung_a30s`);
- `rebuild2.sh` — the supported full-build entry point.

---

## 6. Legacy standalone build (R24U branch)

This is the v1.x recipe. It is **not** used for crDroid ROM kernels, because it
produces a kernel-only uname and a kernel-tree `dtb.img` that can hang this
device at the boot logo if flashed as a full package.

```bash
cd /root/kbuild/eureka_r24
git checkout R24U
bash tools_eureka/build_device.sh a30s
```

Key differences from the modern path:

- defconfig `arch/arm64/configs/full/exynos7885-a30s_defconfig`;
- vendor flag style, **never `LLVM=1`** (lld-native ThinLTO hangs this SoC);
- `LOCALVERSION=-R24U-KSU`, uname `4.4.302-p6-Eureka-R24U-KSU_R24U`;
- output `out_a30s/arch/arm64/boot/{Image,dtb.img}` and an AK3 zip under
  `kernel_zip/anykernel/`.

If you need a flashable multi-device package, use the AROMA packager
(`package_aroma_v1.1.sh`) from the old `eureka-build-tools` repo; the plain AK3
zip is a build artifact, not a release.

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `No release config set for target` | `breakfast` was piped/subshelled | run `breakfast` bare, then build |
| `OBJCOPY: No such file or directory` | `TARGET_KERNEL_NO_GCC` path | keep the explicit `OBJCOPY=`/`OBJDUMP=`/`NM=`/`AR=`/`STRIP=` flags |
| `fixdep` `Error 139` | Neutron host tools | keep HOSTCC/HOSTCXX pinned to stock clang |
| arm64 build produces x86 objects | missing `CLANG_TRIPLE`/`CROSS_COMPILE` | pass both in `TARGET_KERNEL_ADDITIONAL_FLAGS` |
| Link hangs forever | `LLVM=1` crept in | remove it; use the pinned flags |
| `uname` has `_dirty` or extra text | built off-branch / dirty tree | checkout `rom-crdroid16`, commit or stash |
| `Image` md5 differs from `out/.../kernel` | stale/shadowed output | `m bootimage` again; check the KERNEL_OBJ path |
| boot.img > 37,748,736 B | config drift | shrink the image before flashing; the boot partition is fixed |
| OTA/Updater does not offer a build | feed timestamp <= running build date | regenerate the feed with a newer `--timestamp` |

---

## 8. Golden rules

1. The ROM-tree build is the release path; the standalone build is legacy.
2. Vendor flag style, no `LLVM=1`.
3. One feature per test build, device-verified before it becomes a release.
4. The boot image is capped by the partition (`37,748,736 B`); derive every
   `dd` count from the staged file, never hardcode it.
5. The user is the flash/boot authority: build and package here, flash and
   report on the device.
