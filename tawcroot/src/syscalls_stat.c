/* stat-family handlers: newfstatat, fstat, statx (+ STATX_MNT_ID
 * emulation). Every result is decorated root-owned; emulated
 * hardlink names report the link object. Path plumbing is shared
 * with syscalls_fs.c via syscalls_fs_internal.h. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "linkstore.h"
#include "path_scratch.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "shm.h"
#include "syscalls_fs.h"
#include "syscalls_fs_internal.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Hardlink emulation, stat side. An emulated name is a symlink whose
 * target is the opaque `tawcroot:link:<token>` literal (linkstore.h).
 * NOFOLLOW stats must report the *object*: real mode, shared st_ino,
 * st_nlink from the sidecar count. Detection is reactive — only paid
 * when the leaf actually stat'd as a symlink (+1 readlinkat), so
 * non-symlink hot paths pay nothing.
 *
 * Returns 1 when `local` was replaced with the object's stat, 0 when
 * the entry is not an emulated name (or the object is missing — the
 * dangling name then keeps behaving as a plain symlink, which keeps it
 * visible and unlinkable for cleanup). */
static int stat_fixup_emulated(int fd, const char *path, struct stat *local)
{
	if (tawcroot_store_link_fd < 0) return 0;
	if (!S_ISLNK(local->st_mode)) return 0;
	char tok[TAWCROOT_LINK_TOKEN_MAX];
	if (tawcroot_link_leaf_token(fd, path, tok, sizeof tok) != 1)
		return 0;
	struct stat ost;
	long rv = TAWC_RAW(TAWC_SYS_fstatat, tawcroot_store_link_fd,
			   (long)tok, (long)&ost, AT_SYMLINK_NOFOLLOW, 0, 0);
	if (rv != 0) return 0;
	ost.st_nlink = (__typeof__(ost.st_nlink))
		tawcroot_link_count_for_stat(tok);
	*local = ost;
	return 1;
}

/* FOLLOW stats that landed in the store (the resolver mapped a token):
 * the kernel already stat'd the object; only st_nlink needs the
 * sidecar count. */
static void stat_fix_nlink_in_store(int fd, const char *path,
				    struct stat *local)
{
	if (fd != tawcroot_store_link_fd || tawcroot_store_link_fd < 0)
		return;
	local->st_nlink = (__typeof__(local->st_nlink))
		tawcroot_link_count_for_stat(path);
}

/* fd-based stats (fstat, AT_EMPTY_PATH fstatat/statx) of an open link
 * OBJECT need the sidecar count too. The plan first accepted fd-nlink
 * staying 1 ("no known consumer compares fd-nlink to path-nlink...
 * revisit only on evidence") — the evidence arrived immediately: GNU
 * tar CREATE registers hardlinks from the fstat of the fd it just
 * opened, not the fstatat it walked with, so an object fd reporting
 * nlink 1 silently dissolves hardlink structure at archive time (tar
 * stored both names with full data). Cost gates: only S_ISREG/S_ISLNK
 * with kernel nlink 1 ON THE STORE'S OWN FILESYSTEM (objects cannot
 * live elsewhere — skips /system binds, memfds) while a store is open
 * pays one /proc/self/fd readlink; the sidecar read happens on actual
 * store hits only. Returns the count, or 0 for "not an object". */
static unsigned long fd_object_nlink(int fd, unsigned int mode,
				     unsigned long nlink, unsigned long dev)
{
	if (tawcroot_store_link_fd < 0) return 0;
	if (nlink != 1 || dev != tawcroot_store_dev) return 0;
	if (!S_ISREG(mode) && !S_ISLNK(mode)) return 0;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *hostp = scratch->buf[0];
	long n = tawcroot_proc_fd_to_host_path(fd, hostp,
					       TAWCROOT_PATH_SCRATCH_SIZE);
	if (n <= 0) return 0;
	char tok[TAWCROOT_LINK_TOKEN_MAX];
	if (!tawcroot_link_host_path_token(hostp, tok, sizeof tok)) return 0;
	return tawcroot_link_count_for_stat(tok);
}

