/* Filesystem syscall handlers — phase 1.
 *
 * Path-bearing syscall handlers, dispatched from the SIGSYS handler.
 * Each handler:
 *   1. Pulls the guest path through `process_vm_readv` (EFAULT-safe).
 *   2. Translates via tawcroot_path_translate(...mode), which folds
 *      `..`, clamps at root, applies the well-known-symlink memo
 *      (mode-gated for sole-component leaves), and resolves bind
 *      mounts.
 *   3. Issues the host syscall against (base_fd, suffix). Symlink
 *      resolution is left to the kernel for the leaf component, with
 *      tawcroot_path_resolve_symlinks pre-walking non-leaf in-rootfs
 *      symlinks so absolute targets fold back through the bind table.
 *      We deliberately do NOT use openat2 with RESOLVE_IN_ROOT: when
 *      base_fd is a bind src dirfd, RESOLVE_IN_ROOT re-roots an
 *      absolute symlink target at the bind src, breaking symlinks
 *      whose target lives in a different bind (e.g. Android's
 *      /system/lib64/libc.so → /apex/com.android.runtime/lib64/bionic/
 *      libc.so).
 *
 * Argument shape per arch is in tawcroot_syscall_args (see arch.h):
 *   args.a = arg0 (kernel reg 0), .b = arg1, ...
 *
 * Dirfd handling: for *at variants with a non-AT_FDCWD `dirfd` and a
 * relative path, we pass `dirfd` through to the kernel verbatim
 * (see translate_at) so resolution honours the dirfd's
 * inode — needed for gpg/pacman-key-style openat(homedir_fd, "name").
 * Relative paths containing `..` are first lifted to guest-absolute
 * via /proc/self/fd/<dirfd> so the rootfs prefix clamps the escape.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "path.h"
#include "path_scratch.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "shm.h"
#include "syscalls_fs.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Classify an already-fetched local path as a `/dev/shm` intercept.
 *   NONE — not shm (fall through to normal translation)
 *   NAME — `/dev/shm/<name>` (*name_out points into `local_path`)
 *   DIR  — `/dev/shm` directory itself
 * Callers fetch the guest string once (fetch_guest_path) and classify
 * that copy — the same bytes later reach the translator, so a racing
 * guest thread can't swap the path between classification and use. */
enum { SHM_PEEK_NONE = 0, SHM_PEEK_NAME = 1, SHM_PEEK_DIR = 2 };
static int classify_shm(const char *local_path, const char **name_out)
{
	*name_out = tawcroot_shm_match(local_path);
	if (*name_out) return SHM_PEEK_NAME;
	if (tawcroot_shm_is_dir(local_path)) return SHM_PEEK_DIR;
	return SHM_PEEK_NONE;
}

/* Fetch the guest path once into scratch->buf[slot] for a handler that
 * classifies intercepts before translating. Returns 0 / -errno. */
static long fetch_guest_path(struct tawcroot_path_scratch *scratch, int slot,
			     const char *guest_path)
{
	long n = tawc_copy_string_from_guest(scratch->buf[slot],
					     TAWCROOT_PATH_SCRATCH_SIZE,
					     guest_path);
	return n < 0 ? n : 0;
}

/* AT_EMPTY_PATH support: returns 1 when `gpath` is NULL or "", 0 when
 * non-empty, -EFAULT on an unreadable guest pointer. Peeks a single
 * byte — tawc_copy_string_from_guest with a tiny cap would return
 * -ENAMETOOLONG for any real path, mis-firing on legitimate input. */
static long guest_path_is_empty(const char *gpath)
{
	if (!gpath) return 1;
	char first = -1;
	long pe = tawc_copy_from_guest(&first, 1, gpath);
	if (pe < 0) return pe;
	return first == 0;
}

/* A translated guest path, ready to issue against the host: pass `fd`
 * as the *at dirfd and `path` as the pathname. `is_root` is set when
 * the guest path resolved to the directory `fd` itself; `path` is then
 * "" and the caller picks its syscall's semantics for that case ("."
 * for most, AT_EMPTY_PATH, or a direct errno). */
struct fs_path {
	int         fd;
	int         is_root;
	const char *path;
};

/* Forward decl — body later in the file. Uses scratch buffers `slot`
 * and `slot + 1`; `out->path` points into `slot + 1`. */
static long translate_at(struct tawcroot_path_scratch *scratch, int slot,
			 int dirfd, const char *guest_path,
			 tawcroot_path_mode mode, struct fs_path *out);
static long translate_local(struct tawcroot_path_scratch *scratch, int slot,
			    int dirfd, tawcroot_path_mode mode,
			    struct fs_path *out);
static void decorate_stat(struct stat *st);

/* Pick the resolution mode for an openat based on the kernel flags.
 * O_NOFOLLOW + O_PATH means "don't follow the leaf even if it's a
 * symlink". Kernel O_CREAT semantics split on O_EXCL:
 *   - O_CREAT|O_EXCL never follows the leaf (an existing symlink —
 *     even dangling — is EEXIST), so the leaf must reach the kernel
 *     un-resolved: PARENT_CREATE.
 *   - plain O_CREAT FOLLOWS an existing leaf symlink (and creates at
 *     the target of a dangling one). PARENT_CREATE here let the host
 *     kernel chase an absolute symlink target against the HOST root —
 *     `open("/etc/resolv.conf", O_WRONLY|O_CREAT)` with the usual
 *     resolv.conf → /run/... symlink wrote outside the rootfs view.
 *     FOLLOW makes our resolver walk (and clamp) the leaf; a missing
 *     leaf stops the resolver early and the kernel creates it.
 * (O_NOFOLLOW / O_CREAT are arch-specific bits — pulled from
 * <linux/fcntl.h> at the top of this file.) */
static tawcroot_path_mode openat_mode(int flags)
{
	if (flags & O_NOFOLLOW) return TAWCROOT_PATH_NOFOLLOW;
	if (flags & O_CREAT)
		return (flags & O_EXCL) ? TAWCROOT_PATH_PARENT_CREATE
					: TAWCROOT_PATH_FOLLOW;
	return TAWCROOT_PATH_FOLLOW;
}

/* Probe (dirfd, path)'s leaf for an emulated name and open the OBJECT
 * with the guest's flags. One helper for handle_openat's two NOFOLLOW
 * shapes (O_PATH pre-probe / reactive post-ELOOP) so their flag
 * handling can't diverge. O_CREAT/O_EXCL are masked off the object
 * open — resolver-side store writes don't exist in any mode; -ENOENT
 * (not a token, or a dangling one) tells the caller to keep the name's
 * plain-symlink behavior, visible and unlinkable for cleanup. */
static long open_emulated_leaf(int dirfd, const char *path, int flags,
			       int mode)
{
	char tok[TAWCROOT_LINK_TOKEN_MAX];
	if (tawcroot_link_leaf_token(dirfd, path, tok, sizeof tok) != 1)
		return TAWC_ENOENT;
	return tawc_openat(tawcroot_store_link_fd, tok,
			   flags & ~(O_CREAT | O_EXCL), mode);
}

