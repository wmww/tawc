/* Unit tests for guest sigaltstack virtualization (src/sigalt.c). */

#include <cleat/test.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#include "sigalt.h"

#ifndef SS_AUTODISARM
# define SS_AUTODISARM (1U << 31)
#endif

static char g_buf[65536];
static const stack_t DISABLED = { .ss_flags = SS_DISABLE };

static stack_t mk(void *sp, int flags, size_t size)
{
	return (stack_t){ .ss_sp = sp, .ss_flags = flags, .ss_size = size };
}

/* Stands in for the real off-stack sigaltstack. */
static stack_t g_applied;
static long g_apply_rv;
static int g_apply_calls;

static long fake_apply(const stack_t *ss)
{
	g_apply_calls++;
	if (g_apply_rv < 0) return g_apply_rv;
	g_applied = *ss;
	return 0;
}

static void reset(void)
{
	tawc_sigalt_reset();
	g_applied = DISABLED;
	g_apply_rv = 0;
	g_apply_calls = 0;
}

#define TID 100

/* What handle_exit does: the live slot is freed by zeroing the byte
 * at the exit instruction. */
static void thread_exit(const stack_t *cur, int tid)
{
	uint8_t *busy = tawc_sigalt_thread_exit(cur, tid);
	if (busy) *busy = 0;
}

static void commit_ok_(TestCtx *test_ctx, stack_t *cur, const stack_t *ss,
		       int tid)
{
	test_int_eq(tawc_sigalt_commit(cur, ss, tid, fake_apply), 0);
	/* The kernel gets exactly what uc_stack will hold. */
	test_true(g_applied.ss_sp == cur->ss_sp);
	test_int_eq(g_applied.ss_size, cur->ss_size);
	test_int_eq(g_applied.ss_flags, cur->ss_flags);
}
#define commit_ok(...) commit_ok_(test_ctx, __VA_ARGS__)

test(sigalt_big_stack_installed_verbatim)
{
	reset();
	stack_t cur = DISABLED, old;
	stack_t ss = mk(g_buf, 0, TAWC_SIGALT_MIN);
	test_int_eq(tawc_sigalt_check(&cur, 1, &ss, &old), 0);
	test_int_eq(old.ss_flags, SS_DISABLE);
	commit_ok(&cur, &ss, TID);
	test_true(cur.ss_sp == (void *)g_buf);
	test_int_eq(cur.ss_size, TAWC_SIGALT_MIN);
}

test(sigalt_small_stack_substituted_and_read_back)
{
	reset();
	stack_t cur = DISABLED, old;
	stack_t ss = mk(g_buf, 0, TAWC_SIGALT_MIN - 1);
	commit_ok(&cur, &ss, TID);
	test_true(tawc_sigalt_is_slab(cur.ss_sp));
	test_int_eq(cur.ss_size, TAWC_SIGALT_SLOT);

	test_int_eq(tawc_sigalt_check(&cur, 1, NULL, &old), 0);
	test_true(old.ss_sp == (void *)g_buf);
	test_int_eq(old.ss_size, TAWC_SIGALT_MIN - 1);
	test_int_eq(old.ss_flags, 0);

	/* Replacing a substituted stack reuses the slot. */
	void *slot = cur.ss_sp;
	ss = mk(g_buf + 8, 0, TAWC_SIGALT_KERN_MIN);
	commit_ok(&cur, &ss, TID);
	test_true(cur.ss_sp == slot);
}

test(sigalt_validation_matches_kernel)
{
	reset();
	stack_t cur = DISABLED;
	stack_t ss = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN - 1);
	test_int_eq(tawc_sigalt_check(&cur, 1, &ss, NULL), -ENOMEM);
	ss = mk(g_buf, 4, 32768);
	test_int_eq(tawc_sigalt_check(&cur, 1, &ss, NULL), -EINVAL);
	/* SS_DISABLE ignores size. */
	ss = mk(NULL, SS_DISABLE, 0);
	test_int_eq(tawc_sigalt_check(&cur, 1, &ss, NULL), 0);
}

test(sigalt_on_stack_is_eperm_and_reported)
{
	reset();
	stack_t cur = mk(g_buf, 0, 32768), old;
	stack_t ss = mk(NULL, SS_DISABLE, 0);
	uintptr_t sp = (uintptr_t)g_buf + 100;
	test_int_eq(tawc_sigalt_check(&cur, sp, &ss, &old), -EPERM);
	test_int_eq(tawc_sigalt_check(&cur, sp, NULL, &old), 0);
	test_int_eq(old.ss_flags, SS_ONSTACK);
}

