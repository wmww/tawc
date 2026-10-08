/* Fd-shape syscall handlers — internal-fd protection.
 *
 * See include/fdtab.h for the rationale. Every handler here is the
 * minimum viable wrapper around the corresponding host syscall; the
 * only intercept is "if a guest argument names a reserved fd, lie".
 *
 * This is NOT a security boundary — see notes/tawcroot/overview.md §"What it
 * explicitly is not". A guest that wants to corrupt our state has
 * other avenues (e.g. mmap over our text). The intercept is for
 * accidental damage from libraries that close-all-fds before exec
 * (libc init), test harnesses that close_range() everything, or
 * pacman-style tools that drop fd tables on fork. Those workloads
 * are common; defending against them is cheap.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "dirent_filter.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "linkstore.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "shm.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

int    tawcroot_reserved_fds[TAWCROOT_MAX_RESERVED_FDS];
size_t tawcroot_n_reserved_fds;

/* The SIGSYS handler reads this table lock-free from sibling threads,
 * and reserves happen from handler context too (shm_open, chroot swap,
 * lazy linkstore fds, socket parents), so two guest threads can be in
 * here at once. Claim a slot with a compare-exchange from "not live" to
 * `fd`, then raise the reader-visible high-water mark; both stores are
 * releases, and tawcroot_fd_is_reserved's acquire load of the count
 * pairs with them. The window where the fd exists but is not yet
 * published is inherent (the fd is born in the kernel before any store)
 * — a sibling thread's close_fds-style sweep in exactly that instant
 * can still close it; bounded, unfixable without stopping the world. */
long tawcroot_fd_record_reserved(int fd)
{
	if (fd < TAWCROOT_RESERVED_FD_BASE) return TAWC_EINVAL;
	for (size_t i = 0; i < TAWCROOT_MAX_RESERVED_FDS; i++) {
		int slot = __atomic_load_n(&tawcroot_reserved_fds[i],
					   __ATOMIC_RELAXED);
		if (slot >= TAWCROOT_RESERVED_FD_BASE) continue;  /* live */
		if (!__atomic_compare_exchange_n(&tawcroot_reserved_fds[i],
						 &slot, fd, 0,
						 __ATOMIC_RELEASE,
						 __ATOMIC_RELAXED))
			continue;  /* lost this slot to another thread */
		size_t n = __atomic_load_n(&tawcroot_n_reserved_fds,
					   __ATOMIC_RELAXED);
		while (n < i + 1 &&
		       !__atomic_compare_exchange_n(&tawcroot_n_reserved_fds,
						    &n, i + 1, 0,
						    __ATOMIC_RELEASE,
						    __ATOMIC_RELAXED))
			;
		return 0;
	}
	return TAWC_ENOSPC;
}

void tawcroot_fd_forget_reserved(int fd)
{
	if (fd < TAWCROOT_RESERVED_FD_BASE) return;
	size_t n = __atomic_load_n(&tawcroot_n_reserved_fds, __ATOMIC_ACQUIRE);
	for (size_t i = 0; i < n; i++) {
		int slot = __atomic_load_n(&tawcroot_reserved_fds[i],
					   __ATOMIC_RELAXED);
		if (slot != fd) continue;
		/* Leave the high-water mark alone: the slot stays inside the
		 * readers' scan range, holding a value no fd can equal. */
		__atomic_store_n(&tawcroot_reserved_fds[i],
				 TAWCROOT_RESERVED_FD_NONE, __ATOMIC_RELEASE);
		return;
	}
}

long tawcroot_fd_reserve(int fd)
{
	if (fd < 0) return TAWC_EBADF;
	long r = tawc_fcntl(fd, F_DUPFD_CLOEXEC, TAWCROOT_RESERVED_FD_BASE);
	if (r < 0) return r;
	/* Table full: an unrecorded high fd would be invisible to
	 * tawcroot_fd_is_reserved — i.e. not actually protected from the
	 * guest. Fail closed rather than hand back a pseudo-reserved fd,
	 * dropping the dup and leaving the caller's original open. */
	long rec = tawcroot_fd_record_reserved((int)r);
	if (rec < 0) {
		tawc_close((int)r);
		return rec;
	}
	tawc_close(fd);
	return r;
}

static long handle_close(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	/* Reserved fds: lie. The guest sees success, our handler keeps the
	 * fd alive for downstream path translation. Together with the
	 * skip in close_range and the newfd check in dup2/3 this makes our
	 * reserved fds un-killable from the guest, so handler-side state
	 * for path translation can stay immutable post-init.
	 *
	 * The BPF close trap covers the whole half-space above the base,
	 * which is a superset of the table (the guest's own high fds land
	 * here too) — so don't fake success for an fd that isn't actually
	 * ours; forward the real close. */
	if (!tawcroot_fd_is_reserved(fd))
		return TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
	return 0;
}