static long handle_openat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flags = (int)args->c;
	int mode  = (int)args->d;
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	long fe = fetch_guest_path(scratch, 0, gpath);
	if (fe) return fe;

	/* /proc/self/maps and /proc/<our-pid>/maps: synthesize a shadow fd
	 * backed by a memfd containing the kernel's maps output with each
	 * path field reverse-translated through the rootfs/bind tables.
	 * Without this, sandboxes that grep their own maps (Mozilla's, ld.so
	 * $ORIGIN resolvers, crash handlers) see host paths the guest's
	 * world view doesn't contain.
	 *
	 * /proc/sys/kernel/overflow{uid,gid}: synthesize a memfd holding
	 * the Linux-conventional "65534\n". Android's SELinux denies the
	 * untrusted_app domain any read under /proc/sys/kernel, so bwrap
	 * (which reads both sysctls before unshare(CLONE_NEWUSER) to set up
	 * its uid/gid maps) bails with a stderr message that doesn't match
	 * glycin's "namespace setup failed" autodetect substrings.
	 * Synthesizing here lets bwrap proceed to the unshare, fail with
	 * the substring glycin recognizes, and trigger its NotSandboxed
	 * fallback. See notes/tawcroot/path-translation.md "More /proc reverse-translation
	 * paths" for the wider context.
	 *
	 * /proc/bus/pci/devices: synthesize an empty memfd. Android exposes
	 * the file as an unreadable placeholder (`-?????????`); opening it
	 * returns EACCES. libpci's procfs back-end calls its default
	 * error handler — `exit(1)` — on that, killing whatever dlopen'd
	 * libpci.so.3. Mozilla's `glxtest` probe is the proximate consumer
	 * (called once per Firefox start to sniff the GPU); its non-zero
	 * exit force-disables HW_COMPOSITING and Firefox falls back to
	 * software WebRender + SHM. An empty file is the legitimate
	 * "no PCI devices visible" state libpci handles cleanly: callers
	 * see an empty device list, log "no GPU found via PCI", and
	 * continue to whatever non-PCI probe they have (eglQueryString
	 * for Mozilla). See notes/firefox.md "libpci probe".
	 *
	 * Only intercept O_RDONLY (no O_DIRECTORY, no O_PATH). Other flag
	 * combos fall through to normal translation so the kernel produces
	 * the conventional -ENOTDIR / O_PATH-fd behavior.
	 *
	 * Fd-relative form: openat(proc_dir_fd, "self/maps", ...) or
	 * openat(proc_dir_fd, "sys/kernel/overflowuid", ...). We resolve
	 * dirfd via /proc/self/fd/<n>, join with the guest path, and
	 * re-classify. One extra readlinkat per non-AT_FDCWD relative
	 * O_RDONLY-ish open; only fires when the absolute peek didn't match. */
	if ((flags & O_ACCMODE) == O_RDONLY &&
	    (flags & O_DIRECTORY) == 0 &&
	    (flags & O_PATH) == 0) {
		long shadow;
		int hit = tawcroot_proc_shadow_open(path, &shadow);
		if (!hit && dirfd != AT_FDCWD && path[0] != '/' &&
		    tawcroot_could_be_proc_relative(path)) {
			char *composed = scratch->buf[2];
			if (tawcroot_compose_fd_relative(dirfd, path,
						composed,
						TAWCROOT_PATH_SCRATCH_SIZE) > 0)
				hit = tawcroot_proc_shadow_open(composed,
								&shadow);
		}
		if (hit) {
			/* The shadow memfds are created MFD_CLOEXEC.
			 * If the guest didn't ask for O_CLOEXEC, clear
			 * FD_CLOEXEC so the fd survives the guest's next
			 * exec like a real /proc file would. */
			if (shadow >= 0 && (flags & O_CLOEXEC) == 0)
				(void)tawc_fcntl((int)shadow, F_SETFD, 0);
			return shadow;
		}
	}

	const char *shm_name;
	if (classify_shm(path, &shm_name) == SHM_PEEK_NAME)
		return tawcroot_shm_open(shm_name, flags, mode);
	/* SHM_PEEK_DIR (open of /dev/shm itself) falls through; the
	 * translator returns -ENOENT, matching what a host with no
	 * /dev/shm dir would do. */

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd, openat_mode(flags), &t);
	if (e) return e;

	/* Empty t.path → guest asked for "/" or for a bind dst's root.
	 * Pass "." so the kernel resolves it to the dir t.fd points at;
	 * this works on every kernel we target (AT_EMPTY_PATH on openat
	 * only landed in 6.6, kernel 5.4 device doesn't have it). */
	const char *p = t.is_root ? "." : t.path;

	/* O_TMPFILE (plan stage 4). With O_EXCL ("will never be linked"):
	 * pure passthrough — anonymous file creation is allowed (only
	 * link is denied), fstat nlink 0 is exact, and a later linkat
	 * fails ENOENT in-kernel like the real thing. The linkable form
	 * needs a NAME the publish rename can move, so the file is
	 * created at <store>/tmp/<ino> and its fd returned; linkat
	 * detects tmp-resident sources by host path and publishes with
	 * one atomic NOREPLACE rename. O_CREAT alongside stays on the
	 * passthrough (kernel EINVAL); no store (EAGAIN sentinel) falls
	 * through too — scratch keeps working, publish degrades to the
	 * documented EXDEV. */
	if ((flags & TAWC_O_TMPFILE) == TAWC_O_TMPFILE &&
	    !(flags & (O_EXCL | O_CREAT)) &&
	    tawcroot_linkstore_state() != TAWCROOT_STORE_OFF &&
	    tawcroot_linkstore_state() != TAWCROOT_STORE_DEGRADED) {
		long tf = tawcroot_link_tmpfile_open(t.fd, p,
						     flags & ~TAWC_O_TMPFILE,
						     mode);
		if (tf != TAWC_EAGAIN) return tf;
	}

	/* Hardlink emulation: the resolver followed an emulated name into
	 * the store. Plain O_CREAT through a *dangling* token (object lost
	 * to a partial host copy, a crashed-NEW window, or degraded mode)
	 * must NOT create a fresh uncounted object in link/ — return
	 * ENOENT, matching "not linked yet". A live object drops O_CREAT
	 * (it exists; a concurrent teardown then surfaces as ENOENT, same
	 * story). O_CREAT|O_EXCL never reaches here: PARENT_CREATE mode
	 * keeps the leaf un-resolved and the kernel EEXISTs the symlink. */
	if (t.fd == tawcroot_store_link_fd && tawcroot_store_link_fd >= 0 &&
	    (flags & O_CREAT)) {
		struct stat probe;
		if (TAWC_RAW(TAWC_SYS_fstatat, t.fd, (long)p, (long)&probe,
			     AT_SYMLINK_NOFOLLOW, 0, 0) != 0)
			return TAWC_ENOENT;
		flags &= ~(O_CREAT | O_EXCL);
	}

	/* Hardlink emulation, NOFOLLOW leaf. O_PATH|O_NOFOLLOW must be
	 * pre-probed: the kernel opens the token symlink itself (O_PATH
	 * opens symlinks — no ELOOP to react to), where the guest expects
	 * an O_PATH fd to the shared file. Opening the object instead is
	 * right for symlink objects too: an O_PATH fd to the symlink,
	 * exactly what a real hardlinked symlink gives. Plain O_NOFOLLOW
	 * is reactive (below): non-symlink hot paths pay nothing. */
	if ((flags & O_NOFOLLOW) && (flags & O_PATH) &&
	    tawcroot_store_link_fd >= 0 && !t.is_root) {
		long ofd = open_emulated_leaf(t.fd, p, flags, mode);
		if (ofd != TAWC_ENOENT) return ofd;
	}

	/* Plain openat — the kernel chases a leaf symlink against the
	 * process's actual fs root. Non-leaf in-rootfs symlinks are
	 * pre-folded by tawcroot_path_resolve_symlinks during translate
	 * (path_orchestrate.c), so by here `t.path` no longer contains
	 * unresolved rootfs-side directory components.
	 *
	 * We don't use openat2 with RESOLVE_IN_ROOT: that would clamp
	 * absolute symlink targets at t.fd, but when t.fd is a
	 * bind src dirfd a leaf symlink whose target points into a
	 * *different* bind (e.g. /system/lib64/libc.so →
	 * /apex/com.android.runtime/lib64/bionic/libc.so on Android)
	 * needs the kernel to follow through the host root, not the
	 * bind src. See test_prod_rootfs.c::prod_rootfs_cross_bind_abs_symlink
	 * and notes/tawcroot/path-translation.md "Cross-bind absolute symlinks". */
	long fd = tawc_openat(t.fd, p, flags, mode);

	/* Reactive O_NOFOLLOW: the kernel just ELOOPed a leaf symlink; if
	 * it is an emulated name, open the object KEEPING O_NOFOLLOW — a
	 * regular object opens (the name "is" a regular file), a symlink
	 * object still ELOOPs, correctly. Dangling (ENOENT) returns the
	 * original ELOOP, matching a plain dangling symlink. */
	if (fd == TAWC_ELOOP && (flags & O_NOFOLLOW) &&
	    tawcroot_store_link_fd >= 0 && !t.is_root) {
		long ofd = open_emulated_leaf(t.fd, p, flags, mode);
		if (ofd != TAWC_ENOENT) return ofd;
	}
	return fd;
}

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
/* Rebuild st_dev from statx's split major/minor (the kernel's
 * new_encode_dev layout, which newfstatat's st_dev uses on both our
 * arches) so statx callers can compare against tawcroot_store_dev. */
#define TAWC_MKDEV(maj, min)                                              \
	(((unsigned long)((min) & 0xffu)) |                               \
	 ((unsigned long)(maj) << 8) |                                    \
	 ((unsigned long)((min) & ~0xffu) << 12))

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
	 * gpath==0; the gpath="" case fell through to translate_at
	 * which then translates the empty string against the kernel cwd
	 * and stats the wrong inode (cwd dir instead of dirfd). wc, which
	 * uses fstat() on its input fd to decide buffer strategy, then read
	 * stale size and segfaulted. We pass through as-is (the dirfd is
	 * the guest's, but we don't yet track guest fd
	 * provenance — the kernel will see whatever fd it is and stat the
	 * corresponding inode). */
	if (flags & AT_EMPTY_PATH) {
		long empty = guest_path_is_empty(gpath);
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
	long fe = fetch_guest_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = classify_shm(scratch->buf[0], &shm_name);
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

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd, pmode, &t);
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

/* Fetch a guest-pointer path string into a stack-local buffer through
 * the EFAULT-safe usercopy helper, then translate. This is the front
 * door for every path-bearing handler — the path string is fetched
 * ONCE into scratch->buf[slot] and every later step (shm / proc-shadow
 * classification via translate_local callers, translation, *at issue)
 * works on that copy, so the guest cannot swap the string between
 * classification and use.
 *
 * `dirfd` is the *at-syscall's directory fd. When the guest passes a
 * non-AT_FDCWD dirfd AND the path is relative, the kernel's intent is
 * "resolve this relative to the dirfd's inode." The dirfd is one we
 * previously handed back from a translated openat, so its inode is
 * already inside the rootfs view — pass it through unchanged and let
 * the kernel do the resolution. Without this, `tawcroot_path_translate`
 * would resolve the relative path via the kernel's CWD and ignore the
 * dirfd, which gpg/pacman-key trips when they walk a homedir's contents
 * via openat(homedir_fd, "pubring.gpg", ...).
 *
 * Pass dirfd = -100 (AT_FDCWD) for non-*at syscalls (e.g. the legacy
 * x86_64 wrappers) — the relative-path branch then treats them as
 * "relative to cwd" and routes through full translation as before.
 *
 * `..` traversal in a fd-relative path is intercepted in
 * translate_at: the path is lifted to guest-absolute via
 * the dirfd's /proc/self/fd link, then path_translate's fold clamps
 * `..` at the rootfs root. Without that, the kernel walks `..` past
 * the dirfd freely and systemd's path_is_root_at probe (chase.c)
 * misclassifies the rootfs and aborts with
 * `Assertion 'path_is_absolute(p)' failed`. */
/* Honours the guest's dirfd for fd-relative resolution — see the big
 * comment above for why this matters. Returns `dirfd` itself in
 * `out->fd` and the literal guest-supplied path in `out->path` when
 * the kernel should resolve directly off `dirfd`.
 *
 * Special case: a relative path containing `..` would let the kernel
 * walk above the dirfd and leak the host filesystem when the dirfd
 * sits at the rootfs root.
 * Reproducer: systemd's `path_is_root_at(rootfs_fd, NULL)` opens
 * `(rootfs_fd, "..")`, gets the host's parent of the rootfs, compares
 * inodes against rootfs_fd, sees a mismatch, concludes "not at root",
 * and aborts with `Assertion 'path_is_absolute(p)' failed at chase.c`.
 * Lift these to the equivalent guest-absolute path via the dirfd's
 * /proc/self/fd link so tawcroot_path_fold_absolute clamps the `..`
 * at the rootfs boundary, mimicking real chroot(2) semantics. The lift
 * works for dirfds opened through a bind dst too: dirfd_to_guest_abs
 * reverse-translates bind-src host paths to /<bind.dst>/<remainder>,
 * and the second-pass tawcroot_path_translate routes back through the
 * bind so `..` clamps at the bind boundary. Outside-rootfs+outside-binds
 * fds (e.g. host /proc tree) still ENOENT and fall through to kernel
 * passthrough — there's nothing in our view to escape into. */
static long translate_local(struct tawcroot_path_scratch *scratch, int slot,
			    int dirfd, tawcroot_path_mode mode,
			    struct fs_path *out)
{
	char  *path_buf   = scratch->buf[slot];
	char  *suffix     = scratch->buf[slot + 1];
	size_t suffix_cap = TAWCROOT_PATH_SCRATCH_SIZE;
	out->path = suffix;

	if (dirfd != AT_FDCWD) {
		if (path_buf[0] != '/') {
			/* Reserved fds must behave as EBADF (fdtab.h
			 * contract) — without this, a guest could use our
			 * rootfs/bind O_PATH fds as resolution anchors.
			 * Absolute paths fall through: the kernel ignores
			 * dirfd entirely for those, even invalid ones. */
			if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
			/* Lift EVERY non-empty fd-relative path to guest-
			 * absolute via the dirfd's /proc/self/fd link, then
			 * run the full translator so the `..` clamp AND the
			 * symlink resolver apply (notes/tawcroot/path-translation.md
			 * §"Translation rules" item 4: escapes blocked for
			 * both absolute and relative requests). Earlier only
			 * paths containing `..` were lifted; a dotdot-free
			 * relative path went to the kernel verbatim, and any
			 * in-rootfs symlink with an absolute target along it
			 * was chased against the HOST root — e.g.
			 * openat(etc_fd, "resolv.conf") with resolv.conf →
			 * /run/resolv.conf landed on the host's /run.
			 * Cost: one /proc/self/fd readlink per *at call with
			 * a relative path and real dirfd.
			 *
			 * The lift resolves via the dirfd's CURRENT path, so
			 * a dirfd whose directory was renamed/unlinked after
			 * open diverges from kernel inode-based resolution —
			 * same trade proot makes. */
			if (path_buf[0] != 0) {
				TAWCROOT_PATH_SCRATCH_AUTO(lift);
				char *abs = lift->buf[0];
				long al = tawcroot_fd_to_guest_abs(dirfd, abs,
				                                   TAWCROOT_PATH_SCRATCH_SIZE);
				if (al >= 0) {
					size_t pos = (size_t)al;
					long je = 0;
					if (abs[pos - 1] != '/')
						je = tawc_str_append(
							abs,
							TAWCROOT_PATH_SCRATCH_SIZE,
							&pos, "/");
					if (!je) je = tawc_str_append(
							abs,
							TAWCROOT_PATH_SCRATCH_SIZE,
							&pos, path_buf);
					if (je) return je;
					tawcroot_path_result r =
						tawcroot_path_translate(
							abs, suffix,
							suffix_cap, mode);
					if (r.err) return r.err;
					out->fd      = r.base_fd;
					out->is_root = (suffix[0] == 0);
					return 0;
				}
				/* Outside-rootfs dirfd (-ENOENT) or readlink
				 * failure: fall through to kernel-resolved
				 * passthrough. The kernel's resolution can't
				 * escape into the rootfs from outside it, so
				 * the leak this branch protects against
				 * doesn't apply. */
			}
			/* Empty path (kernel empty-path semantics must apply
			 * verbatim) or outside-view dirfd: kernel resolves
			 * off the dirfd. Copy path_buf into suffix so callers
			 * with one output buffer still work.
			 *
			 * is_root stays 0 even for an empty input: that flag
			 * means "the guest path TRANSLATED to the base_fd
			 * itself" (e.g. "/" or a bind root) and makes callers
			 * substitute "." / AT_EMPTY_PATH. A literally-empty
			 * guest path must instead reach the kernel verbatim so
			 * the kernel's own empty-path semantics apply: -ENOENT
			 * for most syscalls, the O_PATH-symlink magic for
			 * readlinkat(fd, ""). */
			long ce = tawc_str_copy(suffix, suffix_cap, path_buf);
			if (ce < 0) return ce;
			out->fd      = dirfd;
			out->is_root = 0;
			return 0;
		}
		/* Absolute path: dirfd is ignored by the kernel; fall
		 * through to the normal translation. */
	}

