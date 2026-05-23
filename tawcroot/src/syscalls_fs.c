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
 * (see fetch_and_translate_at) so resolution honours the dirfd's
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
#include "io.h"
#include "path.h"
#include "path_scratch.h"
#include "proc_rewrite.h"
#include "raw_sys.h"
#include "shm.h"
#include "syscalls_fs.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Peek the guest path to classify a possible `/dev/shm` intercept.
 *   0 = not shm (fall through to normal translation)
 *   1 = `/dev/shm/<name>` (writes name pointer into `*name_out`,
 *       valid for the lifetime of `buf`)
 *   2 = `/dev/shm` directory itself
 * `buf` is caller-owned scratch (320 bytes covers `/dev/shm/` +
 * 255-byte POSIX shm name with headroom; longer paths can't match). */
enum { SHM_PEEK_NONE = 0, SHM_PEEK_NAME = 1, SHM_PEEK_DIR = 2 };
static int peek_shm(const char *gpath, char *buf, size_t cap,
		    const char **name_out)
{
	if (!gpath) return SHM_PEEK_NONE;
	long sp = tawc_copy_string_from_guest(buf, cap, gpath);
	if (sp < 0) return SHM_PEEK_NONE;
	const char *n = tawcroot_shm_match(buf);
	if (n) { *name_out = n; return SHM_PEEK_NAME; }
	if (tawcroot_shm_is_dir(buf)) return SHM_PEEK_DIR;
	return SHM_PEEK_NONE;
}

/* Forward decls — bodies later in the file. */
static long fetch_and_translate(const char *guest_path,
				char *path_buf, size_t path_cap,
				char *suffix,   size_t suffix_cap,
				int  *base_fd_out, int *use_empty_path,
				tawcroot_path_mode mode);
static long fetch_and_translate_at(int dirfd, const char *guest_path,
				   char *path_buf, size_t path_cap,
				   char *suffix,   size_t suffix_cap,
				   int  *base_fd_out, int *use_empty_path,
				   tawcroot_path_mode mode);
static void decorate_stat(struct stat *st);

/* Pick the resolution mode for an openat based on the kernel flags.
 * O_NOFOLLOW + O_PATH means "don't follow the leaf even if it's a
 * symlink"; O_CREAT means the leaf may not exist. Both fall under
 * NOFOLLOW / PARENT_CREATE for memoizer purposes — we don't want a
 * `lstat`-style operation against the SYMLINK to be silently rewritten
 * to its target. (O_NOFOLLOW / O_CREAT are arch-specific bits — pulled
 * from <linux/fcntl.h> at the top of this file.) */
static tawcroot_path_mode openat_mode(int flags)
{
	if (flags & O_NOFOLLOW) return TAWCROOT_PATH_NOFOLLOW;
	if (flags & O_CREAT)    return TAWCROOT_PATH_PARENT_CREATE;
	return TAWCROOT_PATH_FOLLOW;
}

/* Forward decls — bodies live below, near the other /proc/self
 * helpers (is_proc_self_exe etc.). */
static int         is_proc_self_maps(const char *path);
static int         is_proc_self_exe(const char *path);
static const char *match_proc_sys_overflow_id(const char *path);
static int         is_proc_bus_pci_devices(const char *path);
static long        open_proc_maps_shadow(void);
static long        open_proc_overflow_id_shadow(const char *memfd_name);
static long        open_proc_bus_pci_devices_shadow(void);
static int         try_proc_shadow(const char *path, long *out);
static long        compose_fd_relative(int dirfd, const char *gpath_str,
				       char *out, size_t cap);

/* Fast-out: does this guest-relative leaf even have a chance of
 * composing into a /proc/<x> path that we shadow? Legitimate first chars
 * are {b,s,t,m,e,digit} — covering "bus/" (pci/devices), "self/", "sys/",
 * "task/", "maps", "exe", and numeric "<tid>/" forms. Anything else (the
 * vast majority of fd-relative opens — config files, dotfiles, library
 * names, etc.) skips the readlinkat. */
static int could_be_proc_relative(const char *p)
{
	char c = p[0];
	return c == 'b' || c == 's' || c == 't' || c == 'm' || c == 'e' ||
	       (c >= '0' && c <= '9');
}

static long handle_openat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flags = (int)args->c;
	int mode  = (int)args->d;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

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
	 * fallback. See notes/tawcroot.md "More /proc reverse-translation
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
	 * the conventional -ENOTDIR / O_PATH-fd behavior. The 64-byte peek
	 * is wide enough for "/proc/<10-digit-pid>/task/<10-digit-tid>/maps"
	 * and for "/proc/sys/kernel/overflowuid"; longer paths can't match.
	 *
	 * Fd-relative form: openat(proc_dir_fd, "self/maps", ...) or
	 * openat(proc_dir_fd, "sys/kernel/overflowuid", ...). We resolve
	 * dirfd via /proc/self/fd/<n>, join with the guest path, and
	 * re-classify. One extra readlinkat per non-AT_FDCWD relative
	 * O_RDONLY-ish open; only fires when the absolute peek didn't match. */
	if (gpath &&
	    (flags & 3) == 0 /*O_RDONLY*/ &&
	    (flags & O_DIRECTORY) == 0 &&
	    (flags & O_PATH) == 0) {
		char tmp[64];
		long pn = tawc_copy_string_from_guest(tmp, sizeof tmp, gpath);
		if (pn >= 0) {
			long shadow;
			if (try_proc_shadow(tmp, &shadow))
				return shadow;
			if (dirfd != AT_FDCWD && tmp[0] != '/' &&
			    could_be_proc_relative(tmp)) {
				char *composed = scratch->buf[2];
				if (compose_fd_relative(dirfd, tmp,
							composed,
							TAWCROOT_PATH_SCRATCH_SIZE) > 0 &&
				    try_proc_shadow(composed, &shadow))
					return shadow;
			}
		}
	}

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(gpath, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME)
			return tawcroot_shm_open(shm_name, flags, mode);
		/* SHM_PEEK_DIR (open of /dev/shm itself) falls through; the
		 * translator returns -ENOENT, matching what a host with no
		 * /dev/shm dir would do. */
	}

	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty,
					openat_mode(flags));
	if (e) return e;

	/* Empty suffix → guest asked for "/" or for a bind dst's root.
	 * Pass "." so the kernel resolves it to the dir base_fd points at;
	 * this works on every kernel we target (AT_EMPTY_PATH on openat
	 * only landed in 6.6, kernel 5.4 device doesn't have it). */
	const char *p = use_empty ? "." : suffix;

	/* Plain openat — the kernel chases a leaf symlink against the
	 * process's actual fs root. Non-leaf in-rootfs symlinks are
	 * pre-folded by tawcroot_path_resolve_symlinks during translate
	 * (path_orchestrate.c), so by here `suffix` no longer contains
	 * unresolved rootfs-side directory components.
	 *
	 * We don't use openat2 with RESOLVE_IN_ROOT: that would clamp
	 * absolute symlink targets at base_fd, but when base_fd is a
	 * bind src dirfd a leaf symlink whose target points into a
	 * *different* bind (e.g. /system/lib64/libc.so →
	 * /apex/com.android.runtime/lib64/bionic/libc.so on Android)
	 * needs the kernel to follow through the host root, not the
	 * bind src. See test_prod_rootfs.c::prod_rootfs_cross_bind_abs_symlink
	 * and notes/tawcroot.md "Cross-bind absolute symlinks". */
	return tawc_openat(base_fd, p, flags, mode);
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
	 * gpath==0; the gpath="" case fell through to fetch_and_translate
	 * which then translates the empty string against the kernel cwd
	 * and stats the wrong inode (cwd dir instead of dirfd). wc, which
	 * uses fstat() on its input fd to decide buffer strategy, then read
	 * stale size and segfaulted. We pass through as-is (the dirfd is
	 * the guest's, but we don't yet track guest fd
	 * provenance — the kernel will see whatever fd it is and stat the
	 * corresponding inode). */
	if (flags & AT_EMPTY_PATH) {
		int empty = (gpath == 0);
		if (!empty) {
			/* Peek the first byte. tawc_copy_string_from_guest
			 * with a small cap returns -ENAMETOOLONG for any
			 * non-empty path, which would mis-fire on real paths;
			 * use the raw byte copy instead. */
			char first = -1;
			long pe = tawc_copy_from_guest(&first, 1, gpath);
			if (pe < 0) return pe;     /* -EFAULT */
			if (first == 0) empty = 1;
		}
		if (empty) {
			long rv = TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)"",
					   (long)&local, flags, 0, 0);
			if (rv != 0) return rv;
			decorate_stat(&local);
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return rv;
		}
	}

	if (!gpath) return TAWC_EFAULT;

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(gpath, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME || kind == SHM_PEEK_DIR) {
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

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty, pmode);
	if (e) return e;

	const char *resolved = suffix;
	int         rv_flags = flags;
	if (use_empty) {
		resolved = "";
		rv_flags |= AT_EMPTY_PATH;
	}

	long rv = TAWC_RAW(TAWC_SYS_fstatat, base_fd, (long)resolved,
			   (long)&local, rv_flags, 0, 0);
	if (rv != 0) return rv;
	decorate_stat(&local);
	long ce = tawc_copy_to_guest(out, &local, sizeof local);
	if (ce < 0) return ce;
	return rv;
}

