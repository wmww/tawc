# tawcroot: io_uring is denied at setup, not actually intercepted

`tawcroot/src/syscalls_control.c:134-140`:

```c
static long handle_io_uring_setup(const tawcroot_syscall_args *args,
                                  ucontext_t *uc)
{
    (void)args;
    (void)uc;
    return TAWC_ENOSYS;
}
```

`io_uring_register` (NR 427) and `io_uring_enter` (NR 426) are **not
trapped** — `grep -rn io_uring_register tawcroot/src tawcroot/include`
returns nothing. Only `io_uring_setup` (NR 425) has a handler.

## Why this is currently safe (and the gap that makes it fragile)

Without `io_uring_setup`, the guest can't create a ring → no ring fd
→ no `io_uring_register`/`io_uring_enter` calls. Programs that probe
io_uring fall back to syscall-based I/O cleanly (notes/tawcroot.md
"Open questions" #1).

The fragility: tawcroot's own filter doesn't reject the post-setup
syscalls. We rely on the *guest never having a ring fd*. That holds
as long as:

1. Every io_uring_setup goes through tawcroot's handler (true — it's
   trapped).
2. The guest doesn't inherit a ring fd from a non-tawcroot parent
   across exec.
3. The Android stacked filter happens to block `io_uring_register` /
   `io_uring_enter` from `untrusted_app` (the OnePlus 9 / Android 14
   filter does, but we don't depend on it explicitly).

## Why the deny isn't enough long-term

The notes plan calls full SQE rewriting a *correctness* upgrade, not
just perf. Once any shipped workload exercises io_uring (modern
databases inside the chroot, container runtimes, fio benchmarks,
maybe a future libhybris that wraps Vulkan I/O on top of io_uring on
newer Android), the deny path forces a syscall-based fallback that
may be unacceptably slow or — worse — silently fail in unexpected
ways if the program treats io_uring_setup -ENOSYS as fatal rather
than fall-back-able.

## Fix sketch (deferred)

`notes/tawcroot.md` "Open questions" #1 already has the design:
trap `io_uring_setup`/`io_uring_register`/`io_uring_enter` plus the
ring `mmap`; on enter walk SQEs from cached head to tail; for each
path-bearing op (`OPENAT`, `OPENAT2`, `STATX`, `RENAMEAT`, `UNLINKAT`,
`LINKAT`, `SYMLINKAT`, `MKDIRAT`, …) rewrite the SQE's path pointer
to a translated buffer we own; stash the buffer keyed by `user_data`
and free on CQE drain. Reject `IORING_SETUP_SQPOLL` (needs
`CAP_SYS_NICE` which `untrusted_app` doesn't have anyway) so we keep
the enter-time chokepoint.

Estimated ~500-1000 lines as `src/uring.c`. New `IORING_OP_*` show up
each kernel release, so an explicit allowlist + warn-pass on unknown
opcodes is the right discipline.

## Severity

Low for now. **No workload tawcroot ships today exercises io_uring**
(bash, pacman, gpg, Firefox, gtk apps all use the syscall API). It
becomes Medium when:

- A target needs io_uring throughput.
- A guest treats `io_uring_setup -ENOSYS` as fatal.
- Stacked filter analysis on a new Android version finds `untrusted_app`
  no longer blocks io_uring_register/enter — at which point the
  "ring fd inheritance" edge case becomes reachable.

## Defense-in-depth fix (small, can land before full interception)

Trap `io_uring_register` and `io_uring_enter` with handlers that
return -ENOSYS too. Closes the inheritance edge case at minimal cost
and makes "no io_uring traffic ever escapes" enforceable independently
of Android's stacked filter. ~10 lines.
