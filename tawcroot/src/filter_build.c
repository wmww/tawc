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
			   const int *reserved_fds, size_t n_reserved,
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
	 * `close` is special-cased to only TRAP when args[0] is in the
	 * reserved fd set — see filter.c's original block comment for
	 * the perf rationale (pacman/gpgme closefrom dance). */
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 0));

	for (size_t t = 0; t < n_traps; t++) {
		if (trap_nrs[t] == TAWC_SYS_close && reserved_fds && n_reserved > 0) {
			size_t n_res = n_reserved;
			/* JEQ-close jf skips: 1 (LD args) + n_res (JEQs)
			 * + 1 (RET ALLOW) + 1 (RET TRAP) + 1 (LD nr) = n_res + 4. */
			uint8_t jf_skip = (uint8_t)(4 + n_res);
			EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
					(uint32_t)trap_nrs[t], 0, jf_skip));
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 16));
			for (size_t rj = 0; rj < n_res; rj++) {
				uint8_t jt = (uint8_t)(n_res - rj);
				EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
						(uint32_t)reserved_fds[rj], jt, 0));
			}
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
			EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_TRAP));
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