/* Fetch a guest-pointer path string into a stack-local buffer through
 * the EFAULT-safe usercopy helper, then translate. This is the front
 * door for every path-bearing handler — the path string lives in this
 * buffer for the duration of the call, so the guest cannot modify it
 * out from under us between argument-read and *at issue.
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
 * fetch_and_translate_at: the path is lifted to guest-absolute via
 * the dirfd's /proc/self/fd link, then path_translate's fold clamps
 * `..` at the rootfs root. Without that, the kernel walks `..` past
 * the dirfd freely and systemd's path_is_root_at probe (chase.c)
 * misclassifies the rootfs and aborts with
 * `Assertion 'path_is_absolute(p)' failed`. */
static long fetch_and_translate(const char *guest_path,
				char *path_buf, size_t path_cap,
				char *suffix,   size_t suffix_cap,
				int  *base_fd_out, int *use_empty_path,
				tawcroot_path_mode mode)
{
	long n = tawc_copy_string_from_guest(path_buf, path_cap, guest_path);
	if (n < 0) return n;
	tawcroot_path_result r =
		tawcroot_path_translate(path_buf, suffix, suffix_cap, mode);
	if (r.err) return r.err;
	*base_fd_out    = r.base_fd;
	*use_empty_path = (suffix[0] == 0);
	return 0;
}

/* True iff `path` contains a `..` path component. Component-aware: treats
 * "foo..bar" / "..baz" as not-dotdot. */
static int path_has_dotdot_component(const char *path)
{
	const char *p = path;
	while (*p) {
		if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || p[2] == '/'))
			return 1;
		while (*p && *p != '/') p++;
		while (*p == '/') p++;
	}
	return 0;
}

/* Variant that honours the guest's dirfd for fd-relative resolution.
 * See big comment above for why this matters. Returns -1 in
 * `*base_fd_out` and the literal guest-supplied path in `path_buf`
 * (also via the suffix output, kept identical) when the kernel should
 * resolve directly off `dirfd`; the caller then issues the raw *at
 * syscall with `dirfd` and the literal path.
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
static long fetch_and_translate_at(int dirfd, const char *guest_path,
				   char *path_buf, size_t path_cap,
				   char *suffix,   size_t suffix_cap,
				   int  *base_fd_out, int *use_empty_path,
				   tawcroot_path_mode mode)
{
	if (dirfd != AT_FDCWD) {
		long n = tawc_copy_string_from_guest(path_buf, path_cap,
		                                     guest_path);
		if (n < 0) return n;
		if (path_buf[0] != '/') {
			if (path_has_dotdot_component(path_buf)) {
				TAWCROOT_PATH_SCRATCH_AUTO(scratch);
				char *abs = scratch->buf[0];
				long al = tawcroot_fd_to_guest_abs(dirfd, abs,
				                                   TAWCROOT_PATH_SCRATCH_SIZE);
				if (al >= 0) {
					if (abs[al - 1] != '/') {
						if ((size_t)al + 1 >= TAWCROOT_PATH_SCRATCH_SIZE)
							return TAWC_ENAMETOOLONG;
						abs[al++] = '/';
					}
					size_t pi = 0;
					while (path_buf[pi] &&
					       (size_t)al + 1 < TAWCROOT_PATH_SCRATCH_SIZE)
						abs[al++] = path_buf[pi++];
					if (path_buf[pi]) return TAWC_ENAMETOOLONG;
					abs[al] = 0;
					tawcroot_path_result r =
						tawcroot_path_translate(
							abs, suffix,
							suffix_cap, mode);
					if (r.err) return r.err;
					*base_fd_out    = r.base_fd;
					*use_empty_path = (suffix[0] == 0);
					return 0;
				}
				/* Outside-rootfs dirfd (-ENOENT) or readlink
				 * failure: fall through to kernel-resolved
				 * passthrough. The kernel's resolution can't
				 * escape into the rootfs from outside it, so
				 * the leak this branch protects against
				 * doesn't apply. */
			}
			/* Relative path with explicit dirfd: kernel resolves
			 * off the dirfd. Copy path_buf into suffix so callers
			 * with one output buffer still work. */
			size_t i = 0;
			while (path_buf[i] && i + 1 < suffix_cap) {
				suffix[i] = path_buf[i];
				i++;
			}
			if (i + 1 >= suffix_cap) return TAWC_ENAMETOOLONG;
			suffix[i] = '\0';
			*base_fd_out    = dirfd;
			*use_empty_path = (suffix[0] == '\0');
			return 0;
		}
		/* Absolute path: dirfd is ignored by the kernel; fall
		 * through to the normal translation. */
	}
	return fetch_and_translate(guest_path, path_buf, path_cap,
	                           suffix, suffix_cap, base_fd_out,
	                           use_empty_path, mode);
}


