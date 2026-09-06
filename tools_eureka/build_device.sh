#!/bin/bash
# Build ONE Eureka device with the PROVEN recipe (H3, user-verified booting+KSU+modules):
#   R24U tree (rsuntk KSU 32473) + full/<dev>_defconfig + Neutron Clang 24
#   with VENDOR FLAG STYLE (no LLVM=1, gold-plugin ThinLTO like vendor build.sh).
# USAGE: ./build_device.sh <codename> [out_suffix]
#   e.g. ./build_device.sh a30s          -> out_a30s/
#        ./build_device.sh a30s rel_a30s
# Per-device out dir => safe for parallel builds of multiple devices.
# Packaging: kernel_zip/anykernel AK3 template + dtbo_gen.py patched dtbo
# (byte-identical to original R24U release dtbo, proven on a30s).
set -e
cd /root/kbuild/eureka_r24

CODENAME=$1
SUFFIX=${2:-$CODENAME}
OUT=out_$SUFFIX
case "$CODENAME" in
  a10|a20|a20e|a30|a30s|a40|m20|jackpotlte) ;;
  *) echo "usage: $0 <a10|a20|a20e|a30|a30s|a40|m20|jackpotlte> [out_suffix]"; exit 1 ;;
esac
DEFCONFIG=full/exynos7885-${CODENAME}_defconfig

TC=/root/toolchains
export PATH="$TC/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib"
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export CLANG_TRIPLE=aarch64-linux-gnu-
export ANDROID_MAJOR_VERSION=r
export KBUILD_BUILD_USER=HyperRamzey
export KBUILD_BUILD_HOST=Eureka-R24U
export LOCALVERSION=-R24U-KSU
export PLATFORM_VERSION=11
echo >.scmversion
CORES=8

echo "=== $CODENAME: defconfig ($DEFCONFIG — vendor full/) ==="
rm -rf $OUT
make O=$OUT ARCH=arm64 ANDROID_MAJOR_VERSION=r $DEFCONFIG >/dev/null
grep -E 'CONFIG_(KSU|LTO_CLANG|CC_OPTIMIZE)' $OUT/.config | head -4

echo "=== $CODENAME: build (Neutron 24, vendor flag style — E recipe, NO LLVM=1) ==="
make O=$OUT -j$CORES \
  ARCH=arm64 ANDROID_MAJOR_VERSION=r \
  CC=clang HOSTCC=clang HOSTCXX=clang++ \
  LLVM_DIS=llvm-dis AR=llvm-ar NM=llvm-nm LD=ld.lld OBJDUMP=llvm-objdump STRIP=llvm-strip \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
  >/root/kbuild/build_${SUFFIX}.log 2>&1 || {
    echo "BUILD_ERROR_$CODENAME"
    tail -30 /root/kbuild/build_${SUFFIX}.log
    exit 1
  }
ls -la $OUT/arch/arm64/boot/Image || { echo "NO_IMAGE_$CODENAME"; exit 1; }
strings $OUT/arch/arm64/boot/Image | grep -m1 'Linux version'
echo "release: $(cat $OUT/include/config/kernel.release) (utsrelease $(cat $OUT/include/config/kernel.release | wc -c) chars)"

echo "=== $CODENAME: package ==="
AK=kernel_zip/anykernel
rm -f $AK/Image $AK/dtb.img $AK/dtbo.img
cp -f $OUT/arch/arm64/boot/Image $AK/Image
cp -f $OUT/arch/arm64/boot/dtb.img $AK/dtb.img
# DTBO: Eureka DM-verity-patched dtbo from stock (byte-identical to original
# release dtbo; proven fix for the a30s bootlogo hang). Devices without a stock
# dtbo in kernel_zip/dtbo/ package WITHOUT dtbo flash (m20/jackpotlte).
if [ -f "kernel_zip/dtbo/$CODENAME/dtbo.img" ]; then
  python3 /mnt/g/projects/eureka-build-tools/dtbo_gen.py \
    "kernel_zip/dtbo/$CODENAME/dtbo.img" "$AK/dtbo.img" || {
      echo "DTBO_PATCH_ERROR_$CODENAME"
      exit 1
    }
else
  echo "NOTE_$CODENAME: no stock dtbo — packaging WITHOUT dtbo flash"
fi
cd $AK
ZIPNAME="Eureka-R24U-KSU_${CODENAME}.zip"
rm -f "$ZIPNAME"
if [ -f dtbo.img ]; then
  zip -r9 "$ZIPNAME" META-INF tools anykernel.sh Image dtb.img dtbo.img version >/dev/null
else
  mv anykernel.sh anykernel.sh.bak
  sed '58,61d' anykernel.sh.bak >anykernel.sh
  zip -r9 "$ZIPNAME" META-INF tools anykernel.sh Image dtb.img version >/dev/null
  rm -f anykernel.sh
  mv anykernel.sh.bak anykernel.sh
fi
sha256sum "$ZIPNAME" >"${ZIPNAME}.sha256"
unzip -t "$ZIPNAME" >/dev/null && echo ZIP_OK
ls -la "$ZIPNAME"
echo "=== $CODENAME DONE ==="
