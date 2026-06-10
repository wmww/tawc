# Rootfs /tmp sweep

`/tmp` in every install method is a plain directory on flash
(`<rootfs>/tmp`), not tmpfs: tawcroot's `prepareSpawn` just mkdirs it,
chroot mounts proc/dev/etc. but no tmpfs, proot path-translates into
it. Nothing ever clears it — the rootfs has no init, so there is no
`systemd-tmpfiles` or boot-time cleanup, and neither the app nor any
script touches it. Contents persist across sessions and Android
reboots until the install is wiped.

It also gets more traffic than a normal `/tmp`: `RootfsEnv` sets both
`TMPDIR=/tmp` and `XDG_RUNTIME_DIR=/tmp`, so runtime sockets,
gpg-agent state, etc. all accumulate there and count against app
storage.

A real tmpfs is not reachable rootlessly: `mount()` needs
CAP_SYS_ADMIN, Android SELinux blocks user-namespace creation for
untrusted apps, and there is no app-writable tmpfs path on the device
to bind from. Emulating one in tawcroot's SIGSYS handler (memfd-backed
VFS, like the existing `/dev/shm` shim but with directories, readdir,
rename…) conflicts with the no-malloc handler constraints and isn't
worth it. So: sweep the directory instead.

## Design

Age-based sweep at app-process start.

**Where: `TawcApplication.onCreate`'s existing `tawc-startup` daemon
thread**, alongside `BootstrapCache.sweepStale()` and
`TawcInstaller.installAll` — iterate every install in
`InstallationStore` and sweep `<rootfs>/tmp`.

Not in `prepareSpawn`: it runs on every spawn (broker, in-app
terminal, install steps, compositor clients) while other sessions are
live, and with `XDG_RUNTIME_DIR=/tmp` live sessions keep sockets
there. A "no session active" signal doesn't exist and would be racy.

**What: delete entries older than a threshold (~3 days, constant),
not a full clear.** A full clear at app start looks safe (guests are
children of the app process) but `setsid`'d guests from a previous
app process can outlive it — Android's phantom-process killer reaps
them neither promptly nor reliably — and yanking `/tmp` from under a
live guest is the one failure mode to avoid. Aging sidesteps it and
matches the `systemd-tmpfiles` semantics programs expect of `/tmp`.

## Mechanics

- Plain Kotlin depth-first walk of `<rootfs>/tmp`; the rootfs is
  app-uid-owned for tawcroot/proot, so no shell/`find` dependency.
- `lstat`-based mtimes (never follow symlinks). Delete old files
  first, then remove directories that are old *and* now empty. Never
  delete `/tmp` itself.
- Skip `/tmp/.X11-unix` by name: tawcroot treats it as a bind key,
  chroot has a real bind mount there, and the backing `share/xtmp`
  dir is shared across spawn surfaces. Not worth mount detection for
  one well-known path.
- Best-effort: failures get one summary log line, never abort
  startup. Known case: chroot guests run as real root and can leave
  root-owned files the app uid can't delete — acceptable for a
  debug-only method (a tmpfs mount in `ChrootMounter` would fix that
  properly if it ever matters).

Result: bounded `/tmp` lifetime with no session-tracking machinery,
identical behavior across spawn surfaces, one small sweeper class
plus a few lines in `TawcApplication`.