/* Decorate uid/gid in a kernel `struct stat` to look root-owned. The
 * member layout differs between x86_64 and aarch64 (see bionic's
 * __STAT64_BODY in <sys/stat.h>), but `st_uid` / `st_gid` are valid
 * member references on both. The struct is the kernel's `newfstatat`
 * output layout — bionic's `struct stat` is defined to match. */
static void decorate_stat(struct stat *st)
{
	st->st_uid = 0;
	st->st_gid = 0;
}

/* Shared tail for the stat-shaped handlers: on kernel success,
 * decorate the kernel-filled `local` as root-owned and copy it to the
 * guest's out-pointer. Returns `rv` or the copy's -EFAULT. */
static long finish_stat(long rv, struct stat *local, struct stat *guest_out)
{
	if (rv != 0) return rv;
	decorate_stat(local);
	long ce = tawc_copy_to_guest(guest_out, local, sizeof *local);
	return ce < 0 ? ce : 0;
}

static long handle_newfstatat(const tawcroot_syscall_args *args,
			      ucontext_t *uc)
{
	(void)uc;
	int    dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	struct stat *out = (struct stat *)(uintptr_t)args->c;
	int    flags = (int)args->d;

	if (!out) return TAWC_EFAULT;

	struct stat local;

	/* AT_EMPTY_PATH (kernel 6+ — used by io_uring + glibc fstat) means
	 * "stat the file referred to by dirfd". glibc's fstat() implements
	 * this as fstatat(fd, "", &st, AT_EMPTY_PATH) with a NON-NULL empty
	 * string, NOT a NULL pointer. Earlier we only short-circuited
	 * gpath==0; the gpath="" case fell through to tawcroot_fs_translate_at
	 * which then translates the empty string against the kernel cwd
	 * and stats the wrong inode (cwd dir instead of dirfd). wc, which
	 * uses fstat() on its input fd to decide buffer strategy, then read
	 * stale size and segfaulted. We pass through as-is (the dirfd is
	 * the guest's, but we don't yet track guest fd
	 * provenance — the kernel will see whatever fd it is and stat the
	 * corresponding inode). */
	if (flags & AT_EMPTY_PATH) {
		long empty = tawcroot_fs_path_is_empty(gpath);
		if (empty < 0) return empty;
		if (empty) {
			if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
			long rv = TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)"",
					   (long)&local, flags, 0, 0);
			if (rv == 0) {
				unsigned long c = fd_object_nlink(
					dirfd, local.st_mode, local.st_nlink,
					(unsigned long)local.st_dev);
				if (c) local.st_nlink =
					(__typeof__(local.st_nlink))c;
			}
			return finish_stat(rv, &local, out);
		}
	}

	if (!gpath) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long fe = tawcroot_fs_fetch_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = tawcroot_fs_classify_shm(scratch->buf[0], &shm_name);
		if (kind != SHM_PEEK_NONE) {
			long r = (kind == SHM_PEEK_NAME)
				? tawcroot_shm_stat_name(shm_name, &local)
				: (tawcroot_shm_stat_dir(&local), 0L);
			if (r < 0) return r;
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return 0;
		}
	}

	/* /proc shadows. Without this the path would be translated and
	 * handed to the kernel, hitting the real (SELinux-denied) inode —
	 * so /proc/stat read fine but stat'ed EACCES, and LibreOffice,
	 * which only ever STATS /proc/version, never started. Flags don't
	 * matter: every shadowed path is a regular file, so
	 * AT_SYMLINK_NOFOLLOW makes no difference. */
	{
		int kind = tawcroot_fs_proc_shadow_classify_at(scratch, dirfd,
							       scratch->buf[0]);
		if (kind != TAWCROOT_PROC_SHADOW_NONE) {
			long r = tawcroot_proc_shadow_stat(kind, &local);
			if (r < 0) return r;
			return finish_stat(0, &local, out);
		}
	}

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	struct fs_path t;
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd, pmode,
					     TAWCROOT_PATH_INTENT_READ, &t);
	if (e) return e;

	const char *resolved = t.path;
	int         rv_flags = flags;
	if (t.is_root) {
		resolved = "";
		rv_flags |= AT_EMPTY_PATH;
	}

	long rv = TAWC_RAW(TAWC_SYS_fstatat, t.fd, (long)resolved,
			   (long)&local, rv_flags, 0, 0);
	if (rv == 0) {
		if (pmode == TAWCROOT_PATH_NOFOLLOW)
			(void)stat_fixup_emulated(t.fd, t.path, &local);
		else
			stat_fix_nlink_in_store(t.fd, t.path, &local);
	}
	return finish_stat(rv, &local, out);
}

