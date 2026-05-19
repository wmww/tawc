/* Runtime-control syscall handlers — guest-side denials and shadow
 * virtualization for operations that would otherwise compromise
 * tawcroot's own invariants.
 *
 * Surface (notes/tawcroot.md §"Guest signal/seccomp control"):
 *   - `seccomp(2)` / `prctl(PR_SET_SECCOMP)`: refuse with -EPERM. A
 *     guest filter on top of ours would RET_KILL or RET_ERRNO before
 *     our trap, or RET_TRAP into a handler the guest owns — all of
 *     which break translation. Programs that probe with EPERM fall
 *     back to a no-filter path; pacman/glibc init don't depend on
 *     stacking.
 *   - `rt_sigaction(SIGSYS, ...)`: virtualize. The guest's intended
 *     disposition lives in a shadow buffer; reads/writes of SIGSYS
 *     hit the shadow and never the kernel. The real kernel disposition
 *     stays our SIGSYS handler.
 *   - `rt_sigprocmask`: pass through, but transparently strip SIGSYS
 *     from any new mask the guest installs and OR-in the shadow bit
 *     when reporting the previous mask. Guest reads back what it set;
 *     the kernel never actually blocks SIGSYS, so traps continue
 *     reaching our handler.
 *   - Other signals are unaffected.
 */

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "raw_sys.h"
#include "signal_shadow.h"
#include "syscalls_control.h"
#include "sysnr.h"
#include "usercopy.h"

#ifndef PR_GET_SECCOMP
# define PR_GET_SECCOMP 21
#endif
#ifndef PR_SET_SECCOMP
# define PR_SET_SECCOMP 22
#endif

#ifndef SIGSYS
# define SIGSYS 31
#endif
/* sigset bit position is (signo - 1). */
#define SIGSYS_BIT (1ULL << (SIGSYS - 1))

#ifndef SIG_BLOCK
# define SIG_BLOCK   0
# define SIG_UNBLOCK 1
# define SIG_SETMASK 2
#endif

/* Shadow state for the guest's SIGSYS view lives in signal_shadow.c.
 * Two pieces, scoped differently:
 *   - The sigaction is process-global (POSIX dispositions are
 *     process-wide), protected by a seqlock against concurrent
 *     sigaction(SIGSYS) calls from multiple threads.
 *   - The "blocked" bit is per-thread (POSIX masks are per-thread),
 *     stored in a TID-keyed open-address table — the kernel mask in
 *     uc->uc_sigmask is per-thread for free, but we strip SIGSYS from
 *     it to keep traps coming, so the shadow has to live separately.
 * Sizing of the action buffer (TAWC_KERN_SIGACTION_SIZE) is exposed
 * via signal_shadow.h:
 *   x86_64: handler ptr (8) + flags (8) + sa_restorer (8) + mask (8) = 32
 *   aarch64: handler ptr (8) + flags (8) + mask (8)              = 24
 * (Earlier revs oversized to 64; over-read past the guest struct in
 * both directions, review finding B2.) */

/* Refuse guest filter install. We can't honestly stack the guest's BPF
 * filter on top of ours: it could KILL_PROCESS our raw_syscall stub,
 * return ERRNO before our path-translation trap, or RET_TRAP into a
 * guest-owned SIGSYS path that tawcroot virtualizes away.
 *
 * Firefox currently tolerates EPERM here without UI warnings on the
 * tested Arch ARM rootfs (Firefox 150.0.3 / OnePlus 9, 2026-05-19).
 * Earlier notes claimed this tripped a libhybris bionic-Q linker
 * teardown abort; that was not reproducible on the current stack.
 *
 * SECCOMP_GET_ACTION_AVAIL and other read-only ops pass through to
 * the kernel verbatim because they don't change state. */
static long handle_seccomp(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	unsigned int op = (unsigned int)args->a;
	/* SECCOMP_SET_MODE_STRICT = 0, SECCOMP_SET_MODE_FILTER = 1. */
	if (op == 0 || op == 1) return TAWC_EPERM;
	/* SECCOMP_GET_ACTION_AVAIL = 2, SECCOMP_GET_NOTIF_SIZES = 3, etc.
	 * Read-only — pass through. */
	return TAWC_RAW(TAWC_SYS_seccomp, args->a, args->b, args->c,
			args->d, args->e, 0);
}

/* Defense-in-depth denials. Trapped so the guest can't mutate kernel
 * state our path-translation layer assumes is fixed: pivot_root would
 * desync our root-relative bookkeeping (we don't model mounts, so the
 * "pivot the rootfs onto a sibling mount" semantics have nothing to
 * pivot to); mount/umount2 would tear down our setup binds (/dev/shm,
 * /proc, libhybris stage); unshare/setns would hand the guest a
 * namespace where our fd-relative /proc walks no longer name what we
 * think they name. Lying with -EPERM is the same posture proot takes.
 *
 * chroot is NOT in this list — it has its own handler in chroot.c
 * that swaps the root-view bookkeeping. */
