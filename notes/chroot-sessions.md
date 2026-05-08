# Chroot session invariant

**Every chroot invocation must run in its own session/process group.**
This is the contract. Each launch into the chroot is logically a fresh
job — like a fresh login or a `cron` invocation — and must not inherit
the launcher's job-control context.

Consequences if violated:
- Daemons spawned inside (notably gpg-agent under `pacman-key`) inherit
  whatever signal mask / pgrp the launcher had, and misbehave: gpg-agent
  spins at 100% CPU in its main loop instead of cleanly daemonising.
- The integration test framework's `kill -- -<pgid>` cleanup targets
  whatever PGID was inherited, which on Android is the system zygote's
  (PID 909) — so a test trying to kill its app would try to signal every
  other Android app on the device.
- Controlling-tty inheritance (rare on Android, but the same model).

## Single Kotlin entry point

There is exactly one place that knows how to enter the chroot:
[InstallationMethod.startInside]. All callers route here:

| Caller | Path |
|---|---|
| In-app installer (Installer pipeline) | `method.runInside` → `MethodRunHelper.runInside` → `method.startInside` |
| In-app launcher (LauncherActivity) | same |
| Host scripts (`tawc-chroot-run.sh`, `install-test-deps.sh`) | broker `RUNINSIDE` request → `method.startInside` |
| Integration tests (`chroot_run`, `chroot_spawn`) | same broker path |

`startInside` upholds the session invariant — `setsid` is built into
the spawn for tawcroot/proot; chroot's `su` provides one implicitly.
There is no other path that enters a chroot.

There is no on-disk `enter.sh` for any method. All bind-table /
profile.d / mount / chroot logic lives in Kotlin (`TawcrootMethod`,
`ProotMethod`, `ChrootMethod` + `ChrootMounter.mountScript`).

## Wire protocol

The broker accepts a `RUNINSIDE <install-id>` header (optionally
followed by `CMD <command>`); see notes/exec-broker.md. The host
helper exposes this as `tawc-exec --in-chroot <id> [-- CMD]`.

## Detecting violations

`tests/integration/src/chroot_process.rs::ensure_pgid` asserts that
the discovered PGID is not the broker's. A failure surfaces as a clear
panic message rather than a 10-second `kill -- -<pgid>` timeout that
looks like flake.
