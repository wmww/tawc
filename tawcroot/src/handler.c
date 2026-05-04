/* Phase-0 SIGSYS handler.
 *
 * Async-signal-safe by construction (notes/tawcroot.md "Why the handler
 * is async-signal-safe"): no malloc, no stdio, no libc calls with hidden
 * mutable state. Just reads ucontext, optionally inspects siginfo, and
 * writes a return value back into the saved register frame.
 *
 * The recorded `tawcroot_handler_obs` is a single-slot snapshot — one
 * writer per TRAP, atomic-ish via plain stores guarded by a release fence.
 * Phase 1 will replace this with the dispatch table; the observation slot
 * stays around as a debug hatch.
 *
 * We rely on rt_sigaction directly (not bionic's libc wrapper) because
 * we link `-nostdlib`. On x86_64 the kernel mandates SA_RESTORER and a
 * user-supplied trampoline; on aarch64 the kernel installs a VDSO
 * trampoline and `sa_restorer` is unused. The trampoline lives in
 * arch/<arch>_stub.S as `tawcroot_sigreturn_trampoline`.
 */

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>

#include <linux/seccomp.h>

#include "arch.h"
#include "handler.h"
#include "io.h"
#include "raw_sys.h"
#include "dispatch.h"
#include "usercopy.h"

/* Match kernel `struct sigaction` layout — bionic's struct is the same
 * on both arches we care about, but we avoid <signal.h>'s sigaction
 * alias to be explicit about where each field comes from. */
struct kernel_sigaction {
	void (*k_sa_handler)(int, siginfo_t *, void *);
	unsigned long sa_flags;
	void (*sa_restorer)(void);
	uint64_t sa_mask;   /* sigsetsize=8 → exactly one u64 */
};

#ifndef SA_RESTORER
# define SA_RESTORER 0x04000000
#endif

extern void tawcroot_sigreturn_trampoline(void);

/* Phase-0 debug observation slot. Per review finding D2 the production
 * handler should not write to a mutable process-wide global on every
 * TRAP — that's contention for multi-threaded guests and violates the
 * "no mutable handler state without snapshot rules" principle from
 * notes/tawcroot.md "Threading and `vfork` invariants". Gate it behind
 * TAWCROOT_TESTHOST so production stays clean; testhost binaries keep
 * the slot for the smoke driver.
 *
 * Single-threaded-only: writes to g_obs are *not* race-free under a
 * multi-threaded guest. Two threads trapping concurrently will
 * interleave the field stores below, and tawcroot_handler_observe
 * may snapshot a half-updated record. This is acceptable because the
 * existing testhost smoke (smoke.c, child.c, phase1.c) is
 * single-threaded by construction. The day someone writes a
 * multi-threaded handler test, this slot needs the same seqlock
 * treatment as the SIGSYS-shadow state in signal_shadow.c — until
 * then the simpler plain-store form keeps the test driver code
 * ergonomic. */
#ifdef TAWCROOT_TESTHOST
static volatile tawcroot_handler_obs g_obs;
#endif

