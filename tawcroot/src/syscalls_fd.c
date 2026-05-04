/* Fd-shape syscall handlers — phase 0.5 internal-fd protection.
 *
 * See include/fdtab.h for the rationale. Every handler here is the
 * minimum viable wrapper around the corresponding host syscall; the
 * only intercept is "if a guest argument names a reserved fd, lie".
 *
 * This is NOT a security boundary — see notes/tawcroot.md §"What it
 * explicitly is not". A guest that wants to corrupt our state has
 * other avenues (e.g. mmap over our text). The intercept is for
 * accidental damage from libraries that close-all-fds before exec
 * (libc init), test harnesses that close_range() everything, or
 * pacman-style tools that drop fd tables on fork. Those workloads
 * are common; defending against them is cheap.
 */

#include <stddef.h>
#include <stdint.h>

#include "dirent_filter.h"
#include "dispatch.h"
#include "fdtab.h"
#include "raw_sys.h"
#include "sysnr.h"

#define EBADF_NEG  (-9)
#define EINVAL_NEG (-22)

#ifndef F_DUPFD
# define F_DUPFD          0
#endif
#ifndef F_DUPFD_CLOEXEC
# define F_DUPFD_CLOEXEC  1030
#endif

int    tawcroot_reserved_fds[TAWCROOT_MAX_RESERVED_FDS];
size_t tawcroot_n_reserved_fds;

long tawcroot_fd_reserve(int fd)
{
	if (fd < 0) return EBADF_NEG;
	long r = tawc_fcntl(fd, F_DUPFD_CLOEXEC, TAWCROOT_RESERVED_FD_BASE);
	if (r < 0) return r;
	tawc_close(fd);
	if (tawcroot_n_reserved_fds >= TAWCROOT_MAX_RESERVED_FDS) {
		/* No room in the BPF-filter array; the reservation is still
		 * valid (the fd lives at >= base) but close-loop fast-pathing
		 * loses precision — handler-side check still catches it. */
		return r;
	}
	tawcroot_reserved_fds[tawcroot_n_reserved_fds++] = (int)r;
	return r;
}

static long handle_close(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	(void)args;
	/* Reserved fds: lie. The BPF filter only routes close() here when
	 * the argument matches one of our reserved slots, so by definition
	 * we never want the kernel to actually close it — the guest sees
	 * success, our handler keeps the fd alive for downstream path
	 * translation. This makes our reserved fds un-killable from the
	 * guest (close_range, dup2/3, fcntl F_DUPFD all already reject or
	 * trim around the reserved range), which means handler-side state
	 * for path translation can stay immutable post-init. */
	return 0;
}

static long handle_close_range(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	(void)uc;
	unsigned int first = (unsigned int)args->a;
	unsigned int last  = (unsigned int)args->b;
	unsigned int flags = (unsigned int)args->c;

	/* Entire range is above the reserved boundary → success no-op
	 * (the guest sees no fds to close). */
	if (first >= TAWCROOT_RESERVED_FD_BASE) return 0;

	/* Trim the upper end so the kernel never sees our reserved slots. */
	if (last >= TAWCROOT_RESERVED_FD_BASE) {
		last = TAWCROOT_RESERVED_FD_BASE - 1;
	}
	return TAWC_RAW(TAWC_SYS_close_range, first, last, flags, 0, 0, 0);
}

static long handle_dup(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int oldfd = (int)args->a;
	if (tawcroot_fd_is_reserved(oldfd)) return EBADF_NEG;
	return TAWC_RAW(TAWC_SYS_dup, oldfd, 0, 0, 0, 0, 0);
}

#if defined(__x86_64__)
static long handle_dup2(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int oldfd = (int)args->a;
	int newfd = (int)args->b;
	if (tawcroot_fd_is_reserved(oldfd) ||
	    tawcroot_fd_is_reserved(newfd)) return EBADF_NEG;
	return TAWC_RAW(TAWC_SYS_dup2, oldfd, newfd, 0, 0, 0, 0);
}
#endif

static long handle_dup3(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int oldfd = (int)args->a;
	int newfd = (int)args->b;
	int flags = (int)args->c;
	if (tawcroot_fd_is_reserved(oldfd) ||
	    tawcroot_fd_is_reserved(newfd)) return EBADF_NEG;
	return TAWC_RAW(TAWC_SYS_dup3, oldfd, newfd, flags, 0, 0, 0);
}

