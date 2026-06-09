/* Fd-shape syscall handlers — internal-fd protection.
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
#include "errno_neg.h"
#include "fdtab.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_uapi.h"
#include "usercopy.h"

int    tawcroot_reserved_fds[TAWCROOT_MAX_RESERVED_FDS];
size_t tawcroot_n_reserved_fds;

long tawcroot_fd_reserve(int fd)
{
	if (fd < 0) return TAWC_EBADF;
	if (tawcroot_n_reserved_fds >= TAWCROOT_MAX_RESERVED_FDS) {
		/* Table full: an unrecorded high fd would be invisible to
		 * both the BPF close trap and tawcroot_fd_is_reserved —
		 * i.e. not actually protected from the guest. Fail closed
		 * rather than hand back a pseudo-reserved fd. */
		return TAWC_ENOSPC;
	}
	long r = tawc_fcntl(fd, F_DUPFD_CLOEXEC, TAWCROOT_RESERVED_FD_BASE);
	if (r < 0) return r;
	tawc_close(fd);
	tawcroot_reserved_fds[tawcroot_n_reserved_fds++] = (int)r;
	return r;
}

static long handle_close(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	/* Reserved fds: lie. The guest sees success, our handler keeps the
	 * fd alive for downstream path translation. Together with the
	 * range rejections in close_range / dup2/3 / fcntl F_DUPFD this
	 * makes our reserved fds un-killable from the guest, so handler-
	 * side state for path translation can stay immutable post-init.
	 *
	 * The BPF filter normally routes close() here only for fds in the
	 * init-time reserved set, but Android's stacked filter can TRAP
	 * syscalls we didn't ask for — don't fake success for an fd that
	 * isn't actually ours; forward the real close. */
	if (!tawcroot_fd_is_reserved(fd))
		return TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
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
	if (tawcroot_fd_is_reserved(oldfd)) return TAWC_EBADF;
	return TAWC_RAW(TAWC_SYS_dup, oldfd, 0, 0, 0, 0, 0);
}

#if defined(__x86_64__)
/* Route through dup3 from the stub — Android's app-sandbox seccomp
 * filter rejects dup2 (NR 33) in favour of dup3 (NR 292), so a raw
 * dup2 re-issue inside the handler nests another SIGSYS while our
 * outer SIGSYS is auto-masked, and the kernel kills with default
 * action. Same shape as the accept→accept4 redirect below.
 *
 * Semantic difference: dup2(fd, fd) is a no-op returning fd, while
 * dup3(fd, fd, 0) is EINVAL. fcntl F_GETFD distinguishes "valid fd"
 * (return newfd) from "closed" (return EBADF). */
static long handle_dup2(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int oldfd = (int)args->a;
	int newfd = (int)args->b;
	if (tawcroot_fd_is_reserved(oldfd) ||
	    newfd >= (int)TAWCROOT_RESERVED_FD_BASE ||
	    tawcroot_fd_is_reserved(newfd)) return TAWC_EBADF;
	if (oldfd == newfd) {
		long r = tawc_fcntl(oldfd, F_GETFD, 0);
		return r < 0 ? r : (long)newfd;
	}
	return TAWC_RAW(TAWC_SYS_dup3, oldfd, newfd, 0, 0, 0, 0);
}
#endif

static long handle_dup3(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int oldfd = (int)args->a;
	int newfd = (int)args->b;
	int flags = (int)args->c;
	/* Reject the WHOLE reserved range for newfd, not just currently-
	 * reserved slots: a guest-owned fd inside the range would be
	 * skipped by close_range's trim and indistinguishable from ours
	 * to future reservations. */
	if (tawcroot_fd_is_reserved(oldfd) ||
	    newfd >= (int)TAWCROOT_RESERVED_FD_BASE ||
	    tawcroot_fd_is_reserved(newfd)) return TAWC_EBADF;
	return TAWC_RAW(TAWC_SYS_dup3, oldfd, newfd, flags, 0, 0, 0);
}

/* fchdir: reserved fds must behave as EBADF (fdtab.h contract) — an
 * untrapped fchdir(reserved_fd) would land the kernel cwd on the rootfs
 * or a bind src dir via a guest-visible route. Ordinary guest fds pass
 * through; they were handed out by translated openat and point inside
 * the view. */
static long handle_fchdir(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	return TAWC_RAW(TAWC_SYS_fchdir, fd, 0, 0, 0, 0, 0);
}

static long handle_fcntl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd  = (int)args->a;
	int op  = (int)args->b;
	long a3 = args->c;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;

	/* F_DUPFD/F_DUPFD_CLOEXEC: a dup landing in the reserved range
	 * would hand the guest one of our slots. Refuse with EINVAL —
	 * the same error the kernel gives when the requested minimum
	 * exceeds RLIMIT_NOFILE — rather than silently trimming, which
	 * would violate the "result >= arg" F_DUPFD contract. */
	if (op == F_DUPFD || op == F_DUPFD_CLOEXEC) {
		if (a3 >= TAWCROOT_RESERVED_FD_BASE) return TAWC_EINVAL;
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
	long ln = tawc_readlinkat(AT_FDCWD, self_path,
	                          proc_link, sizeof proc_link);
	if (ln <= 0) return n;
	if (!tawcroot_dirent_filter_is_proc_fd_link(proc_link, ln)) return n;

	return tawcroot_dirent_filter_compact(buf, n,
	                                      tawcroot_reserved_fds,
	                                      tawcroot_n_reserved_fds);
}