	tawcroot_path_result r =
		tawcroot_path_translate(path_buf, suffix, suffix_cap, mode);
	if (r.err) return r.err;
	out->fd      = r.base_fd;
	out->is_root = (suffix[0] == 0);
	return 0;
}

static long translate_at(struct tawcroot_path_scratch *scratch, int slot,
			 int dirfd, const char *guest_path,
			 tawcroot_path_mode mode, struct fs_path *out)
{
	long n = tawc_copy_string_from_guest(scratch->buf[slot],
					     TAWCROOT_PATH_SCRATCH_SIZE,
					     guest_path);
	if (n < 0) return n;
	return translate_local(scratch, slot, dirfd, mode, out);
}


static long handle_readlinkat(const tawcroot_syscall_args *args,
			      ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	char       *buf   = (char *)(uintptr_t)args->c;
	int         size  = (int)args->d;
	/* Kernel checks bufsiz first: <= 0 is EINVAL, not EFAULT. */
	if (size <= 0) return TAWC_EINVAL;
	if (!gpath || !buf) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	/* Phase 2e: synthesize the /proc self-magic links.
	 *
	 *   - exe: the kernel's view points at libtawcroot.so; the guest
	 *     wants the path it originally asked us to exec.
	 *   - cwd: the kernel's link target is the HOST cwd; reverse-
	 *     translate through the same walk getcwd uses (and like
	 *     getcwd, refuse with -ENOENT rather than leak a host path
	 *     when the kernel cwd is outside the view).
	 *   - root is left to the kernel: we never kernel-chroot, so its
	 *     "/" answer is already what a guest expects, emulated
	 *     chroot included.
	 *
	 * The fd-relative case (readlinkat(proc_self_fd, "exe", ...)) is
	 * caught by re-composing through the dirfd's /proc/self/fd/<n>
	 * link. `link_cls` also remembers when the guest asked for some
	 * OTHER process's exe link, so the result-equality substitution
	 * below doesn't fire for it (every guest's kernel exe is the same
	 * libtawcroot.so — substituting would return the CALLER's exe
	 * path for a different process's link). */
	char *path = scratch->buf[0];
	long fe = fetch_guest_path(scratch, 0, gpath);
	if (fe) return fe;
	const char *cls = path;
	char *composed = scratch->buf[1];
	if (dirfd != AT_FDCWD && path[0] != '/' &&
	    tawcroot_could_be_proc_relative(path) &&
	    tawcroot_compose_fd_relative(dirfd, path, composed,
					 TAWCROOT_PATH_SCRATCH_SIZE) > 0)
		cls = composed;
	int link_cls = tawcroot_proc_link_classify(cls);
	if (link_cls == TAWCROOT_PROC_LINK_EXE_SELF &&
	    tawcroot_guest_exe_path_len > 0) {
		size_t len = tawcroot_guest_exe_path_len;
		if (len > (size_t)size) len = (size_t)size;
		long ce = tawc_copy_to_guest(buf,
			tawcroot_guest_exe_path, len);
		if (ce < 0) return ce;
		return (long)len;
	}
	if (link_cls == TAWCROOT_PROC_LINK_CWD) {
		/* `cls` may point into buf[1]; dead from here on. */
		char *cwd = scratch->buf[1];
		long cn = tawcroot_cwd_to_guest_abs(cwd,
					TAWCROOT_PATH_SCRATCH_SIZE);
		if (cn < 0) return cn;
		if (cn > (long)size) cn = size;
		long ce = tawc_copy_to_guest(buf, cwd, (size_t)cn);
		if (ce < 0) return ce;
		return cn;
	}

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd,
				 TAWCROOT_PATH_NOFOLLOW, &t);
	if (e) return e;
	/* When the guest path resolves exactly to a bind dst (or rootfs root),
	 * translate_at gives us (reserved_dir_fd, ""). The kernel
	 * answers `readlinkat(dir_fd, "", ...)` with ENOENT, but the
	 * semantically correct error is EINVAL — those paths are directories,
	 * not symlinks. Without this, glibc realpath aborts canonicalisation
	 * the moment it hits /proc/self (it readlinkats "/proc" first to
	 * check, gets ENOENT instead of EINVAL, and gives up before reaching
	 * the /proc/self/exe synthesis). The guest-supplied dirfd case
	 * (empty path on a guest O_PATH symlink fd) keeps the kernel call —
	 * those fds are not in our reserved range. */
	if (t.is_root && tawcroot_fd_is_reserved(t.fd)) return TAWC_EINVAL;
	const char *p = t.is_root ? "" : t.path;

	/* Read into a kernel-side scratch so we can post-process the result
	 * before forwarding it. We reuse translate_at's path buffer (dead
	 * from here on) as the scratch — a fresh PATH_MAX buffer would
	 * push the handler frame past the stack budget noted in
	 * notes/tawcroot/sigsys-handler.md "Threading and `vfork` invariants". Cost vs.
	 * the pre-fix direct
	 * kernel→guest write: every readlinkat now pays a process_vm_writev
	 * round-trip even when no substitution fires; the equality test
	 * itself is gated by a length pre-check and is free.
	 *
	 * The substitution catches two surfaces that both leak
	 * libtawcroot.so's host path upward:
	 *   - readlinkat(O_PATH-fd, "") — glibc realpath opens
	 *     /proc/self/exe with O_PATH|O_NOFOLLOW (kernel fd lands on
	 *     libtawcroot.so) then queries the empty-path symlink target.
	 *   - readlink("/proc/self/fd/<n>") — same /proc/self/fd trick used
	 *     by alternate realpath paths and by anything resolving its
	 *     own binary location ($ORIGIN, Firefox XPCOM lookup).
	 * In both, the kernel returns libtawcroot.so's path; the guest's
	 * view doesn't contain it, so a follow-up stat()/open() ENOENTs.
	 *
	 * The read uses the FULL scratch cap regardless of the guest's
	 * `size`, clamping only at copy time: every post-processing step
	 * needs the untruncated target — a guest-sized read could truncate
	 * an emulated name's token literal mid-token, miss the object
	 * lookup, and leak the half-token into guest-readable output
	 * (which the forgery guard's cleanliness argument forbids), and
	 * the exe-substitution equality test would silently fail the same
	 * way. The kernel's own contract (return min(bufsiz, len) bytes,
	 * silent truncation) is reproduced by the final clamp. */
	char *readlink_scratch = scratch->buf[0];
	long n = tawc_readlinkat(t.fd, p, readlink_scratch,
				 TAWCROOT_PATH_SCRATCH_SIZE);
	if (n < 0) return n;

	/* Hardlink emulation: an emulated name is "a regular file" to the
	 * guest — readlink answers EINVAL, and the opaque token literal
	 * must never appear in guest-readable output (that's what keeps
	 * legit tar archives free of forgeable targets). Exceptions: a
	 * symlink *object* (hardlink-of-a-symlink) forwards the object's
	 * real target; a dangling token (object lost) keeps plain-symlink
	 * behavior so the name stays visible and unlinkable. */
	if (tawcroot_store_link_fd >= 0 && n > 0 &&
	    n < (long)TAWCROOT_PATH_SCRATCH_SIZE) {
		readlink_scratch[n] = 0;
		const char *tokp;
		if (tawcroot_link_target_is_token(readlink_scratch, &tokp)) {
			struct stat ost;
			long orv = TAWC_RAW(TAWC_SYS_fstatat,
					    tawcroot_store_link_fd,
					    (long)tokp, (long)&ost,
					    AT_SYMLINK_NOFOLLOW, 0, 0);
			if (orv == 0 && S_ISLNK(ost.st_mode)) {
				char tok[TAWCROOT_LINK_TOKEN_MAX];
				long ce2 = tawc_str_copy(tok, sizeof tok,
							 tokp);
				if (ce2 < 0) return TAWC_EINVAL;
				n = tawc_readlinkat(tawcroot_store_link_fd,
						    tok, readlink_scratch,
						    TAWCROOT_PATH_SCRATCH_SIZE);
				if (n < 0) return n;
			} else if (orv == 0) {
				return TAWC_EINVAL;
			}
		}
	}
	if (link_cls != TAWCROOT_PROC_LINK_EXE_OTHER &&
	    tawcroot_guest_exe_path_len > 0 &&
	    tawcroot_self_host_path_len > 0 &&
	    (size_t)n == tawcroot_self_host_path_len &&
	    memcmp(readlink_scratch, tawcroot_self_host_path,
	           tawcroot_self_host_path_len) == 0) {
		size_t glen = tawcroot_guest_exe_path_len;
		if (glen > (size_t)size) glen = (size_t)size;
		long ce = tawc_copy_to_guest(buf, tawcroot_guest_exe_path, glen);
		if (ce < 0) return ce;
		return (long)glen;
	}
	/* /proc/<pid>/fd/<n> magic links resolve to HOST paths for fds
	 * inside the rootfs/bind view. Bun's realpath (Claude Code et al.)
	 * canonicalizes via open(dir) + readlink(/proc/self/fd/<n>), then
	 * ENOENTs on the leaked host path. Reverse-translate through the
	 * same longest-prefix walk as getcwd. Outside-view targets
	 * (sockets, memfds, pipes) pass through verbatim — fd links
	 * legitimately point outside the view, and "socket:[...]" isn't
	 * a path at all. buf[0] holds the kernel result and buf[1] the
	 * dead translated path; buf[2] is free for the guest rewrite. */
	if (link_cls == TAWCROOT_PROC_LINK_FD && n > 0 &&
	    readlink_scratch[0] == '/') {
		char *guest = scratch->buf[2];
		long gn = tawcroot_host_path_to_guest_abs(
			readlink_scratch, (size_t)n,
			guest, TAWCROOT_PATH_SCRATCH_SIZE);
		if (gn > 0) {
			if (gn > (long)size) gn = size;
			long ge = tawc_copy_to_guest(buf, guest, (size_t)gn);
			if (ge < 0) return ge;
			return gn;
		}
	}
	if (n > (long)size) n = size;
	long ce = tawc_copy_to_guest(buf, readlink_scratch, (size_t)n);
	if (ce < 0) return ce;
	return n;
}

