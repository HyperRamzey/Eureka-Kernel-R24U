# Eureka-Kernel R24U — Feature Roadmap

Research into post-v1 kernel features for the Exynos7885 family
(SM-A105F/A205F/A202F/A305F/A307FN/A405FN/M205F/A260F). Each section surveys
what already exists in this tree, what upstream work is required, and the
expected difficulty.

## Status legend

- 🟢 present in tree — research/verify only
- 🟡 code present but disabled in defconfig — enable + test
- 🟠 code absent — port required
- ⛔ infeasible on this base

---

## 1. ASV/ECT voltage-floor survey (thermal+battery headroom research)

**Status: 🟢 mechanism present; 🟠 tuning interface absent**

The ECT (Exynos Calibration Table) parser is fully present
(`drivers/soc/samsung/ect_parser.c`): DVFS domain tables, PLL tables, and
**voltage tables** are parsed from the `ect` firmware blob at boot
(`ect_parse_voltage_table` at ect_parser.c:273). The CPU DVFS driver consumes
the per-domain ASV table at runtime via `cal_dfs_get_asv_table()`
(drivers/cpufreq/exynos-acme.c:845), which selects the ASV bin the SoC was
binned to and applies the matching voltage table for each frequency.

What is **not** present: any runtime margin/override interface. Samsung
consuming code has no knob to add a positive voltage margin (undervolt is
hardwired into the per-ASV table in firmware). A "custom ASV" feature would
mean either:

- intercepting `ect_parse_voltage_table` and adding an offset before the
  table is consumed (port of the old Eureka "ASV underclock" mod), or
- a sysfs override on `domain->table_size`/voltage arrays post-parse.

**Difficulty: MEDIUM** (well-understood code path, but binning risk — an
aggressive margin hangs are exactly the kind of thing this project's boot
testing already exists for).

## 2. Baseband-guard (modem watchdog hardening)

**Status: 🟢 core present (Shannon ss310ap CP-state machine)**

The baseband control driver
(`drivers/misc/modem_v1/modem_ctrl_ss310ap.c`) implements the CP boot state
machine (CP_POWER_ON/OFF/CRASH_EXIT via `link_pm` and `link_ctrlmsg`), and
`modem_notifier.c`/`modem_argos_notifier.c` propagate CP state to the
network stack. What the vendor shipped **without**: an auto-recovery policy —
if the CP fails to reach ONLINE within the timeout, the vendor kernel simply
sits in `CP_CRASH_EXIT` and demands a manual reboot.

A "baseband-guard" module would be a small daemon+kernel hook that:
1. watches the CP state sysfs (`/sys/devices/.../modem_ctl/cp_state` or the
   modem_v1 misc char dev),
2. on repeated crash/restart cycles, forces an orderly modem subsystem reset
   (via `modem_force_crash_exit` or a `link_device` re-init) before user
   visibility,
3. rate-limits recovery attempts, then escalates to a warm reboot.

**Difficulty: LOW-MEDIUM** (all the hooks exist; the policy is new code, no
upstream port needed). Primary risk: fighting the Argos/modem notifier state
machine during recovery.

## 3. NTSync (Wine/Proton Windows-semaphore sync)

**Status: ⛔ infeasible as a faithful upstream port; 🟠 a reduced subset is portable**

Upstream `ntsync` (merged mainline 6.10) exposes the NT synchronization
primitives (semaphore/mutex/event counters with `NTSYNC_IOC_...` ioctls) on
a misc char device backed by extended futex internals (`futex_waitv`,
multi-wait, `atomic_try_cmpxchg`).

Against this 4.4.302 base:

- `atomic_try_cmpxchg` does not exist in 4.4 atomics (only
  `atomic_cmpxchg` + manual compare; a small compat shim suffices).
- `futex_waitv` (vector wait) — the heart of multi-object NT waits — is a
  5.16+ feature (915628) with large futex-core rework; backporting it into
  4.4's futex.c realistically means cherry-picking the 5.16 wait queue
  plumbing. The 4.4 futex hash-bucket infrastructure (futex.c:1980
  `futex_requeue`, plist wait queues) is still compatible with the older
  single-wait ntsync subset.
