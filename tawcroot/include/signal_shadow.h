/* Guest-visible SIGSYS state shadows for syscalls_control.c.
 *
 * Two pieces of state, with different scoping:
 *
 *   - "blocked": per-thread shadow tracking whether the guest blocked
 *     SIGSYS via rt_sigprocmask. POSIX: signal masks are per-thread,
 *     and the kernel mask in uc->uc_sigmask is per-thread for free,
 *     but we deliberately strip SIGSYS from it (to keep traps coming)
 *     so the shadow has to live separately. Implemented as a fixed
 *     open-address TID-keyed table with linear probing.
 *
 *   - "action": process-global shadow of the guest's sigaction(SIGSYS).
 *     POSIX: signal dispositions are process-wide. Implemented as a
 *     seqlock-protected byte buffer.
 *
 * Both helpers are pure (no syscalls), async-signal-safe, and safe to
 * call from multiple threads concurrently. They make no allocation
 * decisions and use only __atomic_* builtins on lock-free widths
 * (a _Static_assert in the .c locks that invariant in) — usable from
 * the `-nostdlib -ffreestanding` production binary and from hosted
 * glibc unit tests without translation.
 *
 * Issue: tawcroot-handler-signal-state-not-thread-safe.md
 */

#ifndef TAWCROOT_SIGNAL_SHADOW_H
#define TAWCROOT_SIGNAL_SHADOW_H

#include <stddef.h>

/* Size of the kernel struct sigaction at sigsetsize=8 — must match
 * what handle_rt_sigaction copies between guest and shadow. */
#if defined(__x86_64__)
# define TAWC_KERN_SIGACTION_SIZE 32
#elif defined(__aarch64__)
# define TAWC_KERN_SIGACTION_SIZE 24
#else
# error "unsupported arch"
#endif

/* Per-thread "blocked" shadow. tid is the kernel tid (gettid()).
 * Unknown tids default to 0 (not blocked). */
int  tawc_sigshadow_blocked_get(int tid);
void tawc_sigshadow_blocked_set(int tid, int blocked);

/* Process-global sigaction shadow. _get fills `out` with exactly
 * TAWC_KERN_SIGACTION_SIZE bytes from the most recent _set, or all
 * zeros if no guest sigaction(SIGSYS) has ever happened (BSS-zero
 * default — also matches a kernel SIG_DFL action, which is what the
 * guest expects to read back as the pre-call disposition). _set
 * publishes a new TAWC_KERN_SIGACTION_SIZE-byte snapshot. */
void tawc_sigshadow_action_get(unsigned char *out);
void tawc_sigshadow_action_set(const unsigned char *in);

/* Reset all state to "fresh process" — empty TID table, action unset.
 * For tests; not called from production. */
void tawc_sigshadow_reset(void);

/* Capacity of the TID-keyed blocked table. Exposed so overflow tests
 * don't hard-code the constant. For tests; not called from production. */
unsigned tawc_sigshadow_capacity(void);

#endif