static long handle_faccessat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int mode  = (int)args->c;
	if (!gpath) return TAWC_EFAULT;

	/* Plain faccessat is a THREE-argument syscall — args->d is
	 * whatever the guest left in the 4th arg register; reading it as
	 * flags would randomly flip resolution mode. Only faccessat2
	 * carries real flags. We can't forward those: Android RET_TRAPs
	 * faccessat2 (recursive SIGSYS, see the "." comment below) and
	 * plain faccessat ignores flags. -ENOSYS makes glibc/musl fall
	 * back to their own AT_SYMLINK_NOFOLLOW / AT_EACCESS emulation
	 * on top of syscalls we do translate. */
	if (args->nr == TAWC_SYS_faccessat2 && (int)args->d != 0)
		return TAWC_ENOSYS;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long fe = fetch_guest_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = classify_shm(scratch->buf[0], &shm_name);
		if (kind == SHM_PEEK_NAME) return tawcroot_shm_access_name(shm_name);
		if (kind == SHM_PEEK_DIR)  return tawcroot_shm_access_dir();
	}

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd, TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;
	/* Empty t.path → guest asked for "/" or for a bind dst's root.
	 * Pass "." so the kernel resolves it to the dir t.fd points at;
	 * AT_EMPTY_PATH would be cleaner but faccessat (NR 269) ignores
	 * flags entirely (faccessat2 added them, but Android's
	 * untrusted_app filter RET_TRAPs that NR — calling it from inside
	 * our handler causes a recursive SIGSYS that gets routed to the
	 * default disposition and kills the process). The "." rewrite
	 * matches what handle_openat does for the same case. Without it,
	 * `access("/", ...)` translated to `faccessat(rootfd, "", ...)`
	 * and the kernel returned ENOENT for the empty path — which broke
	 * xbps's `xbps_pkgdb_lock`, observed as
	 * `[pkgdb] rootdir /: No such file or directory`. */
	const char *p = t.is_root ? "." : t.path;
	return TAWC_RAW(TAWC_SYS_faccessat, t.fd, (long)p,
	                mode, 0, 0, 0);
}

static long handle_chdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, AT_FDCWD, gpath,
			      TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;

	/* Empty t.path → guest asked for "/", which is the directory the
	 * base fd already refers to. fchdir(t.fd) directly — avoids the
	 * AT_EMPTY_PATH-on-openat thing which only landed in kernel 6.6. */
	if (t.is_root) {
		return TAWC_RAW(TAWC_SYS_fchdir, t.fd, 0, 0, 0, 0, 0);
	}

	int flags = O_DIRECTORY | O_PATH | O_CLOEXEC;
	long fd = tawc_openat(t.fd, t.path, flags, 0);
	if (fd < 0) return fd;

	long rv = TAWC_RAW(TAWC_SYS_fchdir, fd, 0, 0, 0, 0, 0);
	tawc_close((int)fd);
	return rv;
}

/* getcwd reverse-translation. Kernel returns the host cwd; we reverse-
 * translate via the shared longest-prefix walk (rootfs AND bind srcs —
 * `cd /system` into a bind leaves the kernel cwd at the bind src, and
 * matching only the rootfs prefix made getcwd fail ENOENT after an
 * ordinary cd), copy to the guest's buffer, and return a length
 * matching the kernel's getcwd contract (INCLUDING the trailing NUL).
 *
 * If the kernel cwd is outside the view, return -ENOENT — proot leaks
 * the host path here, but exposing the host through a getcwd answer
 * the guest can read is exactly the kind of accidental escape we
 * close in §"Translation rules".
 *
 * The result is staged in a stack-local buffer and copied through the
 * guarded helper — a wild guest pointer must EFAULT, not crash the
 * SIGSYS handler (review finding B1+B6). */
static long handle_getcwd(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	char  *out = (char *)(uintptr_t)args->a;
	size_t cap = (size_t)args->b;
	if (!out) return TAWC_EFAULT;
	/* Kernel contract: a zero-size buffer is ERANGE, not EFAULT. */
	if (cap == 0) return TAWC_ERANGE;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *tmp = scratch->buf[1];
	long n = tawcroot_cwd_to_guest_abs(tmp, TAWCROOT_PATH_SCRATCH_SIZE);
	if (n < 0) return n;
	if ((size_t)n + 1 > cap) return TAWC_ERANGE;
	long ce = tawc_copy_to_guest(out, tmp, (size_t)n + 1);
	if (ce < 0) return ce;
	return n + 1;
}

/* Translate-and-pass-through wrappers for the simpler path-bearing
 * syscalls. These all take (dirfd, path, ...) at the kernel layer; the
 * guest's dirfd is currently passed through (fd provenance comes later)
 * and the path is translated. */

#define DECLARE_AT_PASS(name, sysnr, narg, pmode)                         \
static long handle_##name(const tawcroot_syscall_args *args,             \
			  ucontext_t *uc)                                 \
{                                                                         \
	(void)uc;                                                         \
	int dirfd = (int)args->a;                                         \
	const char *gpath = (const char *)(uintptr_t)args->b;             \
	if (!gpath) return TAWC_EFAULT;                                       \
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);                                  \
	struct fs_path t;                                                     \
	long e = translate_at(scratch, 0, dirfd, gpath, pmode, &t);           \
	if (e) return e;                                                  \
	const char *p = t.is_root ? "." : t.path;                         \
	return TAWC_RAW(sysnr, t.fd, (long)p,                             \
			args->c, args->d,                                 \
			(narg) > 4 ? args->e : 0,                         \
			(narg) > 5 ? args->f : 0);                        \
}

DECLARE_AT_PASS(mkdirat,    TAWC_SYS_mkdirat,    3, TAWCROOT_PATH_PARENT_CREATE)
DECLARE_AT_PASS(mknodat,    TAWC_SYS_mknodat,    4, TAWCROOT_PATH_PARENT_CREATE)

/* fchmodat: translate and ATTEMPT the host chmod — modes matter inside
 * the rootfs (executable bits, go-w checks) and the app uid owns rootfs
 * files, so it normally succeeds for real. But when the host refuses
 * with EPERM/EACCES, report success: the guest believes it is root, and
 * root doesn't get permission errors from chmod. The concrete hit is
 * sshd's pty_setowner() chmod on /dev/pts/N (Android SELinux denies
 * setattr on app ptys), which is fatal() to every TTY login. Same
 * contract as the fchownat/fchown fakes, and what proot's fake_id0
 * does; non-permission errors (ENOENT, ENOTDIR, EROFS) pass through —
 * the translate step already keeps missing paths honest.
 *
 * The swallow is gated on virtual euid == 0: a guest that genuinely
 * dropped privileges should see real permission errors. */
static long handle_fchmodat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, dirfd, gpath,
			      TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;
	const char *p = t.is_root ? "." : t.path;
	long rv = TAWC_RAW(TAWC_SYS_fchmodat, t.fd, (long)p, args->c, 0, 0, 0);
	if ((rv == TAWC_EPERM || rv == TAWC_EACCES) &&
	    tawcroot_identity_euid() == 0)
		return 0;
	return rv;
}

/* unlinkat with /dev/shm intercept. Routes /dev/shm/<name> through the
 * in-handler emulation; everything else through the standard
 * translate-and-pass-through path. */
static long handle_unlinkat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flag = (int)args->c;
	if (!gpath) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long fe = fetch_guest_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = classify_shm(scratch->buf[0], &shm_name);
		if (kind == SHM_PEEK_NAME) {
			if (flag & AT_REMOVEDIR) return TAWC_ENOTDIR;
			return tawcroot_shm_unlink(shm_name);
		}
		if (kind == SHM_PEEK_DIR)
			return (flag & AT_REMOVEDIR) ? TAWC_EBUSY : TAWC_EISDIR;
	}

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd,
				 TAWCROOT_PATH_PARENT_REMOVE, &t);
	if (e) return e;
	/* Path resolved to the rootfs/bind root itself: match kernel
	 * errno for operating on "/". rmdir("/") → EBUSY, unlink("/") →
	 * EISDIR (not the EINVAL an empty t.path would otherwise yield). */
	if (t.is_root)
		return (flag & AT_REMOVEDIR) ? TAWC_EBUSY : TAWC_EISDIR;

	/* Hardlink emulation: unlinking an emulated name is a counted DEL
	 * (park → decrement → delete object at zero). Probe only when a
	 * store is open; AT_REMOVEDIR passes through (kernel ENOTDIRs a
	 * symlink leaf itself). A non-READY store (newer version) gets a
	 * plain unlink of the name symlink — object leaks, safe. */
	if (!(flag & AT_REMOVEDIR) && tawcroot_store_link_fd >= 0) {
		char tok[TAWCROOT_LINK_TOKEN_MAX];
		if (tawcroot_link_leaf_token(t.fd, t.path, tok,
					     sizeof tok) == 1 &&
		    tawcroot_linkstore_state() == TAWCROOT_STORE_READY)
			return tawcroot_link_del(tok, t.fd, t.path);
	}
	return TAWC_RAW(TAWC_SYS_unlinkat, t.fd, (long)t.path, flag, 0, 0, 0);
}

/* utimensat: translate path and pass through. Two special cases:
 *
 * - `pathname == NULL` (Linux extension: operate on dirfd directly) is
 *   forwarded unchanged; AT_PASS can't express it because it rejects
 *   null paths up front.
 * - AT_SYMLINK_NOFOLLOW must drive path translation too, otherwise
 *   the resolver walks through the final symlink and we end up
 *   utimensat-ing the link target instead of the link itself. That's
 *   what pacman's libarchive symlink-extraction path hits (`utimensat
 *   (AT_FDCWD, name, ts, AT_SYMLINK_NOFOLLOW)` against a freshly
 *   created symlink whose target doesn't exist yet → ENOENT → the
 *   "Can't restore time" warning that floods install logs).
 *
 * Without trapping this at all, the guest-visible path goes straight
 * to the host kernel and fails to resolve. */
static long handle_utimensat(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flags = (int)args->d;
	if (!gpath) {
		/* Linux-extension: NULL pathname → operate on dirfd. The
		 * dirfd is one of ours (translated openat handed it back);
		 * forward unchanged — but reserved fds must answer EBADF
		 * (fdtab.h contract), like every other dirfd anchor. */
		if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
		return TAWC_RAW(TAWC_SYS_utimensat, dirfd, 0,
				args->c, flags, 0, 0);
	}
	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, dirfd, gpath,
			      pmode, &t);
	if (e) return e;
	const char *p = t.is_root ? "." : t.path;

	/* Hardlink emulation: NOFOLLOW times on an emulated name land on
	 * the object (the shared file's real mtime, visible through every
	 * name). A dangling token (ENOENT) falls through — the name keeps
	 * plain-symlink behavior. */
	if ((flags & AT_SYMLINK_NOFOLLOW) && tawcroot_store_link_fd >= 0 &&
	    !t.is_root) {
		char tok[TAWCROOT_LINK_TOKEN_MAX];
		if (tawcroot_link_leaf_token(t.fd, t.path, tok,
					     sizeof tok) == 1) {
			long rv = TAWC_RAW(TAWC_SYS_utimensat,
					   tawcroot_store_link_fd, (long)tok,
					   args->c, flags, 0, 0);
			if (rv != TAWC_ENOENT) return rv;
		}
	}
	return TAWC_RAW(TAWC_SYS_utimensat, t.fd, (long)p,
			args->c, flags, 0, 0);
}

/* fchownat: translate path, validate it exists, then fake success
 * (Android untrusted_app uid can't chown; the on-disk file stays
 * app-owned and the guest sees what it expected — proot `-0`). The
 * existence probe is what keeps us honest: `chown("/nope", ...)` must
 * ENOENT like the kernel, not fake-succeed.
 *
 * The fake is gated on virtual euid == 0: a genuinely-dropped guest
 * gets the host's real chown result (normally EPERM) instead. */
