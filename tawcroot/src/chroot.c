/* chroot(2) emulation — handler-side root-view swap.
 *
 * See include/chroot.h for the why; notes/tawcroot/path-translation.md §"chroot
 * emulation" for the full design. The handler:
 *
 *   1. Pulls the guest path through usercopy (EFAULT-safe).
 *   2. Translates via tawcroot_path_translate(FOLLOW). Bind/memo/
 *      symlink resolution all run; the result is a (base_fd, suffix)
 *      pair pointing at the *target* of the chroot.
 *   3. Opens that target O_PATH | O_DIRECTORY | O_CLOEXEC. Wrong
 *      kind (ENOTDIR), missing (ENOENT), permission denied — these
 *      surface to the guest verbatim; we don't sugarcoat.
 *   4. Reserves the new fd into the high-fd range via fd_reserve so
 *      the guest can't close it.
 *   5. Reads /proc/self/fd/<new_fd> for the canonical host path of
 *      the new root (kernel resolves any symlinks/binds in our
 *      openat path; we want what the kernel sees, not what we asked
 *      for).
 *   6. Calls tawcroot_path_binds_reanchor to filter the bind table:
 *      surviving binds get their dst rewritten to be relative to
 *      the new root; binds outside the new view are deactivated.
 *      The deactivated src_fds stay reserved (un-closeable from the
 *      guest) but no longer route any paths.
 *   7. Replaces tawcroot_rootfs_fd and tawcroot_rootfs_host_path.
 *      The old fd stays in the reserved table — leaking exactly one
 *      fd per chroot. The reserved-fd table is bounded
 *      (TAWCROOT_MAX_RESERVED_FDS == 64); a process that loops
 *      chroot calls will eventually exhaust the table and subsequent
 *      chroots return -ENOSPC. In every workload we target the
 *      chroot happens before execve (post-execve the table is empty
 *      again), so the cap doesn't bite. Same posture as deactivated
 *      bind src_fds.
 *   8. Rebuilds the well-known-symlink memo cache against the new
 *      root: the old cache memoized lib/lib64/usr/lib/etc against
 *      the *outer* root and is no longer correct.
 *
 * Step 6 also discards `tawcroot_path_binds_reanchor`'s aggregate
 * return value: a bind whose stripped dst would overflow `b->dst[]`
 * is silently deactivated like any outside-of-view bind, mirroring
 * the dst-too-long rejection path in `tawcroot_path_add_bind`. The
 * handler has no log channel; surfacing the ENAMETOOLONG would
 * mean wedging it into chroot's own return value, which the guest
 * would then see as a bogus chroot failure.
 *
 * Failure modes that abort the whole operation (rootfs_fd / bind
 * table left untouched):
 *   - usercopy of the guest path fails → return -EFAULT.
 *   - path translate fails → propagate that errno.
 *   - openat fails → propagate (-ENOENT / -ENOTDIR / -EACCES / …).
 *   - fd_reserve fails (table full) → return -ENOSPC; close the
 *     unreserved fd to avoid leaking.
 *   - readlink of /proc/self/fd/<new_fd> fails (very surprising —
 *     means /proc isn't mounted) → close new_fd, return -EIO.
 *
 * Steps 6/7/8 are interlocked by ordering: bind reanchoring uses
 * the OLD rootfs host path before we overwrite it, then we swap fd
 * and host_path together, then memos get refilled against the new
 * fd. A SIGSYS-handler trap on another thread mid-sequence sees a
 * (mostly) consistent view; we can't make this fully atomic without
 * locking, which the handler invariants forbid. In practice chroot
 * is a startup-time event (or a once-only hop in pacman 6.x); the
 * window is single-digit microseconds.
 *
 * pivot_root continues to be denied with -EPERM via the
 * `fake_eperm` registration in syscalls_control.c — different
 * syscall, different dispatch slot. The semantics aren't
 * emulatable in our model (pivot_root requires the new root and
 * old root to be separately mounted; we don't model mounts at
 * all), and no targeted workload hits it. The chroot handler
 * covers what we actually need.
 */

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "chroot.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "syscalls_control.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Translate the guest's chroot target to (base_fd, suffix) and open
 * an O_PATH dirfd for it. On success returns >=0 (the new fd, not yet
 * reserved) and sets *ro_out to the translation's RO bit; on failure
 * returns -errno. Chroot itself is a read operation and is ALLOWED
 * into an RO bind (like the kernel); the bit is what makes the whole
 * root view read-only after the swap. */
