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
 * (see tawcroot_fs_translate_at) so resolution honours the dirfd's
 * inode — needed for gpg/pacman-key-style openat(homedir_fd, "name").
 * Relative paths containing `..` are first lifted to guest-absolute
 * via /proc/self/fd/<dirfd> so the rootfs prefix clamps the escape.
 *
 * This file owns the shared translate helpers (syscalls_fs_internal.h)
 * and the open/access/metadata handlers. Siblings: syscalls_stat.c
 * (stat family), syscalls_link.c (linkat/renameat), syscalls_xattr.c,
 * syscalls_fs_legacy.c (x86_64 non-*at aliases).
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
#include "rescue.h"
#include "shm.h"
#include "syscalls_fs.h"
#include "syscalls_fs_internal.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Callers fetch the guest string once (tawcroot_fs_fetch_path) and
 * classify that copy — the same bytes later reach the translator, so a
 * racing guest thread can't swap the path between classification and
 * use. */
int tawcroot_fs_classify_shm(const char *local_path, const char **name_out)
{
	*name_out = tawcroot_shm_match(local_path);
	if (*name_out) return SHM_PEEK_NAME;
	if (tawcroot_shm_is_dir(local_path)) return SHM_PEEK_DIR;
	return SHM_PEEK_NONE;
}

/* Classify an already-fetched guest path as a /proc shadow, retrying
 * through fd-relative composition when the absolute form missed —
 * without it, fstatat(proc_dirfd, "version", ...) still reaches the
 * denied inode. `path` is the fetched copy (scratch->buf[0]); the
 * composed form lands in scratch->buf[2], untouched by
 * tawcroot_fs_translate_local (which uses slots 0 and 1). Returns a
 * TAWCROOT_PROC_SHADOW_* kind.
 *
 * One helper, four call sites (openat + the three metadata handlers),
 * so the open and metadata surfaces cannot drift on the fd-relative
 * form the way they once drifted on the absolute one. */
int tawcroot_fs_proc_shadow_classify_at(struct tawcroot_path_scratch *scratch,
					int dirfd, const char *path)
{
	int kind = tawcroot_proc_shadow_classify(path);
	if (kind != TAWCROOT_PROC_SHADOW_NONE) return kind;
	if (dirfd == AT_FDCWD || path[0] == '/' ||
	    !tawcroot_could_be_proc_relative(path))
		return TAWCROOT_PROC_SHADOW_NONE;
	char *composed = scratch->buf[2];
	if (tawcroot_compose_fd_relative(dirfd, path, composed,
					 TAWCROOT_PATH_SCRATCH_SIZE) <= 0)
		return TAWCROOT_PROC_SHADOW_NONE;
	return tawcroot_proc_shadow_classify(composed);
}

long tawcroot_fs_fetch_path(struct tawcroot_path_scratch *scratch, int slot,
			    const char *guest_path)
{
	long n = tawc_copy_string_from_guest(scratch->buf[slot],
					     TAWCROOT_PATH_SCRATCH_SIZE,
					     guest_path);
	return n < 0 ? n : 0;
}

/* Peeks a single
 * byte — tawc_copy_string_from_guest with a tiny cap would return
 * -ENAMETOOLONG for any real path, mis-firing on legitimate input. */
long tawcroot_fs_path_is_empty(const char *gpath)
{
	if (!gpath) return 1;
	char first = -1;
	long pe = tawc_copy_from_guest(&first, 1, gpath);
	if (pe < 0) return pe;
	return first == 0;
}

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

/* See syscalls_fs.h. Non-static so the cleat unit table can exercise
 * it directly. */
tawcroot_path_intent tawcroot_openat_intent(int flags)
{
	if (flags & O_PATH) return TAWCROOT_PATH_INTENT_READ;
	if (flags & O_CREAT) return TAWCROOT_PATH_INTENT_CREATE;
	if ((flags & O_ACCMODE) != O_RDONLY)
		return TAWCROOT_PATH_INTENT_WRITE;
	if (flags & O_TRUNC)
		return TAWCROOT_PATH_INTENT_WRITE;
	return TAWCROOT_PATH_INTENT_READ;
}