test(sigalt_disable_and_exit_free_slots)
{
	reset();
	stack_t small = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN);
	stack_t cur[TAWC_SIGALT_SLOTS + 1];
	for (int i = 0; i < TAWC_SIGALT_SLOTS; i++) {
		cur[i] = DISABLED;
		commit_ok(&cur[i], &small, TID + i);
		test_true(tawc_sigalt_is_slab(cur[i].ss_sp));
	}
	/* Exhausted: fall back to the guest's own stack. */
	cur[TAWC_SIGALT_SLOTS] = DISABLED;
	commit_ok(&cur[TAWC_SIGALT_SLOTS], &small, TID + TAWC_SIGALT_SLOTS);
	test_true(cur[TAWC_SIGALT_SLOTS].ss_sp == (void *)g_buf);

	/* Disabling keeps the slot as a fallback. */
	stack_t dis = mk(NULL, SS_DISABLE, 0);
	commit_ok(&cur[0], &dis, TID);
	test_true(tawc_sigalt_is_slab(cur[0].ss_sp));
	stack_t x = DISABLED;
	commit_ok(&x, &small, TID + 1000);
	test_true(x.ss_sp == (void *)g_buf);
	/* Thread 1 exits. Thread 0 moves to a big stack: its slot is
	 * retired (the handler frame is still on it) until its next
	 * commit reclaims it. */
	thread_exit(&cur[1], TID + 1);
	stack_t big = mk(g_buf, 0, TAWC_SIGALT_MIN);
	commit_ok(&cur[0], &big, TID);
	commit_ok(&cur[0], &big, TID);

	stack_t a = DISABLED, b = DISABLED, c = DISABLED;
	commit_ok(&a, &small, TID + 2000);
	commit_ok(&b, &small, TID + 2001);
	commit_ok(&c, &small, TID + 2002);
	test_true(tawc_sigalt_is_slab(a.ss_sp));
	test_true(tawc_sigalt_is_slab(b.ss_sp));
	test_true(a.ss_sp != b.ss_sp);
	test_true(c.ss_sp == (void *)g_buf);
}

/* A disabled guest altstack becomes a fallback slot (the guest's stack
 * may be unmapped next, as musl does before a detached thread's
 * exit(2)); the guest still reads SS_DISABLE. */
test(sigalt_disable_keeps_fallback_slot)
{
	reset();
	stack_t cur = DISABLED, old;
	stack_t ss = mk(g_buf, 0, 32768);
	commit_ok(&cur, &ss, TID);
	test_true(g_applied.ss_sp == (void *)g_buf);
	ss = mk(NULL, SS_DISABLE, 0);
	commit_ok(&cur, &ss, TID);
	test_true(tawc_sigalt_is_slab(g_applied.ss_sp));
	test_int_eq(g_applied.ss_size, TAWC_SIGALT_SLOT);
	test_int_eq(g_apply_calls, 2);

	/* Guest view: disabled, and running on the fallback isn't
	 * "on stack" (changing it isn't EPERM). */
	uintptr_t sp = (uintptr_t)cur.ss_sp + 100;
	test_int_eq(tawc_sigalt_check(&cur, sp, &ss, &old), 0);
	test_true(old.ss_sp == NULL);
	test_int_eq(old.ss_size, 0);
	test_int_eq(old.ss_flags, SS_DISABLE);
}

/* SS_AUTODISARM survives substitution: the kernel's disarm/re-arm and
 * the guest's readback are as on the guest's own stack. A later plain
 * disable drops it, as the kernel would. */
test(sigalt_autodisarm_kept_on_slot)
{
	reset();
	stack_t cur = DISABLED, old;
	stack_t ss = mk(g_buf, SS_AUTODISARM, TAWC_SIGALT_KERN_MIN);
	commit_ok(&cur, &ss, TID);
	test_true(tawc_sigalt_is_slab(cur.ss_sp));
	test_int_eq((unsigned)cur.ss_flags, SS_AUTODISARM);
	test_int_eq(tawc_sigalt_check(&cur, 1, NULL, &old), 0);
	test_int_eq((unsigned)old.ss_flags, SS_AUTODISARM);
	/* Running on it: autodisarm means not "on stack". */
	uintptr_t sp = (uintptr_t)cur.ss_sp + 100;
	test_int_eq(tawc_sigalt_check(&cur, sp, &ss, NULL), 0);

	ss = mk(NULL, SS_DISABLE, 0);
	commit_ok(&cur, &ss, TID);
	test_int_eq(cur.ss_flags, 0);
	test_int_eq(tawc_sigalt_check(&cur, 1, NULL, &old), 0);
	test_int_eq(old.ss_flags, SS_DISABLE);
}

