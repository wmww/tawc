/* Filesystem oracle used by the manual symlink resolver.
 *
 * The resolver walks a guest path component-by-component and asks the
 * oracle "is this prefix a symlink? if so, what's the target?" via a
 * single readlink op. In production the oracle wraps raw `readlinkat`
 * against `tawcroot_rootfs_fd`; in unit tests, a mock supplies an
 * in-memory FS so the resolver can be exercised without touching disk.
 *
 * Semantics of `readlink`:
 *   - `suffix` is rootfs-relative, no leading '/', NUL-terminated.
 *     Empty string means "the rootfs root itself" — never a symlink,
 *     return -EINVAL.
 *   - On success: returns target length (positive); writes target into
 *     `out` (without NUL). Caller passes `out_cap-1` to leave room for
 *     the resolver's own NUL.
 *   - Not a symlink (regular file, dir, etc.): return -EINVAL.
 *   - Path component missing: return -ENOENT.
 *   - Other error: return -errno (treated as fatal by the resolver).
 *
 * Async-signal-safe in production (calls go through `tawcroot_raw_syscall`).
 * The mock used in tests only runs in the cleat-orchestrator process under
 * hosted glibc, which never enters a SIGSYS handler — signal-safety doesn't
 * apply there.
 */

#pragma once

#include <stddef.h>

typedef long (*tawcroot_path_readlink_fn)(void *ctx, const char *suffix,
					  char *out, size_t out_cap);

/* Hardlink emulation: readlink a link OBJECT by token (linkstore.h).
 * Same return contract as `readlink` — -EINVAL when the object is not
 * a symlink (the overwhelmingly common case), -ENOENT when it is
 * missing (dangling token). Only consulted when a walk hits an opaque
 * `tawcroot:link:<token>` target: a symlink object (hardlink-of-a-
 * symlink) has its target spliced back into the guest-side walk —
 * relative targets resolve against the NAME's directory, exactly like
 * a real hardlinked symlink. May be NULL (no store). */
typedef long (*tawcroot_path_readlink_store_fn)(void *ctx, const char *token,
						char *out, size_t out_cap);

struct tawcroot_path_oracle {
	void                            *ctx;
	tawcroot_path_readlink_fn        readlink;
	tawcroot_path_readlink_store_fn  readlink_store;  /* may be NULL */
};