/* ---- Read-only binds, stage 2: fd-based metadata residue ----------
 *
 * Path-layer enforcement (the central check in path_orchestrate.c,
 * plus its impure companions in tawcroot_path_translate — the /proc
 * magic-link check and the missing-target errno fidelity, see path.c)
 * and the kernel's fd-access-mode backstop cover every write spelled
 * as an in-view path. What's left is syscalls that mutate METADATA
 * through an fd legitimately opened read-only through an RO bind
 * (fchmod/fchown/futimens/f*xattr). All cold; the verdict is kernel
 * ground truth at call time — readlink /proc/self/fd/<n>, longest-
 * prefix-match against the RO bind srcs — no taint table, nothing to
 * propagate, works for inherited and SCM_RIGHTS-passed fds. */

/* 1 iff `fd`'s kernel-side path lies in an RO part of the view.
 * `opath_passes`: the callers whose raw syscall EBADFs an O_PATH fd
 * (fchmod/fchown/f*xattr) pass 1 so the kernel produces that errno
 * instead of a wrong EROFS; utimensat(fd, NULL) is the one metadata
 * write that legitimately operates on O_PATH fds and passes 0. */
int tawcroot_fs_fd_in_ro_bind(int fd, int opath_passes)
{
	if (!tawcroot_view_has_ro()) return 0;
	if (opath_passes) {
		long fl = tawc_fcntl(fd, F_GETFL, 0);
		if (fl >= 0 && (fl & O_PATH)) return 0;
	}
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *hostp = scratch->buf[0];
	long n = tawcroot_proc_fd_to_host_path(fd, hostp,
					       TAWCROOT_PATH_SCRATCH_SIZE);
	if (n <= 0) return 0;
	return tawcroot_host_path_in_ro_bind(hostp, (size_t)n);
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
	long fe = tawcroot_fs_fetch_path(scratch, 0, gpath);
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
	 * /proc/stat, /proc/version, /proc/uptime, /proc/loadavg: same
	 * SELinux story, different casualties — procps needs `btime`,
	 * LibreOffice's oosplash hard-exits when /proc/version is missing,
	 * `uptime`/`w`/`top` want the other two. See the synthesizers.
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
	 * Fd-relative form (tawcroot_fs_proc_shadow_classify_at):
	 * openat(proc_dir_fd, "self/maps", ...) or openat(proc_dir_fd, "sys/kernel/overflowuid",
	 * ...). One extra readlinkat per non-AT_FDCWD relative O_RDONLY-ish
	 * open; only fires when the absolute classify didn't match. */
	if ((flags & O_ACCMODE) == O_RDONLY &&
	    (flags & O_DIRECTORY) == 0 &&
	    (flags & O_PATH) == 0) {
		int kind = tawcroot_fs_proc_shadow_classify_at(scratch, dirfd,
								path);
		if (kind != TAWCROOT_PROC_SHADOW_NONE) {
			long shadow = tawcroot_proc_shadow_open(kind);
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
	if (tawcroot_fs_classify_shm(path, &shm_name) == SHM_PEEK_NAME)
		return tawcroot_shm_open(shm_name, flags, mode);
	/* SHM_PEEK_DIR (open of /dev/shm itself) falls through; the
	 * translator returns -ENOENT, matching what a host with no
	 * /dev/shm dir would do. */

	/* Narrower-mode reopen of our own memfd via /proc/self/fd/<n>:
	 * SELinux denies it; see the retry below. Parsed before
	 * translation reuses the scratch. */
	int self_fd = -1;
	if ((flags & O_ACCMODE) != O_RDWR && tawc_starts_with(path, "/proc/"))
		self_fd = tawcroot_proc_self_fd_num(path);

	struct fs_path t;
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd,
					     openat_mode(flags),
					     tawcroot_openat_intent(flags), &t);

	/* O_CREAT-on-existing-file fidelity: POSIX/Linux allow
	 * open(existing, O_RDONLY|O_CREAT) on an RO fs — the create is a
	 * no-op when the file exists. The classifier calls any O_CREAT a
	 * CREATE (refused like a write), so the central check EROFSes it;
	 * retry with O_CREAT dropped and READ intent when the flags are a
	 * write-free accmode without O_EXCL/O_TRUNC. A resulting ENOENT
	 * (nothing to open — the create WOULD have been needed) is
	 * rewritten to EROFS below, matching the kernel's create-on-RO-fs
	 * answer. The same retry also covers a magic-link EROFS from
	 * ro_check_proc_magic_link: O_RDONLY|O_CREAT on /proc/self/fd/<n>
	 * legitimately re-opens read-only once O_CREAT is dropped. */
	int erofs_creat_retry = 0;
	if (e == TAWC_EROFS && (flags & O_CREAT) && !(flags & O_EXCL) &&
	    (flags & O_ACCMODE) == O_RDONLY && !(flags & O_TRUNC)) {
		erofs_creat_retry = 1;
		flags &= ~O_CREAT;
		e = tawcroot_fs_translate_local(scratch, 0, dirfd,
						openat_mode(flags),
						TAWCROOT_PATH_INTENT_READ, &t);
		if (e == TAWC_ENOENT) return TAWC_EROFS;
	}
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

	/* Android SELinux denies `open` on the app's memfds, so a guest
	 * can't get a read-only description of its own memfd (Firefox's
	 * HaveMemfd probe and freezable regions). Move a still-private
	 * memfd to a file and retry (shm.h). */
	if (fd == TAWC_EACCES && self_fd >= 0 &&
	    tawcroot_shm_migrate_guest_memfd(self_fd) == 0)
		fd = tawc_openat(t.fd, p, flags, mode);

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
	/* See the O_CREAT retry above: the leaf turned out not to exist,
	 * so the create the guest asked for would have been real — EROFS. */
	if (erofs_creat_retry && fd == TAWC_ENOENT) return TAWC_EROFS;
	return fd;
}

/* Fetch a guest-pointer path string into a stack-local buffer through
 * the EFAULT-safe usercopy helper, then translate. This is the front
 * door for every path-bearing handler — the path string is fetched
 * ONCE into scratch->buf[slot] and every later step (shm / proc-shadow
 * classification via tawcroot_fs_translate_local callers, translation,
 * *at issue)
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
 * tawcroot_fs_translate_at: the path is lifted to guest-absolute via
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
long tawcroot_fs_translate_local(struct tawcroot_path_scratch *scratch,
				 int slot, int dirfd, tawcroot_path_mode mode,
				 tawcroot_path_intent intent,
				 struct fs_path *out)
{
	char  *path_buf   = scratch->buf[slot];
	char  *suffix     = scratch->buf[slot + 1];
	size_t suffix_cap = TAWCROOT_PATH_SCRATCH_SIZE;
	out->path = suffix;
	out->ro   = 0;

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
							suffix_cap, mode,
							intent);
					if (r.err) return r.err;
					out->fd      = r.base_fd;
					out->is_root = (suffix[0] == 0);
					out->ro      = r.ro;
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
			/* The one route the translator never sees, so record
			 * it for the DAC-override rescue here. */
			tawcroot_rescue_note(dirfd, suffix);
			return 0;
		}
		/* Absolute path: dirfd is ignored by the kernel; fall
		 * through to the normal translation. */
	}

	tawcroot_path_result r =
		tawcroot_path_translate(path_buf, suffix, suffix_cap, mode,
					intent);
	if (r.err) return r.err;
	out->fd      = r.base_fd;
	out->is_root = (suffix[0] == 0);
	out->ro      = r.ro;
	return 0;
}