/* close_range(2) uapi. The flags live in <linux/close_range.h>, which
 * the <linux/fcntl.h> in tawc_uapi.h doesn't pull in and which older
 * kernel headers don't ship at all, so pin them here along with the
 * clone flag the UNSHARE variant needs and the RLIMIT the fallback
 * queries. */
#define TAWC_CLOSE_RANGE_UNSHARE 0x02U
#define TAWC_CLOSE_RANGE_CLOEXEC 0x04U
#define TAWC_CLONE_FILES         0x0400U
#define TAWC_RLIMIT_NOFILE       7

/* One getdents64 batch for the close_range walk. /proc/<pid>/fd names
 * are bare decimals, so a record is at most 19 + 11 bytes rounded up
 * to 32 — this holds 16 fds per batch and keeps the frame well inside
 * the 1 KiB handler cap (stack_budget.h). */
#define CLOSE_RANGE_DENTS_BUF 512

/* Apply the guest's requested action to one fd. Per-fd errors are
 * ignored, exactly as the kernel's close_range does. */
static void close_range_act(unsigned int fd, unsigned int flags)
{
	if (flags & TAWC_CLOSE_RANGE_CLOEXEC)
		(void)TAWC_RAW(TAWC_SYS_fcntl, (long)fd, F_SETFD,
			       FD_CLOEXEC, 0, 0, 0);
	else
		(void)TAWC_RAW(TAWC_SYS_close, (long)fd, 0, 0, 0, 0, 0);
}

/* Last-resort walk when /proc/self/fd can't be opened (EMFILE/ENFILE,
 * no /proc mounted): try every number up to the soft RLIMIT_NOFILE.
 * Slow, but bounded and correct, and only reachable when the process
 * is already out of descriptors. */
static long close_range_linear(unsigned int first, unsigned int last,
			       unsigned int flags)
{
	struct { unsigned long long cur, max; } rl = { 0, 0 };
	unsigned int hi = last;
	if (TAWC_RAW(TAWC_SYS_prlimit64, 0, TAWC_RLIMIT_NOFILE,
		     0, (long)&rl, 0, 0) == 0 &&
	    rl.cur > 0 && rl.cur - 1 < (unsigned long long)hi)
		hi = (unsigned int)(rl.cur - 1);
	if (first > hi) return 0;
	for (unsigned int fd = first;; fd++) {
		if (!tawcroot_fd_is_reserved((int)fd))
			close_range_act(fd, flags);
		if (fd == hi) break;
	}
	return 0;
}

/* One pass over an already-rewound /proc/self/fd. Returns the number of
 * fds acted on, or -errno if getdents64 itself failed. */
static long close_range_pass(int dirfd, unsigned int first,
			     unsigned int last, unsigned int flags)
{
	char buf[CLOSE_RANGE_DENTS_BUF];
	long acted = 0;
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_getdents64, dirfd, (long)buf,
				  (long)sizeof buf, 0, 0, 0);
		if (n < 0) return n;
		if (n == 0) return acted;
		for (long i = 0; i + 19 <= n;) {
			unsigned short reclen;
			__builtin_memcpy(&reclen, buf + i + 16, 2);
			if (reclen < 20 || i + (long)reclen > n) break;
			const char *name = buf + i + 19;
			/* The kernel NUL-terminates inside the record; bail
			 * on the buffer rather than trust it blindly. */
			long k = i + 19;
			while (k < i + reclen && buf[k]) k++;
			if (k == i + reclen) break;
			i += reclen;

			int fd;
			if (!tawcroot_dirent_filter_dname_to_fd(name, &fd))
				continue;  /* "." / ".." */
			if ((unsigned int)fd < first ||
			    (unsigned int)fd > last) continue;
			if (fd == dirfd) continue;  /* ours, closed below */
			if (tawcroot_fd_is_reserved(fd)) continue;
			close_range_act((unsigned int)fd, flags);
			acted++;
		}
	}
}

