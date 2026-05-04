/* Filesystem syscall handlers — phase 1.
 *
 * Path-bearing syscall handlers, dispatched from the SIGSYS handler.
 * Each handler:
 *   1. Pulls the guest path through `process_vm_readv` (EFAULT-safe).
 *   2. Translates via tawcroot_path_translate(...mode), which folds
 *      `..`, clamps at root, applies the well-known-symlink memo
 *      (mode-gated for sole-component leaves), and resolves bind
 *      mounts.
 *   3. Issues the host syscall against (base_fd, suffix). For
 *      openat, on kernel ≥5.6 we route through openat2 with
 *      RESOLVE_IN_ROOT so the kernel handles arbitrary non-final-
 *      component symlinks (incl. absolute targets) by re-rooting
 *      resolution at base_fd.
 *
 * Argument shape per arch is in tawcroot_syscall_args (see arch.h):
 *   args.a = arg0 (kernel reg 0), .b = arg1, ...
 *
 * Phase-1 policy: the guest's `dirfd` is currently ignored for *at
 * variants — every path is resolved relative to the rootfs (or the
 * cwd, reverse-translated). Fd-provenance (resolving `/proc/self/fd/N`
 * etc.) is a follow-up.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>
#include <linux/stat.h>

#include "dispatch.h"
#include "io.h"
#include "path.h"
#include "proc_rewrite.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "usercopy.h"

#define TAWC_PATH_MAX 4096

#ifndef AT_EMPTY_PATH
# define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_SYMLINK_NOFOLLOW
# define AT_SYMLINK_NOFOLLOW 0x100
#endif

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
 * to its target. */
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0x20000
#endif
#ifndef O_CREAT
# define O_CREAT    0x40
#endif
static tawcroot_path_mode openat_mode(int flags)
{
	if (flags & O_NOFOLLOW) return TAWCROOT_PATH_NOFOLLOW;
	if (flags & O_CREAT)    return TAWCROOT_PATH_PARENT_CREATE;
	return TAWCROOT_PATH_FOLLOW;
}

/* Forward decls — bodies live below, near the other /proc/self
 * helpers (is_proc_self_exe etc.). */
static int  is_proc_self_maps(const char *path);
static long open_proc_maps_shadow(void);

static long handle_openat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int flags = (int)args->c;
	int mode  = (int)args->d;

	/* /proc/self/maps and /proc/<our-pid>/maps: synthesize a shadow fd
	 * backed by a memfd containing the kernel's maps output with each
	 * path field reverse-translated through the rootfs/bind tables.
	 * Without this, sandboxes that grep their own maps (Mozilla's, ld.so
	 * $ORIGIN resolvers, crash handlers) see host paths the guest's
	 * world view doesn't contain.
	 *
	 * Only intercept O_RDONLY (no O_DIRECTORY, no O_PATH). Other flag
	 * combos fall through to normal translation so the kernel produces
	 * the conventional -ENOTDIR / O_PATH-fd behavior. The 64-byte peek
	 * is wide enough for "/proc/<10-digit-pid>/maps"; longer paths can't
	 * match anyway. */
	if (gpath &&
	    (flags & 3) == 0 /*O_RDONLY*/ &&
	    (flags & 0x10000 /*O_DIRECTORY*/) == 0 &&
	    (flags & 0x200000 /*O_PATH*/) == 0) {
		char tmp[64];
		long pn = tawc_copy_string_from_guest(tmp, sizeof tmp, gpath);
		if (pn >= 0 && is_proc_self_maps(tmp))
			return open_proc_maps_shadow();
	}

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty,
					openat_mode(flags));
	if (e) return e;

	/* Empty suffix → guest asked for "/" or for a bind dst's root.
	 * Pass "." so the kernel resolves it to the dir base_fd points at;
	 * this works on every kernel we target (AT_EMPTY_PATH on openat
	 * only landed in 6.6, kernel 5.4 device doesn't have it). */
	const char *p = use_empty ? "." : suffix;

	/* On kernel ≥ 5.6, route through openat2 with RESOLVE_IN_ROOT.
	 * The kernel then handles arbitrary in-rootfs symlinks (including
	 * absolute targets that would otherwise escape) by re-rooting them
	 * at base_fd. When base_fd is a bind src (not the rootfs root),
	 * RESOLVE_IN_ROOT clamps at the bind src — guest can't `..` back
	 * into the rootfs view through the bind, mirroring mount-namespace
	 * semantics. Older kernels stay on the string-fold + well-known-
	 * memo path, which handles the typical glibc-rootfs `/lib`-style
	 * symlinks but not arbitrary ones. */
	if (tawcroot_openat2_works) {
		struct tawc_open_how how;
		how.flags = (uint64_t)(uint32_t)flags;
		/* The kernel rejects open_how with non-zero `mode` when no
		 * creation flag is set. Mirror libc's discipline.
		 *
		 * O_TMPFILE is `__O_TMPFILE | O_DIRECTORY` (0x400000 | 0x10000
		 * = 0x410000); checking via plain bitwise AND would match on
		 * O_DIRECTORY alone, so test for the full bit pattern. (Review
		 * finding B8.) */
		int has_create   = (flags & 0x40 /*O_CREAT*/) != 0;
		int has_tmpfile  = (flags & 0x410000 /*O_TMPFILE*/) == 0x410000;
		how.mode = (has_create || has_tmpfile)
			? (uint64_t)(uint32_t)mode : 0;
		how.resolve = TAWC_RESOLVE_IN_ROOT;
		return tawc_openat2(base_fd, p, &how, sizeof how);
	}
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

	if (!out) return -14;  /* EFAULT */

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
	 * the guest's, but for phase-1 minimum we don't track guest fd
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
			if (rv == 0) decorate_stat(&local);
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return rv;
		}
	}

	if (!gpath) return -14;

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
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
	if (rv == 0) decorate_stat(&local);
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
 * Caveat: `..` traversal from a fd near the rootfs root can still
 * escape, since the kernel walks `..` past the dirfd freely. proot
 * has the same gap on kernels without RESOLVE_BENEATH; tightening via
 * openat2 is a phase-2 follow-up. For our targets (gpg, pacman, libc
 * apps) this passthrough is safe in practice. */
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