/* Return 1 iff `n` names our process: either our TGID (== getpid()) or
 * a TID belonging to it. Validation reads `/proc/<n>/status` and checks
 * the `Tgid:` line. Unknown / non-existent / cross-process TIDs return 0.
 *
 * Not cached: only the /proc/<n>/<x> path-classification call sites
 * reach here, and the common case (n == TGID) short-circuits without
 * any syscall. Per-thread crash dumpers walking every TID is the worst
 * case; if it ever shows up as hot we can stick a small MRU here. */
static int is_my_tid(long n)
{
	if (n <= 0 || n > 0x7fffffff) return 0;
	long mypid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	if (n == mypid) return 1;

	char path[64];
	const char *prefix = "/proc/";
	size_t i = 0;
	while (prefix[i]) { path[i] = prefix[i]; i++; }
	int wrote = tawc_int_to_str(path + i, sizeof path - i, (int)n);
	if (wrote <= 0) return 0;
	i += (size_t)wrote;
	const char *suffix = "/status";
	size_t j = 0;
	while (suffix[j]) {
		if (i + 1 >= sizeof path) return 0;
		path[i++] = suffix[j++];
	}
	path[i] = 0;

	long fd = tawc_openat(AT_FDCWD, path,
			      0 /*O_RDONLY*/ | 0x80000 /*O_CLOEXEC*/, 0);
	if (fd < 0) return 0;
	char buf[512];
	long r = tawc_read((int)fd, buf, sizeof buf - 1);
	tawc_close((int)fd);
	if (r <= 0) return 0;
	buf[r] = 0;

	/* Walk lines looking for "Tgid:". The kernel emits it within the
	 * first ~12 lines, well inside the 511-byte read. Anchor at start
	 * of line so a hypothetical other field whose value happens to
	 * contain the byte sequence "Tgid:" can't fool the scan. */
	const char *p = buf;
	while (*p) {
		if ((p == buf || p[-1] == '\n') &&
		    p[0] == 'T' && p[1] == 'g' && p[2] == 'i' &&
		    p[3] == 'd' && p[4] == ':') {
			p += 5;
			while (*p == ' ' || *p == '\t') p++;
			long parsed = 0;
			while (*p >= '0' && *p <= '9') {
				parsed = parsed * 10 + (*p - '0');
				p++;
			}
			return parsed == mypid;
		}
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;
	}
	return 0;
}

/* If `path` is "/proc/self/<x>" or "/proc/<tid>/<x>" — where <tid> is
 * our TGID or any TID belonging to it — return a pointer to "<x>". Also
 * peels an optional "task/<tid>/" segment after `self/` or `<tid>/`,
 * since /proc/<pid>/task/<tid>/maps is per-mm (identical to .../maps) and
 * /exe is a symlink to the same exe. Returns NULL on no match.
 *
 * Strict on the prefix bytes: paths like "/proc/foo/../self/exe" are
 * caught only after the guest's libc canonicalizes them (the typical
 * flow). */
static const char *strip_proc_self_prefix(const char *path)
{
	if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
	    path[3] != 'o' || path[4] != 'c' || path[5] != '/')
		return 0;
	const char *t = path + 6;
	const char *after_pid;
	if (t[0] == 's' && t[1] == 'e' && t[2] == 'l' && t[3] == 'f' &&
	    t[4] == '/') {
		after_pid = t + 5;
	} else if (t[0] >= '0' && t[0] <= '9') {
		long n = 0;
		const char *p = t;
		while (*p >= '0' && *p <= '9') {
			n = n * 10 + (*p - '0'); p++;
			if (n > 0x7fffffff) return 0;
		}
		if (p[0] != '/') return 0;
		if (!is_my_tid(n)) return 0;
		after_pid = p + 1;
	} else {
		return 0;
	}

	/* Optional "task/<tid>/" peel. On any structural problem with the
	 * inner segment (no digits, overflow, missing trailing slash, or
	 * the tid isn't ours) we leave `after_pid` alone; the caller's
	 * tail-match (e.g. against "maps") then fails and synthesis
	 * doesn't fire. Returning NULL would also be safe but loses the
	 * outer match — and `/proc/self/task/<x>` is never a synthesis
	 * target anyway, so the difference is academic. */
	if (after_pid[0] == 't' && after_pid[1] == 'a' &&
	    after_pid[2] == 's' && after_pid[3] == 'k' &&
	    after_pid[4] == '/') {
		const char *q = after_pid + 5;
		long m = 0;
		const char *p = q;
		while (*p >= '0' && *p <= '9') {
			m = m * 10 + (*p - '0'); p++;
			if (m > 0x7fffffff) return after_pid;
		}
		if (p == q || p[0] != '/') return after_pid;
		if (!is_my_tid(m)) return after_pid;
		return p + 1;
	}
	return after_pid;
}

/* Compose `dirfd`'s host path (resolved via /proc/self/fd/<n>) with a
 * relative guest path into `out`. Used to catch fd-relative /proc/self
 * accesses (e.g. openat(proc_dir_fd, "self/maps", ...)) before
 * strip_proc_self_prefix runs. Returns the composed length on success
 * or -errno. Caller passes the literal guest-supplied relative string;
 * dirfd must be non-negative and != AT_FDCWD. */
static long compose_fd_relative(int dirfd, const char *gpath_str,
				char *out, size_t cap)
{
	if (dirfd < 0 || dirfd == AT_FDCWD) return TAWC_EINVAL;
	if (gpath_str[0] == '/') return TAWC_EINVAL;

	long n = tawcroot_proc_fd_to_host_path(dirfd, out, cap);
	if (n < 0) return n;
	size_t dl = (size_t)n;
	if (out[dl - 1] != '/') {
		if (dl + 1 >= cap) return TAWC_ENAMETOOLONG;
		out[dl++] = '/';
	}
	size_t gi = 0;
	while (gpath_str[gi]) {
		if (dl + 1 >= cap) return TAWC_ENAMETOOLONG;
		out[dl++] = gpath_str[gi++];
	}
	out[dl] = 0;
	return (long)dl;
}

static int is_proc_self_exe(const char *path)
{
	const char *tail = strip_proc_self_prefix(path);
	return tail && tawc_streq(tail, "exe");
}

static int is_proc_self_maps(const char *path)
{
	const char *tail = strip_proc_self_prefix(path);
	return tail && tawc_streq(tail, "maps");
}