/* statx with fake-root decoration. Layout differs from `struct stat`
 * — see <linux/stat.h>. We always set uid/gid to 0 (matches proot `-0`)
 * AND set the corresponding stx_mask bits so the guest sees the fields
 * as populated. Without the mask update, a guest that didn't request
 * STATX_UID would see a kernel-zero (because the kernel didn't fill it)
 * AND mask=0 — semantically "uid not asked for", which is consistent
 * but doesn't reflect our fake-root view. (Review C11.) */
#ifndef STATX_UID
# define STATX_UID 0x00000008U
#endif
#ifndef STATX_GID
# define STATX_GID 0x00000010U
#endif
#ifndef STATX_NLINK
# define STATX_NLINK 0x00000004U
#endif
#ifndef STATX_INO
# define STATX_INO 0x00000100U
#endif
#ifndef STATX_MNT_ID
# define STATX_MNT_ID 0x00001000U
#endif
#ifndef STATX_MNT_ID_UNIQUE
# define STATX_MNT_ID_UNIQUE 0x00004000U
#endif
/* STATX_MNT_ID emulation for pre-5.8 kernels.
 *
 * systemd >=260 (kernel baseline 5.10) statx()es with STATX_MNT_ID and
 * hard-fails with EUNATCH when the result lacks it — the old
 * /proc/self/fdinfo fallback was removed. Debian sid's systemd tools
 * (machine-id-setup, tmpfiles, sysusers, anything chase()-based) are
 * therefore dead on 5.4-kernel devices. Emulate what the kernel can't:
 * parse `mnt_id:` from /proc/self/fdinfo of an fd on the statx target —
 * the same number statx would have returned. Best-effort: on any
 * failure the mask bit stays clear and the guest sees the kernel's
 * answer unchanged. Only runs when the guest asked for a mount id and
 * the kernel didn't deliver one, so 5.8+ kernels never take this path. */
static void statx_mnt_id_from_fdinfo(int fd, struct statx *sx)
{
	char path[40];
	size_t pos = 0;
	if (tawc_str_append(path, sizeof path, &pos, "/proc/self/fdinfo/"))
		return;
	if (tawc_str_append_dec(path, sizeof path, &pos, fd))
		return;
	long ifd = TAWC_RAW(TAWC_SYS_openat, AT_FDCWD, (long)path,
			    O_RDONLY | O_CLOEXEC, 0, 0, 0);
	if (ifd < 0) return;
	/* mnt_id sits in the kernel's fixed generic header (pos/flags/
	 * mnt_id) ahead of any ->show_fdinfo extras; one short read
	 * always covers it. */
	char buf[256];
	long n = TAWC_RAW(TAWC_SYS_read, ifd, (long)buf,
			  (long)(sizeof buf - 1), 0, 0, 0);
	TAWC_RAW(TAWC_SYS_close, ifd, 0, 0, 0, 0, 0);
	if (n <= 0) return;
	buf[n] = 0;
	const char *p = buf;
	while (*p) {
		if (tawc_starts_with(p, "mnt_id:")) {
			p += 7;
			while (*p == ' ' || *p == '\t') p++;
			if (*p < '0' || *p > '9') return;
			sx->stx_mnt_id = (__u64)tawc_parse_long(p);
			sx->stx_mask |= STATX_MNT_ID;
			return;
		}
		while (*p && *p != '\n') p++;
		if (*p) p++;
	}
}