#if defined(__x86_64__)
/* poll(fds, nfds, timeout_ms) → ppoll(fds, nfds, &ts, NULL, 8). Same
 * shape as the dup2→dup3 / accept→accept4 redirects: Android's app-
 * sandbox seccomp filter RET_TRAPs the legacy poll(2) on x86_64,
 * preferring ppoll. A raw poll re-issued from the handler nests SIGSYS
 * while ours is auto-masked, killing the process.
 *
 * Convert: timeout_ms < 0 → NULL timespec (infinite); else timespec
 * derived from the millisecond value. The fifth arg (sigsetsize) is
 * required by the kernel ABI but its value is irrelevant when sigmask
 * is NULL; pass 8 to match a kernel-sized sigset_t. */
static long handle_poll(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	long fds_p   = args->a;
	long nfds    = args->b;
	int  tmo_ms  = (int)args->c;
	if (tmo_ms < 0) {
		return TAWC_RAW(TAWC_SYS_ppoll, fds_p, nfds, 0, 0, 8, 0);
	}
	/* The kernel writes the remaining time back into *tmo_p on return;
	 * `ts` is stack-local and discarded after the call, so the
	 * write-back is intentionally dropped. */
	struct { long tv_sec; long tv_nsec; } ts = {
		(long)tmo_ms / 1000,
		(long)(tmo_ms % 1000) * 1000000L,
	};
	return TAWC_RAW(TAWC_SYS_ppoll, fds_p, nfds, (long)&ts, 0, 8, 0);
}

/* epoll_wait(epfd, events, maxevents, timeout) →
 *     epoll_pwait(epfd, events, maxevents, timeout, NULL, 8).
 * Same shape and rationale as handle_poll above: Android's app-sandbox
 * seccomp filter RET_TRAPs the legacy epoll_wait(2) on x86_64. mio's
 * epoll backend issues epoll_wait directly and treats -ENOSYS as fatal
 * (wezterm: "polling for events: ENOSYS; terminating"). The first four
 * args are identical between the two; sigsetsize is irrelevant when
 * sigmask is NULL but the kernel ABI requires the slot — pass 8 for
 * symmetry with handle_poll. */
static long handle_epoll_wait(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return TAWC_RAW(TAWC_SYS_epoll_pwait,
	                args->a, args->b, args->c, args->d, 0, 8);
}
#endif

/* ioctl translation, primarily for the {TC,GET,SET}S2 family.
 *
 * Android's untrusted_app sepolicy whitelists tty ioctls via an
 * `allowxperm` set called `unpriv_tty_ioctls`. That set covers the
 * legacy variants (TCGETS / TCSETS / TCSETSW / TCSETSF, TIOCG/SWINSZ,
 * TIOCS/GPGRP, FIONREAD, FIONBIO, FIOCLEX/NCLEX) but on at least the
 * Android 15 emulator does NOT include the newer "termios2" variants
 * (TCGETS2 / TCSETS2 / TCSETSW2 / TCSETSF2) introduced for arbitrary-
 * baud support. The kernel's SELinux check rejects them with -EACCES
 * before the devpts driver even runs.
 *
 * Modern glibc's tcgetattr/tcsetattr issue TCGETS2 first and only
 * fall back to TCGETS on -EINVAL, NOT on -EACCES. Result: bash
 * (and every other glibc tty consumer) sees -EACCES from
 * tcgetattr(stdin), concludes stdin isn't a tty, and skips both
 * the prompt and readline — which is exactly what "lxterminal /
 * wezterm render but show no prompt or accept input" looks like.
 *
 * Strategy: try the native termios2 ioctl FIRST. The xperm gap is
 * Android-version- and vendor-specific — the OnePlus 9 honours
 * TCGETS2 fine — and on policies that allow it we want the kernel's
 * full struct termios2 (with the real c_ispeed/c_ospeed) rather than
 * a synthetic legacy view. Only on -EACCES (the SELinux xperm-deny
 * signature) do we fall back to the legacy ioctl. Other errors
 * (-ENOTTY, -EFAULT, -EINVAL, …) pass through unmodified — they're
 * legitimate kernel responses, not policy denials.
 *
 * Fallback details: the kernel-ABI structs are identical for the
 * first 36 bytes (4*tcflag_t + c_line + c_cc[19]); termios2 adds a
 * trailing speed_t c_ispeed and speed_t c_ospeed (8 bytes total).
 * For TCGETS2 we zero the speed slots — apps that care about
 * arbitrary baud are not relevant inside our pty-only environment,
 * and CBAUD bits in c_cflag carry the symbolic baud (B38400 etc.)
 * unchanged. For TCSETS2/W2/F2 we drop the speed fields, again
 * safe because the kernel reads CBAUD from c_cflag.
 *
 * All other ioctl numbers pass through unmodified. */