static long handle_fchownat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flags = (int)args->e;
	int priv = tawcroot_identity_euid() == 0;

	/* AT_EMPTY_PATH (with NULL or empty path) operates on dirfd
	 * directly — validate the fd. */
	if (flags & AT_EMPTY_PATH) {
		long empty = guest_path_is_empty(gpath);
		if (empty < 0) return empty;
		if (empty) {
			if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
			if (!priv)
				return TAWC_RAW(TAWC_SYS_fchownat, dirfd,
						(long)"", args->c, args->d,
						flags, 0);
			struct stat probe;
			long rv = TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)"",
					   (long)&probe, AT_EMPTY_PATH, 0, 0);
			return rv < 0 ? rv : 0;
		}
	}
	if (!gpath) return TAWC_EFAULT;

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, dirfd, gpath,
			      pmode, &t);
	if (e) return e;
	const char *p = t.is_root ? "" : t.path;
	int sflags = (flags & AT_SYMLINK_NOFOLLOW) | (t.is_root ? AT_EMPTY_PATH : 0);
	/* Hardlink emulation: NOFOLLOW chown on an emulated name targets
	 * the object. Only the unprivileged branch issues a real chown —
	 * the fake-root branch's existence probe is already correct
	 * against the name (present name → success, dangling included:
	 * plain-symlink behavior). ENOENT (dangling) falls through. */
	if (!priv && (flags & AT_SYMLINK_NOFOLLOW) &&
	    tawcroot_store_link_fd >= 0 && !t.is_root) {
		char tok[TAWCROOT_LINK_TOKEN_MAX];
		if (tawcroot_link_leaf_token(t.fd, t.path, tok,
					     sizeof tok) == 1) {
			long rv = TAWC_RAW(TAWC_SYS_fchownat,
					   tawcroot_store_link_fd, (long)tok,
					   args->c, args->d,
					   AT_SYMLINK_NOFOLLOW, 0);
			if (rv != TAWC_ENOENT) return rv;
		}
	}
	if (!priv)
		return TAWC_RAW(TAWC_SYS_fchownat, t.fd, (long)p,
				args->c, args->d, sflags, 0);
	struct stat probe;
	long rv = TAWC_RAW(TAWC_SYS_fstatat, t.fd, (long)p,
			   (long)&probe, sflags, 0, 0);
	return rv < 0 ? rv : 0;  /* exists → fake-root no-op */
}

/* fd-only fchmod: same EPERM/EACCES-swallow contract as fchmodat
 * (SELinux setattr denials — app ptys are the known case — must not
 * reach a fake-root guest as permission errors), same fd validation
 * as fchown below. Non-permission errors pass through; the chmod is
 * genuinely attempted first because modes matter inside the rootfs. */
static long handle_fchmod(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	long rv = TAWC_RAW(TAWC_SYS_fchmod, fd, args->b, 0, 0, 0, 0);
	if ((rv == TAWC_EPERM || rv == TAWC_EACCES) &&
	    tawcroot_identity_euid() == 0)
		return 0;
	return rv;
}

/* fd-only fchown, used by GNU tar when dpkg-deb extracts package
 * control files. Same fake-root contract as fchownat, but validate the
 * fd first: fchown(-1)/fchown(closed) must EBADF like the kernel, and
 * a reserved fd answers EBADF per the fdtab.h contract. Unprivileged
 * (virtual euid != 0) forwards the real syscall. */
static long handle_fchown(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	if (tawcroot_identity_euid() != 0)
		return TAWC_RAW(TAWC_SYS_fchown, fd, args->b, args->c,
				0, 0, 0);
	long r = tawc_fcntl(fd, F_GETFD, 0);
	if (r < 0) return r;  /* -EBADF for a bad/closed fd */
	return 0;
}

/* symlinkat: translate the destination only (the source is the
 * symlink's literal target string, NOT a host path — the kernel writes
 * those bytes into the new inode). The destination is a
 * not-yet-existing leaf, so PARENT_CREATE.
 *
 * Forgery guard (hardlink emulation): a guest-authored target in the
 * `tawcroot:` namespace would be a phantom referrer — an emulated name
 * whose unlink decrements a count it never contributed to, the one
 * route to data loss. EPERM, unconditionally (targets minted while no
 * store is open would go live the moment one is). Fetching the target
 * here also preserves the kernel's EFAULT contract. */
static long handle_symlinkat(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	const char *target = (const char *)(uintptr_t)args->a;
	int newdirfd = (int)args->b;
	const char *linkpath = (const char *)(uintptr_t)args->c;
	if (!target) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *tgt = scratch->buf[2];
	long tn = tawc_copy_string_from_guest(tgt,
					      TAWCROOT_PATH_SCRATCH_SIZE,
					      target);
	if (tn < 0) return tn;
	if (tn >= TAWCROOT_LINK_GUARD_PREFIX_LEN &&
	    memcmp(tgt, TAWCROOT_LINK_GUARD_PREFIX,
		   TAWCROOT_LINK_GUARD_PREFIX_LEN) == 0)
		return TAWC_EPERM;
	struct fs_path t;
	long e = translate_at(scratch, 0, newdirfd, linkpath,
			      TAWCROOT_PATH_PARENT_CREATE, &t);
	if (e) return e;
	if (t.is_root) return TAWC_EINVAL; /* can't create / */
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)tgt, t.fd,
			(long)t.path, 0, 0, 0);
}

/* statx with fake-root decoration. Layout differs from `struct stat`
 * — see <linux/stat.h>. We always set uid/gid to 0 (matches proot `-0`)
 * AND set the corresponding stx_mask bits so the guest sees the fields
 * as populated. Without the mask update, a guest that didn't request
 * STATX_UID would see a kernel-zero (because the kernel didn't fill it)
 * AND mask=0 — semantically "uid not asked for", which is consistent
 * but doesn't reflect our fake-root view. (Review C11.) */
#ifndef STATX_UID
# define STATX_UID 0x00000080U
#endif
#ifndef STATX_GID
# define STATX_GID 0x00000100U
#endif
#ifndef STATX_NLINK
# define STATX_NLINK 0x00000004U
#endif
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
		long empty = guest_path_is_empty(path);
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
			}
			return finish_statx(rv, &local, out);
		}
	}

	if (!path) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long fe = fetch_guest_path(scratch, 0, path);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = classify_shm(scratch->buf[0], &shm_name);
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

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	struct fs_path t;
	long e = translate_local(scratch, 0, dirfd, pmode, &t);
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
				}
			}
		} else if (t.fd == tawcroot_store_link_fd && !t.is_root) {
			local.stx_nlink = (unsigned int)
				tawcroot_link_count_for_stat(t.path);
			local.stx_mask |= STATX_NLINK;
		}
	}
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

/* inotify_add_watch(fd, path, mask): translate the path, then route the
 * kernel call through /proc/self/fd/<base_fd>/<suffix> (no *at variant
 * exists). Untrapped, GLib/GIO file monitors silently watch host paths.
 * IN_DONT_FOLLOW (0x02000000) selects NOFOLLOW resolution. */
static long handle_inotify_add_watch(const tawcroot_syscall_args *args,
				     ucontext_t *uc)
{
	(void)uc;
	int inotify_fd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	unsigned int mask = (unsigned int)args->c;
	if (!gpath) return TAWC_EFAULT;

	tawcroot_path_mode pmode = (mask & 0x02000000U /*IN_DONT_FOLLOW*/)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, AT_FDCWD, gpath,
			      pmode, &t);
	if (e) return e;
	char *host_path = scratch->buf[2];
	long bp = tawc_proc_fd_path(host_path, TAWCROOT_PATH_SCRATCH_SIZE,
				    t.fd, t.path);
	if (bp < 0) return bp;
	return TAWC_RAW(TAWC_SYS_inotify_add_watch, inotify_fd,
			(long)host_path, mask, 0, 0, 0);
}

/* v1 link fallback tail (pre-store emulation, kept for stores that
 * cannot exist: no store path configured, store dir uncreatable, or a
 * cross-fs bind source that cannot reach the store by rename): move
 * the real file to the NEW name (RENAME_NOREPLACE preserves link()'s
 * EEXIST) and leave a guest-absolute symlink at the OLD name. The
 * direction matters: the link(tmp, final) + unlink(tmp) publish idiom
 * (git object/pack finalize) must leave real data at the final name.
 * `orig_rv` is the host linkat's failure, reported when the fallback
 * itself cannot proceed. */
static long link_fallback_v1(int src_fd, const char *src_suf,
			     int dst_fd, const char *dst_suf, long orig_rv)
{
	long re = TAWC_RAW(TAWC_SYS_renameat2, src_fd, (long)src_suf,
			   dst_fd, (long)dst_suf, RENAME_NOREPLACE, 0);
	/* EEXIST/EXDEV are errors link() itself defines — surface them.
	 * Anything else means the emulation can't work here; report the
	 * original link failure. */
	if (re == TAWC_EEXIST || re == TAWC_EXDEV) return re;
	if (re) return orig_rv;
	/* Compose the back-symlink's GUEST-absolute target by reverse-
	 * translating the destination's host path. "/" + dst_suf is only
	 * right when dst_fd is the rootfs — for a destination under a
	 * bind the suffix is bind-relative, and the mis-aimed symlink
	 * would dangle the original name (the publish idiom then unlinks
	 * the only working path to the data). A destination we cannot
	 * reverse-translate gets the rollback, not a wrong target. */
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *host       = scratch->buf[0];
	char *abs_target = scratch->buf[1];
	long se = TAWC_EINVAL;
	long hn = tawcroot_proc_fd_to_host_path(dst_fd, host,
						TAWCROOT_PATH_SCRATCH_SIZE);
	if (hn > 0) {
		size_t pos = (size_t)hn;
		if (!tawc_str_append(host, TAWCROOT_PATH_SCRATCH_SIZE,
				     &pos, "/") &&
		    !tawc_str_append(host, TAWCROOT_PATH_SCRATCH_SIZE,
				     &pos, dst_suf) &&
		    tawcroot_host_path_to_guest_abs(host, pos, abs_target,
						    TAWCROOT_PATH_SCRATCH_SIZE)
			    > 0)
			se = TAWC_RAW(TAWC_SYS_symlinkat, (long)abs_target,
				      src_fd, (long)src_suf, 0, 0, 0);
	}
	if (!se) return 0;
	/* Symlink-back failed: roll the rename back so the failed link
	 * leaves the tree unchanged. */
	TAWC_RAW(TAWC_SYS_renameat2, dst_fd, (long)dst_suf,
		 src_fd, (long)src_suf, 0, 0);
	return orig_rv;
}

/* True when a translate result landed inside the guest view but NOT
 * the rootfs — i.e. on a bind src dirfd. Emulated names must not be
 * planted there: the orchestrator skips the symlink resolver for
 * bind-routed paths, so a token symlink inside a bind is unresolvable
 * on FOLLOW opens (the data would be marooned in the store while
 * lstat claims a regular file). linkat instead degrades: NEW takes
 * the v1 fallback (both names stay real/openable), ADD returns EXDEV
 * (tools fall back to copy). Guest-supplied dirfd passthroughs are
 * not reserved and fall through unchanged. */
static int fs_path_in_bind(const struct fs_path *t)
{
	return t->fd != tawcroot_rootfs_fd &&
	       t->fd != tawcroot_store_link_fd &&
	       tawcroot_fd_is_reserved(t->fd);
}

/* linkat AT_EMPTY_PATH source: the file `olddirfd` itself refers to.
 * A store-resident fd (an open link object) is detected BEFORE the
 * host attempt — a host linkat from an object fd would mint an
 * uncounted referrer on any device whose policy allows it. A named
 * in-view source emulates via NEW on its host-real path (the fd's
 * /proc link tracks renames, so this is the current name); a nameless
 * source (memfd, O_TMPFILE, fully unlinked) gets a deliberate EXDEV
 * per the plan (accepted until the tmp/ stage lands).
 *
 * The host attempt's emulation gate includes ENOENT: the kernel
 * refuses unprivileged AT_EMPTY_PATH linkat with ENOENT (the
 * CAP_DAC_READ_SEARCH check), not EPERM — and uses ENOENT for
 * nlink==0 sources too, which the fstat below routes to EXDEV. */
