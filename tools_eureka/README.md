# Eureka build tools

- `build_device.sh <codename>` — build one device with the proven recipe
  (vendor `full/` defconfig + Neutron Clang 24, vendor flag style, **no `LLVM=1`**).
  Produces `Eureka-R24U-KSU_<codename>.zip` (AnyKernel3) in `kernel_zip/anykernel/`.
- `dtbo_gen.py <stock_dtbo> <out_dtbo>` — regenerates the DM-verity-patched dtbo
  (strips the AVB `firmware` node, sets `customs=(hw_rev, hw_rev_end)`), byte-identical
  to the original R24U release dtbo. Called by `build_device.sh`.

Codenames: a10 a20 a20e a30 a30s a40 m20 jackpotlte.