long tawcroot_fs_translate_at(struct tawcroot_path_scratch *scratch, int slot,
			      int dirfd, const char *guest_path,
			      tawcroot_path_mode mode,
			      tawcroot_path_intent intent, struct fs_path *out)
{
	long n = tawc_copy_string_from_guest(scratch->buf[slot],
					     TAWCROOT_PATH_SCRATCH_SIZE,
					     guest_path);
	if (n < 0) return n;
	return tawcroot_fs_translate_local(scratch, slot, dirfd, mode, intent,
					   out);
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
	long fe = tawcroot_fs_fetch_path(scratch, 0, gpath);
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
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd,
					     TAWCROOT_PATH_NOFOLLOW,
					     TAWCROOT_PATH_INTENT_READ, &t);
	if (e) return e;
	/* When the guest path resolves exactly to a bind dst (or rootfs root),
	 * tawcroot_fs_translate_at gives us (reserved_dir_fd, ""). The kernel
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
	 * before forwarding it. We reuse tawcroot_fs_translate_at's path
	 * buffer (dead from here on) as the scratch — a fresh PATH_MAX buffer would
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
	long fe = tawcroot_fs_fetch_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = tawcroot_fs_classify_shm(scratch->buf[0], &shm_name);
		if (kind == SHM_PEEK_NAME) return tawcroot_shm_access_name(shm_name);
		if (kind == SHM_PEEK_DIR)  return tawcroot_shm_access_dir();
	}

	/* /proc shadows — see the same block in handle_newfstatat. The
	 * shadows are 0444 regular files, so R_OK/F_OK pass and any
	 * W_OK/X_OK bit is EACCES. */
	{
		int kind = tawcroot_fs_proc_shadow_classify_at(scratch, dirfd,
							       scratch->buf[0]);
		if (kind != TAWCROOT_PROC_SHADOW_NONE)
			return tawcroot_proc_shadow_access(kind, mode);
	}

	/* W_OK probes declare write intent: the kernel answers EROFS for
	 * access(W_OK) on a read-only fs, which is exactly what the
	 * central check produces. R_OK/X_OK/F_OK observe only. */
	struct fs_path t;
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd,
					     TAWCROOT_PATH_FOLLOW,
					     (mode & 2 /*W_OK*/)
					 ? TAWCROOT_PATH_INTENT_WRITE
					 : TAWCROOT_PATH_INTENT_READ,
					     &t);
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
	long e = tawcroot_fs_translate_at(scratch, 0, AT_FDCWD, gpath,
					  TAWCROOT_PATH_FOLLOW,
					  TAWCROOT_PATH_INTENT_READ, &t);
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