static long fake_eperm(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_EPERM;
}

/* io_uring_setup: deny with -ENOSYS so guest libraries fall back to
 * syscall-based I/O which we can translate. The plan
 * (notes/tawcroot.md "Open questions" #1) classifies a passed-through
 * io_uring as a *correctness* hazard, not just a missing feature: the
 * kernel reads SQEs from app-shared memory, sees host-relative paths,
 * and silently opens host files — bypassing every translation rule.
 * Programs that probe with -ENOSYS fall back to non-uring paths
 * cleanly. (Review finding D4.)
 *
 * io_uring_register and io_uring_enter trap with the same -ENOSYS for
 * defense-in-depth: with io_uring_setup denied the guest can't create
 * a ring fd, but a ring fd inherited from a non-tawcroot parent across
 * exec would otherwise sail past us untranslated. Trapping the post-
 * setup syscalls makes "no io_uring traffic ever escapes" enforceable
 * independently of the stacked Android filter. See notes/tawcroot.md
 * "io_uring MVP behavior". */
static long handle_io_uring_deny(const tawcroot_syscall_args *args,
				 ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_ENOSYS;
}

/* clone3: deny with -ENOSYS so glibc's __clone falls back to the
 * older clone(2) syscall (NR 220 aarch64 / NR 56 x86_64). Android's
 * untrusted_app filter on Android 14 (and possibly later) RET_KILLs
 * clone3 — at the highest action precedence, that overrides any of
 * our filter actions. We can't intercept the kernel's KILL, but we
 * can make the GUEST not issue clone3 in the first place: trapping
 * via our own filter and returning -ENOSYS happens BEFORE the kernel
 * sees the syscall enter the dispatcher path that runs Android's
 * filter — actually no, all filters are evaluated at syscall entry
 * and the most restrictive action wins. So this only works if Android
 * RET_TRAPs (not RET_KILLs) clone3. Empirically on Android 14 the
 * trap fires via our handler — we get [sigsys] nr=435 — meaning
 * Android's policy is RET_TRAP-or-ALLOW, not RET_KILL. Returning
 * -ENOSYS from our handler causes glibc to set its "clone3 missing"
 * flag and use clone() going forward. */
static long handle_clone3(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_ENOSYS;
}


static long handle_prctl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int op = (int)args->a;
	/* PR_SET_SECCOMP — same guest-filter-stacking rationale as
	 * handle_seccomp above. */
	if (op == PR_SET_SECCOMP) return TAWC_EPERM;
	return TAWC_RAW(TAWC_SYS_prctl, args->a, args->b, args->c,
			args->d, args->e, 0);
}

static long handle_rt_sigaction(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	int    sig         = (int)args->a;
	const void *act    = (const void *)(uintptr_t)args->b;
	void  *oldact      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sig != SIGSYS) {
		return TAWC_RAW(TAWC_SYS_rt_sigaction, args->a, args->b,
				args->c, args->d, 0, 0);
	}

	if (sigsetsize != 8) return TAWC_EINVAL;

	/* Read the guest's new action into a stack-local buffer FIRST so
	 * we know whether the call would have succeeded before exposing
	 * stale shadow contents to a faulting oldact write. */
	unsigned char incoming[TAWC_KERN_SIGACTION_SIZE];
	int have_incoming = 0;
	if (act) {
		long e = tawc_copy_from_guest(incoming, sizeof incoming, act);
		if (e < 0) return TAWC_EFAULT;
		have_incoming = 1;
	}

	if (oldact) {
		unsigned char snap[TAWC_KERN_SIGACTION_SIZE];
		tawc_sigshadow_action_get(snap);
		long e = tawc_copy_to_guest(oldact, snap, sizeof snap);
		if (e < 0) return TAWC_EFAULT;
	}

	if (have_incoming)
		tawc_sigshadow_action_set(incoming);
	return 0;
}

/* Locate the 8-byte kernel sigset embedded in ucontext_t->uc_sigmask.
 * Bionic's <ucontext.h> exposes it as `sigset64_t` whose first member
 * is a `__bionic_sigset_t __bits` of 8 bytes; we treat it as a u64.
 * Modifying *here* is what makes the change persist across sigreturn —
 * a kernel-level rt_sigprocmask issued during a SIGSYS handler is
 * *undone* by sigreturn restoring task->blocked from this field. */
static uint64_t *uc_sigmask_word(ucontext_t *uc)
{
	return (uint64_t *)&uc->uc_sigmask;
}

/* State-mutation order:
 *  1. read guest_set into a local
 *  2. compute new kmask + new_blocked locally
 *  3. mutate uc->uc_sigmask in place
 *  4. copy_to_guest old mask; on EFAULT, roll back uc->uc_sigmask
 *  5. publish blocked shadow via tawc_sigshadow_blocked_set if and
 *     only if step 4 succeeded AND new_blocked changed
 *
 * Step 5 is conditional and unconditionally last, so there's no
 * shadow-rollback path. */
