# Rootfs client segfaults report exit code 0 and lose buffered output

A rootfs process that dies from SIGSEGV comes back through
`rootfs-run.sh`/the exec broker as exit code 0, with all buffered stdout
lost. `sh -c "crashing-app; echo rc=$?"` inside the rootfs prints
`rc=0`, so the masking happens below the rootfs shell — the shell itself
sees a clean exit-0 wait status for the crashed child.

Repro (2026-07-06, OnePlus 9): run `glxinfo` against a gl4es
`libGL.so.1` built with `-DHYBRIS=ON` (crashes in libhybris
`eglCreateWindowSurface` dereferencing a raw XID — confirmed real
SIGSEGV via gl4es's `LIBGL_STACKTRACE=1` handler). Result: rc=0, zero
bytes of output (glibc stdout buffer discarded). No tombstone, no
logcat signal either.

Suspects: libhybris's TLS-thunk SIGSEGV handling or tawcroot's signal /
exit-status plumbing swallowing the fault and exiting the child cleanly.
`weston-simple-egl`-class crashes would be equally masked.

Impact: silent-death debugging is brutal — this spike burned an hour
proving a "successful" run never ran. Worth root-causing before the next
GL bring-up. Workarounds that made it tractable: `stdbuf -o0 -e0` (see
the truth before death) and `LIBGL_STACKTRACE=1` (gl4es-specific).