static long linkat_empty_path(struct tawcroot_path_scratch *scratch,
			      int olddirfd, int newdirfd,
			      const char *newpath, int flags)
{
	if (tawcroot_fd_is_reserved(olddirfd)) return TAWC_EBADF;

	struct fs_path tnew;
	long e = translate_at(scratch, 0, newdirfd, newpath,
			      TAWCROOT_PATH_PARENT_CREATE, &tnew);
	if (e) return e;
	if (tnew.is_root) return TAWC_ENOENT;

	/* Same LATENT upgrade as handle_linkat: the host-path compares
	 * below need the canonical store path only store_open derives. */
	if (tawcroot_store_link_fd < 0 &&
	    tawcroot_linkstore_state() == TAWCROOT_STORE_LATENT)
		(void)tawcroot_linkstore_latent_upgrade();

	char *hostp = scratch->buf[2];
	long hn = tawcroot_proc_fd_to_host_path(olddirfd, hostp,
						TAWCROOT_PATH_SCRATCH_SIZE);
	if (hn <= 0) hostp[0] = 0;

	char tok[TAWCROOT_LINK_TOKEN_MAX];
	if (tawcroot_link_host_path_token(hostp, tok, sizeof tok)) {
		if (tawcroot_linkstore_state() != TAWCROOT_STORE_READY)
			return TAWC_EPERM;
		/* Emulated names never land in binds (fs_path_in_bind). */
		if (fs_path_in_bind(&tnew)) return TAWC_EXDEV;
		return tawcroot_link_add(tok, tnew.fd, tnew.path);
	}
	/* Linkable O_TMPFILE fd: publish (the idiom this flag exists
	 * for — linkat(fd, "", dst, AT_EMPTY_PATH)). */
	if (tawcroot_link_host_path_tmp(hostp, tok, sizeof tok))
		return tawcroot_link_publish_tmp(tok, tnew.fd, tnew.path);

	long rv = TAWC_RAW(TAWC_SYS_linkat, olddirfd, (long)"",
			   tnew.fd, (long)tnew.path, flags, 0);
	if (rv == 0 ||
	    (rv != TAWC_EACCES && rv != TAWC_EPERM && rv != TAWC_ENOENT))
		return rv;

	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, olddirfd, (long)&st, 0, 0, 0, 0) != 0)
		return rv;
	if (S_ISDIR(st.st_mode)) return rv;
	if (st.st_nlink == 0 || hostp[0] != '/') return TAWC_EXDEV;

	/* The /proc link is best-effort: verify it still names THIS inode
	 * (a racing rename/unlink or a " (deleted)" suffix must not make
	 * NEW move some other file into the store), and that it lies
	 * inside the guest view (rootfs or a bind) — moving an arbitrary
	 * app-reachable host file into the store would strand a token
	 * symlink nothing outside tawcroot can read. */
	struct stat pst;
	if (TAWC_RAW(TAWC_SYS_fstatat, AT_FDCWD, (long)hostp, (long)&pst,
		     AT_SYMLINK_NOFOLLOW, 0, 0) != 0 ||
	    pst.st_dev != st.st_dev || pst.st_ino != st.st_ino)
		return TAWC_EXDEV;
	char *gview = scratch->buf[3];
	if (tawcroot_host_path_to_guest_abs(hostp, (size_t)hn, gview,
					    TAWCROOT_PATH_SCRATCH_SIZE) <= 0)
		return TAWC_EXDEV;

	/* Either name landing in a bind: v1, not token symlinks
	 * (fs_path_in_bind). The source side is judged by host path —
	 * in-view but not under the rootfs prefix means a bind. */
	int src_in_bind =
		!(hn >= (long)tawcroot_rootfs_host_path_len &&
		  memcmp(hostp, tawcroot_rootfs_host_path,
			 tawcroot_rootfs_host_path_len) == 0 &&
		  ((size_t)hn == tawcroot_rootfs_host_path_len ||
		   hostp[tawcroot_rootfs_host_path_len] == '/'));
	if (fs_path_in_bind(&tnew) || src_in_bind)
		return link_fallback_v1(AT_FDCWD, hostp,
					tnew.fd, tnew.path, rv);

	switch (tawcroot_linkstore_state()) {
	case TAWCROOT_STORE_READY:
	case TAWCROOT_STORE_LATENT: {
		long nrv = tawcroot_link_new(AT_FDCWD, hostp,
					     tnew.fd, tnew.path);
		if (nrv == TAWC_EPERM &&
		    tawcroot_linkstore_state() == TAWCROOT_STORE_DEGRADED)
			return rv;
		if (nrv == TAWC_EXDEV || nrv == TAWC_EPERM)
			return link_fallback_v1(AT_FDCWD, hostp,
						tnew.fd, tnew.path, rv);
		return nrv;
	}
	case TAWCROOT_STORE_DEGRADED:
		return rv;
	default:
		return link_fallback_v1(AT_FDCWD, hostp,
					tnew.fd, tnew.path, rv);
	}
}

/* linkat: translate both operands, then emulate.
 *
 * Order matters (notes/tawcroot/link-emulation.md):
 *   1. Emulated-name source is detected BEFORE the host attempt —
 *      symlinks are a different SELinux class (lnk_file), so a host
 *      linkat could *succeed* and hardlink the token symlink itself:
 *      a phantom referrer the count never learns about. (The emulator
 *      policy denies lnk_file link too, but other devices may not.)
 *   2. A source already inside the store is store-aware: ADD with the
 *      token from the path. The resolver catches token names in the
 *      rootfs view (FOLLOW lands on link/); the O_PATH backstop below
 *      catches every other FOLLOW spelling (/proc/self/fd/N through a
 *      /proc bind is the live one). NEW must never rename a
 *      store-resident source — that would rename the object itself
 *      out of link/, dangling the whole cluster.
 *   3. Otherwise host linkat first; only EACCES/EPERM engages the
 *      emulation, and a directory source keeps the kernel's EPERM. */
static long handle_linkat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int          olddirfd = (int)args->a;
	const char  *oldpath  = (const char *)(uintptr_t)args->b;
	int          newdirfd = (int)args->c;
	const char  *newpath  = (const char *)(uintptr_t)args->d;
	int          flags    = (int)args->e;

	/* AT_SYMLINK_FOLLOW (0x400) selects whether the source is followed
	 * if it's a symlink. Default: don't follow (link to the symlink).
	 * The destination is a new entry → PARENT_CREATE. */
	tawcroot_path_mode src_mode = (flags & 0x400 /*AT_SYMLINK_FOLLOW*/)
		? TAWCROOT_PATH_FOLLOW
		: TAWCROOT_PATH_NOFOLLOW;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	if (flags & AT_EMPTY_PATH) {
		long empty = guest_path_is_empty(oldpath);
		if (empty < 0) return empty;
		if (empty)
			return linkat_empty_path(scratch, olddirfd,
						 newdirfd, newpath, flags);
	}

	struct fs_path told, tnew;
	long e1 = translate_at(scratch, 0, olddirfd, oldpath, src_mode, &told);
	if (e1) return e1;
	long e2 = translate_at(scratch, 2, newdirfd, newpath,
			       TAWCROOT_PATH_PARENT_CREATE, &tnew);
	if (e2) return e2;
	/* Kernel: a root/empty operand to link is ENOENT (the empty-name
	 * lookup), not EINVAL. */
	if (told.is_root || tnew.is_root) return TAWC_ENOENT;

	/* A LATENT process may be running beside one that already minted
	 * the store (it is created lazily, so any long-lived guest that
	 * started before the distro's first emulated hardlink is LATENT).
	 * Open it BEFORE source detection: with the store fds closed the
	 * probes below all skip, and an existing token name would be
	 * treated as a plain symlink source — a phantom referrer where
	 * host policy allows lnk_file links, a nested token object (both
	 * names ENOENT) where it doesn't. Cold: linkat only, and one
	 * existence probe per call while LATENT. */
	if (tawcroot_store_link_fd < 0 &&
	    tawcroot_linkstore_state() == TAWCROOT_STORE_LATENT)
		(void)tawcroot_linkstore_latent_upgrade();

	if (tawcroot_store_link_fd >= 0) {
		int  ready = tawcroot_linkstore_state()
			     == TAWCROOT_STORE_READY;
		char tok[TAWCROOT_LINK_TOKEN_MAX];
		const char *add_tok = 0;
		if (told.fd == tawcroot_store_link_fd) {
			/* FOLLOW resolution landed in the store. A residual
			 * '/' means a token was used as a directory. */
			for (const char *q = told.path; *q; q++)
				if (*q == '/') return TAWC_ENOTDIR;
			add_tok = told.path;
		} else if (src_mode == TAWCROOT_PATH_NOFOLLOW &&
			   tawcroot_link_leaf_token(told.fd, told.path, tok,
						    sizeof tok) == 1) {
			add_tok = tok;
		} else if (src_mode == TAWCROOT_PATH_FOLLOW) {
			/* Mandatory backstop: FOLLOW spellings the resolver
			 * never sees can still land on an object —
			 * /proc/self/fd/N routes through the /proc bind
			 * (binds skip the resolver) and its magic-link
			 * target is the raw store path. O_PATH-open the
			 * source (kernel chases the leaf, magic links
			 * included) and compare its host path against
			 * <store>/link/. Guest-authored absolute symlinks
			 * can't forge this: the resolver folds their
			 * targets into the guest view before we get here. */
			long pfd = tawc_openat(told.fd, told.path,
					       O_PATH | O_CLOEXEC, 0);
			if (pfd >= 0) {
				char *hostp = scratch->buf[0];
				long hn = tawcroot_proc_fd_to_host_path(
					(int)pfd, hostp,
					TAWCROOT_PATH_SCRATCH_SIZE);
				tawc_close((int)pfd);
				if (hn > 0 &&
				    tawcroot_link_host_path_token(
					    hostp, tok, sizeof tok))
					add_tok = tok;
				/* Linkable O_TMPFILE via the magic-link
				 * spelling (open(2)'s documented publish
				 * idiom): one atomic rename. */
				else if (hn > 0 &&
					 tawcroot_link_host_path_tmp(
						 hostp, tok, sizeof tok))
					return tawcroot_link_publish_tmp(
						tok, tnew.fd, tnew.path);
			}
		}
		if (add_tok) {
			/* Degraded store: mutations refuse; raw EPERM. */
			if (!ready) return TAWC_EPERM;
			/* Emulated names never land in binds (see
			 * fs_path_in_bind). */
			if (fs_path_in_bind(&tnew)) return TAWC_EXDEV;
			return tawcroot_link_add(add_tok, tnew.fd, tnew.path);
		}
	}

	long rv = TAWC_RAW(TAWC_SYS_linkat, told.fd, (long)told.path,
			   tnew.fd, (long)tnew.path, flags, 0);
	if (rv == 0 || (rv != TAWC_EACCES && rv != TAWC_EPERM))
		return rv;

	/* link(2) on a directory is the kernel's own EPERM,
	 * indistinguishable from an SELinux denial by errno alone —
	 * emulating it would "hardlink" the directory by renaming it
	 * away. Stat and pass the kernel's error through. */
	struct stat st;
	long te = TAWC_RAW(TAWC_SYS_fstatat, told.fd, (long)told.path,
			   (long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
	if (te || S_ISDIR(st.st_mode)) return rv;

	/* A plain-symlink NOFOLLOW source becomes a symlink OBJECT: NEW
	 * renames the symlink itself into link/ (fstatat above is
	 * NOFOLLOW, so the token is the symlink's inode), and the
	 * resolver splices the object's target back into the guest walk
	 * (readlink_store oracle member) — real hardlinked-symlink
	 * semantics, relative targets resolving against each NAME's
	 * directory. */

	/* Either operand on a bind: v1 keeps both names real/openable
	 * where NEW's token symlinks would be unresolvable (see
	 * fs_path_in_bind). */
	if (fs_path_in_bind(&told) || fs_path_in_bind(&tnew))
		return link_fallback_v1(told.fd, told.path,
					tnew.fd, tnew.path, rv);

	switch (tawcroot_linkstore_state()) {
	case TAWCROOT_STORE_READY:
	case TAWCROOT_STORE_LATENT: {
		long nrv = tawcroot_link_new(told.fd, told.path,
					     tnew.fd, tnew.path);
		/* EXDEV: the source cannot reach the store by rename
		 * (cross-fs bind) — degrade to the v1 emulation, both
		 * ends stay on the bind's fs. EPERM: the store could not
		 * be created/locked — v1 keeps today's behavior — UNLESS
		 * the lock-time version re-check just flipped the store
		 * to DEGRADED (newer format): then raw EPERM, no writes. */
		if (nrv == TAWC_EPERM &&
		    tawcroot_linkstore_state() == TAWCROOT_STORE_DEGRADED)
			return rv;
		if (nrv == TAWC_EXDEV || nrv == TAWC_EPERM)
			return link_fallback_v1(told.fd, told.path,
						tnew.fd, tnew.path, rv);
		return nrv;
	}
	case TAWCROOT_STORE_DEGRADED:
		return rv;  /* newer store: raw EPERM, zero corruption */
	default:
		return link_fallback_v1(told.fd, told.path,
					tnew.fd, tnew.path, rv);
	}
}

/* renameat2 — translate both operands; old is PARENT_REMOVE, new is
 * PARENT_CREATE. The kernel takes (olddirfd, oldpath, newdirfd, newpath,
 * flags); when the guest passes non-AT_FDCWD dirfds with relative paths
 * we let the kernel resolve off the dirfd (see translate_at).
 * Both renameat and renameat2 funnel through here — renameat just passes
 * flags=0 to the kernel renameat2 (the latter is a strict superset and
 * present on every kernel we target). */
static long do_renameat(int olddirfd, const char *oldpath,
			int newdirfd, const char *newpath,
			unsigned int rflags)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path told, tnew;
	long e1 = translate_at(scratch, 0, olddirfd, oldpath,
			       TAWCROOT_PATH_PARENT_REMOVE, &told);
	if (e1) return e1;
	long e2 = translate_at(scratch, 2, newdirfd, newpath,
			       TAWCROOT_PATH_PARENT_CREATE, &tnew);
	if (e2) return e2;
	if (told.is_root || tnew.is_root) return TAWC_EINVAL;

	/* Hardlink emulation. Only the plain-replace shape needs work:
	 *  - both names resolve to the same token → POSIX same-inode
	 *    no-op (return 0, both names remain; the kernel does this by
	 *    inode, we do it by token);
	 *  - dst is an emulated name → CLOBBER: rename under the store
	 *    lock, then decrement the clobbered cluster.
	 * NOREPLACE gets the kernel's EEXIST off the symlink itself;
	 * EXCHANGE atomically swaps the two entries with no count change;
	 * src-emulated needs nothing (opaque targets are location-
	 * independent). */
	if (tawcroot_store_link_fd >= 0 &&
	    !(rflags & (RENAME_NOREPLACE | RENAME_EXCHANGE))) {
		char ntok[TAWCROOT_LINK_TOKEN_MAX];
		if (tawcroot_link_leaf_token(tnew.fd, tnew.path, ntok,
					     sizeof ntok) == 1) {
			char otok[TAWCROOT_LINK_TOKEN_MAX];
			if (tawcroot_link_leaf_token(told.fd, told.path, otok,
						     sizeof otok) == 1 &&
			    tawc_streq(otok, ntok))
				return 0;
			if (tawcroot_linkstore_state()
			    == TAWCROOT_STORE_READY)
				return tawcroot_link_clobber(
					told.fd, told.path,
					tnew.fd, tnew.path, rflags);
			/* Non-READY store: plain rename below — the
			 * clobbered object leaks (overcount, safe). */
		}
	}
	return TAWC_RAW(TAWC_SYS_renameat2, told.fd, (long)told.path,
			tnew.fd, (long)tnew.path, rflags, 0);
}

