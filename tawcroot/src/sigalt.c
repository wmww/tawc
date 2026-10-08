/* See include/sigalt.h.
 *
 * Slot state is one byte: free, live (CAS-claimed from free), or
 * retired. A slot's recorded guest values are only ever touched by the
 * thread whose altstack it is, so they need no synchronization. A live
 * slot's owner is found from the kernel's view (ss_sp inside the slab),
 * never from a tid: a fork child keeps working with its new tid, and
 * tid reuse can't alias.
 *
 * Retired slots are the exception: the thread swapped away from the
 * slot while its handler frame still sat on it, so it can't be handed
 * out until that frame is gone. They are tagged with the tid and freed
 * at its next commit or exit(2) — by then the thread is off the slot.
 * A reused tid freeing a dead thread's retired slot is harmless.
 *
 * Known leaks, all bounded by the exhaustion fallback: a thread killed
 * without exit(2); other threads' slots in a fork child; a slot claimed
 * by sigaltstack() inside an SS_AUTODISARM handler (the kernel drops
 * that setting when the handler returns). A CLONE_VM|CLONE_VFORK child
 * that replaces an inherited substituted stack frees the parent's slot
 * under it — accepted; nothing known does this. Same for a guest
 * SS_AUTODISARM handler, running on a substituted slot, that replaces
 * its altstack twice: the second commit frees the slot under it. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "sigalt.h"

#ifndef SS_AUTODISARM
# define SS_AUTODISARM (1U << 31)
#endif

_Static_assert(__atomic_always_lock_free(1, 0),
	       "byte atomics must be lock-free for AS-safety");

static unsigned char g_slab[TAWC_SIGALT_SLOTS][TAWC_SIGALT_SLOT]
	__attribute__((aligned(16)));
enum { SLOT_FREE, SLOT_LIVE, SLOT_RETIRED };
static uint8_t g_busy[TAWC_SIGALT_SLOTS];
static int g_retired_tid[TAWC_SIGALT_SLOTS];
static struct { void *sp; size_t size; } g_guest[TAWC_SIGALT_SLOTS];

static int slot_of(const stack_t *ss)
{
	uintptr_t p = (uintptr_t)ss->ss_sp, base = (uintptr_t)g_slab;
	if (!ss->ss_size || p < base || p >= base + sizeof g_slab)
		return -1;
	return (int)((p - base) / TAWC_SIGALT_SLOT);
}

int tawc_sigalt_is_slab(const void *p)
{
	stack_t ss = { .ss_sp = (void *)(uintptr_t)p, .ss_size = 1 };
	return slot_of(&ss) >= 0;
}

/* kernel on_sig_stack() */
static int on_stack(const stack_t *cur, uintptr_t sp)
{
	if ((unsigned)cur->ss_flags & SS_AUTODISARM) return 0;
	uintptr_t base = (uintptr_t)cur->ss_sp;
	return sp > base && sp - base <= cur->ss_size;
}

/* A slot recorded with guest size 0 is a fallback: the guest has no
 * altstack, we keep one so our SIGSYS frame never needs its stack. */
static int is_fallback(int k)
{
	return k >= 0 && g_guest[k].size == 0;
}

long tawc_sigalt_check(const stack_t *cur, uintptr_t guest_sp,
		       const stack_t *new_ss, stack_t *old)
{
	int k = slot_of(cur);
	int on = !is_fallback(k) && on_stack(cur, guest_sp);
	if (new_ss) {
		if (on) return TAWC_EPERM;
		unsigned mode = (unsigned)new_ss->ss_flags & ~SS_AUTODISARM;
		if (mode != SS_DISABLE && mode != SS_ONSTACK && mode != 0)
			return TAWC_EINVAL;
		if (mode != SS_DISABLE &&
		    new_ss->ss_size < TAWC_SIGALT_KERN_MIN)
			return TAWC_ENOMEM;
	}
	if (old) {
		old->ss_sp   = k >= 0 ? g_guest[k].sp   : cur->ss_sp;
		old->ss_size = k >= 0 ? g_guest[k].size : cur->ss_size;
		old->ss_flags = (int)
			((old->ss_size ? (on ? SS_ONSTACK : 0) : SS_DISABLE) |
			 ((unsigned)cur->ss_flags & SS_AUTODISARM));
	}
	return 0;
}

