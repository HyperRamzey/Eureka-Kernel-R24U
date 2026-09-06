# Building Eureka-Kernel-R24U from scratch — complete WSL2 Debian guide

This guide takes a **clean Windows machine** to a **flashable kernel zip** with
zero prior setup. Everything below is the exact process that produced the
device-verified v1.1 release.

> Total time: ~1h (30 min of it is kernel compilation).
> Disk needed: ~15 GB (toolchain 4 GB + source 6 GB + build 4 GB).

## 0. Prerequisites

- Windows 10 1903+ / Windows 11, virtualization enabled in BIOS
- ~15 GB free on the WSL drive
- A device in the table below (and a brain: flashing kernels can need
  recovery + a backup)

| Device | Codename |
| --- | --- |
| Galaxy A10 | a10 |
| Galaxy A20 | a20 |
| Galaxy A20e | a20e |
| Galaxy A30 | a30 |
| Galaxy A30s (SM-A307FN) | a30s |
| Galaxy A40 | a40 |
| Galaxy M20 | m20 |
| Galaxy M30s | jackpotlte |

## 1. Install WSL2 + Debian (one-time)

```powershell
# in an Administrator PowerShell
wsl --install -d Debian
# reboot when prompted, set a UNIX user+password on first launch
```

All later commands run **inside Debian** — launch it with `wsl -d Debian`
(from any terminal). **Never** use the default distro if it's docker-desktop;
always pass `-d Debian`.

## 2. Debian packages

```bash
sudo apt update
sudo apt install -y git ccache bc bison flex libssl-dev python3 python3-pip \
  zip unzip curl wget cpio rsync build-essential gcc-aarch64-linux-gnu \
  crossbuild-essential-armhf
```

## 3. Get the toolchain — Neutron Clang 24

The kernel must be built with Clang 24 (Neutron) or Clang 20 (WeebX); GCC-only
builds are NOT supported for the release recipe.

```bash
sudo mkdir -p /root/toolchains && sudo chown -R $USER /root/toolchains 2>/dev/null || true
cd /tmp

# Neutron clang r24 (release) — grab the latest linux-x86_64 tarball from
# https://github.com/Neutron-Toolchains/Neutron-Clang/releases
# example file name pattern: neon-clang-v24.x-YYYYMMDD-linux.tar.zst
wget -O neutron.tar.zst <URL-from-releases-page>
sudo mkdir -p /root/toolchains/bin
sudo tar -I zstd -xf neutron.tar.zst -C /root/toolchains --strip-components=1
/root/toolchains/bin/clang --version   # must print 24.x
```

If zstd is missing: `sudo apt install -y zstd`.

Verify: `/root/toolchains/bin/clang` must exist and report version 24.x.
This exact path is what `tools_eureka/build_device.sh` expects.

## 4. Get the source

```bash
sudo mkdir -p /root/kbuild
cd /root/kbuild

git clone https://github.com/HyperRamzey/Eureka-Kernel-R24U.git eureka_r24
cd eureka_r24
git submodule update --init   # KernelSU (rsuntk) — see note below
```

**KernelSU submodule note:** the gitlink pins the rsuntk fork at the commit
this kernel integrates (v32473 hooks). If `git submodule update --init` fails
(the fork tar-copied during integration), use the released branch state — the
`drivers/kernelsu/` content is committed in the R24U branch history of this
repo; verify with:

```bash
grep 'KSU_VERSION=3' drivers/kernelsu/Makefile   # rsuntk scheme (32473)
# tiann fork instead prints 'expr 10000+' — wrong fork if you see that
```

## 5. Build one device

```bash
cd /root/kbuild/eureka_r24
bash tools_eureka/build_device.sh a30s
```

What it does (the proven H2/v1.1 recipe):

- defconfig: `arch/arm64/configs/full/exynos7885-<dev>_defconfig`
  (vendor O3 + ThinLTO + `CONFIG_KSU=y`)
- flags: `CC=clang HOSTCC=clang LD=ld.lld ...` — vendor flag style,
  **never `LLVM=1`** (LLVM=1 link path hangs this SoC — proven by bisect)
- `LOCALVERSION=-R24U-KSU`, branch `R24U` appends `_R24U` via setlocalversion
  → uniform `uname -r` = `4.4.302-p6-Eureka-R24U-KSU_R24U`