static long handle_renameat2(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	return do_renameat((int)args->a,
			   (const char *)(uintptr_t)args->b,
			   (int)args->c,
			   (const char *)(uintptr_t)args->d,
			   (unsigned int)args->e);
}

static long handle_renameat(const tawcroot_syscall_args *args,
			    ucontext_t *uc)
{
	(void)uc;
	return do_renameat((int)args->a,
			   (const char *)(uintptr_t)args->b,
			   (int)args->c,
			   (const char *)(uintptr_t)args->d, 0);
}

/* truncate(path, length). POSIX says it follows symlinks; there is no
 * truncateat or AT_-variant in the kernel, so we openat(O_WRONLY) and
 * ftruncate. The dance leaks one fd per call into our internal range
 * (which Phase 0.5 will protect from guest close() — we close it
 * ourselves before returning, so the guest can't observe the fd).
 *
 * On x86_64 this is also legacy (number 76). On aarch64, glibc's
 * truncate(2) wrapper goes through this same syscall (number 45).
 * Trap on both arches. */
static long handle_truncate(const tawcroot_syscall_args *args,
			    ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	long len          = (long)args->b;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, AT_FDCWD, gpath,
			      TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;
	/* t.fd is a directory in every translation path (rootfs or a
	 * bind src, both opened O_DIRECTORY at init), so empty t.path means
	 * the kernel would EISDIR truncate(2) on the dir. */
	if (t.is_root) return TAWC_EISDIR;

	long fd = tawc_openat(t.fd, t.path,
			      O_WRONLY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	long rv = TAWC_RAW(TAWC_SYS_ftruncate, (long)fd, len, 0, 0, 0, 0);
	tawc_close((int)fd);
	return rv;
}

#if defined(__x86_64__)
/* Legacy x86_64 path-bearing syscalls. Android's untrusted_app filter
 * RET_TRAPs these (the lp64-`access`-on-x86_64 issue documented in
 * notes/proot.md). We translate to the *at variant in the handler,
 * which is the cleanest version of the kludge proot's
 * src/tracee/seccomp.c carries. */

/* Re-dispatch a legacy syscall as its modern *at counterpart by
 * rebuilding the argument frame. `nr` is preserved so nr-sensitive
 * handlers (handle_faccessat's faccessat2 check) still see the
 * original syscall number. */
static long via_at(tawcroot_handler_fn fn, const tawcroot_syscall_args *args,
		   ucontext_t *uc, long a, long b, long c, long d, long e)
{
	tawcroot_syscall_args s = *args;
	s.a = a; s.b = b; s.c = c; s.d = d; s.e = e;
	return fn(&s, uc);
}

static long handle_stat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_newfstatat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_lstat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_newfstatat, args, uc,
		AT_FDCWD, args->a, args->b, AT_SYMLINK_NOFOLLOW, 0); }

static long handle_access(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_faccessat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_readlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_readlinkat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }

static long handle_chmod(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_fchmodat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_mkdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_mkdirat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_unlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_unlinkat, args, uc, AT_FDCWD, args->a, 0, 0, 0); }

static long handle_rmdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_unlinkat, args, uc,
		AT_FDCWD, args->a, AT_REMOVEDIR, 0, 0); }

/* Legacy chown/lchown route through handle_fchownat: same translation,
 * existence probe, and euid-gated fake. args->nr is preserved by
 * via_at, so one handler serves both (lchown adds NOFOLLOW). */
static long handle_chown_legacy(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	int flags = args->nr == TAWC_SYS_lchown ? AT_SYMLINK_NOFOLLOW : 0;
	return via_at(handle_fchownat, args, uc,
		      AT_FDCWD, args->a, args->b, args->c, flags);
}

/* Legacy x86_64 link(oldpath, newpath): linkat with flags=0 (source is
 * NOFOLLOW). */
