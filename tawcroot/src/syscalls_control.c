/* Runtime-control syscall handlers — guest-side denials and shadow
 * virtualization for operations that would otherwise compromise
 * tawcroot's own invariants.
 *
 * Surface (notes/tawcroot/sigsys-handler.md §"Guest signal/seccomp control"):
 *   - `seccomp(2)` / `prctl(PR_SET_SECCOMP)`: fake-accept. Validate
 *     the arguments exactly as the kernel would (same EFAULT/EINVAL
 *     shapes, so feature probes keep working), then install nothing
 *     and return success. We can't honestly stack the guest's filter
 *     on ours, and daemons that treat sandbox setup as fatal (sshd
 *     since mid-2026) can't take an -EPERM either.
 *   - `rt_sigaction(SIGSYS, ...)`: virtualize. The guest's intended
 *     disposition lives in a shadow buffer; reads/writes of SIGSYS
 *     hit the shadow and never the kernel. The real kernel disposition
 *     stays our SIGSYS handler.
 *   - `rt_sigaction(other, ...)`: pass through with SIGSYS stripped
 *     from sa_mask (shadowed for readback), so guest handlers never
 *     run with SIGSYS blocked.
 *   - `rt_sigprocmask`: pass through, but transparently strip SIGSYS
 *     from any new mask the guest installs and OR-in the shadow bit
 *     when reporting the previous mask. Guest reads back what it set;
 *     the kernel never actually blocks SIGSYS, so traps continue
 *     reaching our handler.
 *   - `sigaltstack`: virtualize so undersized guest altstacks never
 *     receive our SA_ONSTACK frame, applying it off-stack (sigalt.h).
 *   - Other signals are unaffected.
 */

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "raw_sys.h"
#include "sigalt.h"
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
 * via signal_shadow.h: handler ptr (8) + flags (8) + sa_restorer (8)
 * + mask (8) = 32 on both arches (both define SA_RESTORER).
 * (Earlier revs oversized to 64; over-read past the guest struct in
 * both directions, review finding B2.) */

/* seccomp(2) filter-flag bits the kernel knows (kernel/seccomp.c, as of
 * 6.x). Unknown bits get the kernel's -EINVAL. */
#define TAWC_SECCOMP_FLAG_TSYNC        0x01UL
#define TAWC_SECCOMP_FLAG_NEW_LISTENER 0x08UL
#define TAWC_SECCOMP_FLAG_TSYNC_ESRCH  0x10UL
#define TAWC_SECCOMP_FLAG_KNOWN        0x3fUL
#define TAWC_BPF_MAXINSNS              4096

/* Fake-accept a guest SECCOMP_SET_MODE_FILTER install. We can't honestly
 * stack the guest's BPF filter on top of ours: it could KILL_PROCESS our
 * raw_syscall stub, return ERRNO before our path-translation trap, or
 * RET_TRAP into a guest-owned SIGSYS path that tawcroot virtualizes away.
 * -EPERM is no longer viable either: sshd (openssh-portable 7ab700f170,
 * mid-2026) treats any sandbox-install failure as fatal, killing every
 * connection preauth. So: validate the arguments with the kernel's exact
 * EFAULT/EINVAL shapes — NULL-fprog support probes (systemd et al.
 * expect -EFAULT) must keep working — then install nothing and report
 * success. The guest-visible world stays consistent: PR_GET_SECCOMP
 * passes through and truthfully reports filter mode (ours).
 *
 * Accepted divergence: the insn array is checked for readability, not
 * run through the BPF verifier — a malformed program "installs" where
 * the kernel would EINVAL (notes/tawcroot/status.md). */
static long filter_fake_accept(const void *uargs)
{
	struct {
		uint16_t len;      /* struct sock_fprog, 64-bit layout */
		uint16_t pad[3];
		uint64_t filter;
	} fprog;
	if (tawc_copy_from_guest(&fprog, sizeof fprog, uargs) < 0)
		return TAWC_EFAULT;
	if (fprog.len == 0 || fprog.len > TAWC_BPF_MAXINSNS)
		return TAWC_EINVAL;
	/* Readability probe over the whole insn array (8 bytes per
	 * sock_filter), chunked to keep the handler stack small. A hole
	 * mid-array EFAULTs here just as the kernel's copy would. */
	unsigned char scratch[256];
	uint64_t total = (uint64_t)fprog.len * 8;
	for (uint64_t off = 0; off < total; off += sizeof scratch) {
		uint64_t n = total - off;
		if (n > sizeof scratch) n = sizeof scratch;
		if (tawc_copy_from_guest(scratch, (size_t)n,
				(const void *)(uintptr_t)(fprog.filter + off)) < 0)
			return TAWC_EFAULT;
	}
	return 0;
}