/* Variant that honours the guest's dirfd for fd-relative resolution.
 * See big comment above for why this matters. Returns -1 in
 * `*base_fd_out` and the literal guest-supplied path in `path_buf`
 * (also via the suffix output, kept identical) when the kernel should
 * resolve directly off `dirfd`; the caller then issues the raw *at
 * syscall with `dirfd` and the literal path. */
static long fetch_and_translate_at(int dirfd, const char *guest_path,
				   char *path_buf, size_t path_cap,
				   char *suffix,   size_t suffix_cap,
				   int  *base_fd_out, int *use_empty_path,
				   tawcroot_path_mode mode)
{
	if (dirfd != -100 /*AT_FDCWD*/) {
		long n = tawc_copy_string_from_guest(path_buf, path_cap,
		                                     guest_path);
		if (n < 0) return n;
		if (path_buf[0] != '/') {
			/* Relative path with explicit dirfd: kernel resolves
			 * off the dirfd. Copy path_buf into suffix so callers
			 * with one output buffer still work. */
			size_t i = 0;
			while (path_buf[i] && i + 1 < suffix_cap) {
				suffix[i] = path_buf[i];
				i++;
			}
			if (i + 1 >= suffix_cap) return -36; /* ENAMETOOLONG */
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


/* If `path` is "/proc/self/<x>" or "/proc/<our-pid>/<x>", return a
 * pointer to the byte after the trailing '/' (the "<x>" tail). Returns
 * NULL otherwise. The match is deliberately strict — paths like
 * "/proc/foo/../self/exe" are caught only after the guest's libc has
 * already canonicalized them, which is the typical flow. /proc/<tid>/
 * for any tid in our process is a TODO; today /proc/self covers all
 * single-threaded callers and most multi-threaded ones (libc resolves
 * $ORIGIN once, in the main thread). */
static const char *strip_proc_self_prefix(const char *path)
{
	if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
	    path[3] != 'o' || path[4] != 'c' || path[5] != '/')
		return 0;
	const char *t = path + 6;
	if (t[0] == 's' && t[1] == 'e' && t[2] == 'l' && t[3] == 'f' &&
	    t[4] == '/')
		return t + 5;
	if (!(t[0] >= '0' && t[0] <= '9')) return 0;
	long n = 0;
	const char *p = t;
	while (*p >= '0' && *p <= '9') {
		n = n * 10 + (*p - '0'); p++;
		if (n > 0x7fffffff) return 0;
	}
	if (p[0] != '/') return 0;
	long mypid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	if (mypid != n) return 0;
	return p + 1;
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
	if (region == 0) return -12; /* ENOMEM — defensive */
	char *in_buf  = (char *)(uintptr_t)region;
	char *out_buf = in_buf + MAPS_BUF_SIZE;

	long src = tawc_openat(-100 /*AT_FDCWD*/, "/proc/self/maps",
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

static long handle_readlinkat(const tawcroot_syscall_args *args,
			      ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	char       *buf   = (char *)(uintptr_t)args->c;
	int         size  = (int)args->d;
	if (!gpath || !buf || size <= 0) return -14;

	/* Phase 2e: synthesize /proc/self/exe from the stashed guest exe
	 * path. The kernel's view points at libtawcroot.so; the guest
	 * wants the path it originally asked us to exec. */
	{
		char tmp[TAWC_PATH_MAX];
		long n = tawc_copy_string_from_guest(tmp, sizeof tmp, gpath);
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

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty,
					TAWCROOT_PATH_NOFOLLOW);
	if (e) return e;
	const char *p = use_empty ? "" : suffix;
	return tawc_readlinkat(base_fd, p, buf, (size_t)size);
}

static long handle_faccessat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	int mode  = (int)args->c;
	int flags = (int)args->d;
	if (!gpath) return -14;

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty, pmode);
	if (e) return e;
	const char *p = use_empty ? "" : suffix;
	if (use_empty) flags |= AT_EMPTY_PATH;
	/* Use faccessat (NR 269) instead of faccessat2 (NR 439). Android's
	 * untrusted_app filter RET_TRAPs faccessat2 on recent platform
	 * versions; calling it from inside our handler causes a recursive
	 * SIGSYS that the kernel routes to default disposition (kill).
	 * faccessat doesn't accept flags, but our common callers
	 * (libc access(2) wrappers, bash, glibc) pass mode-only access
	 * checks. AT_EMPTY_PATH and AT_SYMLINK_NOFOLLOW callers will see
	 * default-symlink-follow behaviour, which matches access(2). */
	(void)flags;
	return TAWC_RAW(TAWC_SYS_faccessat, base_fd, (long)p,
	                mode, 0, 0, 0);
}

static long handle_chdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return -14;

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;

	/* Empty suffix → guest asked for "/", which is the directory the
	 * base fd already refers to. fchdir(base_fd) directly — avoids the
	 * AT_EMPTY_PATH-on-openat thing which only landed in kernel 6.6. */
	if (use_empty) {
		return TAWC_RAW(TAWC_SYS_fchdir, base_fd, 0, 0, 0, 0, 0);
	}

	int flags = 0x10000 /*O_DIRECTORY*/ | 0x200000 /*O_PATH*/
		  | 0x80000 /*O_CLOEXEC*/;
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
	if (!out || cap == 0) return -14;

	char host[TAWC_PATH_MAX];
	long r = TAWC_RAW(TAWC_SYS_getcwd, (long)host, (long)sizeof host,
			  0, 0, 0, 0);
	if (r < 0) return r;

	size_t host_len = (size_t)r;
	while (host_len > 0 && host[host_len - 1] == 0) host_len--;

	extern char tawcroot_rootfs_host_path[4096];
	extern size_t tawcroot_rootfs_host_path_len;
	if (host_len < tawcroot_rootfs_host_path_len) return -2;
	for (size_t i = 0; i < tawcroot_rootfs_host_path_len; i++)
		if (host[i] != tawcroot_rootfs_host_path[i]) return -2;
	/* Component-boundary check (review finding B4): a kernel cwd at
	 * "<rootfs>-evil/x" byte-matches the rootfs prefix but is not
	 * inside the view. Require end-of-string or `/` after the prefix. */
	if (host_len > tawcroot_rootfs_host_path_len &&
	    host[tawcroot_rootfs_host_path_len] != '/') return -2;

	/* Build "/<host[prefix:]>" into a stack-local buffer first, then
	 * copy through the guarded helper. Writing directly into `out` was
	 * the original implementation; a wild guest pointer would crash the
	 * SIGSYS handler (review finding B1+B6). The kernel's getcwd
	 * contract: returns length INCLUDING the trailing NUL. */
	char tmp[TAWC_PATH_MAX];
	size_t off = 0;
	if (off + 1 >= cap || off + 1 >= sizeof tmp) return -34; /* ERANGE */
	tmp[off++] = '/';
	for (size_t i = tawcroot_rootfs_host_path_len; i < host_len; i++) {
		if (tmp[off - 1] == '/' && host[i] == '/') continue;
		if (off + 1 >= cap || off + 1 >= sizeof tmp) return -34;
		tmp[off++] = host[i];
	}
	tmp[off] = 0;
	long ce = tawc_copy_to_guest(out, tmp, off + 1);
	if (ce < 0) return ce;
	return (long)(off + 1);
}

/* Translate-and-pass-through wrappers for the simpler path-bearing
 * syscalls. These all take (dirfd, path, ...) at the kernel layer; the
 * guest's dirfd is currently passed through (phase-1 minimum — fd
 * provenance comes later) and the path is translated. */

#define DECLARE_AT_PASS(name, sysnr, narg, pmode)                         \
static long handle_##name(const tawcroot_syscall_args *args,             \
			  ucontext_t *uc)                                 \
{                                                                         \
	(void)uc;                                                         \
	int dirfd = (int)args->a;                                         \
	const char *gpath = (const char *)(uintptr_t)args->b;             \
	if (!gpath) return -14;                                           \
	char path_buf[TAWC_PATH_MAX];                                     \
	char suffix[TAWC_PATH_MAX];                                       \
	int  base_fd, use_empty;                                          \
	long e = fetch_and_translate_at(dirfd, gpath,                     \
					path_buf, sizeof path_buf,        \
					suffix, sizeof suffix,            \
					&base_fd, &use_empty, pmode);     \
	if (e) return e;                                                  \
	const char *p = use_empty ? "." : suffix;                         \
	return TAWC_RAW(sysnr, base_fd, (long)p,                          \
			args->c, args->d,                                 \
			(narg) > 4 ? args->e : 0,                         \
			(narg) > 5 ? args->f : 0);                        \
}