static void sigsys_handler(int sig, siginfo_t *info, void *ucontext)
{
	(void)sig;

	ucontext_t *uc = (ucontext_t *)ucontext;
	tawcroot_syscall_args args;
	tawcroot_arch_read_args(uc, &args);

#ifdef TAWCROOT_TESTHOST
	/* siginfo_t fields populated by the kernel for SIGSYS:
	 *   si_signo, si_errno, si_code (== SYS_SECCOMP for our case),
	 *   si_call_addr, si_syscall, si_arch.
	 * Bionic exposes these as macros that expand to
	 * `_sifields._sigsys._call_addr` etc., so don't reuse the names
	 * for locals — the macro hides the field in unpredictable ways. */
	int   sc_code     = info->si_code;
	void *sc_addr     = info->si_call_addr;
	int   sc_syscall  = info->si_syscall;
	int   sc_arch     = info->si_arch;
	(void)sc_syscall;

	/* Update the snapshot. Plain stores are fine: only one handler can
	 * write at a time on a single CPU (kernel masks SIGSYS for the
	 * duration of this handler — we deliberately leave SA_NODEFER off,
	 * see notes). The volatile qualifier prevents the compiler from
	 * caching reads in the test driver. */
	g_obs.last_nr         = args.nr;
	g_obs.last_arg0       = args.a;
	g_obs.last_call_addr  = (uintptr_t)sc_addr;
	g_obs.last_resume_pc  = tawcroot_arch_resume_pc(uc);
	g_obs.last_si_code    = sc_code;
	g_obs.last_si_arch    = sc_arch;
	g_obs.calls          += 1;
#else
	(void)info;
#endif

	/* Dispatch. Empty slots fall through to -ENOSYS — see comment in
	 * include/dispatch.h about Android's stacked filter potentially
	 * delivering TRAPs we didn't ask for. */
	long rv;
	tawcroot_handler_fn fn = tawcroot_dispatch_get((int)args.nr);
	if (fn) {
		rv = fn(&args, uc);
	} else {
		rv = -38;  /* ENOSYS */
	}
#ifdef TAWCROOT_TRACE
	{
		/* Build the trace line into a stack buffer and emit in one
		 * write so fd 1 / fd 2 don't interleave with our debug output.
		 * Format: "[t] pid=<pid> nr=<nr> rv=<rv>\n" — async-signal-safe. */
		char line[96];
		size_t li = 0;
		const char *p = "[t] pid=";
		while (*p) line[li++] = *p++;
		long pid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
		{
			char tmp[24]; int tn = 0;
			unsigned long u = pid > 0 ? (unsigned long)pid : 0;
			if (u == 0) tmp[tn++] = '0';
			while (u) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
			while (tn--) line[li++] = tmp[tn];
		}
		p = " nr=";
		while (*p) line[li++] = *p++;
		{
			char tmp[24]; int tn = 0;
			long v = args.nr;
			int neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
			if (u == 0) tmp[tn++] = '0';
			while (u) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
			if (neg) line[li++] = '-';
			while (tn--) line[li++] = tmp[tn];
		}
		p = " a=";
		while (*p) line[li++] = *p++;
		{
			char tmp[24]; int tn = 0;
			long v = args.a;
			int neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
			if (u == 0) tmp[tn++] = '0';
			while (u) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
			if (neg) line[li++] = '-';
			while (tn--) line[li++] = tmp[tn];
		}
		p = " rv=";
		while (*p) line[li++] = *p++;
		{
			char tmp[24]; int tn = 0;
			long v = rv;
			int neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
			if (u == 0) tmp[tn++] = '0';
			while (u) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
			if (neg) line[li++] = '-';
			while (tn--) line[li++] = tmp[tn];
		}
		line[li++] = '\n';
		TAWC_RAW(TAWC_SYS_write, 2, (long)line, (long)li, 0, 0, 0);
	}
#endif
	tawcroot_arch_write_return(uc, rv);
}

#ifdef TAWCROOT_TESTHOST
void tawcroot_handler_observe(tawcroot_handler_obs *out)
{
	/* Single-reader snapshot. Volatile reads + a compiler barrier are
	 * enough on x86_64 / aarch64 for our purpose: the handler is the
	 * only writer and runs synchronously on the trapping thread. */
	tawcroot_handler_obs s;
	s.calls          = g_obs.calls;
	s.last_nr        = g_obs.last_nr;
	s.last_arg0      = g_obs.last_arg0;
	s.last_call_addr = g_obs.last_call_addr;
	s.last_resume_pc = g_obs.last_resume_pc;
	s.last_si_code   = g_obs.last_si_code;
	s.last_si_arch   = g_obs.last_si_arch;
	__asm__ __volatile__("" ::: "memory");
	*out = s;
}
#endif

long tawcroot_install_handler(void)
{
	struct kernel_sigaction sa;
	sa.k_sa_handler = sigsys_handler;
	sa.sa_flags     = SA_SIGINFO | SA_RESTORER;
	sa.sa_restorer  = tawcroot_sigreturn_trampoline;
	/* Mask every catchable signal for the handler's duration. The
	 * kernel auto-masks the trapping signal (SIGSYS) while we run,
	 * but other queued signals stay deliverable — and a nested
	 * signal handler that issues a seccomp-trapped syscall produces
	 * a SIGSYS that's *blocked* (because our outer SIGSYS handler is
	 * still in flight). RET_TRAP on a blocked-and-pending SIGSYS
	 * has only one outcome: the kernel kills the process with
	 * default-action SIGSYS, no handler dispatch. Repro: pacman-key
	 * `gpg --import manjaro-arm.gpg` reaps a child via SIGCHLD that
	 * fires inside our rt_sigprocmask handler; bash's SIGCHLD code
	 * then calls rt_sigprocmask, RET_TRAP, kill. SIGKILL/SIGSTOP
	 * can't be masked; the kernel silently drops those bits. */
	sa.sa_mask      = ~(uint64_t)0;

	/* sigsetsize = 8 (size of kernel sigset_t on lp64). */
	return tawc_rt_sigaction(SIGSYS, &sa, NULL, 8);
}
