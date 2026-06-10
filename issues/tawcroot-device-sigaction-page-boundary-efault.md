# tawcroot device tests: B2 sigaction test under-sizes the aarch64 struct

`tawcroot/test.sh --device` (physical phone, 2026-06-09) has 9 failures
that do not reproduce on host (`--host` is fully green). The visible
pattern, repeated across the three smoke suites:

```
[FAIL] rt_sigaction(SIGSYS, &act_at_page_boundary) -> 0 (B2 sizing)
    rv = -14
[FAIL] rt_sigaction(SIGSYS, NULL, &oldact_at_boundary) -> 0 (B2 sizing)
    rv = -14
ROOTFS SYSCALL SMOKE: FAIL (2 failure(s))
```

Root cause: the test is wrong, not the handler.
`test_rt_sigaction_b2_sizing` (tests/testhost/src/rootfs_smoke.c)
places the guest sigaction `sa_size` bytes before a PROT_NONE page and
uses `sa_size = 24` on aarch64, assuming arm64 lacks `sa_restorer`.
But arm64 defines `SA_RESTORER` (uapi `asm/signal.h`, "required for
AArch32 compatibility"), so the kernel `struct sigaction` is
handler+flags+restorer+mask = 32 bytes on both arches — matching the
handler's `TAWC_KERN_SIGACTION_SIZE`. The handler's correct 32-byte
`process_vm_readv`/`writev` spills 8 bytes into the unmapped page and
correctly reports EFAULT; a raw kernel `rt_sigaction` with this
placement would EFAULT the same way. Host passes only because x86_64
uses `sa_size = 32`, which matches.

Fix: use 32 on both arches (drop the `#if`) in
`test_rt_sigaction_b2_sizing`, and fix its comment claiming 24 bytes
on aarch64.

Noticed while bumping the cleat pin (test-framework-only change, same
failures unrelated to it).
