#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include <linux/hashtable.h>
#include <linux/path.h>
#include <linux/susfs_def.h>
#include <linux/statfs.h>

#define SUSFS_VERSION "v2.3.0"
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,0,0)
#define SUSFS_VARIANT "NON-GKI"
#else
#define SUSFS_VARIANT "GKI"
#endif

/*********/
/* MACRO */
/*********/
#define getname_safe(name) (name == NULL ? ERR_PTR(-EINVAL) : getname(name))
#define putname_safe(name) (IS_ERR(name) ? NULL : putname(name))

/********/
/* ENUM */
/********/
enum UID_SCHEME {
	UID_NON_APP_PROC = 0,
	UID_ROOT_PROC_EXCEPT_SU_PROC,
	UID_NON_SU_PROC,
	UID_UMOUNTED_APP_PROC,
	UID_UMOUNTED_PROC,
};

/**********/
/* STRUCT */
/**********/
/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
/* Tool wire ABI (prctl era, ksu_susfs universal binary):
 *   struct sus_path_v1  { unsigned long target_ino; char target_pathname[256]; }         (264B)
 *   struct sus_path_v154 { unsigned long target_ino; char target_pathname[256];
 *                          unsigned int i_uid; }                                         (272B)
 * The kernel resolves the inode itself via kern_path(), so only the pathname
 * (offset 8) is consumed; reading 264B is safe for both tool variants.
 * No err field on the wire — errors go to prctl arg5 only. */
struct st_susfs_sus_path {
	unsigned long                           target_ino;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
};

struct st_susfs_sus_path_list {
	struct list_head                        list;
	struct st_susfs_sus_path                info;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
};
#endif

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
/* Wire ABI: struct sus_mount_v1 { char target_pathname[256]; unsigned long target_dev; } (264B)
 * The kernel resolves target_dev itself via kern_path(); only pathname (offset 0) is consumed. */
struct st_susfs_sus_mount {
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	unsigned long                           target_dev;
};

struct st_susfs_sus_mount_list {
	struct list_head                        list;
	struct st_susfs_sus_mount               info;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
};

/* try_umount — wire ABI: struct try_umount_v1 { char path[256]; int mnt_mode; } (260B)
 * mnt_mode: 0 = plain umount, 1 = MNT_DETACH. Bridged into the KernelSU mount_list. */
struct st_susfs_try_umount {
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	int                                     mnt_mode;
};
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

/* sus_kstat */
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
#define KSTAT_SPOOF_INO (1 << 0)
#define KSTAT_SPOOF_DEV (1 << 1)
#define KSTAT_SPOOF_NLINK (1 << 2)
#define KSTAT_SPOOF_SIZE (1 << 3)
#define KSTAT_SPOOF_ATIME_TV_SEC (1 << 4)
#define KSTAT_SPOOF_ATIME_TV_NSEC (1 << 5)
#define KSTAT_SPOOF_MTIME_TV_SEC (1 << 6)
#define KSTAT_SPOOF_MTIME_TV_NSEC (1 << 7)
#define KSTAT_SPOOF_CTIME_TV_SEC (1 << 8)
#define KSTAT_SPOOF_CTIME_TV_NSEC (1 << 9)
#define KSTAT_SPOOF_BLOCKS (1 << 10)
#define KSTAT_SPOOF_BLKSIZE (1 << 11)
#define KSTAT_SPOOF_ALL (KSTAT_SPOOF_INO | KSTAT_SPOOF_DEV | KSTAT_SPOOF_NLINK | \
			 KSTAT_SPOOF_SIZE | KSTAT_SPOOF_ATIME_TV_SEC | KSTAT_SPOOF_ATIME_TV_NSEC | \
			 KSTAT_SPOOF_MTIME_TV_SEC | KSTAT_SPOOF_MTIME_TV_NSEC | \
			 KSTAT_SPOOF_CTIME_TV_SEC | KSTAT_SPOOF_CTIME_TV_NSEC | \
			 KSTAT_SPOOF_BLOCKS | KSTAT_SPOOF_BLKSIZE)

/* Wire ABI (prctl era): struct sus_kstat_v1 — 368 bytes exactly:
 *   bool is_statically;                (1B @0, padded to 8)
 *   unsigned long target_ino;           @8
 *   char target_pathname[256];          @16
 *   unsigned long spoofed_ino;          @272
 *   unsigned long spoofed_dev;          @280
 *   unsigned int spoofed_nlink;         @288 (padded to 296)
 *   long long spoofed_size;             @296
 *   long spoofed_atime_tv_sec;          @304
 *   long spoofed_mtime_tv_sec;          @312
 *   long spoofed_ctime_tv_sec;          @320
 *   long spoofed_atime_tv_nsec;         @328
 *   long spoofed_mtime_tv_nsec;         @336
 *   long spoofed_ctime_tv_nsec;         @344
 *   unsigned long spoofed_blksize;      @352
 *   unsigned long long spoofed_blocks;  @360
 * NOTE the tool groups the three sec fields then the three nsec fields, and
 * puts blksize BEFORE blocks — the old backport interleaved sec/nsec and
 * swapped blocks/blksize, which scrambled every spoofed kstat. */
struct st_susfs_sus_kstat {
	bool                                    is_statically;
	unsigned long                           target_ino;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	unsigned long                           spoofed_ino;
	unsigned long                           spoofed_dev;
	unsigned int                            spoofed_nlink;
	long long                               spoofed_size;
	long                                    spoofed_atime_tv_sec;
	long                                    spoofed_mtime_tv_sec;
	long                                    spoofed_ctime_tv_sec;
	long                                    spoofed_atime_tv_nsec;
	long                                    spoofed_mtime_tv_nsec;
	long                                    spoofed_ctime_tv_nsec;
	unsigned long                           spoofed_blksize;
	unsigned long long                      spoofed_blocks;
};

