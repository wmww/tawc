/* Guest sigaltstack virtualization.
 *
 * The SIGSYS handler is registered SA_ONSTACK, so a guest altstack
 * receives our signal frame + handler chain on every trapped syscall
 * the thread makes. A guest stack smaller than TAWC_SIGALT_MIN is
 * swapped for a tawcroot-owned TAWC_SIGALT_SLOT-byte slot from a fixed
 * BSS slab; the guest still reads back its own ss_sp/ss_size. A thread
 * with no guest altstack gets a "fallback" slot at its first trap
 * (guest reads SS_DISABLE), so our frame never lands on a guest stack
 * that may already be unmapped (musl's detached-thread exit).
 *
 * sigaltstack can't be forwarded as-is from the handler: the kernel
 * EPERMs while we run on the altstack. Nor can we leave the change in
 * uc->uc_stack for sigreturn's restore_altstack: x86_64 drops that edit
 * when the handler ran on the altstack (arm64 applies it). So `apply`
 * issues the real call with SP moved off the altstack
 * (tawcroot_raw_syscall_off_stack), and `cur` (&uc->uc_stack) is kept
 * equal to it so the restore is a no-op. _check replicates
 * do_sigaltstack's validation, with the guest's SP deciding
 * on-stack-ness.
 *
 * Async-signal-safe: lock-free atomics only, no syscalls but `apply`,
 * no libc. See notes/tawcroot/sigsys-handler.md "Handler stack
 * budget". */

#pragma once

#include <signal.h>
#include <stdint.h>

#if defined(__aarch64__)
# define TAWC_SIGALT_KERN_MIN 5120   /* kernel MINSIGSTKSZ */
# define TAWC_SIGALT_MIN      12288
#elif defined(__x86_64__)
# define TAWC_SIGALT_KERN_MIN 2048
# define TAWC_SIGALT_MIN      8192
#else
# error "unsupported arch"
#endif
#define TAWC_SIGALT_SLOT  16384
#define TAWC_SIGALT_SLOTS 256

/* Pure. Validates `new_ss` (nullable) like the kernel would and fills
 * `old` (nullable) with the guest-visible current state. `guest_sp` is
 * the trapping thread's SP from the ucontext. Returns 0 or -errno;
 * nothing is written on error. */
long tawc_sigalt_check(const stack_t *cur, uintptr_t guest_sp,
		       const stack_t *new_ss, stack_t *old);

/* Installs `ss` as the kernel's altstack for the calling thread. The
 * production one is tawc_sigalt_apply_kernel; tests substitute. */
typedef long (*tawc_sigalt_apply_fn)(const stack_t *ss);
long tawc_sigalt_apply_kernel(const stack_t *ss);

/* Apply an already-checked `new_ss` via `apply` and mirror it into
 * `cur`, substituting a slab slot for an undersized stack and keeping
 * (or claiming) a fallback slot on SS_DISABLE. If the slab is exhausted
 * the guest's stack is installed as-is (no worse than having no floor).
 * A slot given up here is still under the handler's frame, so it is
 * retired under `tid` rather than freed; the thread's next commit or
 * exit frees it. Returns 0 or apply's -errno, in which case nothing
 * changed. */
long tawc_sigalt_commit(stack_t *cur, const stack_t *new_ss, int tid,
			tawc_sigalt_apply_fn apply);

/* Give a thread with no altstack a fallback slot (guest still reads
 * SS_DISABLE). Called on every trap: a compare once the thread has
 * one, so the exit(2) trap can always deliver even after musl unmaps
 * the thread's own stack. Best effort: no-op if the slab is exhausted
 * or apply fails. */
void tawc_sigalt_ensure(stack_t *cur, tawc_sigalt_apply_fn apply);

/* Thread is exiting: frees its retired slots and returns the state
 * byte of its live slot, or NULL. The handler is running on that slot,
 * so the caller zeroes the byte as its very last memory access before
 * the exit syscall (tawcroot_raw_syscall_off_stack's `release`). */
uint8_t *tawc_sigalt_thread_exit(const stack_t *cur, int tid);

/* For tests; not called from production. */
int  tawc_sigalt_is_slab(const void *p);
void tawc_sigalt_reset(void);
