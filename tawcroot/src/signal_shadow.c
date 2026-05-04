/* See include/signal_shadow.h.
 *
 * Async-signal-safe by construction: only __atomic_* builtins (which
 * we _Static_assert below are lock-free for our widths, so the
 * compiler emits inline ops, never a libgcc outline call that may
 * take a mutex). No malloc, no syscalls, no libc calls.
 *
 * # Threading model
 *
 * Per-thread "blocked" — open-address table indexed by hash(tid) %
 * N_SLOTS, linear probing. A slot has (tid, blocked); empty slots
 * have tid=0. Get scans probes; if it hits a matching tid it returns
 * the bit, if it hits an empty slot it returns 0 (default unblocked),
 * if it walks the whole table without a match it returns 0. Set
 * scans the same way: matching tid → update bit; empty slot → only
 * CAS-claim if `blocked=1` (an absent slot already reads as 0, so
 * set(_,0) on an unknown tid is a no-op). This caps slot pressure
 * at the number of threads with SIGSYS *currently* blocked, not
 * the cumulative number that ever called sigprocmask.
 *
 * Single-writer-per-tid invariant: tawc_sigshadow_blocked_set is only
 * called from handle_rt_sigprocmask, which runs on the trapping
 * thread itself with SIGSYS masked (no SA_NODEFER). Two writers can
 * never have the same tid in production, so the same-tid update path
 * needs no CAS — a plain atomic store is enough. Different-tid
 * concurrency is real and handled via the CAS-claim spin.
 *
 * Overflow handling: if N_SLOTS distinct tids hold blocked=1
 * simultaneously and a new tid wants to block, set() __builtin_trap()s
 * (SIGILL). Silently dropping the update would leave the guest with
 * a wrong shadow mask and no diagnostic; a hard crash points at the
 * exact line and tells us to bump N_SLOTS.
 *
 * TID reuse is a small remaining race: if thread A blocks SIGSYS,
 * exits without unblocking, and the kernel reuses A's tid for thread
 * B, B reads the stale "blocked=1" until it issues its own
 * rt_sigprocmask. Mitigation: realistic guests set-then-read; the
 * kernel doesn't reuse tids while any thread holds them; pid_max is
 * typically ≥32K. Documented in
 * tawcroot-handler-signal-state-not-thread-safe.md. Also note that
 * leaked slots (a thread blocks SIGSYS, never unblocks, then exits)
 * count toward N_SLOTS until tid reuse overwrites them — same issue
 * file tracks the planned exit-side hook fix.
 *
 * Process-global sigaction — classic seqlock. Even sequence = stable,
 * odd = writer in progress. Writers CAS(seq, even, even+1) to claim,
 * publish the bytes via per-byte relaxed atomic stores, then store
 * seq=even+2 with release. Readers snapshot seq, copy via per-byte
 * relaxed atomic loads, acquire-fence, snapshot seq again, retry on
 * disagreement or odd intermediate. Per-byte atomic access on
 * g_action (rather than a plain memcpy) is what keeps this race-free
 * under the C memory model: the seqlock lets us *discard* a torn
 * read, but the load itself must still be a well-defined atomic op.
 * Multi-writer safe via the CAS-claim spin. Action size is small
 * (24/32 bytes) so the inline copy beats a pointer-publish scheme
 * that would need a slab.
 *
 * The "guest ever set sigaction" flag is omitted on purpose: an
 * unset shadow is BSS-zero, and a kernel SIG_DFL action is also all
 * zeros, so callers see the right thing without a separate bit.
 */

#include <stddef.h>
#include <stdint.h>

#include "signal_shadow.h"

/* If any of these widths ever require a libgcc outline atomic call
 * (e.g. on a 32-bit ARM port where 8-byte CAS isn't lock-free), the
 * outline implementation may take a process-global mutex — which
 * isn't async-signal-safe and would deadlock the SIGSYS handler.
 * Fail the build loud the day someone moves us to such an ABI. */
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
	       "uint32_t atomics must be lock-free for AS-safety");
_Static_assert(__atomic_always_lock_free(sizeof(int), 0),
	       "int atomics must be lock-free for AS-safety");
_Static_assert(__atomic_always_lock_free(1, 0),
	       "byte atomics must be lock-free for AS-safety");

/* ---------- per-thread "blocked" shadow ---------- */

#define N_SLOTS 256

struct slot {
	int tid;        /* 0 = empty */
	uint8_t blocked;
	uint8_t _pad[3];
};

static struct slot g_slots[N_SLOTS];

static unsigned slot_hash(int tid)
{
	/* Linear probe from a hash, so we don't pile up at modulo
	 * collisions on small tid sets. Knuth multiplicative hash.
	 * Robustness, not security — tids aren't adversarial. */
	uint32_t u = (uint32_t)tid;
	u *= 2654435761u;
	return u % N_SLOTS;
}

int tawc_sigshadow_blocked_get(int tid)
{
	if (tid <= 0) return 0;
	unsigned start = slot_hash(tid);
	for (unsigned i = 0; i < N_SLOTS; i++) {
		unsigned k = (start + i) % N_SLOTS;
		int slot_tid = __atomic_load_n(&g_slots[k].tid, __ATOMIC_ACQUIRE);
		if (slot_tid == tid)
			return __atomic_load_n(&g_slots[k].blocked,
					      __ATOMIC_RELAXED);
		if (slot_tid == 0)
			return 0;  /* probe-stop on empty slot */
	}
	return 0;  /* table full, no match */
}