/* Fill stx_mnt_id for a statx result `sx` describing the target at
 * (dirfd, path), when the guest requested a mount id and the kernel
 * result lacks one. Empty/NULL `path` means the target is `dirfd`
 * itself (AT_FDCWD pins the cwd — fdinfo has no entry for the
 * sentinel). Exported for the shm statx synthesizers (shm.c), which
 * fill from their memfds, and for the hosted parser test.
 *
 * The leaf open is O_NOFOLLOW unconditionally: under FOLLOW resolution
 * the caller's path already has its leaf fully resolved by the
 * translator (the kernel must never see an unclamped symlink —
 * path_resolve.h), so a symlink here only appears if a guest thread
 * swapped the leaf after the caller's statx; pinning it keeps the
 * lookup on the correct mount instead of letting the kernel chase an
 * absolute target across the host root. The dev/ino guard below
 * closes the rest of that race: the fdinfo value is adopted only when
 * the opened object is the one `sx` describes. */
void tawcroot_statx_fill_mnt_id(int dirfd, const char *path,
				unsigned int req_mask, struct statx *sx)
{
	if (!(req_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))) return;
	if (sx->stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) return;
	if (!path || !path[0]) {
		if (dirfd != AT_FDCWD) {
			statx_mnt_id_from_fdinfo(dirfd, sx);
			return;
		}
		long cfd = TAWC_RAW(TAWC_SYS_openat, AT_FDCWD, (long)".",
				    O_PATH | O_CLOEXEC, 0, 0, 0);
		if (cfd < 0) return;
		statx_mnt_id_from_fdinfo((int)cfd, sx);
		TAWC_RAW(TAWC_SYS_close, cfd, 0, 0, 0, 0, 0);
		return;
	}
	long fd = TAWC_RAW(TAWC_SYS_openat, dirfd, (long)path,
			   O_PATH | O_CLOEXEC | O_NOFOLLOW, 0, 0, 0);
	if (fd < 0) return;
	struct statx chk;
	long crv = TAWC_RAW(TAWC_SYS_statx, (int)fd, (long)"",
			    AT_EMPTY_PATH, STATX_INO, (long)&chk, 0);
	if (crv == 0 &&
	    chk.stx_ino == sx->stx_ino &&
	    chk.stx_dev_major == sx->stx_dev_major &&
	    chk.stx_dev_minor == sx->stx_dev_minor)
		statx_mnt_id_from_fdinfo((int)fd, sx);
	TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
}

/* statx flavour of finish_stat: zero uid/gid AND set the mask bits so
 * the guest sees the fields as populated (review C11). */
static long finish_statx(long rv, struct statx *local,
			 struct statx *guest_out)
{
	if (rv != 0) return rv;
	local->stx_uid = 0;
	local->stx_gid = 0;
	local->stx_mask |= STATX_UID | STATX_GID;
	long ce = tawc_copy_to_guest(guest_out, local, sizeof *local);
	return ce < 0 ? ce : 0;
}

