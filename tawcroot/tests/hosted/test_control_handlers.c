/* Hosted handler-level tests for syscalls_control.c. */

#include <cleat/test.h>

#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "hosted.h"

#include "errno_neg.h"
#include "sysnr.h"

#if defined(__x86_64__)
/* Legacy getpgrp routes to getpgid(0) — Android's untrusted_app filter
 * RET_TRAPs the x86_64-only getpgrp number, and the -ENOSYS
 * fallthrough used to break bash's job-control init on the in-app
 * terminal's pty. */
test(hosted_getpgrp_routes_to_getpgid)
{
	th_view v;
	th_setup(&v, "ctl-getpgrp");

	test_int_eq(th_sys(TAWC_SYS_getpgrp, 0, 0, 0, 0, 0, 0),
		    (long)getpgid(0));

	th_teardown(&v);
}

/* Legacy time(2) → clock_gettime(CLOCK_REALTIME). Both RET_TRAPped by
 * the real emulator filter (empirical audit: notes/tawcroot/status.md);
 * clock_gettime is allowlisted. Return value and the *tloc write-back
 * must both match wall clock. */
test(hosted_legacy_time_routes_to_clock_gettime)
{
	th_view v;
	th_setup(&v, "ctl-time");

	time_t ref = time(NULL);
	long rv = th_sys(TAWC_SYS_time, 0, 0, 0, 0, 0, 0);
	test_true(rv >= (long)ref && rv <= (long)ref + 2);

	/* With a tloc pointer the same value is copied back to the guest. */
	long tloc = 0;
	long rv2 = th_sys(TAWC_SYS_time, (long)&tloc, 0, 0, 0, 0, 0);
	test_int_eq(rv2, tloc);
	test_true(tloc >= (long)ref);

	th_teardown(&v);
}

/* Legacy alarm(2) → setitimer(ITIMER_REAL). Returns the whole seconds
 * left on the previous timer (partial second rounded up). Ignore
 * SIGALRM so the armed timer can't kill the test if it ever fires. */
test(hosted_legacy_alarm_routes_to_setitimer)
{
	th_view v;
	th_setup(&v, "ctl-alarm");

	struct sigaction old_sa, ign = { 0 };
	ign.sa_handler = SIG_IGN;
	sigaction(SIGALRM, &ign, &old_sa);

	/* No prior timer → 0. Arm a large value so the disarm can't race. */
	test_int_eq(th_sys(TAWC_SYS_alarm, 1000, 0, 0, 0, 0, 0), 0);
	/* seconds is unsigned int in the kernel ABI: garbage in the high
	 * register bits with a low half of 0 must disarm, not arm a
	 * 2^32-second timer. The ~999.99s remainder rounds up to 1000. */
	test_int_eq(th_sys(TAWC_SYS_alarm, 1L << 32, 0, 0, 0, 0, 0), 1000);
	/* Confirm the disarm: no timer left. */
	test_int_eq(th_sys(TAWC_SYS_alarm, 0, 0, 0, 0, 0, 0), 0);

	sigaction(SIGALRM, &old_sa, NULL);
	th_teardown(&v);
}
#endif

/* Guest seccomp installs are fake-accepted: kernel-faithful argument
 * validation (EFAULT/EINVAL shapes preserved so support probes keep
 * working), then success with nothing installed. See the
 * filter_fake_accept comment in syscalls_control.c and
 * notes/tawcroot/status.md "Accepted syscall-fidelity divergences".
 * The proof that nothing installs lives in the fork-based smoke
 * (rootfs_smoke.c drives a KILL_PROCESS program through the real trap
 * path); here we cover the validation matrix under ASan. */

/* struct sock_filter: u16 code, u8 jt, u8 jf, u32 k. One BPF_RET|BPF_K
 * SECCOMP_RET_KILL_PROCESS insn — valid shape, lethal if ever truly
 * installed. */
typedef struct { uint16_t code; uint8_t jt, jf; uint32_t k; } tf_insn;
typedef struct { uint16_t len; uint16_t pad[3]; uint64_t filter; } tf_fprog;

test(hosted_seccomp_filter_fake_accept)
{
	th_view v;
	th_setup(&v, "ctl-seccomp-accept");

	tf_insn  kill_insn = { 0x06, 0, 0, 0x80000000u };
	tf_fprog fprog = { 1, {0, 0, 0}, (uint64_t)(uintptr_t)&kill_insn };

	/* seccomp(2) and the prctl spelling both fake-accept. */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1 /*SET_MODE_FILTER*/, 0,
			   &fprog, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_prctl, 22 /*PR_SET_SECCOMP*/,
			   2 /*SECCOMP_MODE_FILTER*/, &fprog, 0, 0, 0), 0);
	/* Strict mode too (seccomp op 0, prctl mode 1). */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 0, 0, NULL, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_prctl, 22, 1, 0, 0, 0, 0), 0);

	th_teardown(&v);
}