/* Match the /proc/sys/kernel/overflow{uid,gid} pair. Returns the memfd
 * label on a hit (the suffix is the only thing that varies between the
 * two), NULL otherwise. The label is what shows up under
 * /proc/self/fd/<fd> as "/memfd:tawcroot-overflow{uid,gid} (deleted)" —
 * useful when reading the diagnostic output of a guest that strerror()s
 * its way through bwrap. */
static const char *match_proc_sys_overflow_id(const char *path)
{
	if (tawc_streq(path, "/proc/sys/kernel/overflowuid"))
		return "tawcroot-overflowuid";
	if (tawc_streq(path, "/proc/sys/kernel/overflowgid"))
		return "tawcroot-overflowgid";
	return NULL;
}

/* Match the single file libpci's procfs back-end opens to enumerate
 * PCI devices. The directory itself (`/proc/bus/pci`) and per-bus
 * subdirs are not matched — guests that want to walk them get the
 * kernel's normal -EACCES, same as today. */
static int is_proc_bus_pci_devices(const char *path)
{
	return tawc_streq(path, "/proc/bus/pci/devices");
}

/* /proc/self/maps shadow fd. Read the kernel's maps file in full,
 * reverse-translate each path field via the rootfs/bind tables, and
 * write the result into a memfd that we hand back to the guest.
 *
 * Sized for typical desktop workloads: 1 MB read buffer covers Firefox's
 * ~5–10K mappings comfortably; output may be slightly larger or smaller
 * depending on whether reverse-translation lengthens or shortens paths,
 * so the output buffer matches the read buffer's ceiling. Pathological
 * maps (>1 MB) are truncated — same behavior as a guest with a small
 * read buffer would see, just at our boundary instead of theirs.
 *
 * All allocations are anonymous mmaps (not on the SIGSYS handler's tiny
 * stack) and freed before return. memfd_create needs no privileges and
 * works on every kernel we target (≥ 3.17). */
#define MAPS_BUF_SIZE  ((size_t)1 << 20)

static long open_proc_maps_shadow(void)
{
	long region = tawc_mmap(0, 2 * MAPS_BUF_SIZE,
				3 /*PROT_READ|PROT_WRITE*/,
				0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/,
				-1, 0);
	if (region < 0 && region > -4096) return region;
	if (region == 0) return TAWC_ENOMEM; /* defensive */
	char *in_buf  = (char *)(uintptr_t)region;
	char *out_buf = in_buf + MAPS_BUF_SIZE;

	long src = tawc_openat(AT_FDCWD, "/proc/self/maps",
			       0 /*O_RDONLY*/ | 0x80000 /*O_CLOEXEC*/, 0);
	if (src < 0) {
		(void)tawc_munmap((void *)(uintptr_t)region, 2 * MAPS_BUF_SIZE);
		return src;
	}

	size_t in_len = 0;
	while (in_len < MAPS_BUF_SIZE) {
		long n = tawc_read((int)src, in_buf + in_len,
				   MAPS_BUF_SIZE - in_len);
		if (n == 0) break;
		if (n < 0) {
			tawc_close((int)src);
			(void)tawc_munmap((void *)(uintptr_t)region,
					  2 * MAPS_BUF_SIZE);
			return n;
		}
		in_len += (size_t)n;
	}
	tawc_close((int)src);

	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = tawcroot_rootfs_host_path,
		.rootfs_host_path_len = tawcroot_rootfs_host_path_len,
		.binds                = tawcroot_binds,
		.n_binds              = tawcroot_n_binds,
	};
	long out_len = tawcroot_proc_maps_rewrite(&ctx, in_buf, in_len,
						  out_buf, MAPS_BUF_SIZE);
	if (out_len < 0) {
		(void)tawc_munmap((void *)(uintptr_t)region, 2 * MAPS_BUF_SIZE);
		return out_len;
	}

	long memfd = tawc_memfd_create("tawcroot-maps", 1U /*MFD_CLOEXEC*/);
	if (memfd < 0) {
		(void)tawc_munmap((void *)(uintptr_t)region, 2 * MAPS_BUF_SIZE);
		return memfd;
	}

	size_t written = 0;
	while (written < (size_t)out_len) {
		long w = tawc_write((int)memfd, out_buf + written,
				    (size_t)out_len - written);
		if (w < 0) {
			tawc_close((int)memfd);
			(void)tawc_munmap((void *)(uintptr_t)region,
					  2 * MAPS_BUF_SIZE);
			return w;
		}
		written += (size_t)w;
	}

	(void)tawc_munmap((void *)(uintptr_t)region, 2 * MAPS_BUF_SIZE);

	long sk = tawc_lseek((int)memfd, 0, 0 /*SEEK_SET*/);
	if (sk < 0) {
		tawc_close((int)memfd);
		return sk;
	}
	return memfd;
}

/* /proc/sys/kernel/overflow{uid,gid} shadow fd. Returns a memfd preloaded
 * with the Linux-conventional "65534\n" (documented in
 * Documentation/admin-guide/sysctl/kernel.rst). Stays in lockstep with
 * the kernel default; the value hasn't changed since the sysctl landed,
 * and uid/gid have always shared it. The `memfd_name` distinguishes
 * the two in /proc/self/fd/<fd> readlinks for diagnostic clarity. */
static long open_proc_overflow_id_shadow(const char *memfd_name)
{
	static const char bytes[] = "65534\n";
	const size_t      len     = sizeof bytes - 1;

	long memfd = tawc_memfd_create(memfd_name, 1U /*MFD_CLOEXEC*/);
	if (memfd < 0) return memfd;

	size_t written = 0;
	while (written < len) {
		long w = tawc_write((int)memfd,
				    &bytes[written],
				    len - written);
		if (w < 0) {
			tawc_close((int)memfd);
			return w;
		}
		written += (size_t)w;
	}

	long sk = tawc_lseek((int)memfd, 0, 0 /*SEEK_SET*/);
	if (sk < 0) {
		tawc_close((int)memfd);
		return sk;
	}
	return memfd;
}

/* /proc/bus/pci/devices shadow fd. Returns an empty memfd — that's the
 * legitimate "no PCI devices visible" state that libpci's procfs back-
 * end is designed to handle. See the head-of-handle_openat comment for
 * why this matters (Mozilla glxtest -> WebRender disable cascade). The
 * memfd starts at offset 0 with size 0, so no write loop or lseek is
 * needed; -errno from memfd_create flows back to the guest verbatim,
 * same as the other two shadows. */
static long open_proc_bus_pci_devices_shadow(void)
{
	return tawc_memfd_create("tawcroot-pci-devices",
				 1U /*MFD_CLOEXEC*/);
}