#define DECLARE_AT_PASS(name, sysnr, narg, pmode, pintent)                \
static long handle_##name(const tawcroot_syscall_args *args,             \
			  ucontext_t *uc)                                 \
{                                                                         \
	(void)uc;                                                         \
	int dirfd = (int)args->a;                                         \
	const char *gpath = (const char *)(uintptr_t)args->b;             \
	if (!gpath) return TAWC_EFAULT;                                       \
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);                                  \
	struct fs_path t;                                                     \
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,         \
					  pmode, pintent, &t);                \
	if (e) return e;                                                  \
	const char *p = t.is_root ? "." : t.path;                         \
	return TAWC_RAW(sysnr, t.fd, (long)p,                             \
			args->c, args->d,                                 \
			(narg) > 4 ? args->e : 0,                         \
			(narg) > 5 ? args->f : 0);                        \
}

DECLARE_AT_PASS(mkdirat,    TAWC_SYS_mkdirat,    3, TAWCROOT_PATH_PARENT_CREATE,
		TAWCROOT_PATH_INTENT_WRITE)

/* mknodat: translate and attempt the host call. FIFOs, sockets and
 * regular files succeed as the app uid; S_IFCHR/S_IFBLK needs
 * CAP_MKNOD, which untrusted_app never has, so device nodes get EPERM
 * in production (rooted test environments succeed and can't see this).
 * The concrete hit is distro postinst/makedev tooling and debootstrap
 * second stage populating /dev — a raw EPERM aborts package installs.
 *
 * Under virtual euid 0, degrade a refused device mknod to an empty
 * regular file at the name (proot's fake_id0 behaviour): the script
 * proceeds, later opens of the fake node fail at use time. A regular
 * file, not a FIFO — opening a peerless FIFO blocks, a regular file
 * fails benignly. If even the placeholder is refused (guest /dev is a
 * bind of host /dev, where the app can't create anything), report
 * success with nothing created — same swallow contract as fchmodat/
 * fchownat; common /dev names exist on the host side and return EEXIST
 * before reaching this point. Known limit: overlay-style whiteouts
 * (char 0:0) become plain files. Non-permission errors (EEXIST,
 * ENOENT, EROFS, ...) pass through from whichever attempt raised them. */