static long handle_statx(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int    dirfd = (int)args->a;
	const char *path = (const char *)(uintptr_t)args->b;
	int    flags = (int)args->c;
	unsigned int mask = (unsigned int)args->d;
	struct statx *out = (struct statx *)(uintptr_t)args->e;
	if (!out) return TAWC_EFAULT;

	struct statx local;

	/* Same AT_EMPTY_PATH-with-empty-string short-circuit as
	 * handle_newfstatat. glibc's fstat-via-statx passes a non-NULL
	 * empty string; route through dirfd directly rather than
	 * mistakenly translating "". */
	if (flags & AT_EMPTY_PATH) {
		long empty = tawcroot_fs_path_is_empty(path);
		if (empty < 0) return empty;
		if (empty) {
			if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
			long rv = TAWC_RAW(TAWC_SYS_statx, dirfd, (long)"", flags,
					   mask, (long)&local, 0);
			if (rv == 0) {
				unsigned long c = fd_object_nlink(
					dirfd, local.stx_mode,
					local.stx_nlink,
					TAWC_MKDEV(local.stx_dev_major,
						   local.stx_dev_minor));
				if (c) {
					local.stx_nlink = (unsigned int)c;
					local.stx_mask |= STATX_NLINK;
				}
				tawcroot_statx_fill_mnt_id(dirfd, NULL,
							   mask, &local);
			}
			return finish_statx(rv, &local, out);
		}
	}

	if (!path) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long fe = tawcroot_fs_fetch_path(scratch, 0, path);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = tawcroot_fs_classify_shm(scratch->buf[0], &shm_name);
		if (kind != SHM_PEEK_NONE) {
			long r;
			if (kind == SHM_PEEK_NAME) {
				r = tawcroot_shm_statx_name(shm_name, &local,
							    mask);
				if (r < 0) return r;
			} else {
				tawcroot_shm_statx_dir(&local, mask);
			}
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return 0;
		}
	}

	/* /proc shadows — see the same block in handle_newfstatat. */
	{
		int kind = tawcroot_fs_proc_shadow_classify_at(scratch, dirfd,
							       scratch->buf[0]);
		if (kind != TAWCROOT_PROC_SHADOW_NONE) {
			long r = tawcroot_proc_shadow_statx(kind, mask,
							    &local);
			if (r < 0) return r;
			return finish_statx(0, &local, out);
		}
	}

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	struct fs_path t;
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd, pmode,
					     TAWCROOT_PATH_INTENT_READ, &t);
	if (e) return e;

	const char *resolved = t.is_root ? "" : t.path;
	int rv_flags = flags;
	if (t.is_root) rv_flags |= AT_EMPTY_PATH;

	long rv = TAWC_RAW(TAWC_SYS_statx, t.fd, (long)resolved,
			   rv_flags, mask, (long)&local, 0);
	/* Hardlink emulation — same two fixups as handle_newfstatat:
	 * NOFOLLOW leaf that is an emulated name → statx the object and
	 * report the sidecar count; FOLLOW result landing in the store →
	 * count fix only. */
	if (rv == 0 && tawcroot_store_link_fd >= 0) {
		if (pmode == TAWCROOT_PATH_NOFOLLOW &&
		    (local.stx_mode & S_IFMT) == S_IFLNK) {
			char tok[TAWCROOT_LINK_TOKEN_MAX];
			if (tawcroot_link_leaf_token(t.fd, t.path, tok,
						     sizeof tok) == 1) {
				struct statx ox;
				long orv = TAWC_RAW(TAWC_SYS_statx,
					tawcroot_store_link_fd, (long)tok,
					AT_SYMLINK_NOFOLLOW, mask,
					(long)&ox, 0);
				if (orv == 0) {
					ox.stx_nlink = (unsigned int)
						tawcroot_link_count_for_stat(tok);
					ox.stx_mask |= STATX_NLINK;
					local = ox;
					/* `local` now describes the store
					 * object, so the mnt-id fill must
					 * target it too — the generic
					 * leaf fill below would fail its
					 * dev/ino guard against it. */
					tawcroot_statx_fill_mnt_id(
						tawcroot_store_link_fd, tok,
						mask, &local);
				}
			}
		} else if (t.fd == tawcroot_store_link_fd && !t.is_root) {
			local.stx_nlink = (unsigned int)
				tawcroot_link_count_for_stat(t.path);
			local.stx_mask |= STATX_NLINK;
		}
	}
	/* No-op when the kernel supplied a mount id or the emulated-
	 * hardlink branch above already filled one. */
	if (rv == 0)
		tawcroot_statx_fill_mnt_id(t.fd, resolved, mask, &local);
	return finish_statx(rv, &local, out);
}

/* fstat with fake-root decoration. Without this, fstat(fd) reports the
 * real app uid while stat/fstatat/statx fake uid/gid 0 — programs that
 * compare the two (tar/cpio ownership checks) see an inconsistent
 * fake-root world. Reserved fds answer EBADF per the fdtab.h contract. */
static long handle_fstat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	struct stat *out = (struct stat *)(uintptr_t)args->b;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	if (!out) return TAWC_EFAULT;
	struct stat local;
	long rv = TAWC_RAW(TAWC_SYS_fstat, fd, (long)&local, 0, 0, 0, 0);
	if (rv == 0) {
		unsigned long c = fd_object_nlink(fd, local.st_mode,
						  local.st_nlink,
						  (unsigned long)local.st_dev);
		if (c) local.st_nlink = (__typeof__(local.st_nlink))c;
	}
	return finish_stat(rv, &local, out);
}

void tawcroot_fs_stat_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_fstatat,     handle_newfstatat);
	tawcroot_dispatch_install(TAWC_SYS_fstat,       handle_fstat);
	tawcroot_dispatch_install(TAWC_SYS_statx,       handle_statx);
}