/* asm-generic/ioctl.h numbers (same layout for x86_64 and aarch64). */
#define TAWC_TCGETS    0x5401U
#define TAWC_TCSETS    0x5402U
#define TAWC_TCSETSW   0x5403U
#define TAWC_TCSETSF   0x5404U
#define TAWC_TCGETS2   0x802C542AU  /* _IOR('T', 0x2A, struct termios2) */
#define TAWC_TCSETS2   0x402C542BU  /* _IOW('T', 0x2B, struct termios2) */
#define TAWC_TCSETSW2  0x402C542CU  /* _IOW('T', 0x2C, struct termios2) */
#define TAWC_TCSETSF2  0x402C542DU  /* _IOW('T', 0x2D, struct termios2) */

#define TAWC_KERN_TERMIOS_SIZE   36  /* asm/termbits.h: 4*4 + 1 + 19 */
#define TAWC_KERN_TERMIOS2_TAIL   8  /* speed_t c_ispeed + speed_t c_ospeed */

/* TCGETS2 fallback: kernel writes 36 bytes via TCGETS, then we zero
 * the trailing 8 speed bytes the guest expects from a termios2. */
static long handle_tcgets2_fallback(long fd, long arg)
{
	unsigned char buf[TAWC_KERN_TERMIOS_SIZE];
	long rv = TAWC_RAW(TAWC_SYS_ioctl, fd,
	                   (long)TAWC_TCGETS, (long)buf, 0, 0, 0);
	if (rv < 0) return rv;
	long e = tawc_copy_to_guest((void *)(uintptr_t)arg,
	                            buf, sizeof buf);
	if (e < 0) return TAWC_EFAULT;
	unsigned char zero[TAWC_KERN_TERMIOS2_TAIL] = {0};
	e = tawc_copy_to_guest((void *)(uintptr_t)
	                       (arg + TAWC_KERN_TERMIOS_SIZE),
	                       zero, sizeof zero);
	if (e < 0) return TAWC_EFAULT;
	return rv;
}

/* TCSETS{,W,F}2 fallback: pull the first 36 bytes from the guest's
 * termios2 (drop the speed_t tail) and feed them to the legacy
 * setter. */
static long handle_tcsets2_fallback(long fd, unsigned int cmd, long arg)
{
	unsigned char buf[TAWC_KERN_TERMIOS_SIZE];
	long e = tawc_copy_from_guest(buf, sizeof buf,
	                              (const void *)(uintptr_t)arg);
	if (e < 0) return TAWC_EFAULT;
	unsigned int legacy =
		(cmd == TAWC_TCSETS2)  ? TAWC_TCSETS  :
		(cmd == TAWC_TCSETSW2) ? TAWC_TCSETSW : TAWC_TCSETSF;
	return TAWC_RAW(TAWC_SYS_ioctl, fd,
	                (long)legacy, (long)buf, 0, 0, 0);
}

static long handle_ioctl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	long fd  = args->a;
	unsigned int cmd = (unsigned int)args->b;
	long arg = args->c;

	if (cmd == TAWC_TCGETS2 || cmd == TAWC_TCSETS2 ||
	    cmd == TAWC_TCSETSW2 || cmd == TAWC_TCSETSF2) {
		long rv = TAWC_RAW(TAWC_SYS_ioctl, fd, (long)cmd, arg,
		                   args->d, args->e, args->f);
		if (rv != TAWC_EACCES) return rv;
		/* SELinux xperm denial — fall back to the legacy ioctl
		 * the policy allowlists. NULL arg can't reach this path
		 * because the kernel would have returned -EFAULT, not
		 * -EACCES; defensive check anyway. */
		if (!arg) return TAWC_EFAULT;
		if (cmd == TAWC_TCGETS2)
			return handle_tcgets2_fallback(fd, arg);
		return handle_tcsets2_fallback(fd, cmd, arg);
	}
	return TAWC_RAW(TAWC_SYS_ioctl, fd, args->b, arg,
	                args->d, args->e, args->f);
}

void tawcroot_fd_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_close,       handle_close);
	tawcroot_dispatch_install(TAWC_SYS_close_range, handle_close_range);
	tawcroot_dispatch_install(TAWC_SYS_dup,         handle_dup);
	tawcroot_dispatch_install(TAWC_SYS_dup3,        handle_dup3);
	tawcroot_dispatch_install(TAWC_SYS_fchdir,      handle_fchdir);
	tawcroot_dispatch_install(TAWC_SYS_fcntl,       handle_fcntl);
	tawcroot_dispatch_install(TAWC_SYS_getdents64,  handle_getdents64);
	tawcroot_dispatch_install(TAWC_SYS_ioctl,       handle_ioctl);
#if defined(__x86_64__)
	tawcroot_dispatch_install(TAWC_SYS_dup2,        handle_dup2);
	tawcroot_dispatch_install(TAWC_SYS_poll,        handle_poll);
	tawcroot_dispatch_install(TAWC_SYS_epoll_wait,  handle_epoll_wait);
#endif
}