DECLARE_AT_PASS(mkdirat,    TAWC_SYS_mkdirat,    3, TAWCROOT_PATH_PARENT_CREATE)
DECLARE_AT_PASS(unlinkat,   TAWC_SYS_unlinkat,   3, TAWCROOT_PATH_PARENT_REMOVE)
DECLARE_AT_PASS(fchmodat,   TAWC_SYS_fchmodat,   3, TAWCROOT_PATH_FOLLOW)
DECLARE_AT_PASS(mknodat,    TAWC_SYS_mknodat,    4, TAWCROOT_PATH_PARENT_CREATE)

/* utimensat: translate path and pass through. Special-case the
 * Linux extension `pathname == NULL` (operate on dirfd directly) by
 * forwarding without translation; the AT_PASS macro can't express
 * that since it rejects null paths up front.
 *
 * Without this trapping, libarchive (used by pacman) hits the kernel
 * with a guest-visible path that fails to resolve — the package
 * extraction completes but every file gets the current mtime instead
 * of the archive's recorded one, drowning install logs in
 * "Can't restore time" warnings. */
static long handle_utimensat(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	const char *gpath = (const char *)(uintptr_t)args->b;
	if (!gpath) {
		/* Linux-extension: NULL pathname → operate on dirfd. The
		 * dirfd is one of ours (translated openat handed it back);
		 * forward unchanged. */
		return TAWC_RAW(TAWC_SYS_utimensat, dirfd, 0,
				args->c, args->d, 0, 0);
	}
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, gpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty,
					TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	const char *p = use_empty ? "." : suffix;
	return TAWC_RAW(TAWC_SYS_utimensat, base_fd, (long)p,
			args->c, args->d, 0, 0);
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
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int base_fd, use_empty;
	long e = fetch_and_translate_at(newdirfd, linkpath,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty,
					TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return -22; /* EINVAL — can't create / */
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
	if (!out) return -14;

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
			if (rv == 0) {
				local.stx_uid = 0;
				local.stx_gid = 0;
				local.stx_mask |= STATX_UID | STATX_GID;
			}
			long ce = tawc_copy_to_guest(out, &local, sizeof local);
			if (ce < 0) return ce;
			return rv;
		}
	}

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate_at(dirfd, path,
					path_buf, sizeof path_buf,
					suffix, sizeof suffix,
					&base_fd, &use_empty, pmode);
	if (e) return e;

	const char *resolved = use_empty ? "" : suffix;
	int rv_flags = flags;
	if (use_empty) rv_flags |= AT_EMPTY_PATH;

	long rv = TAWC_RAW(TAWC_SYS_statx, base_fd, (long)resolved,
			   rv_flags, mask, (long)&local, 0);
	if (rv == 0) {
		local.stx_uid = 0;
		local.stx_gid = 0;
	}
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
	char abs_target[TAWC_PATH_MAX];
	abs_target[0] = '/';
	size_t i = 0;
	while (src_suf[i] && i + 2 < sizeof abs_target) {
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

	char old_buf[TAWC_PATH_MAX], old_suf[TAWC_PATH_MAX];
	char new_buf[TAWC_PATH_MAX], new_suf[TAWC_PATH_MAX];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate_at(olddirfd, oldpath,
					 old_buf, sizeof old_buf,
					 old_suf, sizeof old_suf,
					 &old_fd, &old_empty, src_mode);
	if (e1) return e1;
	long e2 = fetch_and_translate_at(newdirfd, newpath,
					 new_buf, sizeof new_buf,
					 new_suf, sizeof new_suf,
					 &new_fd, &new_empty,
					 TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return -22;

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
	char old_buf[TAWC_PATH_MAX], old_suf[TAWC_PATH_MAX];
	char new_buf[TAWC_PATH_MAX], new_suf[TAWC_PATH_MAX];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate_at(olddirfd, oldpath,
					 old_buf, sizeof old_buf,
					 old_suf, sizeof old_suf,
					 &old_fd, &old_empty,
					 TAWCROOT_PATH_PARENT_REMOVE);
	if (e1) return e1;
	long e2 = fetch_and_translate_at(newdirfd, newpath,
					 new_buf, sizeof new_buf,
					 new_suf, sizeof new_suf,
					 &new_fd, &new_empty,
					 TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return -22;
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

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	/* base_fd is a directory in every translation path (rootfs or a
	 * bind src, both opened O_DIRECTORY at init), so empty suffix means
	 * the kernel would EISDIR truncate(2) on the dir. */
	if (use_empty) return -21; /* EISDIR */

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
	if (!out) return -14;
	struct stat local;

	tawcroot_path_mode pmode = (flags & AT_SYMLINK_NOFOLLOW)
		? TAWCROOT_PATH_NOFOLLOW
		: TAWCROOT_PATH_FOLLOW;

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty, pmode);
	if (e) return e;

	const char *p = use_empty ? "" : suffix;
	int f = flags;
	if (use_empty) f |= AT_EMPTY_PATH;

	long rv = TAWC_RAW(TAWC_SYS_fstatat, base_fd, (long)p,
			   (long)&local, f, 0, 0);
	if (rv == 0) decorate_stat(&local);
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
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
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
	if (!path || !buf || size <= 0) return -14;

	/* Mirror handle_readlinkat: synthesize /proc/self/exe from the
	 * stashed guest exe path. Without this, x86_64 callers that go
	 * through the legacy readlink(2) (NR 89) bypass the rewrite,
	 * /proc/self/exe resolves to libtawcroot.so, and Firefox's stub
	 * binary fails XPCOM lookup. */
	{
		char tmp[TAWC_PATH_MAX];
		long n = tawc_copy_string_from_guest(tmp, sizeof tmp, path);
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

	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_NOFOLLOW);
	if (e) return e;
	const char *p = use_empty ? "" : suffix;
	return tawc_readlinkat(base_fd, p, buf, (size_t)size);
}

static long handle_chmod(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	int mode = (int)args->b;
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
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
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return -17; /* EEXIST — root already exists */
	return TAWC_RAW(TAWC_SYS_mkdirat, base_fd, (long)suffix, mode, 0, 0, 0);
}

static long handle_unlink_or_rmdir(const tawcroot_syscall_args *args,
				   ucontext_t *uc, int rmdir_flag)
{
	(void)uc;
	const char *path = (const char *)(uintptr_t)args->a;
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(path, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_REMOVE);
	if (e) return e;
	if (use_empty) return -22;
	return TAWC_RAW(TAWC_SYS_unlinkat, base_fd, (long)suffix,
			rmdir_flag, 0, 0, 0);
}

static long handle_unlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return handle_unlink_or_rmdir(args, uc, 0); }