test(hosted_seccomp_filter_validation_shapes)
{
	th_view v;
	th_setup(&v, "ctl-seccomp-shapes");

	tf_insn  kill_insn = { 0x06, 0, 0, 0x80000000u };
	tf_fprog fprog = { 1, {0, 0, 0}, (uint64_t)(uintptr_t)&kill_insn };

	/* NULL-fprog support probe (systemd's pattern) must EFAULT. */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0, NULL, 0, 0, 0),
		    TAWC_EFAULT);
	/* Unreadable insn array EFAULTs like the kernel's copy would. */
	tf_fprog bad_ptr = { 4096, {0, 0, 0}, 0 };
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0, &bad_ptr, 0, 0, 0),
		    TAWC_EFAULT);
	/* len 0 / len > BPF_MAXINSNS. */
	tf_fprog zero_len = { 0, {0, 0, 0}, (uint64_t)(uintptr_t)&kill_insn };
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0, &zero_len, 0, 0, 0),
		    TAWC_EINVAL);
	tf_fprog too_long = { 4097, {0, 0, 0}, (uint64_t)(uintptr_t)&kill_insn };
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0, &too_long, 0, 0, 0),
		    TAWC_EINVAL);
	/* Unknown flag bits. */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0x1000, &fprog, 0, 0, 0),
		    TAWC_EINVAL);
	/* TSYNC + NEW_LISTENER without TSYNC_ESRCH is the kernel's
	 * invalid combo; NEW_LISTENER alone is our one honest refusal
	 * (success would promise a notification fd we can't mint). */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0x09, &fprog, 0, 0, 0),
		    TAWC_EINVAL);
	test_int_eq(th_sys(TAWC_SYS_seccomp, 1, 0x08, &fprog, 0, 0, 0),
		    TAWC_EPERM);
	/* Strict mode with nonzero flags/uargs, bad prctl mode. */
	test_int_eq(th_sys(TAWC_SYS_seccomp, 0, 1, NULL, 0, 0, 0),
		    TAWC_EINVAL);
	test_int_eq(th_sys(TAWC_SYS_seccomp, 0, 0, &fprog, 0, 0, 0),
		    TAWC_EINVAL);
	test_int_eq(th_sys(TAWC_SYS_prctl, 22, 3, 0, 0, 0, 0),
		    TAWC_EINVAL);

	th_teardown(&v);
}

/* Kernel struct sigaction at sigsetsize=8 (both arches). */
typedef struct { uint64_t handler, flags, restorer, mask; } tk_sigaction;

static void tk_noop(int sig) { (void)sig; }

/* A non-SIGSYS action asking for a full sa_mask must not reach the
 * kernel with SIGSYS in it: the handler would run with our trap
 * blocked and its first trapped syscall would get the process killed
 * (ChatGPT's bundled codex: SIGILL probe + siglongjmp). Readback
 * still shows what the guest set. */
test(hosted_rt_sigaction_strips_sigsys_from_sa_mask)
{
	th_view v;
	th_setup(&v, "ctl-sigaction-mask");

	const uint64_t sigsys_bit = 1ULL << (SIGSYS - 1);
	tk_sigaction act = { (uint64_t)(uintptr_t)tk_noop, 0, 0, ~0ULL };
	tk_sigaction old;
	test_int_eq(th_sys(TAWC_SYS_rt_sigaction, SIGUSR1, &act, NULL, 8,
			   0, 0), 0);
	/* The guest's struct is untouched. */
	test_int_eq(act.mask == ~0ULL, 1);

	struct sigaction kernel_view;
	test_int_eq(sigaction(SIGUSR1, NULL, &kernel_view), 0);
	test_int_eq(sigismember(&kernel_view.sa_mask, SIGSYS), 0);
	test_int_eq(sigismember(&kernel_view.sa_mask, SIGTERM), 1);

	/* Guest readback ORs SIGSYS back in... */
	test_int_eq(th_sys(TAWC_SYS_rt_sigaction, SIGUSR1, NULL, &old, 8,
			   0, 0), 0);
	test_int_eq((old.mask & sigsys_bit) != 0, 1);

	/* ...until an action without it replaces the old one. */
	act.mask = 0;
	test_int_eq(th_sys(TAWC_SYS_rt_sigaction, SIGUSR1, &act, &old, 8,
			   0, 0), 0);
	test_int_eq((old.mask & sigsys_bit) != 0, 1);
	test_int_eq(th_sys(TAWC_SYS_rt_sigaction, SIGUSR1, NULL, &old, 8,
			   0, 0), 0);
	test_int_eq(old.mask, 0);

	/* Bad act pointer EFAULTs like the kernel. */
	test_int_eq(th_sys(TAWC_SYS_rt_sigaction, SIGUSR1, (void *)8, NULL,
			   8, 0, 0), TAWC_EFAULT);

	signal(SIGUSR1, SIG_DFL);
	th_teardown(&v);
}
