/* SIGSYS handler.
 *
 * The foundation smoke uses the observation slot to prove the
 * filter/handler contract: record what it saw and return -ENOSYS via
 * ucontext rewrite. Production dispatch uses the same hot-path
 * constraints (no malloc, no stdio, no libc with hidden state). See notes/tawcroot/sigsys-handler.md
 * "SIGSYS handler" and "Why the handler is async-signal-safe".
 *
 * The foundation smoke reads `tawcroot_handler_observe()` after triggering
 * a TRAP to validate that the handler ran and saw the right thing.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint64_t calls;
	long     last_nr;
	long     last_arg0;
	uintptr_t last_call_addr;   /* siginfo_t::si_call_addr */
	uintptr_t last_resume_pc;   /* mcontext_t resume PC    */
	int      last_si_code;
	int      last_si_arch;
} tawcroot_handler_obs;

/* Install the SIGSYS handler. Returns 0 / -errno (raw syscall). */
long tawcroot_install_handler(void);

/* Read the most-recent observation. Returns a snapshot — the caller may
 * inspect any field. Single-threaded testhost only (one writer, one
 * reader, plain stores). Defined only under TAWCROOT_TESTHOST; a
 * production caller fails at link. */
#ifdef TAWCROOT_TESTHOST
void tawcroot_handler_observe(tawcroot_handler_obs *out);
#endif