/* SECCOMP_GET_ACTION_AVAIL and other read-only ops pass through to
 * the kernel verbatim because they don't change state. */
static long handle_seccomp(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	unsigned int  op    = (unsigned int)args->a;
	unsigned long flags = (unsigned long)args->b;
	const void   *uargs = (const void *)(uintptr_t)args->c;

	if (op == 0 /*SECCOMP_SET_MODE_STRICT*/) {
		if (flags != 0 || uargs != NULL) return TAWC_EINVAL;
		return 0;  /* fake accept */
	}
	if (op == 1 /*SECCOMP_SET_MODE_FILTER*/) {
		if (flags & ~TAWC_SECCOMP_FLAG_KNOWN) return TAWC_EINVAL;
		if ((flags & TAWC_SECCOMP_FLAG_TSYNC) &&
		    (flags & TAWC_SECCOMP_FLAG_NEW_LISTENER) &&
		    !(flags & TAWC_SECCOMP_FLAG_TSYNC_ESRCH))
			return TAWC_EINVAL;
		/* User-notification needs a real notif fd the supervisor
		 * polls; success without one would wedge the caller. Honest
		 * refusal — the one seccomp shape we still deny. */
		if (flags & TAWC_SECCOMP_FLAG_NEW_LISTENER)
			return TAWC_EPERM;
		return filter_fake_accept(uargs);
	}
	/* SECCOMP_GET_ACTION_AVAIL = 2, SECCOMP_GET_NOTIF_SIZES = 3, etc.
	 * Read-only — pass through. */
	return TAWC_RAW(TAWC_SYS_seccomp, args->a, args->b, args->c,
			args->d, args->e, 0);
}

static long handle_prctl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int op = (int)args->a;
	/* PR_SET_SECCOMP — same fake-accept as handle_seccomp above.
	 * arg2 is the legacy mode numbering: 1 = strict, 2 = filter. */
	if (op == PR_SET_SECCOMP) {
		unsigned long mode = (unsigned long)args->b;
		if (mode == 1) return 0;  /* fake accept, strict */
		if (mode == 2)
			return filter_fake_accept(
				(const void *)(uintptr_t)args->c);
		return TAWC_EINVAL;
	}
	return TAWC_RAW(TAWC_SYS_prctl, args->a, args->b, args->c,
			args->d, args->e, 0);
}

/* Bit (sig - 1) set = the guest's current action for sig asked to
 * block SIGSYS in sa_mask. Process-global, like dispositions. */
static uint64_t sa_mask_sigsys_shadow;

/* Non-SIGSYS rt_sigaction: forward with SIGSYS cleared from sa_mask.
 * A handler running with SIGSYS really blocked can't take our trap —
 * any trapped syscall it makes (siglongjmp's rt_sigprocmask, say) gets
 * the process force-killed. OpenSSL-style SIGILL CPU probes do exactly
 * this. Readback ORs the bit back in, as rt_sigprocmask does. */
static long sigaction_strip_sigsys(int sig, const void *act, void *oldact,
				   size_t sigsetsize)
{
	if (sigsetsize != 8 || sig < 1 || sig > 64)
		return TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)act,
				(long)oldact, sigsetsize, 0, 0);

	uint64_t sigbit = 1ULL << (sig - 1);
	unsigned char buf[TAWC_KERN_SIGACTION_SIZE];
	uint64_t *mask = (uint64_t *)(buf + TAWC_KERN_SIGACTION_SIZE - 8);
	int had = 0;
	if (act) {
		if (tawc_copy_from_guest(buf, sizeof buf, act) < 0)
			return TAWC_EFAULT;
		had = (*mask & SIGSYS_BIT) != 0;
		*mask &= ~SIGSYS_BIT;
	}

	long r = TAWC_RAW(TAWC_SYS_rt_sigaction, sig,
			  act ? (long)buf : 0, (long)oldact, 8, 0, 0);
	if (r < 0) return r;

	uint64_t prev = __atomic_load_n(&sa_mask_sigsys_shadow,
					__ATOMIC_RELAXED);
	if (oldact && (prev & sigbit)) {
		uint64_t old_mask;
		void *old_mask_p = (unsigned char *)oldact +
				   TAWC_KERN_SIGACTION_SIZE - 8;
		if (tawc_copy_from_guest(&old_mask, 8, old_mask_p) == 0) {
			old_mask |= SIGSYS_BIT;
			(void)tawc_copy_to_guest(old_mask_p, &old_mask, 8);
		}
	}
	if (act) {
		if (had)
			__atomic_fetch_or(&sa_mask_sigsys_shadow, sigbit,
					  __ATOMIC_RELAXED);
		else
			__atomic_fetch_and(&sa_mask_sigsys_shadow, ~sigbit,
					   __ATOMIC_RELAXED);
	}
	return r;
}

