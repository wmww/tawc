/* tawcroot seccomp filter.
 *
 * Goal: prove the IP allowlist contract before we wire up the rest. The
 * filter we install here is intentionally minimal:
 *
 *   1. arch != TAWCROOT_AUDIT_ARCH      → KILL_PROCESS  (defense in depth)
 *   2. instruction_pointer == &stub_ret  → ALLOW         (our raw syscall)
 *   3. syscall_nr == trap_syscall_nr    → TRAP          (smoke-test target)
 *   4. default                          → ALLOW
 *
 * The rootfs/prod filter expands rule 3 into a generated jump table covering the full trap
 * set. The shape of rules 1, 2, and 4 is the contract that Approach A
 * (re-exec into ourselves) depends on — see notes/tawcroot/sigsys-handler.md "Why non-PIE".
 *
 * cBPF is 32-bit. Our stub address is 64-bit on both arches; we have to
 * compare it in two halves, low first then high. seccomp_data layout is:
 *
 *     offset 0  nr                  (s32)
 *     offset 4  arch                (u32)
 *     offset 8  instruction_pointer (u64)   <- low @8, high @12
 *     offset 16 args[0]             (u64)
 *     ...
 *
 * The kernel struct above is the source of truth for the offsets
 * (notes/tawcroot/seccomp-filter.md's cBPF sketch agrees).
 */

#include <stddef.h>
#include <stdint.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

#include "arch.h"
#include "fdtab.h"
#include "filter.h"
#include "filter_build.h"
#include "raw_sys.h"
#include "sysnr.h"

/* The label sitting *immediately after* the SYSCALL/SVC inside the stub.
 *
 * Empirically on Linux x86_64 and aarch64 the kernel populates both
 * `seccomp_data.instruction_pointer` and `siginfo_t::si_call_addr` from
 * pt_regs->ip / pt_regs->pc, which the syscall-entry asm sets to the
 * address AFTER the trapping SYSCALL/SVC (i.e. the instruction the
 * kernel will return to). On x86_64 SYSCALL is 2 bytes; on aarch64 SVC
 * is 4 bytes. So the address we compare against is the `_ret` label,
 * not the `_insn` label sitting on the syscall instruction itself.
 * See notes/tawcroot/sigsys-handler.md "Issuing host syscalls from the handler" for
 * the full breakdown of the three related addresses. */
extern char tawcroot_raw_syscall_ret[];

long tawcroot_set_no_new_privs(void)
{
	return tawc_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
}

long tawcroot_install_smoke_filter(int trap_syscall_nr)
{
	int one[1] = { trap_syscall_nr };
	return tawcroot_install_filter(one, 1);
}

long tawcroot_install_filter(const int *trap_nrs, size_t n_traps)
{
	/* The cBPF jump-table covering the trap set. Two instructions per
	 * trapped syscall: one JEQ on nr, then a RET TRAP if matched. We
	 * emit a fall-through RET ALLOW at the end. The kernel limit is
	 * BPF_MAXINSNS = 4096 — plenty for our ~30 traps. The build itself
	 * is in `filter_build.c` (PROD_C_FOR_TESTS) so unit tests can
	 * walk the program through a hosted BPF interpreter. */
	struct sock_filter prog[4096];
	/* Trap the entire reserved half-space [BASE, ∞) for close via a
	 * range compare — covers fds reserved at runtime (shm/chroot), not
	 * just the install-time set. Disable the fast path (floor 0 → close
	 * TRAPs unconditionally) only if nothing is reserved yet; in
	 * production rootfs_fd is always reserved before install. */
	int floor = (tawcroot_n_reserved_fds > 0)
		? TAWCROOT_RESERVED_FD_BASE : 0;

	long n = tawcroot_build_filter(prog,
				       sizeof prog / sizeof prog[0],
				       trap_nrs, n_traps,
				       (uint64_t)(uintptr_t)&tawcroot_raw_syscall_ret[0],
				       floor,
				       TAWCROOT_AUDIT_ARCH);
	if (n < 0) return n;
	size_t i = (size_t)n;

	struct sock_fprog fprog = {
		.len = (unsigned short)i,
		.filter = prog,
	};
#ifdef TAWCROOT_TRACE
	{
		/* Dump the assembled BPF program to fd 2 in one write per line
		 * so it doesn't interleave with stdout-bound trace output. */
		for (size_t k = 0; k < i; k++) {
			char line[80];
			size_t li = 0;
			const char *p = "[F] ";
			while (*p) line[li++] = *p++;
			/* idx */
			char tmp[24]; int tn;
			unsigned long v;
			tn = 0; v = k; if (v == 0) tmp[tn++] = '0';
			while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
			while (tn--) line[li++] = tmp[tn];
			line[li++] = ' ';
			p = "code=";
			while (*p) line[li++] = *p++;
			tn = 0; v = prog[k].code; if (v == 0) tmp[tn++] = '0';
			while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
			while (tn--) line[li++] = tmp[tn];
			line[li++] = ' ';
			p = "jt=";
			while (*p) line[li++] = *p++;
			tn = 0; v = prog[k].jt; if (v == 0) tmp[tn++] = '0';
			while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
			while (tn--) line[li++] = tmp[tn];
			line[li++] = ' ';
			p = "jf=";
			while (*p) line[li++] = *p++;
			tn = 0; v = prog[k].jf; if (v == 0) tmp[tn++] = '0';
			while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
			while (tn--) line[li++] = tmp[tn];
			line[li++] = ' ';
			p = "k=";
			while (*p) line[li++] = *p++;
			tn = 0; v = prog[k].k; if (v == 0) tmp[tn++] = '0';
			while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
			while (tn--) line[li++] = tmp[tn];
			line[li++] = '\n';
			TAWC_RAW(TAWC_SYS_write, 2, (long)line, (long)li, 0, 0, 0);
		}
	}
#endif
	return tawc_seccomp(SECCOMP_SET_MODE_FILTER, 0, &fprog);
}