void tawc_sigshadow_blocked_set(int tid, int blocked)
{
	if (tid <= 0) return;
	uint8_t v = blocked ? 1 : 0;
	unsigned start = slot_hash(tid);
	for (unsigned i = 0; i < N_SLOTS; i++) {
		unsigned k = (start + i) % N_SLOTS;
		int slot_tid = __atomic_load_n(&g_slots[k].tid, __ATOMIC_ACQUIRE);
		if (slot_tid == tid) {
			__atomic_store_n(&g_slots[k].blocked, v,
					 __ATOMIC_RELAXED);
			return;
		}
		if (slot_tid == 0) {
			/* Don't claim a slot just to record blocked=0:
			 * absent slot already reads as unblocked (the
			 * kernel default), so set(_, 0) on a tid we've
			 * never seen is a no-op. This caps slot pressure
			 * at "threads currently with SIGSYS blocked",
			 * not "threads that ever called sigprocmask". */
			if (!blocked) return;
			int expected = 0;
			if (__atomic_compare_exchange_n(
					&g_slots[k].tid, &expected, tid,
					0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				__atomic_store_n(&g_slots[k].blocked, v,
						 __ATOMIC_RELAXED);
				return;
			}
			/* Another setter beat us to this slot. If they
			 * claimed for OUR tid, update; otherwise keep
			 * probing. */
			if (expected == tid) {
				__atomic_store_n(&g_slots[k].blocked, v,
						 __ATOMIC_RELAXED);
				return;
			}
			/* fall through to next probe */
		}
	}
	/* Table is full of distinct concurrently-blocked tids. With
	 * blocked=0 sets no longer claiming slots, hitting this means
	 * N_SLOTS guest threads have SIGSYS *actively* blocked at the
	 * same time — well outside any realistic workload. We don't
	 * silently drop the update because that produces a wrong-mask
	 * read for the affected thread with no signal to the user.
	 * __builtin_trap() lands as SIGILL on the trapping thread, so
	 * the failure is visible in logcat / a coredump pointing at
	 * this line; the fix is bumping N_SLOTS or moving to a
	 * growable backing store. */
	__builtin_trap();
}

/* ---------- process-global sigaction shadow ---------- */

static uint32_t      g_action_seq;                        /* even=stable, odd=writing */
static unsigned char g_action[TAWC_KERN_SIGACTION_SIZE];  /* protected by seq */

/* Per-byte relaxed atomic copy. The seqlock guards us from torn
 * VALUES, but each individual load/store must still be a well-defined
 * atomic op or the C memory model considers it a data race. RELAXED
 * is enough — ordering across the buffer is established by the
 * acquire/release pair on g_action_seq. */
static void copy_bytes_atomic_load(unsigned char *dst, const unsigned char *src,
				   size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

static void copy_bytes_atomic_store(unsigned char *dst, const unsigned char *src,
				    size_t n)
{
	for (size_t i = 0; i < n; i++)
		__atomic_store_n(&dst[i], src[i], __ATOMIC_RELAXED);
}

static void zero_bytes_atomic(unsigned char *dst, size_t n)
{
	for (size_t i = 0; i < n; i++)
		__atomic_store_n(&dst[i], 0, __ATOMIC_RELAXED);
}

void tawc_sigshadow_action_get(unsigned char *out)
{
	for (;;) {
		uint32_t s1 = __atomic_load_n(&g_action_seq, __ATOMIC_ACQUIRE);
		if (s1 & 1) continue;  /* writer in progress, retry */
		copy_bytes_atomic_load(out, g_action, TAWC_KERN_SIGACTION_SIZE);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint32_t s2 = __atomic_load_n(&g_action_seq, __ATOMIC_RELAXED);
		if (s1 == s2) return;
		/* writer landed mid-copy, retry */
	}
}

static void action_writer_acquire(uint32_t *out_s)
{
	uint32_t s;
	for (;;) {
		s = __atomic_load_n(&g_action_seq, __ATOMIC_RELAXED);
		if (s & 1) continue;  /* another writer holds the lock */
		uint32_t expected = s;
		if (__atomic_compare_exchange_n(&g_action_seq, &expected,
						s + 1, 0,
						__ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			break;
	}
	*out_s = s;
}

void tawc_sigshadow_action_set(const unsigned char *in)
{
	uint32_t s;
	action_writer_acquire(&s);
	copy_bytes_atomic_store(g_action, in, TAWC_KERN_SIGACTION_SIZE);
	__atomic_store_n(&g_action_seq, s + 2, __ATOMIC_RELEASE);
}

void tawc_sigshadow_reset(void)
{
	for (unsigned i = 0; i < N_SLOTS; i++) {
		__atomic_store_n(&g_slots[i].tid, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&g_slots[i].blocked, 0, __ATOMIC_RELAXED);
	}
	uint32_t s;
	action_writer_acquire(&s);
	zero_bytes_atomic(g_action, TAWC_KERN_SIGACTION_SIZE);
	__atomic_store_n(&g_action_seq, s + 2, __ATOMIC_RELEASE);
}

unsigned tawc_sigshadow_capacity(void)
{
	return N_SLOTS;
}
