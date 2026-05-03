# tawcroot: gpg-agent spins at 100% CPU when started from the app process

When `pacman-key --init` runs inside the tawcroot chroot via the
in-app installer (`InstallationService` → `ProcessBuilder` →
`/system/bin/sh` → tawcroot), the gpg-agent daemon that
`pacman-key`'s gpg subcommands spawn lands in a tight user-space loop
at 100% CPU. The `gpg` parent times out trying to reach the agent
("can't connect to the gpg-agent: IPC connect call failed"), which
fails `pacman-key --init` and aborts the install.

The same `tawcroot` binary, the same chroot, the same `pacman-key
--init` command, run via `bash client/tawc-chroot-run` (i.e. from an
adb shell, not the app process), works first try. So this is
something inherited from the app-process startup state, not a generic
gpg-agent-under-tawcroot bug.

## Repro

1. Install the app and trigger the tawcroot install:
   ```
   adb shell am start -n me.phie.tawc/.install.InstallActivity \
       --es autoStart true --es id arch --es method tawcroot
   ```
2. Watch `adb logcat -s tawc-install`. The PKG_KEYRING stage gets to
   "gpg: starting migration from earlier GnuPG versions" then stalls.
3. `adb shell su -c 'ps -ef | grep tawcroot'` shows an orphan
   `tawcroot --exec-child 3` process at 99–100% CPU with `PPid=1`.
   `/proc/<pid>/comm` is `4`, `/proc/<pid>/maps` lists the
   in-rootfs `gpg-agent` binary plus its libgcrypt/libassuan/libnpth
   deps.
4. Killing the orphan with `kill -KILL` lets the in-flight `gpg`
   command return with "agent_genkey failed: No agent running",
   `pacman-key` retries and spawns a new `gpg-agent` that hangs the
   same way.

`bash client/tawc-chroot-run 'pacman-key --init'` from an adb shell
session against the same chroot completes in a few seconds,
generating the master key end-to-end without any orphan process.

## Suspected cause

Some piece of the inherited init state from the app's JVM process
makes gpg-agent's main loop misbehave once it daemonises. Candidates:

* **Signal mask** — JVM threads commonly block SIGCHLD / SIGPIPE /
  SIGUSR1. tawcroot's SIGSYS-handler-side dance restores the SIGSYS
  bit (loader_exec.c), but doesn't reset the rest. gpg-agent may
  depend on one of those firing to wake from `select()`.
* **PR_SET_DUMPABLE / PR_GET_KEEPCAPS** state inherited by
  zygote-spawned children.
* **Inherited fds beyond 0/1/2** — Java's Process API filters,
  but not all build configurations actually scrub. A bad fd in
  gpg-agent's pollset would spin if `POLLNVAL` returns immediately.
* **A trapped syscall returning the wrong errno only when called from
  this specific seccomp / cgroup context.** The BPF filter is the
  same in both cases, so this would have to be SIGSYS handler logic
  that varies based on something (e.g. ucontext register layout
  inherited from the JVM stack).

## Workaround

`ArchPacmanCommon.configure` pins `SigLevel = Never` /
`LocalFileSigLevel = Never` and
`ArchPacmanCommon.initPackageManager` skips both
`pacman-key --init` and `pacman-key --populate` when running under
tawcroot. The cross-mirror MD5 verify on the bootstrap and TLS on
mirror traffic remain the integrity story for installed packages.
This is a known-bad tradeoff (the in-chroot pacman won't catch a
package the mirror corrupts after the bootstrap was checked); fixing
this issue is the prerequisite to restoring SigLevel.

## Where to start debugging

* Capture `cat /proc/<pid>/syscall` and `/proc/<pid>/wchan` from the
  orphan over time. Both showed `running` and `0` in initial
  poking — the process is genuinely in user-space, not stuck in a
  syscall.
* Diff the inherited state between the working adb-shell case and
  the failing app-process case. Focus on signal mask
  (`Sig{Pnd,Blk,Ign,Cgt}` in `/proc/<pid>/status`) and capability sets.
* Try wrapping the in-chroot bash with a tiny native helper that
  unblocks all signals and resets fds before exec'ing pacman-key.
  If that fixes it, the inherited-state hypothesis is confirmed and
  the helper can move into `TawcrootMethod.runInside`.
