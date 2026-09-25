# Rootfs session invariant

**Every entry into a rootfs must run in its own session/process group.**
This is the contract — true for all install methods (chroot, proot,
tawcroot). Each launch is logically a fresh job — like a fresh login
or a `cron` invocation — and must not inherit the launcher's job-control
context.

Consequences if violated:
- Daemons spawned inside (notably gpg-agent under `pacman-key`) inherit
  whatever signal mask / pgrp the launcher had, and misbehave: gpg-agent
  spins at 100% CPU in its main loop instead of cleanly daemonising.
- Controlling-tty inheritance (rare on Android, but the same model).

## Single Kotlin entry point

There is exactly one place that knows how to enter a rootfs:
[InstallationMethod.startInside]. All callers route here:

| Caller | Path |
|---|---|
| In-app installer (Installer pipeline) | `method.runInside` → `MethodRunHelper.runInside` → `method.startInside` |
| In-app launcher (LauncherActivity) | `UserRootfsSession.runInside` → `method.startInside` |
| In-app command runner (home ⋮ Run command, `RunCommandOp`) | `UserRootfsSession.startInside` → `method.startInside` |
| Host scripts (`rootfs-run.sh`, `run-integration-tests.sh`) | broker `RUNINSIDE` request → `UserRootfsSession.startInside` → `method.startInside` |
| Integration tests (`rootfs_run`, `rootfs_spawn`) | same broker path |

`startInside` upholds the session invariant — `setsid` is built into
the spawn for tawcroot/proot; chroot's `su` provides one implicitly.
There is no other path that enters a rootfs.

There is no on-disk `enter.sh` for any method. All bind-table /
mount / env injection / chroot logic lives in Kotlin
(`TawcrootMethod`, `ProotMethod`, `ChrootMethod` +
`ChrootMounter.mountScript`, plus `RootfsEnv` for the in-rootfs env
applied via `/usr/bin/env -i`).

## Wire protocol

The broker accepts a `RUNINSIDE <install-id>` header (optionally
followed by `CMD <command>`); see notes/exec-broker.md. The host
helper exposes this as `tawc-exec --in-rootfs <id> [-- CMD]`.

## Test cleanup

Integration test cleanup is app-side. The broker `test-init` action
uses `ProcessScanner.killAllInRootfs` for the selected install instead
of host pidfiles, PGID reads, `ps`, or `kill -- -PGID`.