static long handle_rt_sigaction(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	int    sig         = (int)args->a;
	const void *act    = (const void *)(uintptr_t)args->b;
	void  *oldact      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sig != SIGSYS)
		return sigaction_strip_sigsys(sig, act, oldact, sigsetsize);

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
static long apply_sigaltstack(const stack_t *ss);

static long handle_rt_sigprocmask(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{
	int    how         = (int)args->a;
	const void *guest_set    = (const void *)(uintptr_t)args->b;
	void  *guest_oldset      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sigsetsize != 8) return TAWC_EINVAL;
	/* Every thread sets its mask early (libc thread start/exit), so
	 * this is where a thread without an altstack gets its fallback. */
	tawc_sigalt_ensure(&uc->uc_stack, apply_sigaltstack);
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

static long apply_sigaltstack(const stack_t *ss)
{
	return tawcroot_raw_syscall_off_stack(TAWC_SYS_sigaltstack,
					      (long)ss, 0);
}

/* sigaltstack(ss, old_ss). Never forwarded verbatim — see sigalt.h.
 * Old is copied out before anything mutates, so EFAULT leaves no
 * trace. */
static long handle_sigaltstack(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	const void *guest_ss  = (const void *)(uintptr_t)args->a;
	void       *guest_old = (void *)(uintptr_t)args->b;
	stack_t ss, old;

	if (guest_ss && tawc_copy_from_guest(&ss, sizeof ss, guest_ss) < 0)
		return TAWC_EFAULT;
	long r = tawc_sigalt_check(&uc->uc_stack, tawcroot_arch_sp(uc),
				   guest_ss ? &ss : NULL,
				   guest_old ? &old : NULL);
	if (r < 0) return r;
	if (guest_old && tawc_copy_to_guest(guest_old, &old, sizeof old) < 0)
		return TAWC_EFAULT;
	if (!guest_ss) return 0;
	long tid = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	return tawc_sigalt_commit(&uc->uc_stack, &ss, (int)tid,
				  apply_sigaltstack);
}

/* exit(2) — per-thread exit (kills only the calling thread, not the
 * process; that's exit_group). Trapped purely so we can clear the
 * dying thread's blocked-shadow slot before the kernel reaps the tid;
 * otherwise a future thread that reuses this tid would read the previous
 * owner's stale "SIGSYS blocked" bit until its own first rt_sigprocmask.
 * (signal_shadow.c has the full rationale.) Also frees the thread's
 * substitute altstack slot — which, SA_ONSTACK, we are running on, so
 * nothing may be read off the stack after that release.
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
	long code = args->a;
	long tid = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	tawc_sigshadow_blocked_clear((int)tid);
	tawc_sigalt_thread_exit(&uc->uc_stack, (int)tid);
	TAWC_RAW(TAWC_SYS_exit, code, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}

#if defined(__x86_64__)
/* x86_64 glibc's getpgrp(3) issues the legacy getpgrp syscall, which
 * Android's untrusted_app filter RET_TRAPs (bionic only ever calls
 * getpgid). Without this handler the -ENOSYS fallthrough reaches
 * bash's job-control init as a garbage process group and interactive
 * shells on a pty print "initialize_job_control: no job control in
 * background" and run with job control off. aarch64 never allocated a
 * getpgrp number — glibc wraps getpgid(0) there — so this is
 * emulator-only. */
static long handle_getpgrp(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_RAW(TAWC_SYS_getpgid, 0, 0, 0, 0, 0, 0);
}

/* time(tloc) → clock_gettime(CLOCK_REALTIME). Legacy time(2) is
 * RET_TRAPped by the real emulator filter (empirical audit:
 * notes/tawcroot/status.md); clock_gettime is allowlisted. Return
 * tv_sec, and mirror it into *tloc when non-NULL (via the guarded
 * copy — tloc is an untrusted guest pointer). */
static long handle_time(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct { long tv_sec; long tv_nsec; } ts;
	long r = TAWC_RAW(TAWC_SYS_clock_gettime, 0 /*CLOCK_REALTIME*/,
	                  (long)&ts, 0, 0, 0, 0);
	if (r < 0) return r;
	if (args->a != 0) {
		long cr = tawc_copy_to_guest((void *)args->a, &ts.tv_sec,
		                             sizeof ts.tv_sec);
		if (cr < 0) return cr;
	}
	return ts.tv_sec;
}

/* alarm(seconds) → setitimer(ITIMER_REAL). glibc's alarm(3) already
 * routes through setitimer so this only fires for programs issuing the
 * raw legacy NR, but the real filter RET_TRAPs it, so without this it
 * -ENOSYSes. Contract: arm a one-shot ITIMER_REAL for `seconds` (0
 * disarms) and return the whole seconds left on the previous timer,
 * rounding a partial second up as the man page specifies. */
static long handle_alarm(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct itv { long sec; long usec; };
	/* seconds is unsigned int in the kernel ABI — only the low 32
	 * register bits are meaningful. */
	struct { struct itv interval; struct itv value; } nv = {
		{ 0, 0 }, { (long)(unsigned int)args->a, 0 },
	}, ov;
	long r = TAWC_RAW(TAWC_SYS_setitimer, 0 /*ITIMER_REAL*/,
	                  (long)&nv, (long)&ov, 0, 0, 0);
	if (r < 0) return r;
	long rem = ov.value.sec;
	if (ov.value.usec != 0) rem++;
	return rem;
}
#endif

void tawcroot_control_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_seccomp,         handle_seccomp);
	tawcroot_dispatch_install(TAWC_SYS_prctl,           handle_prctl);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigaction,    handle_rt_sigaction);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigprocmask,  handle_rt_sigprocmask);
	tawcroot_dispatch_install(TAWC_SYS_sigaltstack,     handle_sigaltstack);
	/* io_uring_setup: deny with -ENOSYS so guest libraries fall back to
	 * syscall-based I/O which we can translate. The plan
	 * (notes/tawcroot/path-translation.md "Open questions" #1) classifies a passed-through
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
	 * independently of the stacked Android filter. See notes/tawcroot/path-translation.md
	 * "io_uring MVP behavior". */
	tawcroot_dispatch_install(TAWC_SYS_io_uring_setup,    tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_enter,    tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_register, tawcroot_deny_enosys);

	/* clone3: deny with -ENOSYS so glibc's __clone falls back to the
	 * older clone(2) syscall (NR 220 aarch64 / NR 56 x86_64). All stacked
	 * seccomp filters are evaluated at syscall entry and the most
	 * restrictive action wins, so this can't shield the guest from an
	 * Android RET_KILL on clone3 — it works only because Android's policy
	 * is empirically not KILL: on Android 14 our trap fires ([sigsys]
	 * nr=435), i.e. Android allows-or-traps clone3. Returning -ENOSYS
	 * causes glibc to set its "clone3 missing" flag and use clone()
	 * going forward, keeping the guest off the risky syscall entirely. */
	tawcroot_dispatch_install(TAWC_SYS_clone3,          tawcroot_deny_enosys);

	tawcroot_dispatch_install(TAWC_SYS_exit,            handle_exit);

#if defined(__x86_64__)
	tawcroot_dispatch_install(TAWC_SYS_getpgrp,         handle_getpgrp);
	tawcroot_dispatch_install(TAWC_SYS_time,            handle_time);
	tawcroot_dispatch_install(TAWC_SYS_alarm,           handle_alarm);
#endif

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
	tawcroot_dispatch_install(TAWC_SYS_pivot_root,      tawcroot_deny_eperm);
	tawcroot_dispatch_install(TAWC_SYS_mount,           tawcroot_deny_eperm);
	tawcroot_dispatch_install(TAWC_SYS_umount2,         tawcroot_deny_eperm);
	tawcroot_dispatch_install(TAWC_SYS_unshare,         tawcroot_deny_eperm);
	tawcroot_dispatch_install(TAWC_SYS_setns,           tawcroot_deny_eperm);
}