/* Classify `path` against the three /proc shadows and synthesize the
 * matching fd. Returns 1 on a hit (with *out set to the new fd or the
 * synthesizer's -errno) and 0 on no match. Centralising the dispatch
 * keeps the absolute-path and fd-relative branches in handle_openat
 * automatically in lockstep — a future fourth shadow only needs one
 * line added here. */
static int try_proc_shadow(const char *path, long *out)
{
	const char *overflow_name;
	if (is_proc_self_maps(path)) {
		*out = open_proc_maps_shadow();
		return 1;
	}
	overflow_name = match_proc_sys_overflow_id(path);
	if (overflow_name) {
		*out = open_proc_overflow_id_shadow(overflow_name);
		return 1;
	}
	if (is_proc_bus_pci_devices(path)) {
		*out = open_proc_bus_pci_devices_shadow();
		return 1;
	}
	return 0;
}

static long handle_readlinkat(const tawcroot_syscall_args *args,
			      ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	char       *buf   = (char *)(uintptr_t)args->c;
	int         size  = (int)args->d;
	if (!gpath || !buf || size <= 0) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	/* Phase 2e: synthesize /proc/self/exe from the stashed guest exe
	 * path. The kernel's view points at libtawcroot.so; the guest
	 * wants the path it originally asked us to exec. The fd-relative
	 * case (readlinkat(proc_self_fd, "exe", ...)) is caught by re-
	 * composing through the dirfd's /proc/self/fd/<n> link. */
	{
		char *tmp = scratch->buf[0];
		long n = tawc_copy_string_from_guest(
			tmp, TAWCROOT_PATH_SCRATCH_SIZE, gpath);
		if (n < 0) return n;
		if (tawcroot_guest_exe_path_len > 0) {
			int hit = is_proc_self_exe(tmp);
			char *composed = scratch->buf[1];
			if (!hit && dirfd != AT_FDCWD && tmp[0] != '/' &&
			    could_be_proc_relative(tmp) &&
			    compose_fd_relative(dirfd, tmp,
						composed,
						TAWCROOT_PATH_SCRATCH_SIZE) > 0)
				hit = is_proc_self_exe(composed);
			if (hit) {
				size_t len = tawcroot_guest_exe_path_len;
				if (len > (size_t)size) len = (size_t)size;
				long ce = tawc_copy_to_guest(buf,
					tawcroot_guest_exe_path, len);
				if (ce < 0) return ce;
				return (long)len;
			}
		}
	}

	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty,
					TAWCROOT_PATH_NOFOLLOW);
	if (e) return e;
	/* When the guest path resolves exactly to a bind dst (or rootfs root),
	 * fetch_and_translate gives us (reserved_dir_fd, ""). The kernel
	 * answers `readlinkat(dir_fd, "", ...)` with ENOENT, but the
	 * semantically correct error is EINVAL — those paths are directories,
	 * not symlinks. Without this, glibc realpath aborts canonicalisation
	 * the moment it hits /proc/self (it readlinkats "/proc" first to
	 * check, gets ENOENT instead of EINVAL, and gives up before reaching
	 * the /proc/self/exe synthesis). The guest-supplied dirfd case
	 * (empty path on a guest O_PATH symlink fd) keeps the kernel call —
	 * those fds are not in our reserved range. */
	if (use_empty && tawcroot_fd_is_reserved(base_fd)) return TAWC_EINVAL;
	const char *p = use_empty ? "" : suffix;

	/* Read into a kernel-side scratch so we can post-process the result
	 * before forwarding it. We reuse `path_buf` (dead from here on) as
	 * the scratch — a fresh PATH_MAX buffer would push the handler
	 * frame past the stack budget noted in notes/tawcroot.md
	 * "Threading and `vfork` invariants". Cost vs. the pre-fix direct
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
	 * Truncation note: if the guest's `size` is shorter than our host
	 * path, the kernel-returned bytes are truncated, the equality test
	 * fails, and the truncated host path falls through unsubstituted.
	 * This is harmless in practice — every realpath caller passes a
	 * PATH_MAX-sized buffer — but worth knowing. */
	char *readlink_scratch = path_buf;
	int   scratch_cap = (size > TAWCROOT_PATH_SCRATCH_SIZE)
	                    ? TAWCROOT_PATH_SCRATCH_SIZE : size;
	long n = tawc_readlinkat(base_fd, p, readlink_scratch,
				 (size_t)scratch_cap);
	if (n < 0) return n;
	if (tawcroot_guest_exe_path_len > 0 &&
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
	int flags = (int)args->d;
	if (!gpath) return TAWC_EFAULT;

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(gpath, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME) return tawcroot_shm_access_name(shm_name);
		if (kind == SHM_PEEK_DIR)  return tawcroot_shm_access_dir();
	}

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty, pmode);
	if (e) return e;
	/* Empty suffix → guest asked for "/" or for a bind dst's root.
	 * Pass "." so the kernel resolves it to the dir base_fd points at;
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
	const char *p = use_empty ? "." : suffix;
	(void)flags;
	return TAWC_RAW(TAWC_SYS_faccessat, base_fd, (long)p,
	                mode, 0, 0, 0);
}

static long handle_chdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;

	/* Empty suffix → guest asked for "/", which is the directory the
	 * base fd already refers to. fchdir(base_fd) directly — avoids the
	 * AT_EMPTY_PATH-on-openat thing which only landed in kernel 6.6. */
	if (use_empty) {
		return TAWC_RAW(TAWC_SYS_fchdir, base_fd, 0, 0, 0, 0, 0);
	}

	int flags = O_DIRECTORY | O_PATH | O_CLOEXEC;
	long fd = tawc_openat(base_fd, suffix, flags, 0);
	if (fd < 0) return fd;

	long rv = TAWC_RAW(TAWC_SYS_fchdir, fd, 0, 0, 0, 0, 0);
	tawc_close((int)fd);
	return rv;
}

/* getcwd reverse-translation. Kernel returns the host cwd; we strip
 * the rootfs prefix, prepend `/`, copy to the guest's buffer, and
 * return a length matching the kernel's getcwd contract.
 *
 * If the kernel cwd is outside the rootfs view, return -ENOENT — proot
 * leaks the host path here, but exposing the host through a getcwd
 * answer the guest can read is exactly the kind of accidental escape
 * we close in §"Translation rules".
 */