/* close_range is EMULATED, never re-issued.
 *
 * Android's zygote seccomp policy is generated from bionic's syscall
 * list, and bionic only gained close_range at API 34 — so every older
 * policy (the app's minSdk is 29) RET_TRAPs NR 436. SIGSYS is masked
 * inside our own SIGSYS handler (sa_mask = ~0), so a raw 436 from here
 * nests a trap the kernel answers by force-killing the process: no
 * log, no exit code. glibc's closefrom-before-exec hits this on every
 * gpg/gpg-agent/dirmngr spawn, which surfaces as pacman's "GPGME
 * error: Invalid crypto engine" (wmww/tawc#14). seccomp runs before
 * the kernel's ENOSYS check, so a newer kernel doesn't help either.
 *
 * So: walk /proc/self/fd with syscalls every Android policy allows
 * (openat, getdents64, close, fcntl, lseek), skipping our reserved
 * slots. Cost scales with open fds rather than with RLIMIT_NOFILE,
 * and the general rule this is an instance of — a handler must never
 * emit a syscall newer than the oldest Android policy we support —
 * is in notes/tawcroot/sigsys-handler.md.
 *
 * Two shapes preceded this one: trimming `last` to the base, which
 * leaked every guest fd at or above it across exec, and then issuing
 * one raw close_range per gap between reserved fds. The exact
 * membership test both of those introduced is what keeps
 * CLOSE_RANGE_CLOEXEC honest — our non-CLOEXEC shm fds must not be
 * swept into CLOEXEC — and the walk keeps it. */
