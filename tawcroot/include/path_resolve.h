/* Manual symlink-aware path canonicalization.
 *
 * tawcroot is not actually `chroot()`'d, so an in-rootfs symlink with
 * an *absolute* target (e.g. `/etc/host-secret -> /etc/passwd`) would
 * otherwise escape our rootfs view: the kernel would resolve the
 * absolute target against the host root, not against
 * `tawcroot_rootfs_fd`. This resolver pre-walks non-leaf rootfs-side
 * symlinks during translate, splicing absolute targets back through
 * the rootfs-fd-relative form so by the time the result reaches the
 * kernel-side syscall the path is already clamped.
 *
 * The leaf component is left to the kernel — for paths that route
 * through a bind src_fd the kernel does the right thing (chases the
 * leaf symlink against the host root, where binds expose real host
 * paths). For paths inside the rootfs the resolver also walks the
 * leaf when mode==FOLLOW, so the kernel never sees an unclamped
 * rootfs-internal absolute symlink.
 *
 * History note: an earlier design conditionally swapped this resolver
 * out for `openat2(RESOLVE_IN_ROOT)` inside `handle_openat` on kernel
 * >=5.6. That broke cross-bind absolute symlinks (Android's
 * `/system/lib64/libc.so → /apex/com.android.runtime/lib64/bionic/`
 * `libc.so` — the absolute target was re-rooted at the bind src
 * dirfd). The resolver is now the single contract regardless of
 * kernel version, and `handle_openat` always uses plain
 * `tawc_openat`. See test_prod_rootfs.c::prod_rootfs_cross_bind_abs_symlink.
 */

#pragma once

#include <stddef.h>

#include "path.h"
#include "path_oracle.h"

/* Resolve symlinks in `suf` (rootfs-relative path, no leading '/',
 * NUL-terminated) in place. On success, `suf` is overwritten with a
 * canonical rootfs-relative path with all symlinks followed per the
 * given mode. On failure, `suf` contents are unspecified.
 *
 * Mode behavior:
 *   FOLLOW         — every component including the leaf
 *   NOFOLLOW
 *   PARENT_CREATE
 *   PARENT_REMOVE  — parent components only; leaf preserved verbatim
 *
 * Returns 0 on success, -errno on failure:
 *   -ELOOP        — exceeded SYMLOOP_MAX (40) symlink hops (covers both
 *                   self-loop and chain-bomb cases)
 *   -ENAMETOOLONG — splicing a target into the path overflowed `cap`
 *
 * If `oracle->readlink` returns -EINVAL for a component (i.e. the
 * component is not a symlink), resolution continues to the next
 * component. If it returns -ENOENT (component missing) or any other
 * error, the resolver stops walking and returns 0; downstream syscalls
 * get to surface their own kernel-defined error (ENOENT / ENOTDIR / ...
 * depending on context). This deferral is deliberate — the resolver's
 * job is symlink clamping, not stat-style existence checking.
 */
long tawcroot_path_resolve_symlinks(char *suf, size_t cap,
				    tawcroot_path_mode mode,
				    const struct tawcroot_path_oracle *oracle);

/* Token-aware variant (hardlink emulation, linkstore.h): when a
 * readlink probe surfaces the opaque `tawcroot:link:<token>` target,
 * the walk stops, `suf` is rewritten to `<token><remainder>` (the
 * remainder keeps trailing components so a mid-path token — a file
 * used as a directory — yields the kernel's own ENOTDIR downstream),
 * and `*token_hit` is set. The caller (orchestrator) re-bases the
 * result at the store's link/ dirfd. With `token_hit == NULL` this is
 * exactly the legacy function: token targets are treated as ordinary
 * relative targets (only reachable in unit tests — production always
 * passes the flag). */
long tawcroot_path_resolve_symlinks_tok(char *suf, size_t cap,
					tawcroot_path_mode mode,
					const struct tawcroot_path_oracle *oracle,
					int *token_hit);
