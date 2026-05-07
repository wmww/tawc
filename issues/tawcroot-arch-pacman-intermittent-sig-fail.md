# tawcroot: pacman intermittently fails signature checks during Arch install

`pacman -Syyu` for the 158-package Arch install set under tawcroot fails
~3-10 packages with "missing required signature" non-deterministically.
Pacman's `--noconfirm` then deletes the "corrupted" packages and aborts
the transaction. The same install via the `proot` method works
reliably.

## Repro

```bash
adb shell am start -n me.phie.tawc/.install.InstallActivity \
    --es autoStart true --es id arch --es method tawcroot \
    --es mirrorProxy 'http://127.0.0.1:8080/proxy/'
adb logcat -s tawc-install
```

Failing package set varies across runs (`hwdata`, `fribidi`, `graphite`,
`iso-codes`, `libcloudproviders`, `gtk3-demos` one run; `gcc`, `lua54`,
`libinput`, `lzo`, `lm_sensors` the next; `pixman`, `libedit`,
`spirv-tools`, `fribidi`, `colord`, `adwaita-icon-theme`,
`libcloudproviders`, `libstemmer` a third). Failure happens at the
`checking package integrity...` stage; `pacman -Sw --debug` on the
named-failed packages individually verifies their signatures cleanly,
so the data on disk is fine.

## Background — what's already fixed

`bind_to_parent` in `tawcroot/src/main.c` used to do:

```c
prctl(PR_SET_PDEATHSIG, SIGKILL, ...);
if (getppid() == 1) exit_group(0);
```

The `getppid() == 1` exit was meant to handle the fork→prctl race
(parent dies before PDEATHSIG is armed). But it ALSO killed every
process whose ppid was `1` for legitimate reasons — and gpgme /
libgpg-error's `posix_spawn` deliberately reparents its workers to
init via fork-then-immediately-exit-intermediate. So every gpgme
spawn (gpgconf for engine probe, gpg for signature verification) hit
the orphan check and exited before producing output. Pacman's libalpm
then reported `GPGME error: Invalid crypto engine` followed by
`missing required signature` on EVERY package.

Fix in place: `bind_to_parent` is split into `arm_pdeathsig` (just
the prctl, called on every entry) and `exit_if_orphan` (the ppid==1
check, called only on top-level invocations — `tawcroot_main` skips
it when `argv[1] == "--exec-child"`). The re-exec path is the same
process as before the execve, so there's no fork→prctl race window
to guard against, and the gpgme-orphan case is in that re-exec path.
Top-level invocations keep the orphan-detect they need. Most
packages verify cleanly with this fix in. Some still don't — the
remainder is the bug below.

## Remaining failure

After the `bind_to_parent` fix:

1. Most signature verifications succeed.
2. A non-deterministic subset (~3-10 / 158) intermittently fails with
   `missing required signature`.
3. `gpg --verify` on the same `.sig`/`.pkg.tar.zst` pair on disk works.
4. `pacman -Sw --debug` on individual failed packages succeeds.
5. `ParallelDownloads = 1` in pacman.conf does NOT change the failure
   rate.

Failed deeper-investigation attempts:

- Suspected glibc `posix_spawn` using `execveat` with `AT_EMPTY_PATH`
  (which `tawcroot/src/syscalls_exec.c::handle_execveat` rejects with
  `-ENOSYS`). Added per-`AT_EMPTY_PATH` handling that readlinks
  `/proc/self/fd/<dirfd>` and routes through the normal `do_exec`
  pipeline. Did NOT fix the failure — instrumentation showed gpgme is
  using plain `execve(/usr/sbin/gpgconf)`, not execveat at all. Patch
  reverted because it doesn't help and complicates the surface. Could
  be reapplied if a future workload genuinely needs it.
- Suspected the rare hung-tawcroot we sometimes see (`tawcroot
  --exec-child <fd>` at 100% CPU, single-threaded, parent of a pacman
  process) is the same root cause: a process that re-execs from inside
  the SIGSYS handler somehow ends up looping in the loader path or
  `tawcroot_path_probe_openat2` rather than reaching the guest binary.
  Not bisected.

The pattern "single-package retries succeed, batched run fails some"
points at accumulating per-process state in the long-lived pacman
process — fd table, gpg-agent connection, libcurl multi-handle
buffers — interacting with tawcroot's exec interception in a way the
shorter retries don't reach. Wrapping the pacman call in a 4-attempt
retry loop didn't help either: tawcroot can hang on individual
re-execs and the retry loop just hangs waiting on the worker.

## Workaround

Use `proot` for Arch installs while this is open:

```
adb shell am start -n me.phie.tawc/.install.InstallActivity \
    --es autoStart true --es id arch --es method proot \
    --es mirrorProxy 'http://127.0.0.1:8080/proxy/'
```

The `tawcroot` method still works for the OTHER distros (Manjaro,
Void) where the bootstrap doesn't go through pacman's gpgme dance at
install time, and for the post-install runtime path (running apps
inside the chroot) — only the Arch bootstrap's `pacman -Syyu` is
affected.

## Where to dig next

Add `strace -ff -e trace=execve,execveat,exit_group,clone,clone3,wait4`
on the in-app pacman invocation (via the dev exec broker) and capture
syscall traces for both a successful and a failing run. Diff those
traces and look for the divergence around the failed `.sig` load.
Likely needles:
- A handler-side syscall returning a wrong errno that gpg silently
  treats as "no sig data".
- A path-translation glitch on a specific guest-relative path
  (`/etc/pacman.d/gnupg/...`) that intermittently routes wrong.
- The hung-tawcroot pattern firing for one of the gpg children, which
  pacman then waits on with a watchdog timeout that dumps the
  signature as missing.

Don't bother retrying with `ParallelDownloads = 1` again — verified
it doesn't change behaviour.