static int slot_claim(void)
{
	for (int k = 0; k < TAWC_SIGALT_SLOTS; k++) {
		uint8_t expected = SLOT_FREE;
		if (__atomic_compare_exchange_n(&g_busy[k], &expected,
						SLOT_LIVE, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return k;
	}
	return -1;
}

static void slot_release(int k)
{
	__atomic_store_n(&g_busy[k], SLOT_FREE, __ATOMIC_RELEASE);
}

static void slot_retire(int k, int tid)
{
	__atomic_store_n(&g_retired_tid[k], tid, __ATOMIC_RELAXED);
	__atomic_store_n(&g_busy[k], SLOT_RETIRED, __ATOMIC_RELEASE);
}

static void reclaim_retired(int tid)
{
	for (int k = 0; k < TAWC_SIGALT_SLOTS; k++)
		if (__atomic_load_n(&g_busy[k], __ATOMIC_ACQUIRE) ==
			    SLOT_RETIRED &&
		    __atomic_load_n(&g_retired_tid[k], __ATOMIC_RELAXED) == tid)
			slot_release(k);
}

long tawc_sigalt_commit(stack_t *cur, const stack_t *new_ss, int tid,
			tawc_sigalt_apply_fn apply)
{
	int k_old = slot_of(cur);
	int k_new = -1;
	unsigned mode = (unsigned)new_ss->ss_flags & ~SS_AUTODISARM;
	stack_t next = *new_ss;

	reclaim_retired(tid);
	if (mode == SS_DISABLE || new_ss->ss_size < TAWC_SIGALT_MIN) {
		/* Disabling keeps (or gets) a fallback slot: threads often
		 * disable and unmap their altstack right before exiting on
		 * a stack they've also unmapped (musl), and the exit(2) trap
		 * still needs somewhere to put its frame. */
		k_new = k_old >= 0 ? k_old : slot_claim();
		if (k_new >= 0) {
			next.ss_sp    = g_slab[k_new];
			next.ss_size  = TAWC_SIGALT_SLOT;
			next.ss_flags = 0;
		} else if (mode == SS_DISABLE) {
			next.ss_sp = NULL;
			next.ss_size = 0;
		}
	}
	long r = apply(&next);
	if (r < 0) {
		if (k_new >= 0 && k_new != k_old) slot_release(k_new);
		return r;
	}
	if (k_new >= 0) {
		g_guest[k_new].sp   = mode == SS_DISABLE ? NULL : new_ss->ss_sp;
		g_guest[k_new].size = mode == SS_DISABLE ? 0 : new_ss->ss_size;
	}
	*cur = next;
	if (k_old >= 0 && k_old != k_new)
		slot_retire(k_old, tid);
	return 0;
}

void tawc_sigalt_ensure(stack_t *cur, tawc_sigalt_apply_fn apply)
{
	if (cur->ss_size && !((unsigned)cur->ss_flags & SS_DISABLE))
		return;
	int k = slot_claim();
	if (k < 0) return;
	stack_t next = { .ss_sp = g_slab[k], .ss_flags = 0,
			 .ss_size = TAWC_SIGALT_SLOT };
	if (apply(&next) < 0) {
		slot_release(k);
		return;
	}
	g_guest[k].sp   = NULL;
	g_guest[k].size = 0;
	*cur = next;
}

void tawc_sigalt_thread_exit(const stack_t *cur, int tid)
{
	int k = slot_of(cur);
	if (k >= 0) slot_release(k);
	reclaim_retired(tid);
}

void tawc_sigalt_reset(void)
{
	for (int k = 0; k < TAWC_SIGALT_SLOTS; k++)
		slot_release(k);
}