static long handle_getcwd(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	char  *out = (char *)(uintptr_t)args->a;
	size_t cap = (size_t)args->b;
	if (!out || cap == 0) return TAWC_EFAULT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *host = scratch->buf[0];
	long r = TAWC_RAW(TAWC_SYS_getcwd, (long)host,
			  TAWCROOT_PATH_SCRATCH_SIZE,
			  0, 0, 0, 0);
	if (r < 0) return r;

	size_t host_len = (size_t)r;
	while (host_len > 0 && host[host_len - 1] == 0) host_len--;

	extern char tawcroot_rootfs_host_path[4096];
	extern size_t tawcroot_rootfs_host_path_len;
	if (host_len < tawcroot_rootfs_host_path_len) return TAWC_ENOENT;
	for (size_t i = 0; i < tawcroot_rootfs_host_path_len; i++)
		if (host[i] != tawcroot_rootfs_host_path[i]) return TAWC_ENOENT;
	/* Component-boundary check (review finding B4): a kernel cwd at
	 * "<rootfs>-evil/x" byte-matches the rootfs prefix but is not
	 * inside the view. Require end-of-string or `/` after the prefix. */
	if (host_len > tawcroot_rootfs_host_path_len &&
	    host[tawcroot_rootfs_host_path_len] != '/') return TAWC_ENOENT;

	/* Build "/<host[prefix:]>" into a stack-local buffer first, then
	 * copy through the guarded helper. Writing directly into `out` was
	 * the original implementation; a wild guest pointer would crash the
	 * SIGSYS handler (review finding B1+B6). The kernel's getcwd
	 * contract: returns length INCLUDING the trailing NUL. */
	char *tmp = scratch->buf[1];
	size_t off = 0;
	if (off + 1 >= cap || off + 1 >= TAWCROOT_PATH_SCRATCH_SIZE)
		return TAWC_ERANGE;
	tmp[off++] = '/';
	for (size_t i = tawcroot_rootfs_host_path_len; i < host_len; i++) {
		if (tmp[off - 1] == '/' && host[i] == '/') continue;
		if (off + 1 >= cap || off + 1 >= TAWCROOT_PATH_SCRATCH_SIZE)
			return TAWC_ERANGE;
		tmp[off++] = host[i];
	}
	tmp[off] = 0;
	long ce = tawc_copy_to_guest(out, tmp, off + 1);
	if (ce < 0) return ce;
	return (long)(off + 1);
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
	char *path_buf = scratch->buf[0];                                     \
	char *suffix = scratch->buf[1];                                       \
	int  base_fd, use_empty;                                              \
	long e = fetch_and_translate_at(dirfd, gpath,                         \
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE, \
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,   \
					&base_fd, &use_empty, pmode);         \
	if (e) return e;                                                  \
	const char *p = use_empty ? "." : suffix;                         \
	return TAWC_RAW(sysnr, base_fd, (long)p,                          \
			args->c, args->d,                                 \
			(narg) > 4 ? args->e : 0,                         \
			(narg) > 5 ? args->f : 0);                        \
}

DECLARE_AT_PASS(mkdirat,    TAWC_SYS_mkdirat,    3, TAWCROOT_PATH_PARENT_CREATE)
DECLARE_AT_PASS(fchmodat,   TAWC_SYS_fchmodat,   3, TAWCROOT_PATH_FOLLOW)
DECLARE_AT_PASS(mknodat,    TAWC_SYS_mknodat,    4, TAWCROOT_PATH_PARENT_CREATE)

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

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(gpath, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME) {
			if (flag & AT_REMOVEDIR) return TAWC_ENOTDIR;
			return tawcroot_shm_unlink(shm_name);
		}
		if (kind == SHM_PEEK_DIR) return (flag & 0x200) ? TAWC_EBUSY : TAWC_EISDIR;
	}

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty,
					TAWCROOT_PATH_PARENT_REMOVE);
	if (e) return e;
	const char *p = use_empty ? "." : suffix;
	return TAWC_RAW(TAWC_SYS_unlinkat, base_fd, (long)p, flag, 0, 0, 0);
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
		 * forward unchanged. */
		return TAWC_RAW(TAWC_SYS_utimensat, dirfd, 0,
				args->c, flags, 0, 0);
	}
	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty, pmode);
	if (e) return e;
	const char *p = use_empty ? "." : suffix;
	return TAWC_RAW(TAWC_SYS_utimensat, base_fd, (long)p,
			args->c, flags, 0, 0);
}

/* fchownat: translate path, but DON'T issue the host syscall (Android
 * untrusted_app uid can't chown anyway). proot `-0` semantics: report
 * success and lie. The on-disk file remains app-owned; guest sees what
 * it expected. */
static long handle_fchownat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return 0;
}

/* fd-only fchown, used by GNU tar when dpkg-deb extracts package
 * control files. Same fake-root contract as fchownat: report success
 * without touching host ownership, which Android app UIDs cannot
 * change anyway. */
static long handle_fchown(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return 0;
}

/* symlinkat: translate the destination only (the source is the
 * symlink's literal target string, NOT a host path — the kernel writes
 * those bytes into the new inode and validates EFAULT on its end).
 * The destination is a not-yet-existing leaf, so PARENT_CREATE. */
static long handle_symlinkat(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	const char *target = (const char *)(uintptr_t)args->a;
	int newdirfd = (int)args->b;
	const char *linkpath = (const char *)(uintptr_t)args->c;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int base_fd, use_empty;
	long e = fetch_and_translate_at(newdirfd, linkpath,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty,
					TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return TAWC_EINVAL; /* can't create / */
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)target, base_fd,
			(long)suffix, 0, 0, 0);
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
		int empty = (path == 0);
		if (!empty) {
			char first = -1;
			long pe = tawc_copy_from_guest(&first, 1, path);
			if (pe < 0) return pe;
			if (first == 0) empty = 1;
		}
		if (empty) {
			long rv = TAWC_RAW(TAWC_SYS_statx, dirfd, (long)"", flags,
					   mask, (long)&local, 0);
			if (rv != 0) return rv;
			local.stx_uid = 0;
			local.stx_gid = 0;
			local.stx_mask |= STATX_UID | STATX_GID;
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return rv;
		}
	}

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(path, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME || kind == SHM_PEEK_DIR) {
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

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, path,
					path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					suffix, TAWCROOT_PATH_SCRATCH_SIZE,
					&base_fd, &use_empty, pmode);
	if (e) return e;

	const char *resolved = use_empty ? "" : suffix;
	int rv_flags = flags;
	if (use_empty) rv_flags |= AT_EMPTY_PATH;

	long rv = TAWC_RAW(TAWC_SYS_statx, base_fd, (long)resolved,
			   rv_flags, mask, (long)&local, 0);
	if (rv != 0) return rv;
	local.stx_uid = 0;
	local.stx_gid = 0;
	long ce = tawc_copy_to_guest(out, &local, sizeof local);
	if (ce < 0) return ce;
	return rv;
}

/* link_with_symlink_fallback: issue host linkat, and on EACCES/EPERM
 * (typical under Android `untrusted_app` SELinux for hardlinks across
 * subtrees) fall back to creating a guest-absolute `symlinkat`. Mirrors
 * proot's --link2symlink. */
