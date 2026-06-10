# tawcroot: more x86_64-only legacy syscalls may fall through to -ENOSYS

While adding the in-app terminal, x86_64 glibc's `getpgrp()` turned out
to issue the legacy `getpgrp` syscall (NR 111), which Android's
untrusted_app seccomp filter RET_TRAPs (bionic only ever calls
`getpgid`). tawcroot's dispatch had no slot for it, so the guest got
`-ENOSYS` and bash disabled job control. Fixed by routing it to
`getpgid(0)` in `syscalls_control.c`.

That fix covers one instance of a broader class: x86_64-only legacy
syscalls that glibc may emit but bionic never uses, hence are likely
absent from Android's app allowlist, hence TRAP into tawcroot and fall
through to `-ENOSYS`. Speculative same-class candidates (flagged in
review, not observed):

- `pause` (34) — an `-ENOSYS` pause() returns immediately instead of
  blocking; could show up as busy-looping or broken signal waits.
- `alarm` (37) — `-ENOSYS` would silently drop SIGALRM timers.

Emulator-only (aarch64 never allocated these numbers; glibc uses the
modern equivalents there). Nothing observed misbehaving yet. If odd
timing/signal bugs appear on the x86_64 emulator, check `[t] nr=...`
trace output (TAWCROOT_TRACE) for unhandled legacy numbers first; the
fix pattern is a small forwarding handler like `handle_getpgrp` plus a
hosted test.
