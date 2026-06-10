/* Hosted implementation of tawcroot_raw_syscall for the cleat tests binary.
 *
 * In production the only definition lives in src/arch/<arch>_stub.S, which
 * also carries `_start` — linking that into a glibc binary clashes. This
 * shim provides the same symbol with the same contract (raw kernel return,
 * negative values are -errno; no libc errno involved) so the whole
 * production C tree can compile into the hosted tests binary under ASan.
 *
 * Implemented with inline asm, NOT glibc syscall(2): syscall(2) folds the
 * kernel return into -1/errno and we'd have to lossily reconstruct it.
 *
 * tawcroot_test_raw_hook is a test-installable interceptor: when non-NULL
 * and it returns true, *ret is the syscall result and the kernel is never
 * entered. Hosted tests use it to deny exit_group, fake chroot, inject
 * faults (EINTR/ENOSPC/EMFILE), or observe syscall sequences. Install
 * NULL to restore pass-through; tests must reset it on the way out.
 */

#include "raw_syscall_host.h"

bool (*tawcroot_test_raw_hook)(long nr, const long args[6], long *ret);

long tawcroot_raw_syscall(long nr, long a, long b, long c,
			  long d, long e, long f)
{
	if (tawcroot_test_raw_hook) {
		long args[6] = { a, b, c, d, e, f };
		long ret;
		if (tawcroot_test_raw_hook(nr, args, &ret))
			return ret;
	}
#if defined(__x86_64__)
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	long ret;
	/* tawcroot_raw_syscall_ret mirrors the asm stub: the label sits
	 * immediately after the syscall insn, where the kernel reports
	 * seccomp_data.instruction_pointer. filter.c takes its address. */
	__asm__ volatile("syscall\n"
			 ".globl tawcroot_raw_syscall_ret\n"
			 "tawcroot_raw_syscall_ret:"
			 : "=a"(ret)
			 : "0"(nr), "D"(a), "S"(b), "d"(c),
			   "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
#elif defined(__aarch64__)
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0\n"
			 ".globl tawcroot_raw_syscall_ret\n"
			 "tawcroot_raw_syscall_ret:"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
#else
#error "no hosted raw-syscall shim for this arch"
#endif
}

/* SA_RESTORER trampoline, same shape as the asm stubs'. handler.c
 * references it; hosted tests don't install the SIGSYS handler, but the
 * symbol must resolve (and stays faithful if one ever does). */
#if defined(__x86_64__)
__asm__(".text\n"
	".globl tawcroot_sigreturn_trampoline\n"
	".type tawcroot_sigreturn_trampoline, @function\n"
	"tawcroot_sigreturn_trampoline:\n"
	"	mov $15, %rax\n"	/* SYS_rt_sigreturn */
	"	syscall\n"
	"	ud2\n");
#elif defined(__aarch64__)
__asm__(".text\n"
	".globl tawcroot_sigreturn_trampoline\n"
	".type tawcroot_sigreturn_trampoline, @function\n"
	"tawcroot_sigreturn_trampoline:\n"
	"	mov x8, #139\n"		/* SYS_rt_sigreturn */
	"	svc #0\n"
	"	brk #0\n");
#endif