static long link_with_symlink_fallback(int src_fd, const char *src_suf,
				       int dst_fd, const char *dst_suf,
				       int flags)
{
	long rv = TAWC_RAW(TAWC_SYS_linkat, src_fd, (long)src_suf,
			   dst_fd, (long)dst_suf, flags, 0);
	if (rv == 0 || (rv != -13 /*EACCES*/ && rv != -1 /*EPERM*/)) {
		return rv;
	}
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *abs_target = scratch->buf[0];
	abs_target[0] = '/';
	size_t i = 0;
	while (src_suf[i] && i + 2 < TAWCROOT_PATH_SCRATCH_SIZE) {
		abs_target[1 + i] = src_suf[i]; i++;
	}
	abs_target[1 + i] = 0;
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)abs_target, dst_fd,
			(long)dst_suf, 0, 0, 0);
}

/* linkat: translate both operands. */
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
	char *old_buf = scratch->buf[0], *old_suf = scratch->buf[1];
	char *new_buf = scratch->buf[2], *new_suf = scratch->buf[3];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate_at(olddirfd, oldpath,
					 old_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					 old_suf, TAWCROOT_PATH_SCRATCH_SIZE,
					 &old_fd, &old_empty, src_mode);
	if (e1) return e1;
	long e2 = fetch_and_translate_at(newdirfd, newpath,
					 new_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					 new_suf, TAWCROOT_PATH_SCRATCH_SIZE,
					 &new_fd, &new_empty,
					 TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return TAWC_EINVAL;

	return link_with_symlink_fallback(old_fd, old_suf, new_fd, new_suf,
					  flags);
}

/* renameat2 — translate both operands; old is PARENT_REMOVE, new is
 * PARENT_CREATE. The kernel takes (olddirfd, oldpath, newdirfd, newpath,
 * flags); when the guest passes non-AT_FDCWD dirfds with relative paths
 * we let the kernel resolve off the dirfd (see fetch_and_translate_at).
 * Both renameat and renameat2 funnel through here — renameat just passes
 * flags=0 to the kernel renameat2 (the latter is a strict superset and
 * present on every kernel we target). */
static long do_renameat(int olddirfd, const char *oldpath,
			int newdirfd, const char *newpath,
			unsigned int rflags)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *old_buf = scratch->buf[0], *old_suf = scratch->buf[1];
	char *new_buf = scratch->buf[2], *new_suf = scratch->buf[3];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate_at(olddirfd, oldpath,
					 old_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					 old_suf, TAWCROOT_PATH_SCRATCH_SIZE,
					 &old_fd, &old_empty,
					 TAWCROOT_PATH_PARENT_REMOVE);
	if (e1) return e1;
	long e2 = fetch_and_translate_at(newdirfd, newpath,
					 new_buf, TAWCROOT_PATH_SCRATCH_SIZE,
					 new_suf, TAWCROOT_PATH_SCRATCH_SIZE,
					 &new_fd, &new_empty,
					 TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return TAWC_EINVAL;
	return TAWC_RAW(TAWC_SYS_renameat2, old_fd, (long)old_suf,
			new_fd, (long)new_suf, rflags, 0);
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
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	/* base_fd is a directory in every translation path (rootfs or a
	 * bind src, both opened O_DIRECTORY at init), so empty suffix means
	 * the kernel would EISDIR truncate(2) on the dir. */
	if (use_empty) return TAWC_EISDIR;

	long fd = tawc_openat(base_fd, suffix,
			      1 /*O_WRONLY*/ | 0x80000 /*O_CLOEXEC*/, 0);
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

static long stat_via_at(const char *path, struct stat *out, int flags)
{
	if (!out) return TAWC_EFAULT;
	struct stat local;

	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(path, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME || kind == SHM_PEEK_DIR) {
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

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty, pmode);
	if (e) return e;

	const char *p = use_empty ? "" : suffix;
	int f = flags;
	if (use_empty) f |= AT_EMPTY_PATH;

	long rv = TAWC_RAW(TAWC_SYS_fstatat, base_fd, (long)p,
			   (long)&local, f, 0, 0);
	if (rv != 0) return rv;
	decorate_stat(&local);
	long ce = tawc_copy_to_guest(out, &local, sizeof local);
	if (ce < 0) return ce;
	return rv;
}

static long handle_stat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ (void)uc; return stat_via_at((const char *)(uintptr_t)args->a,
			       (struct stat *)(uintptr_t)args->b, 0); }

static long handle_lstat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ (void)uc; return stat_via_at((const char *)(uintptr_t)args->a,
			       (struct stat *)(uintptr_t)args->b,
			       AT_SYMLINK_NOFOLLOW); }

static long handle_access(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	int mode = (int)args->b;
	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(path, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME) return tawcroot_shm_access_name(shm_name);
		if (kind == SHM_PEEK_DIR)  return tawcroot_shm_access_dir();
	}
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	const char *p = use_empty ? "." : suffix;
	/* Use faccessat (NR 269) — Android RET_TRAPs faccessat2 from
	 * untrusted_app, and a recursive SIGSYS in our handler is fatal.
	 * See handle_faccessat above for the longer story. */
	return TAWC_RAW(TAWC_SYS_faccessat, base_fd, (long)p, mode, 0, 0, 0);
}

static long handle_readlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	char *buf = (char *)(uintptr_t)args->b;
	int  size = (int)args->c;
	if (!path || !buf || size <= 0) return TAWC_EFAULT;

	/* Mirror handle_readlinkat: synthesize /proc/self/exe from the
	 * stashed guest exe path. Without this, x86_64 callers that go
	 * through the legacy readlink(2) (NR 89) bypass the rewrite,
	 * /proc/self/exe resolves to libtawcroot.so, and Firefox's stub
	 * binary fails XPCOM lookup. */
	{
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *tmp = scratch->buf[0];
		long n = tawc_copy_string_from_guest(
			tmp, TAWCROOT_PATH_SCRATCH_SIZE, path);
		if (n < 0) return n;
		if (tawcroot_guest_exe_path_len > 0 && is_proc_self_exe(tmp)) {
			size_t len = tawcroot_guest_exe_path_len;
			if (len > (size_t)size) len = (size_t)size;
			long ce = tawc_copy_to_guest(buf,
				tawcroot_guest_exe_path, len);
			if (ce < 0) return ce;
			return (long)len;
		}
	}

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_NOFOLLOW);
	if (e) return e;
	/* See handle_readlinkat for the EINVAL-vs-ENOENT rationale. */
	if (use_empty && tawcroot_fd_is_reserved(base_fd)) return TAWC_EINVAL;
	const char *p = use_empty ? "" : suffix;
	return tawc_readlinkat(base_fd, p, buf, (size_t)size);
}

static long handle_chmod(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	int mode = (int)args->b;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	const char *p = use_empty ? "." : suffix;
	return TAWC_RAW(TAWC_SYS_fchmodat, base_fd, (long)p, mode, 0, 0, 0);
}