static long handle_rmdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return handle_unlink_or_rmdir(args, uc, 0x200 /*AT_REMOVEDIR*/); }

static long handle_chown_legacy(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	/* Validate the path pointer even though we ignore the value, so
	 * a guest with a wild pointer sees -EFAULT instead of fake
	 * success — matches the contract every other path-bearing
	 * handler exposes. */
	const char *path = (const char *)(uintptr_t)args->a;
	char path_buf[TAWC_PATH_MAX];
	long n = tawc_copy_string_from_guest(path_buf, sizeof path_buf, path);
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

	char old_buf[TAWC_PATH_MAX], old_suf[TAWC_PATH_MAX];
	char new_buf[TAWC_PATH_MAX], new_suf[TAWC_PATH_MAX];
	int  old_fd, old_empty, new_fd, new_empty;
	long e1 = fetch_and_translate(oldpath, old_buf, sizeof old_buf,
				      old_suf, sizeof old_suf,
				      &old_fd, &old_empty,
				      TAWCROOT_PATH_NOFOLLOW);
	if (e1) return e1;
	long e2 = fetch_and_translate(newpath, new_buf, sizeof new_buf,
				      new_suf, sizeof new_suf,
				      &new_fd, &new_empty,
				      TAWCROOT_PATH_PARENT_CREATE);
	if (e2) return e2;
	if (old_empty || new_empty) return -22;

	return link_with_symlink_fallback(old_fd, old_suf, new_fd, new_suf, 0);
}