static long handle_rt_sigprocmask(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{
	int    how         = (int)args->a;
	const void *guest_set    = (const void *)(uintptr_t)args->b;
	void  *guest_oldset      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sigsetsize != 8) return TAWC_EINVAL;
	/* No-op call (the kernel returns 0 immediately for this shape).
	 * Short-circuit before issuing gettid + a shadow table probe. */
	if (!guest_set && !guest_oldset) return 0;

	uint64_t set_val = 0;
	int have_set = 0;
	if (guest_set) {
		long e = tawc_copy_from_guest(&set_val, 8, guest_set);
		if (e < 0) return TAWC_EFAULT;
		have_set = 1;
	}

	uint64_t *kmask = uc_sigmask_word(uc);
	uint64_t  cur_kmask = *kmask;
	/* The shadow lives in a TID-keyed table — uc->uc_sigmask is
	 * already per-thread (the kernel populated it for the trapping
	 * thread), but we deliberately strip SIGSYS from it, so the
	 * "guest blocked SIGSYS" bit needs its own per-thread store. */
	long tid_l = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	int  tid   = (int)tid_l;
	int  prev_blocked = tawc_sigshadow_blocked_get(tid);
	int  new_blocked  = prev_blocked;

	if (have_set) {
		int sigsys_in_set = (set_val & SIGSYS_BIT) != 0;
		uint64_t kernel_set = set_val & ~SIGSYS_BIT;

		switch (how) {
		case SIG_BLOCK:
			*kmask = cur_kmask | kernel_set;
			if (sigsys_in_set) new_blocked = 1;
			break;
		case SIG_UNBLOCK:
			*kmask = cur_kmask & ~kernel_set;
			if (sigsys_in_set) new_blocked = 0;
			break;
		case SIG_SETMASK:
			*kmask = kernel_set;
			new_blocked = sigsys_in_set;
			break;
		default:
			return TAWC_EINVAL;
		}
	}

	if (guest_oldset) {
		uint64_t old = cur_kmask;
		if (prev_blocked) old |= SIGSYS_BIT;
		long e = tawc_copy_to_guest(guest_oldset, &old, 8);
		if (e < 0) {
			/* Roll back the kernel mask if we'd updated it.
			 * Shadow doesn't need rollback — we haven't
			 * published `new_blocked` yet. */
			if (have_set) *kmask = cur_kmask;
			return TAWC_EFAULT;
		}
	}
	if (have_set && new_blocked != prev_blocked)
		tawc_sigshadow_blocked_set(tid, new_blocked);
	return 0;
}

/* exit(2) — per-thread exit (kills only the calling thread, not the
 * process; that's exit_group). Trapped purely so we can clear the
 * dying thread's blocked-shadow slot before the kernel reaps the tid;
 * otherwise a future thread that reuses this tid would read the previous
 * owner's stale "SIGSYS blocked" bit until its own first rt_sigprocmask.
 * (signal_shadow.c has the full rationale.)
 *
 * exit_group is not hooked: it kills every thread and the OS reclaims
 * everything, so per-slot cleanup would be wasted work. Involuntary
 * thread death (SIGKILL of one thread, fatal signals that bypass exit(2))
 * still leaves a stale slot; uncommon, and the failure mode is bounded
 * to one wrong-mask read on tid reuse.
 *
 * The forwarded exit(2) doesn't return; the __builtin_unreachable() is
 * the bottom of the signal-handler control flow on the trapping thread. */
static long handle_exit(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	long tid = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	tawc_sigshadow_blocked_clear((int)tid);
	TAWC_RAW(TAWC_SYS_exit, args->a, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}

void tawcroot_control_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_seccomp,         handle_seccomp);
	tawcroot_dispatch_install(TAWC_SYS_prctl,           handle_prctl);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigaction,    handle_rt_sigaction);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigprocmask,  handle_rt_sigprocmask);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_setup,    handle_io_uring_deny);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_enter,    handle_io_uring_deny);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_register, handle_io_uring_deny);
	tawcroot_dispatch_install(TAWC_SYS_clone3,          handle_clone3);
	tawcroot_dispatch_install(TAWC_SYS_exit,            handle_exit);
	tawcroot_dispatch_install(TAWC_SYS_pivot_root,      fake_eperm);
	tawcroot_dispatch_install(TAWC_SYS_mount,           fake_eperm);
	tawcroot_dispatch_install(TAWC_SYS_umount2,         fake_eperm);
	tawcroot_dispatch_install(TAWC_SYS_unshare,         fake_eperm);
	tawcroot_dispatch_install(TAWC_SYS_setns,           fake_eperm);
}