static long handle_link_legacy(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{ return via_at(handle_linkat, args, uc,
		AT_FDCWD, args->a, AT_FDCWD, args->b, 0); }

/* Legacy x86_64 symlink(target, linkpath). */
static long handle_symlink_legacy(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{ return via_at(handle_symlinkat, args, uc,
		args->a, AT_FDCWD, args->b, 0, 0); }

/* Legacy x86_64 rename(oldpath, newpath). */
static long handle_rename_legacy(const tawcroot_syscall_args *args,
				 ucontext_t *uc)
{
	(void)uc;
	return do_renameat(AT_FDCWD,
			   (const char *)(uintptr_t)args->a,
			   AT_FDCWD,
			   (const char *)(uintptr_t)args->b, 0);
}

/* Legacy x86_64 open(path, flags, mode). Glibc on Android always
 * uses openat (NR 257) because Android's stacked filter RET_TRAPs
 * NR 2 and glibc has fallback logic, but a static binary or non-
 * glibc libc that issues raw NR 2 directly would otherwise bypass
 * tawcroot's path translation (kernel sees the literal guest path
 * against the host fs). Route through handle_openat with
 * dirfd = AT_FDCWD so legacy callers get the same translation as
 * modern openat callers. */
static long handle_open_legacy(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{ return via_at(handle_openat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }

/* Legacy x86_64 creat(path, mode) ≡ open(path, O_WRONLY|O_CREAT|O_TRUNC).
 * Same routing rationale as handle_open_legacy. */
static long handle_creat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_openat, args, uc,
		AT_FDCWD, args->a, O_WRONLY | O_CREAT | O_TRUNC, args->b, 0); }

/* Shared tail for the legacy time-setting trio: translate (dirfd, path)
 * and issue utimensat with a kernel-side timespec[2] (or NULL = now).
 * All three follow leaf symlinks (none has an AT_SYMLINK_NOFOLLOW). */
static long utimensat_via_translate(int dirfd, const char *gpath,
				    const long ts[4])
{
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, dirfd, gpath,
			      TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;
	const char *p = t.is_root ? "." : t.path;
	return TAWC_RAW(TAWC_SYS_utimensat, t.fd, (long)p,
			(long)ts, 0, 0, 0);
}

/* Legacy x86_64 utime(path, struct utimbuf*): two time_t seconds. */
static long handle_utime(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	const void *gtimes = (const void *)(uintptr_t)args->b;
	if (!gtimes)
		return utimensat_via_translate(AT_FDCWD, gpath, 0);
	long sec[2];
	long e = tawc_copy_from_guest(sec, sizeof sec, gtimes);
	if (e < 0) return e;
	long ts[4] = { sec[0], 0, sec[1], 0 };
	return utimensat_via_translate(AT_FDCWD, gpath, ts);
}

/* timeval[2] → timespec[2] with the kernel's EINVAL range check. */
static long timeval_pair_to_ts(const void *gtimes, long ts[4])
{
	long tv[4];  /* {sec, usec} x2 */
	long e = tawc_copy_from_guest(tv, sizeof tv, gtimes);
	if (e < 0) return e;
	if (tv[1] < 0 || tv[1] >= 1000000 ||
	    tv[3] < 0 || tv[3] >= 1000000) return TAWC_EINVAL;
	ts[0] = tv[0]; ts[1] = tv[1] * 1000;
	ts[2] = tv[2]; ts[3] = tv[3] * 1000;
	return 0;
}

/* Legacy x86_64 utimes(path, struct timeval[2]). */
static long handle_utimes(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	const void *gtimes = (const void *)(uintptr_t)args->b;
	if (!gtimes)
		return utimensat_via_translate(AT_FDCWD, gpath, 0);
	long ts[4];
	long e = timeval_pair_to_ts(gtimes, ts);
	if (e < 0) return e;
	return utimensat_via_translate(AT_FDCWD, gpath, ts);
}

/* Legacy x86_64 futimesat(dirfd, path, struct timeval[2]). */
static long handle_futimesat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	const void *gtimes = (const void *)(uintptr_t)args->c;
	if (!gtimes)
		return utimensat_via_translate(dirfd, gpath, 0);
	long ts[4];
	long e = timeval_pair_to_ts(gtimes, ts);
	if (e < 0) return e;
	return utimensat_via_translate(dirfd, gpath, ts);
}

/* Legacy x86_64 mknod(path, mode, dev). aarch64 has no mknod — only
 * mknodat — so this is x86_64-only. */
static long handle_mknod(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(handle_mknodat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }
#endif  /* __x86_64__ */

/* statfs(path, buf): translate path, then dispatch using a /proc/self/
 * fd-anchored path. statfs has no fd-relative variant; we could `openat
 * O_PATH` + fstatfs, but on Android-shipped 5.4 kernels fstatfs against
 * an O_PATH fd returns -EBADF. The /proc/self/fd path gets the same
 * effect (kernel resolves through the fd) with no version surprises. */
static long handle_statfs(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = translate_at(scratch, 0, AT_FDCWD, gpath,
			      TAWCROOT_PATH_FOLLOW, &t);
	if (e) return e;
	char *host_path = scratch->buf[2];
	long bp = tawc_proc_fd_path(host_path, TAWCROOT_PATH_SCRATCH_SIZE,
				    t.fd, t.path);
	if (bp < 0) return bp;
	return TAWC_RAW(TAWC_SYS_statfs, (long)host_path, args->b, 0, 0, 0, 0);
}

/* Path-bearing xattr handlers. The xattr syscalls don't have an *at
 * family; we route through `/proc/self/fd/<base_fd>/<suffix>`. The
 * `l*xattr` variants don't follow leaf symlinks: we use NOFOLLOW path
 * mode (so the resolver leaves the leaf alone) and dispatch to the
 * matching `l*xattr` syscall (the kernel handles the leaf-NOFOLLOW
 * itself).
 *
 * Empty-suffix corner case: if the guest path resolves to the rootfs/
 * bind dir itself (`use_empty == 1`), the dispatched path is just
 * `/proc/self/fd/<n>`, which IS a symlink in procfs. FOLLOW variants
 * are correct (the kernel walks the magic symlink and lands on the
 * dirfd's underlying inode); NOFOLLOW variants would target the
 * procfs entry itself rather than the dirfd's inode — semantically
 * wrong, so we short-circuit `l*xattr` on the empty suffix with
 * -EOPNOTSUPP. Programs almost never l*xattr the rootfs/bind root
 * directly, but the code shouldn't silently misroute when they do.
 *
 * `f*xattr` (fd-based) is NOT trapped — fds are opaque to us and the
 * kernel's resolution against an open fd is already correct.
 *
 * Most calls will return -EOPNOTSUPP / -ENOTSUP from the kernel
 * (Android app-private storage doesn't carry xattrs); the value of
 * trapping is that the guest sees the right errno against the right
 * path rather than a host-relative error. */
#define DECLARE_PATH_XATTR(name, sysnr, narg, pmode)                      \
static long handle_##name(const tawcroot_syscall_args *args, ucontext_t *uc) \
{                                                                          \
	(void)uc;                                                          \
	const char *gpath = (const char *)(uintptr_t)args->a;                  \
	if (!gpath) return TAWC_EFAULT;                                        \
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);                                   \
	struct fs_path t;                                                      \
	long e = translate_at(scratch, 0, AT_FDCWD, gpath, pmode, &t);         \
	if (e) return e;                                                   \
	if (t.is_root && (pmode) == TAWCROOT_PATH_NOFOLLOW) {                  \
		return TAWC_EOPNOTSUPP; /* see big comment above */            \
	}                                                                      \
	char *host_path = scratch->buf[2];                                      \
	long bp = tawc_proc_fd_path(host_path,                                  \
				    TAWCROOT_PATH_SCRATCH_SIZE,               \
				    t.fd, t.path);                             \
	if (bp < 0) return bp;                                             \
	return TAWC_RAW(sysnr, (long)host_path,                            \
			args->b, args->c, args->d,                         \
			(narg) > 4 ? args->e : 0,                          \
			(narg) > 5 ? args->f : 0);                         \
}

DECLARE_PATH_XATTR(setxattr,     TAWC_SYS_setxattr,     5, TAWCROOT_PATH_FOLLOW)
DECLARE_PATH_XATTR(lsetxattr,    TAWC_SYS_lsetxattr,    5, TAWCROOT_PATH_NOFOLLOW)
DECLARE_PATH_XATTR(getxattr,     TAWC_SYS_getxattr,     4, TAWCROOT_PATH_FOLLOW)
DECLARE_PATH_XATTR(lgetxattr,    TAWC_SYS_lgetxattr,    4, TAWCROOT_PATH_NOFOLLOW)
DECLARE_PATH_XATTR(listxattr,    TAWC_SYS_listxattr,    3, TAWCROOT_PATH_FOLLOW)
DECLARE_PATH_XATTR(llistxattr,   TAWC_SYS_llistxattr,   3, TAWCROOT_PATH_NOFOLLOW)
DECLARE_PATH_XATTR(removexattr,  TAWC_SYS_removexattr,  2, TAWCROOT_PATH_FOLLOW)
DECLARE_PATH_XATTR(lremovexattr, TAWC_SYS_lremovexattr, 2, TAWCROOT_PATH_NOFOLLOW)

void tawcroot_fs_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_openat,      handle_openat);
	tawcroot_dispatch_install(TAWC_SYS_fstatat,     handle_newfstatat);
	tawcroot_dispatch_install(TAWC_SYS_fstat,       handle_fstat);
	tawcroot_dispatch_install(TAWC_SYS_readlinkat,  handle_readlinkat);
	/* openat2/fchmodat2: ENOSYS so callers fall back to the *at
	 * variants we translate; untrapped they'd resolve against the
	 * HOST view (BPF default is RET_ALLOW). The fallback is universal:
	 * every openat2/fchmodat2 caller handles ENOSYS because older
	 * kernels lack the syscalls. */
	tawcroot_dispatch_install(TAWC_SYS_openat2,     tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_fchmodat2,   tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_inotify_add_watch,
				  handle_inotify_add_watch);
	/* Trap both faccessat (NR 269 aarch64 / 48 x86_64) and the
	 * newer faccessat2 (NR 439). Glibc's `access(2)` wrapper issues
	 * the older faccessat on most kernels (faccessat2 only on
	 * 5.8+ via dlopen-style probe), so without trapping faccessat
	 * we'd let the kernel resolve guest paths against the host
	 * filesystem — fontconfig, ld.so PATH lookups, and any libc
	 * `access()` for an in-rootfs path silently get -ENOENT.
	 * The handler issues an inner faccessat via TAWC_RAW (our own
	 * IP allowlist whitelists it; Android's stacked filter accepts
	 * NR 269 but RET_TRAPs faccessat2). */
	tawcroot_dispatch_install(TAWC_SYS_faccessat,   handle_faccessat);
	tawcroot_dispatch_install(TAWC_SYS_faccessat2,  handle_faccessat);
	tawcroot_dispatch_install(TAWC_SYS_chdir,       handle_chdir);
	tawcroot_dispatch_install(TAWC_SYS_getcwd,      handle_getcwd);
	tawcroot_dispatch_install(TAWC_SYS_mkdirat,     handle_mkdirat);
	tawcroot_dispatch_install(TAWC_SYS_unlinkat,    handle_unlinkat);
	tawcroot_dispatch_install(TAWC_SYS_symlinkat,   handle_symlinkat);
	tawcroot_dispatch_install(TAWC_SYS_fchmodat,    handle_fchmodat);
	tawcroot_dispatch_install(TAWC_SYS_fchmod,      handle_fchmod);
	tawcroot_dispatch_install(TAWC_SYS_fchown,      handle_fchown);
	tawcroot_dispatch_install(TAWC_SYS_fchownat,    handle_fchownat);
	tawcroot_dispatch_install(TAWC_SYS_utimensat,   handle_utimensat);
	tawcroot_dispatch_install(TAWC_SYS_statx,       handle_statx);
	tawcroot_dispatch_install(TAWC_SYS_linkat,      handle_linkat);
	tawcroot_dispatch_install(TAWC_SYS_renameat,    handle_renameat);
	tawcroot_dispatch_install(TAWC_SYS_renameat2,   handle_renameat2);
	tawcroot_dispatch_install(TAWC_SYS_truncate,    handle_truncate);
	tawcroot_dispatch_install(TAWC_SYS_mknodat,     handle_mknodat);
	tawcroot_dispatch_install(TAWC_SYS_statfs,      handle_statfs);

	/* Path-bearing xattr family. f*xattr variants take an fd and stay
	 * untrapped — the kernel's fd-based resolution is already correct.
	 * Most of these will return -EOPNOTSUPP on Android app-private
	 * storage; trapping ensures the failure is against the guest-
	 * visible path rather than a host-relative one. */
	tawcroot_dispatch_install(TAWC_SYS_setxattr,    handle_setxattr);
	tawcroot_dispatch_install(TAWC_SYS_lsetxattr,   handle_lsetxattr);
	tawcroot_dispatch_install(TAWC_SYS_getxattr,    handle_getxattr);
	tawcroot_dispatch_install(TAWC_SYS_lgetxattr,   handle_lgetxattr);
	tawcroot_dispatch_install(TAWC_SYS_listxattr,   handle_listxattr);
	tawcroot_dispatch_install(TAWC_SYS_llistxattr,  handle_llistxattr);
	tawcroot_dispatch_install(TAWC_SYS_removexattr, handle_removexattr);
	tawcroot_dispatch_install(TAWC_SYS_lremovexattr,handle_lremovexattr);

#if defined(__x86_64__)
	/* Legacy x86_64 syscalls — the lp64-`access`-on-x86_64 set that
	 * Android's untrusted_app filter RET_ERRNOs but our TRAP wins. */
	tawcroot_dispatch_install(TAWC_SYS_open,        handle_open_legacy);
	tawcroot_dispatch_install(TAWC_SYS_creat,       handle_creat);
	tawcroot_dispatch_install(TAWC_SYS_utime,       handle_utime);
	tawcroot_dispatch_install(TAWC_SYS_utimes,      handle_utimes);
	tawcroot_dispatch_install(TAWC_SYS_futimesat,   handle_futimesat);
	tawcroot_dispatch_install(TAWC_SYS_stat,        handle_stat);
	tawcroot_dispatch_install(TAWC_SYS_lstat,       handle_lstat);
	tawcroot_dispatch_install(TAWC_SYS_access,      handle_access);
	tawcroot_dispatch_install(TAWC_SYS_readlink,    handle_readlink);
	tawcroot_dispatch_install(TAWC_SYS_chmod,       handle_chmod);
	tawcroot_dispatch_install(TAWC_SYS_chown,       handle_chown_legacy);
	tawcroot_dispatch_install(TAWC_SYS_lchown,      handle_chown_legacy);
	tawcroot_dispatch_install(TAWC_SYS_mkdir,       handle_mkdir);
	tawcroot_dispatch_install(TAWC_SYS_rmdir,       handle_rmdir);
	tawcroot_dispatch_install(TAWC_SYS_unlink,      handle_unlink);
	tawcroot_dispatch_install(TAWC_SYS_link,        handle_link_legacy);
	tawcroot_dispatch_install(TAWC_SYS_symlink,     handle_symlink_legacy);
	tawcroot_dispatch_install(TAWC_SYS_rename,      handle_rename_legacy);
	tawcroot_dispatch_install(TAWC_SYS_mknod,       handle_mknod);
#endif
}