static long handle_mknodat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	unsigned int mode = (unsigned int)args->c;
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,
					  TAWCROOT_PATH_PARENT_CREATE,
					  TAWCROOT_PATH_INTENT_WRITE, &t);
	if (e) return e;
	const char *p = t.is_root ? "." : t.path;
	long rv = TAWC_RAW(TAWC_SYS_mknodat, t.fd, (long)p,
			   args->c, args->d, 0, 0);
	unsigned int ifmt = mode & S_IFMT;
	if ((rv != TAWC_EPERM && rv != TAWC_EACCES) ||
	    (ifmt != S_IFCHR && ifmt != S_IFBLK) ||
	    tawcroot_identity_euid() != 0)
		return rv;
	long fd = TAWC_RAW(TAWC_SYS_openat, t.fd, (long)p,
			   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
			   mode & 07777, 0, 0);
	if (fd >= 0) {
		TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
		return 0;
	}
	if (fd == TAWC_EPERM || fd == TAWC_EACCES)
		return 0;
	return fd;
}

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
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,
					  TAWCROOT_PATH_FOLLOW,
					  TAWCROOT_PATH_INTENT_WRITE, &t);
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
	long fe = tawcroot_fs_fetch_path(scratch, 0, gpath);
	if (fe) return fe;

	{
		const char *shm_name;
		int kind = tawcroot_fs_classify_shm(scratch->buf[0], &shm_name);
		if (kind == SHM_PEEK_NAME) {
			if (flag & AT_REMOVEDIR) return TAWC_ENOTDIR;
			return tawcroot_shm_unlink(shm_name);
		}
		if (kind == SHM_PEEK_DIR)
			return (flag & AT_REMOVEDIR) ? TAWC_EBUSY : TAWC_EISDIR;
	}

	struct fs_path t;
	long e = tawcroot_fs_translate_local(scratch, 0, dirfd,
					     TAWCROOT_PATH_PARENT_REMOVE,
					     TAWCROOT_PATH_INTENT_WRITE, &t);
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
		 * (fdtab.h contract), like every other dirfd anchor. An
		 * RO-bind fd is a metadata write through the fd: EROFS
		 * (stage 2). */
		if (tawcroot_fd_is_reserved(dirfd)) return TAWC_EBADF;
		if (tawcroot_fs_fd_in_ro_bind(dirfd, 0)) return TAWC_EROFS;
		return TAWC_RAW(TAWC_SYS_utimensat, dirfd, 0,
				args->c, flags, 0, 0);
	}
	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,
					  pmode, TAWCROOT_PATH_INTENT_WRITE, &t);
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
		long empty = tawcroot_fs_path_is_empty(gpath);
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
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,
					  pmode, TAWCROOT_PATH_INTENT_WRITE, &t);
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
	/* RO-bind fd (stage 2): the host fs is writable, so without this
	 * the chmod would succeed straight through the bind src. EROFS
	 * before the fake-root swallow — root gets EROFS on a real RO fs
	 * too. */
	if (tawcroot_fs_fd_in_ro_bind(fd, 1)) return TAWC_EROFS;
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
	/* RO-bind fd (stage 2): EROFS on BOTH branches — the fake-root
	 * branch must not fake success (root chowning on a real RO fs
	 * gets EROFS), and the forwarding branch must not write through
	 * the host-RW bind src. */
	if (tawcroot_fs_fd_in_ro_bind(fd, 1)) return TAWC_EROFS;
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
	long e = tawcroot_fs_translate_at(scratch, 0, newdirfd, linkpath,
					  TAWCROOT_PATH_PARENT_CREATE,
					  TAWCROOT_PATH_INTENT_WRITE, &t);
	if (e) return e;
	if (t.is_root) return TAWC_EINVAL; /* can't create / */
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)tgt, t.fd,
			(long)t.path, 0, 0, 0);
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
	long e = tawcroot_fs_translate_at(scratch, 0, AT_FDCWD, gpath,
					  pmode, TAWCROOT_PATH_INTENT_READ, &t);
	if (e) return e;
	char *host_path = scratch->buf[2];
	long bp = tawc_proc_fd_path(host_path, TAWCROOT_PATH_SCRATCH_SIZE,
				    t.fd, t.path);
	if (bp < 0) return bp;
	return TAWC_RAW(TAWC_SYS_inotify_add_watch, inotify_fd,
			(long)host_path, mask, 0, 0, 0);
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
	long e = tawcroot_fs_translate_at(scratch, 0, AT_FDCWD, gpath,
					  TAWCROOT_PATH_FOLLOW,
					  TAWCROOT_PATH_INTENT_WRITE, &t);
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

