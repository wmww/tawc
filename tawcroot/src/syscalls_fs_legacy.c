/* Legacy x86_64 path-bearing syscalls. Android's untrusted_app filter
 * RET_TRAPs these (the lp64-`access`-on-x86_64 issue documented in
 * notes/proot.md). We translate to the *at variant in the handler,
 * which is the cleanest version of the kludge proot's
 * src/tracee/seccomp.c carries. aarch64 has none of these. */

#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "syscalls_fs.h"
#include "syscalls_fs_internal.h"
#include "sysnr.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#if defined(__x86_64__)

/* Re-dispatch a legacy syscall as its modern *at counterpart by
 * rebuilding the argument frame and calling the handler installed for
 * `at_nr` (registered by the sibling fs files before this runs). `nr`
 * is preserved so nr-sensitive handlers (handle_faccessat's faccessat2
 * check, handle_chown_legacy) still see the original syscall number. */
static long via_at(int at_nr, const tawcroot_syscall_args *args,
		   ucontext_t *uc, long a, long b, long c, long d, long e)
{
	tawcroot_syscall_args s = *args;
	s.a = a; s.b = b; s.c = c; s.d = d; s.e = e;
	return tawcroot_dispatch_get(at_nr)(&s, uc);
}

static long handle_stat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_fstatat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_lstat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_fstatat, args, uc,
		AT_FDCWD, args->a, args->b, AT_SYMLINK_NOFOLLOW, 0); }

static long handle_access(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_faccessat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_readlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_readlinkat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }

static long handle_chmod(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_fchmodat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_mkdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_mkdirat, args, uc,
		AT_FDCWD, args->a, args->b, 0, 0); }

static long handle_unlink(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_unlinkat, args, uc, AT_FDCWD, args->a, 0, 0, 0); }

static long handle_rmdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_unlinkat, args, uc,
		AT_FDCWD, args->a, AT_REMOVEDIR, 0, 0); }

/* Legacy chown/lchown route through handle_fchownat: same translation,
 * existence probe, and euid-gated fake. args->nr is preserved by
 * via_at, so one handler serves both (lchown adds NOFOLLOW). */
static long handle_chown_legacy(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	int flags = args->nr == TAWC_SYS_lchown ? AT_SYMLINK_NOFOLLOW : 0;
	return via_at(TAWC_SYS_fchownat, args, uc,
		      AT_FDCWD, args->a, args->b, args->c, flags);
}

/* Legacy x86_64 link(oldpath, newpath): linkat with flags=0 (source is
 * NOFOLLOW). */
static long handle_link_legacy(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{ return via_at(TAWC_SYS_linkat, args, uc,
		AT_FDCWD, args->a, AT_FDCWD, args->b, 0); }

/* Legacy x86_64 symlink(target, linkpath). */
static long handle_symlink_legacy(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{ return via_at(TAWC_SYS_symlinkat, args, uc,
		args->a, AT_FDCWD, args->b, 0, 0); }

/* Legacy x86_64 rename(oldpath, newpath): renameat2 with flags=0. */
static long handle_rename_legacy(const tawcroot_syscall_args *args,
				 ucontext_t *uc)
{ return via_at(TAWC_SYS_renameat2, args, uc,
		AT_FDCWD, args->a, AT_FDCWD, args->b, 0); }

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
{ return via_at(TAWC_SYS_openat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }

/* Legacy x86_64 creat(path, mode) ≡ open(path, O_WRONLY|O_CREAT|O_TRUNC).
 * Same routing rationale as handle_open_legacy. */
static long handle_creat(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_openat, args, uc,
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
	long e = tawcroot_fs_translate_at(scratch, 0, dirfd, gpath,
					  TAWCROOT_PATH_FOLLOW,
					  TAWCROOT_PATH_INTENT_WRITE, &t);
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

/* Legacy x86_64 mknod(path, mode, dev). */
static long handle_mknod(const tawcroot_syscall_args *args, ucontext_t *uc)
{ return via_at(TAWC_SYS_mknodat, args, uc,
		AT_FDCWD, args->a, args->b, args->c, 0); }

void tawcroot_fs_legacy_register(void)
{
	/* The lp64-`access`-on-x86_64 set that Android's untrusted_app
	 * filter RET_ERRNOs but our TRAP wins. */
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
}

#endif  /* __x86_64__ */