struct st_susfs_sus_kstat_hlist {
	struct hlist_node                       node;
	unsigned long                           target_ino;
	unsigned long                           target_dev;
	struct kstatfs                          spoofed_kstatfs;
	int                                     spoofed_mnt_id;
	bool                                    is_fuse;
	int                                     flags; /* kernel-internal: which fields to spoof */
	struct st_susfs_sus_kstat               info;
};
#endif

/* spoof_uname */
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
/* Wire ABI: struct uname_v1 { char release[65]; char version[65]; } (130B, no err) */
struct st_susfs_uname {
	char                                    release[__NEW_UTS_LEN+1];
	char                                    version[__NEW_UTS_LEN+1];
};
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
/* Wire ABI: arg3 points to a raw NUL-terminated buffer holding the fake
 * cmdline/bootconfig contents (max SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE).
 * No struct, no err field. */
#endif

/* open_redirect */
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
/* Wire ABI (prctl era): struct open_redirect_v1 — 520 bytes:
 *   unsigned long target_ino;            @0
 *   char target_pathname[256];           @8
 *   char redirected_pathname[256];        @264
 * No uid_scheme on the wire (v2100+ only) — the kernel defaults it. */
struct st_susfs_open_redirect {
	unsigned long                           target_ino;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	char                                    redirected_pathname[SUSFS_MAX_LEN_PATHNAME];
};

struct st_susfs_open_redirect_hlist {
	struct hlist_node                       node;
	unsigned long                           target_ino;
	unsigned long                           target_dev;
	unsigned long                           redirected_ino;
	unsigned long                           redirected_dev;
	int                                     spoofed_mnt_id;
	struct kstatfs                          spoofed_kstatfs;
	struct st_susfs_open_redirect           info;
	int                                     uid_scheme; /* kernel-internal */
	bool                                    reversed_lookup_only;
};
#endif

/* sus_map */
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
/* Wire ABI: struct sus_map_v1 { char target_pathname[256]; } (256B, no err) */
struct st_susfs_sus_map {
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
};
#endif

/* get enabled features */
/* Wire ABI, dual mode:
 *  - arg4 == 0: v1.5.3-1.5.8 bitmask — arg3 points to an unsigned long; the
 *    kernel writes a u64 feature bitmask (bit order = the tools g_feature_names_154
 *    table: 0 SUS_PATH, 1 SUS_MOUNT, 2 AUTO_ADD_KSU_DEFAULT_MOUNT, 3 AUTO_ADD_SUS_BIND_MOUNT,
 *    4 SUS_KSTAT, 5 SUS_OVERLAYFS, 6 TRY_UMOUNT, 7 AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT,
 *    8 SPOOF_UNAME, 9 ENABLE_LOG, 10 HIDE_KSU_SUSFS_SYMBOLS, 11 SPOOF_CMDLINE_OR_BOOTCONFIG,
 *    12 OPEN_REDIRECT, 13 SUS_SU, 14 HAS_MAGIC_MOUNT).
 *  - arg4 != 0: v1.5.9+ string — arg3 points to a buffer of size arg4; the
 *    kernel writes the NUL-terminated "CONFIG_KSU_SUSFS_*\n" list bounded by it. */

/* show variant */
/* Wire ABI: arg3 = char buf[16]; kernel writes the variant string (max 16B incl. NUL). */

/* show version */
/* Wire ABI: arg3 = char buf[16]; kernel writes the version string (max 16B incl. NUL). */

/***********************/
/* FORWARD DECLARATION */
/***********************/
/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
int susfs_add_sus_path(struct st_susfs_sus_path __user *user_info);
int susfs_add_sus_path_loop(struct st_susfs_sus_path __user *user_info);
#endif

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
int susfs_add_sus_mount(struct st_susfs_sus_mount __user *user_info);
int susfs_set_hide_sus_mnts_for_non_su_procs(unsigned long enabled);
int susfs_add_try_umount(struct st_susfs_try_umount __user *user_info);
int susfs_is_sus_mount(const struct path *mnt_path, const struct path *root);
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

/* sus_kstat */
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
int susfs_add_sus_kstat(struct st_susfs_sus_kstat __user *user_info);
int susfs_update_sus_kstat(struct st_susfs_sus_kstat __user *user_info);
#endif

/* spoof_uname */
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
int susfs_set_uname(struct st_susfs_uname __user *user_info);
void susfs_spoof_uname(struct new_utsname* tmp);
#endif

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
int susfs_enable_log(unsigned long enabled);
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
int susfs_set_cmdline_or_bootconfig(const char __user *user_buf);
#endif

/* open_redirect */
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
int susfs_add_open_redirect(struct st_susfs_open_redirect __user *user_info, unsigned long uid_scheme_arg);
#endif

/* sus_map */
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
int susfs_add_sus_map(struct st_susfs_sus_map __user *user_info);
#endif

int susfs_set_avc_log_spoofing(unsigned long enabled);

int susfs_get_enabled_features(void __user *user_buf, unsigned long bufsz);
int susfs_show_variant(char __user *user_buf);
int susfs_show_version(char __user *user_buf);

void susfs_start_sdcard_monitor_fn(void);

/* susfs_init */
int susfs_init(void);

#endif