static long handle_close_range(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	(void)uc;
	unsigned int first = (unsigned int)args->a;
	unsigned int last  = (unsigned int)args->b;
	unsigned int flags = (unsigned int)args->c;

	if (first > last) return TAWC_EINVAL;
	if (flags & ~(TAWC_CLOSE_RANGE_UNSHARE | TAWC_CLOSE_RANGE_CLOEXEC))
		return TAWC_EINVAL;

	/* unshare(2) is ancient and allowed by every Android app policy
	 * (verified on the emulator), so this one is safe to emit raw.
	 * Our own filter traps NR unshare for the guest, but the raw stub
	 * is IP-allowlisted past it. */
	if (flags & TAWC_CLOSE_RANGE_UNSHARE) {
		long rv = TAWC_RAW(TAWC_SYS_unshare, TAWC_CLONE_FILES,
				   0, 0, 0, 0, 0);
		if (rv < 0) return rv;
	}

	long dirfd = TAWC_RAW(TAWC_SYS_openat, AT_FDCWD,
			      (long)"/proc/self/fd",
			      O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	if (dirfd < 0) return close_range_linear(first, last, flags);

	/* Closing mutates the directory we're iterating, so repeat until
	 * a pass finds nothing left to close (glibc __closefrom_fallback's
	 * shape). CLOEXEC mode doesn't remove entries, so one pass does. */
	for (;;) {
		long acted = close_range_pass((int)dirfd, first, last, flags);
		if (acted < 0) {
			tawc_close((int)dirfd);
			return close_range_linear(first, last, flags);
		}
		if (acted == 0 || (flags & TAWC_CLOSE_RANGE_CLOEXEC)) break;
		if (tawc_lseek((int)dirfd, 0, SEEK_SET) < 0) break;
	}
	tawc_close((int)dirfd);
	return 0;
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
	/* Only the fds we actually hold are off limits. `newfd` anywhere
	 * else — including above the base — is the guest's to claim; the
	 * kernel keeps the two sets disjoint from then on (fdtab.h). */
	if (tawcroot_fd_is_reserved(oldfd) ||
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

	/* F_DUPFD/F_DUPFD_CLOEXEC need no guard: the kernel returns the
	 * lowest free fd at or above the requested minimum, and our
	 * reserved fds are not free. (This used to -EINVAL any minimum at
	 * or above the base, which broke guests holding that many fds.) */
	long sr;
	if (tawcroot_shm_seal_fcntl(fd, op, a3, &sr)) return sr;
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

	/* Classify the dirfd via its /proc link (one readlinkat per
	 * getdents64 call — the probe buffer must be big enough to
	 * classify rootfs-view dirs too, not just the /proc/<pid>/fd
	 * shape, so it lives in a scratch slot). */
	char self_path[32];
	if (tawc_proc_fd_path(self_path, sizeof self_path, fd, 0) < 0)
		return n;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *proc_link = scratch->buf[0];
	long ln = tawc_readlinkat(AT_FDCWD, self_path,
	                          proc_link, TAWCROOT_PATH_SCRATCH_SIZE);
	if (ln <= 0 || ln >= (long)TAWCROOT_PATH_SCRATCH_SIZE) return n;

	/* Hardlink emulation: emulated names must not advertise DT_LNK
	 * (type-trusting walkers would never stat into the fixed-up stat
	 * handlers — the file would silently vanish from find/rg results).
	 * Rewrite DT_LNK → DT_UNKNOWN for rootfs-view dirs — in-buffer
	 * byte flip, zero extra syscalls beyond the probe that already
	 * ran. Only when a store is open; without one the flip would cost
	 * lstats for nothing. */
	if (tawcroot_store_link_fd >= 0 &&
	    tawcroot_rootfs_host_path_len > 0 &&
	    (size_t)ln >= tawcroot_rootfs_host_path_len &&
	    memcmp(proc_link, tawcroot_rootfs_host_path,
	           tawcroot_rootfs_host_path_len) == 0 &&
	    ((size_t)ln == tawcroot_rootfs_host_path_len ||
	     proc_link[tawcroot_rootfs_host_path_len] == '/'))
		return tawcroot_dirent_filter_delink_types(buf, n);

	if (!tawcroot_dirent_filter_is_proc_fd_link(proc_link, ln)) {
		/* Bind dirs need the same rewrite — the NOFOLLOW stat
		 * fixups apply inside binds, so a token name there (legacy
		 * of the pre-gate code; linkat no longer plants them) would
		 * otherwise say DT_LNK while fstatat says S_IFREG. Gate on
		 * the store's own fs first: cross-fs binds (/system,
		 * procfs) cannot hold objects, so they skip both the
		 * reverse-translation walk and the flip. */
		if (tawcroot_store_link_fd >= 0 && tawcroot_store_dev) {
			struct stat dst;
			char *gv = scratch->buf[1];
			if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&dst,
				     0, 0, 0, 0) == 0 &&
			    (unsigned long)dst.st_dev == tawcroot_store_dev &&
			    tawcroot_host_path_to_guest_abs(
				    proc_link, (size_t)ln, gv,
				    TAWCROOT_PATH_SCRATCH_SIZE) > 0)
				return tawcroot_dirent_filter_delink_types(
					buf, n);
		}
		return n;
	}

	/* Re-issue until a batch survives filtering or the kernel truly
	 * EOFs. A batch containing ONLY reserved-fd entries (tiny guest
	 * buffer, or a dir of mostly our fds) compacts to 0 — returning
	 * that to the guest is a false end-of-directory, hiding the real
	 * entries in later batches. Loop instead. */
	for (;;) {
		long compacted = tawcroot_dirent_filter_compact(
			buf, n, tawcroot_reserved_fds,
			tawcroot_n_reserved_fds);
		if (compacted > 0) return compacted;
		/* Whole batch filtered away — pull the next one. */
		n = TAWC_RAW(TAWC_SYS_getdents64, fd, (long)buf,
		             (long)count, 0, 0, 0);
		if (n <= 0) return n;  /* true EOF (0) or error */
	}
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

/* Legacy getdents(2) (NR 78). Unlike poll/dup2/epoll_wait above,
 * Android's filter ALLOWS this one — untrapped it went raw to the
 * kernel, so legacy callers saw reserved fds in /proc/self/fd again
 * and emulated hardlinks as DT_LNK. Trap it ourselves and route
 * through handle_getdents64 (same arg shape: fd, buf, count), then
 * repack the records into legacy layout in place. In-place works
 * because both layouts give identical reclen for the same name and
 * legacy d_ino/d_off are 64-bit on x86_64; see
 * tawcroot_dirent_filter_repack_legacy. */
static long handle_getdents(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	long n = handle_getdents64(args, uc);
	if (n <= 0) return n;
	return tawcroot_dirent_filter_repack_legacy(
		(void *)(uintptr_t)args->b, n);
}

/* The legacy fd/poll-family redirects. Each was confirmed RET_TRAPped
 * by the real emulator filter (empirical audit: notes/tawcroot/
 * status.md) — Android allowlists only the flags-taking modern variant.
 * Untrapped they'd -ENOSYS. Same stub-reissue rule as handle_poll: a
 * raw legacy re-issue would re-trap and nest SIGSYS, so we issue the
 * MODERN sibling, which the filter allows. */

/* select(nfds,r,w,e,timeval*) → pselect6(nfds,r,w,e,timespec*,NULL).
 * The fd-set pointers pass straight to the kernel. Timeout differs in
 * both type (timeval→timespec) and write-back: legacy select updates
 * *timeout with the unslept remainder; pselect6 does not, and — as in
 * handle_poll — we intentionally drop that write-back. The 6th arg is
 * a {sigset*, size} pointer, NULL here (no signal mask). */
static long handle_select(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if (args->e == 0)
		return TAWC_RAW(TAWC_SYS_pselect6, args->a, args->b,
		                args->c, args->d, 0, 0);
	struct { long tv_sec; long tv_usec; } tv;
	long cr = tawc_copy_from_guest(&tv, sizeof tv, (const void *)args->e);
	if (cr < 0) return cr;
	/* Kernel select rejects negative fields but NORMALIZES an
	 * overflowing tv_usec into seconds (pselect6 would EINVAL it). */
	if (tv.tv_sec < 0 || tv.tv_usec < 0) return TAWC_EINVAL;
	struct { long tv_sec; long tv_nsec; } ts = {
		tv.tv_sec + tv.tv_usec / 1000000L,
		(tv.tv_usec % 1000000L) * 1000L,
	};
	return TAWC_RAW(TAWC_SYS_pselect6, args->a, args->b, args->c,
	                args->d, (long)&ts, 0);
}

static long handle_pipe(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return TAWC_RAW(TAWC_SYS_pipe2, args->a, 0, 0, 0, 0, 0);
}

static long handle_eventfd(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return TAWC_RAW(TAWC_SYS_eventfd2, args->a, 0, 0, 0, 0, 0);
}

static long handle_signalfd(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return TAWC_RAW(TAWC_SYS_signalfd4, args->a, args->b, args->c,
	                0, 0, 0);
}

static long handle_epoll_create(const tawcroot_syscall_args *args,
                                ucontext_t *uc)
{
	(void)uc;
	/* Legacy `size` is only a hint, but the kernel still validates
	 * it; the value itself is meaningless to the modern call. */
	if ((int)args->a <= 0) return TAWC_EINVAL;
	return TAWC_RAW(TAWC_SYS_epoll_create1, 0, 0, 0, 0, 0, 0);
}

static long handle_inotify_init(const tawcroot_syscall_args *args,
                                ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_RAW(TAWC_SYS_inotify_init1, 0, 0, 0, 0, 0, 0);
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

/* Read-only netdevice ioctls (SIOCGIFNAME, SIOCGIFFLAGS, ...) are
 * served by the kernel's dev_ioctl the same way on any socket family,
 * but Android's app policy allowlists them only on inet sockets.
 * glibc's if_indextoname / if_nametoindex issue them on an AF_UNIX
 * socket (__opensock tries AF_UNIX first) and get EACCES — libtorrent
 * then can't name any route's device, decides no interface reaches the
 * internet, and skips every tracker. On EACCES, retry once on a
 * transient AF_INET socket. */
static int is_netdev_get_ioctl(unsigned int cmd)
{
	switch (cmd) {
	case 0x8910:  /* SIOCGIFNAME */
	case 0x8912:  /* SIOCGIFCONF */
	case 0x8913:  /* SIOCGIFFLAGS */
	case 0x891d:  /* SIOCGIFMETRIC */
	case 0x8921:  /* SIOCGIFMTU */
	case 0x8933:  /* SIOCGIFINDEX */
	case 0x8942:  /* SIOCGIFTXQLEN */
		return 1;
	}
	return 0;
}

static long netdev_ioctl_on_inet(unsigned int cmd, long arg)
{
	long s = TAWC_RAW(TAWC_SYS_socket, 2 /* AF_INET */,
			  2 /* SOCK_DGRAM */ | O_CLOEXEC, 0, 0, 0, 0);
	if (s < 0) return TAWC_EACCES;
	long rv = TAWC_RAW(TAWC_SYS_ioctl, s, (long)cmd, arg, 0, 0, 0);
	tawc_close((int)s);
	return rv;
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
	long rv = TAWC_RAW(TAWC_SYS_ioctl, fd, args->b, arg,
	                   args->d, args->e, args->f);
	if (rv == TAWC_EACCES && is_netdev_get_ioctl(cmd))
		return netdev_ioctl_on_inet(cmd, arg);
	return rv;
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
	tawcroot_dispatch_install(TAWC_SYS_getdents,     handle_getdents);
	tawcroot_dispatch_install(TAWC_SYS_select,       handle_select);
	tawcroot_dispatch_install(TAWC_SYS_pipe,         handle_pipe);
	tawcroot_dispatch_install(TAWC_SYS_eventfd,      handle_eventfd);
	tawcroot_dispatch_install(TAWC_SYS_signalfd,     handle_signalfd);
	tawcroot_dispatch_install(TAWC_SYS_epoll_create, handle_epoll_create);
	tawcroot_dispatch_install(TAWC_SYS_inotify_init, handle_inotify_init);
#endif
}
