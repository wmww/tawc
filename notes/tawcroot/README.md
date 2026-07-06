# tawcroot — fast rootless chroot via systrap

`tawcroot/` is a from-scratch C implementation of a proot-style fake
chroot, using **seccomp `RET_TRAP` + in-process `SIGSYS` handling**
(the "systrap" technique gVisor uses for its platform layer) instead
of `ptrace`. The goal is a strict superset of proot's TAWC use case —
same compat envelope at meaningfully lower syscall overhead, fewer
compat hacks, and a codebase shaped for our needs rather than
inherited from proot's ptrace heritage.

**Status: default and only officially supported install method.**
Release builds ship only tawcroot; chroot/proot are dev-only and only
appear in debug builds. See `notes/installation.md` "Install methods"
for the build-time gating.

This doc is the design + implementation plan. The code is being built
fresh. Refer to `deps/proot/` (the Termux fork we currently vendor) as
reference when something proot solves is non-obvious — particularly
the Android-specific quirks in its `src/tracee/seccomp.c` and the
loader/exec dance — but **don't port proot's architecture**. proot
inherited a tracer/tracee design from its ptrace heritage that doesn't
fit a single-process systrap model, and its code quality is uneven in
ways we don't want to inherit.

## What it is, in one paragraph

A small C program that, given a rootfs path and a command, sets up a
seccomp-bpf filter that traps path-bearing and a few other syscalls,
installs a `SIGSYS` handler that emulates those syscalls against the
rootfs, and launches the guest through a re-exec + in-process ELF
loader so the handler remains installed. Path translation, fake-root
uid/stat behavior, and Android-specific syscall fixups all happen
in-process, in the same thread that issued the syscall. No tracer
process, no `ptrace`, no per-syscall context switch.


## Contents

This design + implementation note is split across:

- [overview.md](overview.md) — language choice, why not proot, expansion goals, and what tawcroot explicitly is not.
- [architecture.md](architecture.md) — architecture overview and process layout.
- [sigsys-handler.md](sigsys-handler.md) — the `SIGSYS` handler: guest memory access, saved registers, issuing host syscalls, non-PIE, threading/`vfork`, signal/seccomp control, async-signal-safety, coding conventions.
- [path-translation.md](path-translation.md) — translation rules, which syscalls need trapping, `execve`, `/proc/self/exe`, `/proc/<pid>/fd/<n>` and `getcwd` reverse translation, chroot emulation.
- [seccomp-filter.md](seccomp-filter.md) — the seccomp filter and non-root filter installation.
- [bootstrap-and-modules.md](bootstrap-and-modules.md) — bootstrap & entry, module layout.
- [build-and-install.md](build-and-install.md) — build integration and installer integration.
- [testing.md](testing.md) — testing strategy and layers.
- [phasing.md](phasing.md) — phased implementation plan and history.
- [status.md](status.md) — known gaps before MVP, accepted syscall-fidelity divergences, future work, confirmed environment, maintenance contract.

Cross-references elsewhere in the tree cite these files by section name (e.g. `notes/tawcroot/path-translation.md §"chroot emulation"`); the heading lives in the file whose topic matches.