static long handle_fcntl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd  = (int)args->a;
	int op  = (int)args->b;
	long a3 = args->c;
	if (tawcroot_fd_is_reserved(fd)) return EBADF_NEG;

	/* F_DUPFD/F_DUPFD_CLOEXEC: cap the requested minimum at base-1 so
	 * the kernel never lands the dup in our reserved range. The guest
	 * either gets a low fd or -EMFILE, both of which match what would
	 * happen on a process running near rlimit. */
	if (op == F_DUPFD || op == F_DUPFD_CLOEXEC) {
		if (a3 >= TAWCROOT_RESERVED_FD_BASE) {
			a3 = TAWCROOT_RESERVED_FD_BASE - 1;
		}
	}
	return TAWC_RAW(TAWC_SYS_fcntl, fd, op, a3, 0, 0, 0);
}

/* glibc's __closefrom_fallback opens /proc/self/fd, getdents64-iterates,
 * and close()s every fd >= start_fd. Each pass that closed at least one
 * fd triggers an lseek(0)+retry to handle "closes mid-iteration may
 * invalidate the cursor". With handle_close fake-succeeding for our
 * reserved fds (so they survive the guest's closefrom — a gpgme/curl
 * pre-exec hygiene routine), every retry pass sees the same reserved
 * fds, "closes" them again, retries forever. Pacman-key under the
 * in-app installer hangs at 100% CPU for this reason.
 *
 * Fix: when getdents64 reads /proc/<our pid>/fd, drop dirent entries
 * whose d_name parses to a number that the BPF close-trap predicate
 * recognises as reserved. The guest sees a /proc/self/fd that doesn't
 * mention our internal fds at all; closefrom finishes after one pass.
 *
 * Only filter when the dirfd resolves to /proc/self/fd or
 * /proc/<digits>/fd to avoid hiding a file literally named "1000" in
 * some unrelated user dir (vanishingly unlikely but cheap to guard).
 * The check is one readlinkat per getdents64 call. Caching across calls
 * adds complexity for negligible gain — non-procfs callers eat one
 * tiny extra syscall and move on. */

/* Cap the readlink probe; "/proc/<10-digit pid>/fd" fits in 22 bytes. */
#define PROC_FD_LINK_MAX     32

static long handle_getdents64(const tawcroot_syscall_args *args,
			      ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	void *buf = (void *)(uintptr_t)args->b;
	unsigned int count = (unsigned int)args->c;

	long n = TAWC_RAW(TAWC_SYS_getdents64, fd, (long)buf,
	                  (long)count, 0, 0, 0);
	if (n <= 0 || tawcroot_n_reserved_fds == 0) return n;

	/* Cheap check: only filter when the dirfd resolves to
	 * /proc/<self|digits>/fd. Non-proc dirfds short-circuit. */
	char proc_link[PROC_FD_LINK_MAX];
	char self_path[32];
	{
		const char *prefix = "/proc/self/fd/";
		size_t off = 0;
		while (prefix[off] && off + 1 < sizeof self_path) {
			self_path[off] = prefix[off];
			off++;
		}
		/* Append fd as decimal. */
		char tmp[12]; int tn = 0;
		unsigned int u = fd >= 0 ? (unsigned int)fd : 0;
		if (u == 0) tmp[tn++] = '0';
		while (u) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
		while (tn--) {
			if (off + 1 >= sizeof self_path) return n;
			self_path[off++] = tmp[tn];
		}
		self_path[off] = 0;
	}
	long ln = tawc_readlinkat(-100 /*AT_FDCWD*/, self_path,
	                          proc_link, sizeof proc_link);
	if (ln <= 0) return n;
	if (!tawcroot_dirent_filter_is_proc_fd_link(proc_link, ln)) return n;

	return tawcroot_dirent_filter_compact(buf, n,
	                                      tawcroot_reserved_fds,
	                                      tawcroot_n_reserved_fds);
}

void tawcroot_fd_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_close,       handle_close);
	tawcroot_dispatch_install(TAWC_SYS_close_range, handle_close_range);
	tawcroot_dispatch_install(TAWC_SYS_dup,         handle_dup);
	tawcroot_dispatch_install(TAWC_SYS_dup3,        handle_dup3);
	tawcroot_dispatch_install(TAWC_SYS_fcntl,       handle_fcntl);
	tawcroot_dispatch_install(TAWC_SYS_getdents64,  handle_getdents64);
#if defined(__x86_64__)
	tawcroot_dispatch_install(TAWC_SYS_dup2,        handle_dup2);
#endif
}
