/* Pure cBPF builder. See filter_build.h for the API contract.
 *
 * Lives in PROD_C_FOR_TESTS so the cleat orchestrator can build the
 * program in its own address space (without touching seccomp(2)) and
 * walk it through a tiny BPF interpreter for table-driven testing.
 *
 * This file MUST stay free of `raw_sys.h`, `fdtab.h`, or any header
 * that pulls in our process-globals — otherwise the hosted glibc test
 * build clashes with `_start`/raw-syscall expectations. The dispatch-
 * table source-of-truth is upstream of this builder; trap_nrs comes
 * in as a plain array.
 */

#include <stddef.h>
#include <stdint.h>

#include <linux/filter.h>
#include <linux/seccomp.h>

#include "errno_neg.h"
#include "filter_build.h"
#include "sysnr.h"

/* sock_filter constructors. Plain brace initializers (not compound
 * literals with an explicit cast — clang rejects those in array-
 * initializer position with `-Werror -Wmissing-braces`).
 *
 * sock_filter layout: { __u16 code, __u8 jt, __u8 jf, __u32 k }.
 *
 * <linux/filter.h> reuses `BPF_A` as an accumulator-source flag,
 * hence the TAWC_ prefix on these. */
#define TAWC_BPF_S(_code, _k)            { (_code), 0, 0, (_k) }
#define TAWC_BPF_J(_code, _k, _jt, _jf)  { (_code), (_jt), (_jf), (_k) }

long tawcroot_build_filter(struct sock_filter *prog, size_t prog_cap,
			   const int *trap_nrs, size_t n_traps,
			   uint64_t stub_ret_addr,
			   int reserved_fd_floor,
			   uint32_t audit_arch)
{
	if (!prog || prog_cap == 0) return TAWC_EINVAL;
	if (n_traps > 1900) return TAWC_E2BIG;  /* kernel cap is 4096 */

	const uint32_t stub_lo = (uint32_t)(stub_ret_addr & 0xffffffffu);
	const uint32_t stub_hi = (uint32_t)(stub_ret_addr >> 32);
	size_t i = 0;

	/* Cap-check helper. The fixed-size sequences below are short
	 * enough that we just check up front per emit; clearer than
	 * threading a "did we overflow" flag. */
#define EMIT_OR_FAIL(_ins) do {                                            \
		if (i >= prog_cap) return TAWC_E2BIG;                      \
		prog[i++] = (struct sock_filter)_ins;                      \
	} while (0)

	/* Prologue: arch check, IP allowlist (low/high 32). */
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 4));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, audit_arch, 1, 0));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 8));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, stub_lo, 0, 3));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 12));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, stub_hi, 0, 1));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

	/* Load syscall_nr once, then linear JEQ + TRAP for each trap_nr.
	 * `close` is special-cased to a RANGE compare: TRAP only when
	 * args[0] >= reserved_fd_floor. This covers the entire reserved
	 * half-space [floor, ∞) in a fixed five-instruction block, so fds
	 * reserved AFTER filter install (shm_open, post-chroot root fd)
	 * are protected too — the earlier per-fd JEQ list baked in only
	 * the install-time set and let a guest close() of a runtime-
	 * reserved fd through to the kernel (silent state corruption; see
	 * issues/tawcroot-close-fastpath-misses-runtime-reserved-fds.md).
	 * The fast path still skips the handler for the common closefrom
	 * loop's low fds; only the ≤64 high fds in the loop now TRAP.
	 *
	 * args[0] is a u64 fd; we compare the low 32 bits only (fds never
	 * exceed INT_MAX, and a negative fd's 0xFFFF.... low word is ≥
	 * floor → traps → handler forwards the real close → kernel EBADF,
	 * which is correct). */
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 0));

	for (size_t t = 0; t < n_traps; t++) {
		if (trap_nrs[t] == TAWC_SYS_close && reserved_fd_floor > 0) {
			/* not-close jf skips: LD args, JGE, RET TRAP,
			 * RET ALLOW = 4, landing on the LD-nr reload. */
			EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
					(uint32_t)trap_nrs[t], 0, 4));
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 16));
			/* args[0] >= floor → fall through to RET TRAP;
			 * else jump +1 to RET ALLOW. */
			EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JGE | BPF_K,
					(uint32_t)reserved_fd_floor, 0, 1));
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_TRAP));
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
			/* Re-load nr so subsequent JEQs see A == nr. The
			 * not-close jf above lands here. */
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 0));
			continue;
		}
		EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
				(uint32_t)trap_nrs[t], 0, 1));
		EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_TRAP));
	}

	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

#undef EMIT_OR_FAIL
	return (long)i;
}