/* statfs(path, buf): translate path, then dispatch using a /proc/self/
 * fd-anchored path. statfs has no fd-relative variant; we could `openat
 * O_PATH` + fstatfs, but on Android-shipped 5.4 kernels fstatfs against
 * an O_PATH fd returns -EBADF. The /proc/self/fd path gets the same
 * effect (kernel resolves through the fd) with no version surprises. */
/* LP64 kernel `struct statfs` mirror (asm-generic/statfs.h; identical
 * on aarch64 and x86_64 — every field is 8 bytes). Local so the
 * ST_RDONLY decoration below can stage the kernel result without
 * pulling <sys/statfs.h> into the freestanding build. */
struct tawc_statfs64 {
	long f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	int  f_fsid[2];
	long f_namelen, f_frsize, f_flags, f_spare[4];
};
#define TAWC_ST_RDONLY 0x0001

static long handle_statfs(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	void       *out   = (void *)(uintptr_t)args->b;
	if (!gpath || !out) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	struct fs_path t;
	long e = tawcroot_fs_translate_at(scratch, 0, AT_FDCWD, gpath,
					  TAWCROOT_PATH_FOLLOW,
					  TAWCROOT_PATH_INTENT_READ, &t);
	if (e) return e;
	char *host_path = scratch->buf[2];
	long bp = tawc_proc_fd_path(host_path, TAWCROOT_PATH_SCRATCH_SIZE,
				    t.fd, t.path);
	if (bp < 0) return bp;
	/* Stage the kernel result locally so an RO route can OR
	 * ST_RDONLY into f_flags — `df`, pacman's free-space check, and
	 * RO-detecting installers then see the truth instead of the host
	 * mount's RW answer. Cost vs. the old direct-to-guest write: one
	 * copy_to_guest round-trip on a cold syscall. */
	struct tawc_statfs64 local;
	long rv = TAWC_RAW(TAWC_SYS_statfs, (long)host_path, (long)&local,
			   0, 0, 0, 0);
	if (rv != 0) return rv;
	if (t.ro) local.f_flags |= TAWC_ST_RDONLY;
	long ce = tawc_copy_to_guest(out, &local, sizeof local);
	return ce < 0 ? ce : 0;
}

void tawcroot_fs_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_openat,      handle_openat);
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
	tawcroot_dispatch_install(TAWC_SYS_truncate,    handle_truncate);
	tawcroot_dispatch_install(TAWC_SYS_mknodat,     handle_mknodat);
	tawcroot_dispatch_install(TAWC_SYS_statfs,      handle_statfs);

	tawcroot_fs_stat_register();
	tawcroot_fs_link_register();
	tawcroot_fs_xattr_register();
#if defined(__x86_64__)
	tawcroot_fs_legacy_register();
#endif
}