- Wine/Proton today only *prefers* ntsync; it fully falls back to esync/fsync
  userspace paths. That makes a **reduced ntsync** (semaphore + mutex + event,
  no waitv multi-wait) genuinely useful for gaming use-cases via Box86/64 +
  Wine on these A-series devices.

**Difficulty: HIGH for full fidelity (waitv backport is a mini-project),
MEDIUM for the reduced subset.**

## 4. Networking features: CIFS / TTL / IPset / connmark

**Status: mixed — one enable, one present, two full ports sitting in-tree**

| Feature | v1 config state | In-tree code | Action |
|---|---|---|---|
| **CIFS/SMB** | `CONFIG_CIFS is not set` | `fs/cifs/Kconfig` full backport-style tree (this vendor kernel carries a CIFS tree) | 🟡 enable `CONFIG_CIFS=y/m` (+ `CIFS_XATTR`, `CIFS_POSIX`) in defconfig — no code change |
| **TTL/HL target** | `CONFIG_IP_NF_TARGET_TTL=y`, `CONFIG_NETFILTER_XT_TARGET_HL=y` | present | 🟢 nothing to do — usable today (`-j TTL --ttl-set`) |
| **IPset** | `CONFIG_IP_SET is not set` | **complete ipset tree present** (`net/netfilter/ipset/` — bitmap/hash/list set types incl. `ip_set_hash_gen.h`) + `NETFILTER_XT_SET` Kconfig exists | 🟡 enable `CONFIG_IP_SET=y` + `CONFIG_IP_SET_HASH_IP` etc. + `NETFILTER_XT_SET` — no code change |
| **connmark** | `CONFIG_NETFILTER_XT_TARGET_CONNMARK=y`, match + save/restore present | present | 🟢 usable today |

The surprising finding: this vendor tree ships the **whole ipset framework
disabled**. Enabling it is a pure defconfig exercise (module-capable too:
`CONFIG_IP_SET=m` works with this kernel's module support — KSU modules
already prove module path works).

**Difficulty: LOW for CIFS/IPset (enable+test only).**

---

## 5. Deferred / dropped items

- **CIP upstream maintenance**: the CIP114 merge caused the F/G/H3 boot hang
  (root cause #3, proven by device testing); the project deliberately ships
  the vendor 4.4.302 base unmodified. Any future security backports must be
  individually cherry-picked and device-tested, never bulk-merged.
- **AROMA multi-variant installer**: v1 ships per-device AnyKernel3 zips
  (kernel + dtb + DM-verity-patched dtbo, or kernel-only for m20/jackpotlte).
  The original Eureka AROMA flow (variant menu + selinux enforcement + DTB
  freq variants) remains implementable later from the vendor's `build.sh`
  CUSTOM_DTB table if a single multi-device zip is wanted.

## Verification paths (how each item gets proven)

1. Any config-only item (CIFS, IPset): build all 8 devices via
   `tools_eureka/build_device.sh`, a30s first-boot smoke test, then release.
2. Kernel-code items (ASV margin, baseband-guard, ntsync): bisect-style
   single-variable test zips on the a30s, exactly the H2/H3 methodology that
   proved the current recipe.

## References

- ECT parser & DVFS voltage tables: `drivers/soc/samsung/ect_parser.c`
  (`ect_parse_voltage_table`); consumption: `drivers/cpufreq/exynos-acme.c`
  (`cal_dfs_get_asv_table`).
- modem CP state machine: `drivers/misc/modem_v1/modem_ctrl_ss310ap.c`,
  `modem_notifier.c`, `modem_argos_notifier.c`.
- ntsync upstream: merged in Linux 6.10 (misc char device `ntsync`);
  `futex_waitv` in 5.16+.
- ipset in-tree: `net/netfilter/ipset/` (complete framework, disabled);
  `net/netfilter/Kconfig:605` (`NETFILTER_XT_SET`).
- CIFS in-tree: `fs/cifs/Kconfig` (disabled).