- build on branch **R24U** (the release string depends on it)
- `.scmversion` is emptied automatically
- output: `out_<codename>/arch/arm64/boot/{Image,dtb.img}` and a
  flashable AK3 zip `Eureka-R24U-KSU_<codename>.zip` in `kernel_zip/anykernel/`

Expect the build to take 10–25 min depending on CPU. On success you see
`=== <codename> DONE ===` and the zip's sha256.

### Build all 8 devices

```bash
# parallel waves (2×4) — same recipe, per-device out dirs
bash /mnt/g/projects/eureka-build-tools/build_all_nocip.sh   # if you cloned the tools repo
```

Or just loop:

```bash
for d in a10 a20 a20e a30 a30s a40 m20 jackpotlte; do \
  bash tools_eureka/build_device.sh $d; done
```

## 6. Package for release — AROMA (REQUIRED for flashing!)

**The plain AK3 zip is only a build artifact.** The flashable release uses the
original AROMA two-layer structure (outer AROMA menu + inner AK3 kernel zip +
original Eureka DTBs + DM-verity-patched DTBO). This is the packaging that
boots on devices; a zip that flashes the kernel-tree-built `dtb.img` hangs at
the boot logo (this is exactly what broke the first v1 release).

Per device:

```bash
# in the eureka-build-tools repo (host side) — paths assume /mnt/g = G:
bash package_aroma_v1.1.sh a30s [out_cifs_a30s/arch/arm64/boot/Image] [out.zip]
```

The packager:

1. builds the inner AK3 zip from the H2-proven template + your new `Image`
2. copies the original AROMA assets + all 40 original Eureka DTBs
3. regenerates the device's DM-verity-patched DTBO via
   `tools_eureka/dtbo_gen.py` (byte-identical to the original release dtbo —
   verify any edits with `--verify`)
4. verifies the inner Image sha256 equals your build's Image (IMAGE_MATCH_OK)

## 7. Verify before shipping

```bash
# golden uname (uniform across devices):
cat out_<dev>/include/config/kernel.release
# -> 4.4.302-p6-Eureka-R24U-KSU_R24U

# KSU fork check (rsuntk, not tiann):
strings out_<dev>/arch/arm64/boot/Image | grep -c 'KPROBES is disabled'
# -> 0  (a nonzero = tiann fork = wrong build)

# zip integrity:
unzip -t Eureka-R24U-KSU_<dev>.zip
sha256sum -c Eureka-R24U-KSU_<dev>.zip.sha256
```

Device smoke test (after flashing, over adb with root):

```
uname -r                                         # golden string
cat /proc/fs/cifs/DebugData | head -3            # CIFS version line (v1.1+)
su -c '/data/local/tmp/ipset create t hash:ip'   # IPset works (v1.1+)
```

## 8. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `No rule to make target full/...defconfig` | wrong cwd or old commit | `git pull`; build from repo root |
| Link hangs forever | `LLVM=1` crept in | use `build_device.sh` as-is (vendor flags) |
| uname has suffix `_dirty` or extra text | built off-branch / dirty tree | checkout `R24U`, commit or stash local changes |
| `lz4: command not found` etc. | missing package | `sudo apt install lz4 zstd` |
| Image > 30 MB | defconfig drift | compare with the release defconfigs (`git diff v1.1`) |
| Boot logo hang after flash | zip flashed tree-built dtb | only flash AROMA-packaged zips (§6) |
| m20/jackpotlte dtbo note | no device dtbo in tree | packaging keeps original dtbo (menu option is no-op) |

## 9. Repo layout (what lives where)

```
tools_eureka/build_device.sh   # per-device build (the recipe above)
tools_eureka/dtbo_gen.py       # DM-verity dtbo patcher (+--verify)
kernel_zip/anykernel/          # AK3 template (inner zip source)
kernel_zip/dtbo/<dev>/dtbo.img # stock dtbo per device (patch source)
arch/arm64/configs/full/       # per-device vendor full/ defconfigs
```

## 10. Golden rules (from session-verified bisects)

1. Vendor flag style, **no LLVM=1** — lld-native ThinLTO hangs this SoC.
2. Build on branch `R24U` for the uniform release uname.
3. Never bulk-merge CIP: any security backport is cherry-picked + device-tested.
4. Single-variable changes: one feature per test zip, exactly like the
   H2/H3 bisect ladder that proved this recipe.
5. The user is the flash/boot authority: agent builds + packages, human
   flashes + reports.