test(sigalt_disable_with_slab_exhausted_is_applied)
{
	reset();
	stack_t small = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN);
	stack_t s[TAWC_SIGALT_SLOTS];
	for (int i = 0; i < TAWC_SIGALT_SLOTS; i++) {
		s[i] = DISABLED;
		commit_ok(&s[i], &small, TID + i);
	}
	stack_t cur = mk(g_buf, 0, 32768);
	stack_t ss = mk(NULL, SS_DISABLE, 0);
	commit_ok(&cur, &ss, TID + 5000);
	test_int_eq(g_applied.ss_size, 0);
}

test(sigalt_ensure_gives_fallback_once)
{
	reset();
	stack_t cur = DISABLED, old;
	tawc_sigalt_ensure(&cur, fake_apply);
	test_true(tawc_sigalt_is_slab(cur.ss_sp));
	test_true(g_applied.ss_sp == cur.ss_sp);
	test_int_eq(g_applied.ss_size, TAWC_SIGALT_SLOT);
	test_int_eq(tawc_sigalt_check(&cur, 1, NULL, &old), 0);
	test_int_eq(old.ss_flags, SS_DISABLE);
	test_int_eq(old.ss_size, 0);

	/* Already has one (fallback or guest's): no-op. */
	tawc_sigalt_ensure(&cur, fake_apply);
	stack_t guest = mk(g_buf, 0, 32768);
	tawc_sigalt_ensure(&guest, fake_apply);
	test_int_eq(g_apply_calls, 1);

	/* A guest stack replaces the fallback, which is retired. */
	stack_t big = mk(g_buf, 0, TAWC_SIGALT_MIN);
	commit_ok(&cur, &big, TID);
	test_true(cur.ss_sp == (void *)g_buf);

	/* Apply failure leaves it alone and frees the slot. */
	stack_t c2 = DISABLED;
	g_apply_rv = -ENOMEM;
	tawc_sigalt_ensure(&c2, fake_apply);
	test_int_eq(c2.ss_flags, SS_DISABLE);
	test_true(c2.ss_sp == NULL);
}

test(sigalt_thread_exit_hands_back_live_slot_only)
{
	reset();
	stack_t none = DISABLED;
	test_true(tawc_sigalt_thread_exit(&none, TID) == NULL);
	stack_t big = mk(g_buf, 0, TAWC_SIGALT_MIN), cur = DISABLED;
	commit_ok(&cur, &big, TID);
	test_true(tawc_sigalt_thread_exit(&cur, TID) == NULL);

	stack_t small = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN);
	cur = DISABLED;
	commit_ok(&cur, &small, TID);
	void *slot = cur.ss_sp;
	uint8_t *busy = tawc_sigalt_thread_exit(&cur, TID);
	test_nonnull(busy);
	/* Not freed until the byte is zeroed... */
	stack_t other = DISABLED;
	commit_ok(&other, &small, TID + 1);
	test_true(other.ss_sp != slot);
	/* ...then it is. */
	*busy = 0;
	stack_t third = DISABLED;
	commit_ok(&third, &small, TID + 2);
	test_true(third.ss_sp == slot);
}

test(sigalt_apply_failure_changes_nothing)
{
	reset();
	stack_t small = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN);
	stack_t cur = DISABLED;
	g_apply_rv = -ENOMEM;
	test_int_eq(tawc_sigalt_commit(&cur, &small, TID, fake_apply), -ENOMEM);
	test_int_eq(cur.ss_flags, SS_DISABLE);
	/* The slot it tried to claim went back. */
	g_apply_rv = 0;
	stack_t s[TAWC_SIGALT_SLOTS];
	for (int i = 0; i < TAWC_SIGALT_SLOTS; i++) {
		s[i] = DISABLED;
		commit_ok(&s[i], &small, TID + i);
		test_true(tawc_sigalt_is_slab(s[i].ss_sp));
	}
}

test(sigalt_retired_slot_freed_by_owner_only)
{
	reset();
	stack_t small = mk(g_buf, 0, TAWC_SIGALT_KERN_MIN);
	stack_t dis = mk(NULL, SS_DISABLE, 0);
	stack_t cur[TAWC_SIGALT_SLOTS];
	for (int i = 0; i < TAWC_SIGALT_SLOTS; i++) {
		cur[i] = DISABLED;
		commit_ok(&cur[i], &small, TID + i);
	}
	void *slot0 = cur[0].ss_sp;
	commit_ok(&cur[0], &dis, TID);
	/* Another thread's commit doesn't free it. */
	stack_t other = DISABLED;
	commit_ok(&other, &small, TID + 1);
	test_true(other.ss_sp == (void *)g_buf);
	/* The owner's next commit does, and may take it right back. */
	commit_ok(&cur[0], &small, TID);
	test_true(cur[0].ss_sp == slot0);
}