static long handle_mkdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	int mode = (int)args->b;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return TAWC_EEXIST; /* root already exists */
	return TAWC_RAW(TAWC_SYS_mkdirat, base_fd, (long)suffix, mode, 0, 0, 0);
}

static long handle_unlink_or_rmdir(const tawcroot_syscall_args *args,
				   ucontext_t *uc, int rmdir_flag)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	{
		char shm_buf[320];
		const char *shm_name;
		int kind = peek_shm(path, shm_buf, sizeof shm_buf, &shm_name);
		if (kind == SHM_PEEK_NAME) {
			if (rmdir_flag) return TAWC_ENOTDIR;
			return tawcroot_shm_unlink(shm_name);
		}
		if (kind == SHM_PEEK_DIR) return rmdir_flag ? TAWC_EBUSY : TAWC_EISDIR;
	}
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_REMOVE);
	if (e) return e;
	if (use_empty) return TAWC_EINVAL;
	return TAWC_RAW(TAWC_SYS_unlinkat, base_fd, (long)suffix,
			rmdir_flag, 0, 0, 0);
}

static long handle_unlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return handle_unlink_or_rmdir(args, uc, 0); }

static long handle_rmdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return handle_unlink_or_rmdir(args, uc, AT_REMOVEDIR); }

static long handle_chown_legacy(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	/* Validate the path pointer even though we ignore the value, so
	 * a guest with a wild pointer sees -EFAULT instead of fake
	 * success — matches the contract every other path-bearing
	 * handler exposes. */
	const char *path = (const char *)(uintptr_t)args->a;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	long n = tawc_copy_string_from_guest(
		path_buf, TAWCROOT_PATH_SCRATCH_SIZE, path);
	if (n < 0) return n;
	return 0;  /* fake-root no-op, like fchownat */
}

/* Legacy x86_64 link(oldpath, newpath). flags=0 → src is NOFOLLOW. */
static long handle_link_legacy(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	(void)uc;
	const char *oldpath = (const char *)(uintptr_t)args->a;
	const char *newpath = (const char *)(uintptr_t)args->b;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *old_buf = scratch->buf[0], *old_suf = scratch->buf[1];
	char *new_buf = scratch->buf[2], *new_suf = scratch->buf[3];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate(oldpath, old_buf,
				      TAWCROOT_PATH_SCRATCH_SIZE,
				      old_suf, TAWCROOT_PATH_SCRATCH_SIZE,
				      &old_fd, &old_empty,
				      TAWCROOT_PATH_NOFOLLOW);
	if (e1) return e1;
	long e2 = fetch_and_translate(newpath, new_buf,
				      TAWCROOT_PATH_SCRATCH_SIZE,
				      new_suf, TAWCROOT_PATH_SCRATCH_SIZE,
				      &new_fd, &new_empty,
				      TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return TAWC_EINVAL;

	return link_with_symlink_fallback(old_fd, old_suf, new_fd, new_suf, 0);
}

/* Legacy x86_64 symlink(target, linkpath). */
static long handle_symlink_legacy(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{
	(void)uc;
	const char *target   = (const char *)(uintptr_t)args->a;
	const char *linkpath = (const char *)(uintptr_t)args->b;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(linkpath, path_buf,
				     TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return TAWC_EINVAL;
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)target, base_fd,
			(long)suffix, 0, 0, 0);
}

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
{
	tawcroot_syscall_args shifted = *args;
	shifted.a = (long)AT_FDCWD;  /* dirfd  */
	shifted.b = args->a;         /* path   */
	shifted.c = args->b;         /* flags  */
	shifted.d = args->c;         /* mode   */
	return handle_openat(&shifted, uc);
}

/* Legacy x86_64 mknod(path, mode, dev). aarch64 has no mknod — only
 * mknodat — so this is x86_64-only. Routes to the modern mknodat
 * with our base_fd / suffix. */
static long handle_mknod(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return TAWC_EFAULT;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return TAWC_EINVAL; /* can't mknod the dir itself */
	return TAWC_RAW(TAWC_SYS_mknodat, base_fd, (long)suffix,
			args->b, args->c, 0, 0);
}
#endif  /* __x86_64__ */

/* Build a "/proc/self/fd/<base_fd>[/suffix]" path into `out`. Used by
 * the path-bearing syscall handlers that don't have an *at variant
 * (statfs, *xattr): we open the rootfs/bind dir as `base_fd`, then
 * pass /proc/self/fd/<n>/<suffix> to the kernel as the path. The
 * kernel resolves /proc/self/fd/<n> to the dir's underlying inode
 * and walks the suffix from there.
 *
 * `use_empty=1` means the syscall should target `base_fd` itself
 * (suffix == ""); we omit the trailing "/<suffix>" so the path is
 * just "/proc/self/fd/<n>". Returns 0 / -ENAMETOOLONG. */
static long build_proc_fd_path(char *out, size_t cap,
			       int base_fd, const char *suffix, int use_empty)
{
	const char *prefix = "/proc/self/fd/";
	size_t i = 0;
	while (prefix[i]) {
		if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
		out[i] = prefix[i];
		i++;
	}
	int wrote = tawc_int_to_str(out + i, cap - i, base_fd);
	if (wrote <= 0) return TAWC_ENAMETOOLONG;
	i += (size_t)wrote;
	if (!use_empty) {
		if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
		out[i++] = '/';
		size_t j = 0;
		while (suffix[j]) {
			if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
			out[i++] = suffix[j++];
		}
	}
	if (i >= cap) return TAWC_ENAMETOOLONG;
	out[i] = 0;
	return 0;
}

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
	char *path_buf = scratch->buf[0];
	char *suffix = scratch->buf[1];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, TAWCROOT_PATH_SCRATCH_SIZE,
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	char *host_path = scratch->buf[2];
	long bp = build_proc_fd_path(host_path, TAWCROOT_PATH_SCRATCH_SIZE,
				     base_fd, suffix, use_empty);
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
	char *path_buf = scratch->buf[0];                                      \
	char *suffix = scratch->buf[1];                                        \
	int  base_fd, use_empty;                                               \
	long e = fetch_and_translate(gpath, path_buf,                          \
				     TAWCROOT_PATH_SCRATCH_SIZE,              \
				     suffix, TAWCROOT_PATH_SCRATCH_SIZE,      \
				     &base_fd, &use_empty, pmode);             \
	if (e) return e;                                                   \
	if (use_empty && (pmode) == TAWCROOT_PATH_NOFOLLOW) {                  \
		return TAWC_EOPNOTSUPP; /* see big comment above */            \
	}                                                                      \
	char *host_path = scratch->buf[2];                                      \
	long bp = build_proc_fd_path(host_path,                                 \
				     TAWCROOT_PATH_SCRATCH_SIZE,              \
				     base_fd, suffix, use_empty);              \
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
	tawcroot_dispatch_install(TAWC_SYS_readlinkat,  handle_readlinkat);
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
