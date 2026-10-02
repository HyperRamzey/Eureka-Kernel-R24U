#ifndef _UAPI_LINUX_OPENAT2_H
#define _UAPI_LINUX_OPENAT2_H

#include <linux/types.h>

/*
 * The openat2() syscall significantly simplifies the security issues
 * associated with openat(2) and provides an alternative to the slower
 * O_PATH dance for sandboxing userspace.
 *
 * Re-declared here for the 4.4 tree. Values match upstream (5.6+) exactly:
 * a caller compiled against a newer bionic must see identical bits.
 *
 *   RESOLVE_NO_XDEV       Disallow traversal of bind mounts.
 *   RESOLVE_NO_MAGICLINKS Disallow all magic-link resolution.
 *   RESOLVE_NO_SYMLINKS   Disallow all symlink resolution.
 *   RESOLVE_BENEATH       Do not permit the resolution to escape dirfd.
 *                         Implies rejection of absolute symlinks.
 *   RESOLVE_IN_ROOT       Treat dirfd as the root of the (sub)filesystem.
 *   RESOLVE_CACHED        Do not consult the page cache.
 *
 * This tree honours NONE of these (see fs/open.c) and returns -EINVAL rather
 * than pretending.
 */
#define RESOLVE_NO_XDEV		0x01
#define RESOLVE_NO_MAGICLINKS	0x02
#define RESOLVE_NO_SYMLINKS	0x04
#define RESOLVE_BENEATH		0x08
#define RESOLVE_IN_ROOT		0x10
#define RESOLVE_CACHED		0x20

#define RESOLVE_MAX		0x3f

struct open_how {
	__u64		flags;
	__u64		mode;
	__u64		resolve;
};

#endif /* _UAPI_LINUX_OPENAT2_H */