static long open_chroot_target(const char *guest_path, int *ro_out)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	long n = tawc_copy_string_from_guest(
		path_buf, TAWCROOT_PATH_SCRATCH_SIZE, guest_path);
	if (n < 0) return n;

	char *suffix = scratch->buf[1];
	tawcroot_path_result r = tawcroot_path_translate(
		path_buf, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
		TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
	if (r.err) return r.err;
	*ro_out = r.ro;

	/* tawcroot_path_translate writes "" into suffix when the request
	 * resolves to the directory base_fd already refers to (e.g.,
	 * chroot("/") with no binds). openat() on "" returns -ENOENT on
	 * kernels < 6.6 even with AT_EMPTY_PATH; openat on "." resolves
	 * to "the directory base_fd refers to" on every kernel we
	 * target, which is what we want. */
	const char *p = suffix[0] ? suffix : ".";
	int flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
	return tawc_openat(r.base_fd, p, flags, 0);
}

static long handle_chroot(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return TAWC_EFAULT;

	int new_root_ro = 0;
	long new_fd = open_chroot_target(gpath, &new_root_ro);
	if (new_fd < 0) return new_fd;

	long resv = tawcroot_fd_reserve((int)new_fd);
	if (resv < 0) {
		tawc_close((int)new_fd);
		return resv;
	}
	int new_root_fd = (int)resv;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *new_host = scratch->buf[0];
	long hp = tawcroot_proc_fd_to_host_path(new_root_fd, new_host,
	                                        TAWCROOT_PATH_SCRATCH_SIZE);
	if (hp < 0) {
		/* Can't recover the host path — abort. The reserved fd
		 * stays in tawcroot_reserved_fds, leaking until process
		 * exit, but the guest sees the rootfs view unchanged. */
		return hp;
	}
	size_t new_host_len = (size_t)hp;

	/* Re-anchor binds against the new root BEFORE overwriting the
	 * old host_path string — binds_reanchor needs the old prefix to
	 * compute each bind's host-side coordinates. */
	(void)tawcroot_path_binds_reanchor(
		tawcroot_binds, tawcroot_n_binds,
		tawcroot_rootfs_host_path, tawcroot_rootfs_host_path_len,
		new_host, new_host_len);

	/* Swap rootfs_fd + host_path.
	 *
	 * Update order: clear `_len = 0` first so a concurrent reader
	 * doing the standard "read len, then iterate len bytes" pattern
	 * sees either the OLD complete view or a transient zero-length
	 * (interpreted by every reader as "cwd outside the rootfs"
	 * → -ENOENT). Then overwrite the path bytes (only the first
	 * new_host_len + 1 are written; tail bytes from any longer prior
	 * value retain stale data but are unreachable since every reader
	 * bounds on `_len`). Finally publish the new length and swap fd.
	 *
	 * This is NOT a full memory ordering guarantee — without
	 * explicit barriers, an aarch64 reader could observe the new
	 * `_len` before the path bytes — but the failure mode is bounded
	 * (the reader returns -ENOENT and the guest retries, which
	 * works post-update). See notes/tawcroot/path-translation.md §"chroot emulation"
	 * for the full race analysis. */
	tawcroot_rootfs_host_path_len = 0;
	for (size_t i = 0; i < new_host_len; i++)
		tawcroot_rootfs_host_path[i] = new_host[i];
	tawcroot_rootfs_host_path[new_host_len] = 0;
	tawcroot_rootfs_host_path_len = new_host_len;
	tawcroot_rootfs_fd = new_root_fd;
	/* Chroot into an RO bind dst makes the whole view read-only:
	 * after the swap, routing goes through rootfs_fd rather than the
	 * (now re-anchored / deactivated) bind, so the flag must ride the
	 * root-view globals. Chroot into an RW target from an RO root
	 * un-sets it, like the kernel's mount flags would. An RW bind
	 * nested under the RO dst stays writable via the surviving
	 * re-anchored bind entry — longest-prefix match, no extra code. */
	tawcroot_root_ro = new_root_ro;

	/* Re-memoize against the new root. The old memos (lib → usr/lib
	 * etc.) point at the OUTER root's symlinks; rebuilding picks up
	 * whatever the inner root's well-known directories actually
	 * resolve to. Empty (no symlinks at the well-known names) is
	 * fine — the symlink resolver covers everything memos miss. */
	tawcroot_path_memoize_well_known();

	return 0;
}

void tawcroot_chroot_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_chroot, handle_chroot);
}
