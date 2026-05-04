# tawcroot: residual TID-reuse race on guest "SIGSYS blocked" shadow

Slot pressure has been narrowed (see "Slot leak — narrowed" below)
and overflow now panics loudly instead of failing silent. What
remains is the small TID-reuse race inherent to a per-thread state
model without a thread-exit hook.

## What was fixed

`syscalls_control.c` previously kept two pieces of guest-visible
signal state as plain process-globals — a torn-write hazard the
moment a multi-threaded guest touched signal state. Both have moved
into `signal_shadow.c`:

- `g_guest_sigsys_blocked` → per-thread shadow in a TID-keyed
  open-address table (256 slots, linear probe). Slot updates use
  `__atomic_compare_exchange` to claim and `__atomic_store/load` for
  the bit. Per-thread isolation is enforced by lookup; concurrent
  setters on the same tid serialize via the bit's atomic store;
  concurrent setters on different tids that hash to the same slot
  resolve via the CAS-claim spin.

- `g_guest_sigsys_action` → process-wide seqlock. Even seq = stable,
  odd = writer in progress. Writers `CAS(seq, even→even+1)` to claim,
  publish bytes, then store `seq+=1`. Readers retry on inconsistent
  reads. Multi-writer-safe via the CAS-claim spin.

Helpers are pure (no syscalls), async-signal-safe (only
`__atomic_*` builtins + plain byte copy), and exercised under
hosted glibc by `tests/unit/test_signal_shadow.c` — single-thread
basics plus a 16-thread blocked-isolation stress and a
4×4-writer/reader seqlock stress (50K iterations each).

## Slot leak — narrowed

The blocked-shadow table is `static struct slot g_slots[256]`. The
original design had every thread that ever called `rt_sigprocmask`
consuming a slot, regardless of whether it was actually blocking
SIGSYS, with overflow silently dropped — a long-lived guest with
thread-pool churn would fill the table and start reading wrong-mask
state for new threads.

Two fixes landed:

1. **`set(_, 0)` no longer claims a slot.** An absent slot already
   reads as 0 (the kernel default), so recording an explicit
   blocked=0 for a never-seen tid is wasteful. With this change,
   slot pressure is bounded by the count of threads with SIGSYS
   *currently* blocked — for realistic workloads (pacman, glibc,
   Firefox), well under 10. Slots still leak when a thread blocks
   SIGSYS and exits without unblocking, but that's a much smaller
   set than "every signal-aware thread that ever ran".

2. **Overflow `__builtin_trap()`s.** If N_SLOTS distinct tids
   genuinely have SIGSYS blocked at once, set() lands as SIGILL on
   the trapping thread, pointing at the source line. Silent
   wrong-state-forever is replaced by a visible crash that says
   "bump N_SLOTS or move to growable storage". Regression test:
   `blocked_set_zero_on_unknown_tid_does_not_claim_slot`.

Remaining: no thread-exit hook, so a thread that blocks SIGSYS and
exits without unblocking still leaks its slot. Same fix as the
TID-reuse race below.

## Residual: TID reuse

If thread A blocks SIGSYS (slot for tid=A_tid set to blocked=1), A
exits without unblocking, and the kernel later reuses A's tid for
thread B, B will read the stale 1 until it issues an
`rt_sigprocmask` with `SIG_SETMASK` (always rewrites the bit) or
`SIG_BLOCK`/`SIG_UNBLOCK` with SIGSYS in the set (only those touch
the bit — blocking an unrelated signal leaves the SIGSYS shadow
untouched). POSIX-wise B should inherit its creator's mask, not
A's stale state.

The TID-reuse race and the residual leak (block-then-exit-without-
unblock) share a root cause: no signal that a thread exited.
Mitigations:

- A clone hook to clear the new tid's slot when a thread is
  created. We don't trap clone(2) in handler dispatch (only
  clone3, which we currently fake-ENOSYS); even if we did, the
  parent-side trap doesn't have the child tid yet.
- A `tgkill(0, tid, 0)` probe on every read to detect a dead-tid
  slot. Async-signal-safe, but a syscall per probe per
  rt_sigprocmask is too expensive.
- **An exit-side hook clearing the dying thread's slot.** Trap
  `exit`/`exit_group` (and ideally `tkill`/`tgkill` of self) and
  call a `tawc_sigshadow_blocked_clear(tid)`. Fixes both the
  reuse race and the residual leak. Cost: one extra trap per
  thread teardown, acceptable. This is the cleanest fix.

In practice:
- Realistic guests set-then-read; the bug only fires when a thread
  reads its mask without first writing it.
- The kernel doesn't reuse tids while any thread holds them;
  pid_max is typically ≥32K on Android.
- Behavior under the race: B reads back a wrong "old" mask once;
  no crash, no data corruption.

The current "documented residual" stance reflects a cost-vs-impact
trade — none of the mitigations are free, and none of the
realistic guest workloads we ship to (pacman, glibc init, Firefox
content procs) hit the read-without-write pattern that exposes the
bug.

## Test gap

Multi-thread end-to-end coverage of the rt_sigprocmask/rt_sigaction
handlers (running through the actual SIGSYS handler in a guest) is
still TODO — the existing testhost smoke is single-threaded. The
unit tests cover the lock-free primitives directly.

## Severity

Low. Reduced from "first thing to break under any multi-threaded
guest" to "wrong mask read once after tid reuse, or noisy crash if
256 distinct threads concurrently block SIGSYS". The latter has not
been observed in any workload we ship to. Keep the issue open until
the exit-side hook lands or the multi-thread phase-1 testhost lets
us verify.