/* Legacy x86_64 symlink(target, linkpath). */
static long handle_symlink_legacy(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{
	(void)uc;
	const char *target   = (const char *)(uintptr_t)args->a;
	const char *linkpath = (const char *)(uintptr_t)args->b;
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(linkpath, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return -22;
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)target, base_fd,
			(long)suffix, 0, 0, 0);
}

/* Legacy x86_64 rename(oldpath, newpath). */
static long handle_rename_legacy(const tawcroot_syscall_args *args,
				 ucontext_t *uc)
{
	(void)uc;
	return do_renameat(-100 /*AT_FDCWD*/,
			   (const char *)(uintptr_t)args->a,
			   -100 /*AT_FDCWD*/,
			   (const char *)(uintptr_t)args->b, 0);
}

/* Legacy x86_64 mknod(path, mode, dev). aarch64 has no mknod — only
 * mknodat — so this is x86_64-only. Routes to the modern mknodat
 * with our base_fd / suffix. */
static long handle_mknod(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const char *gpath = (const char *)(uintptr_t)args->a;
	if (!gpath) return -14;
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_PARENT_CREATE);
	if (e) return e;
	if (use_empty) return -22;  /* EINVAL — can't mknod the dir itself */
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
		if (i + 1 >= cap) return -36;
		out[i] = prefix[i];
		i++;
	}
	int wrote = tawc_int_to_str(out + i, cap - i, base_fd);
	if (wrote <= 0) return -36;
	i += (size_t)wrote;
	if (!use_empty) {
		if (i + 1 >= cap) return -36;
		out[i++] = '/';
		size_t j = 0;
		while (suffix[j]) {
			if (i + 1 >= cap) return -36;
			out[i++] = suffix[j++];
		}
	}
	if (i >= cap) return -36;
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
	if (!gpath) return -14;
	char path_buf[TAWC_PATH_MAX];
	char suffix[TAWC_PATH_MAX];
	int  base_fd, use_empty;
	long e = fetch_and_translate(gpath, path_buf, sizeof path_buf,
				     suffix, sizeof suffix,
				     &base_fd, &use_empty,
				     TAWCROOT_PATH_FOLLOW);
	if (e) return e;
	char host_path[TAWC_PATH_MAX];
	long bp = build_proc_fd_path(host_path, sizeof host_path,
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
	const char *gpath = (const char *)(uintptr_t)args->a;              \
	if (!gpath) return -14;                                            \
	char path_buf[TAWC_PATH_MAX];                                      \
	char suffix[TAWC_PATH_MAX];                                        \
	int  base_fd, use_empty;                                           \
	long e = fetch_and_translate(gpath, path_buf, sizeof path_buf,     \
				     suffix, sizeof suffix,                \
				     &base_fd, &use_empty, pmode);         \
	if (e) return e;                                                   \
	if (use_empty && (pmode) == TAWCROOT_PATH_NOFOLLOW)                \
		return -95; /* EOPNOTSUPP — see big comment above */       \
	char host_path[TAWC_PATH_MAX];                                     \
	long bp = build_proc_fd_path(host_path, sizeof host_path,          \
				     base_fd, suffix, use_empty);          \
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
