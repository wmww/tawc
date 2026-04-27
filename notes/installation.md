# In-app installation system

The tawc Android app includes a Kotlin re-implementation of the chroot
install/run/destroy logic that previously lived only in the
`client/arch-chroot-*` shell scripts. This lets the app:

- ship a "Manage installations" screen as a first-class feature
- store the chroot inside its private data dir, so uninstalling the app
  reclaims everything
- offer the same operations to adb / integration tests via broadcasts,
  so non-UI workflows can be ported off the legacy scripts incrementally

The legacy `client/arch-chroot-*` scripts still exist and still target
`/data/local/arch-chroot/` — they're independent of this system.

## On-disk layout

Everything lives under the app's private data dir:

    /data/data/me.phie.tawc/
      cache/install/                 # downloaded bootstrap tarballs (kept across runs)
      installations/
        arch/
          metadata.json              # JSON: id, distro, arch, method, sourceUrl, …
          rootfs/                    # the chroot itself (what `arch-chroot` would chroot into)

`arch` is the default installation id. Multi-installation support is a
matter of varying the id; the on-disk layout, the [Installation] data
class, and [InstallationStore] already handle it.

## Code layout (`me.phie.tawc.install`)

| File | Role |
| --- | --- |
| `Installation.kt`              | Immutable metadata for one installed environment (id/distro/arch/method). |
| `InstallationStore.kt`         | Filesystem layout + JSON metadata I/O. Lists / loads / saves installations. |
| `Su.kt`                        | Wrapper around Magisk `su`. Pipes the script via stdin (no shell-quoting headaches), streams combined stdout/stderr line-by-line via a callback. |
| `Downloader.kt`                | HTTP downloader for bootstrap tarballs. Caches by content length. |
| `Archive.kt`                   | Tar extraction (decompresses zstd in-process to a sibling `.tar`, then hands it to `toybox tar` running as root). |
| `ChrootMounter.kt`             | Builds the bind-mount shell snippet and provides defensive-cleanup `unmount`. Per `arch-chroot-run`, the mounts live inside a single `su` invocation's private namespace, not globally. |
| `ChrootRunner.kt`              | Concatenates `ChrootMounter.mountScript(rootfs)` with the chroot exec into one `su -c` shell so the mounts and the command share that shell's mount namespace. Base64-encodes the command into the wrapper script to dodge quoting hell. |
| `ArchInstaller.kt`             | Stage machine: download → extract → configure → pacman init → pacman install. Mirrors `arch-chroot-create*`. (No separate mount stage — see *Mount lifecycle*.) |
| `InstallProgress.kt`           | Stage enum + progress event used by the service. |
| `InstallationService.kt`       | Foreground service that runs install/uninstall in a coroutine and exposes `progress` (StateFlow) and `log` (SharedFlow). |
| `InstallationCommandReceiver.kt`| BroadcastReceiver that maps adb broadcasts onto the install package. |
| `ManageInstallationsActivity.kt`| Plain-Android UI: status, progress bar, log tail, install/uninstall/refresh buttons. Bound to the service for live updates. |

The `MainActivity` home screen has a new "Manage installations" button
that opens `ManageInstallationsActivity`.

## Stages of an Arch install

`ArchInstaller.install()` walks through these stages, reporting
`InstallProgress` to the UI and a per-line log to logcat (`tawc-install`):

1. **DOWNLOADING** — bootstrap tarball into `cache/install/`.
   - x86_64: `https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst` (toplevel `root.x86_64/` is flattened post-extract because toybox `tar` doesn't `--strip-components`)
   - aarch64: `http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz` (no wrapper dir; HTTP — see *Network security* below)
2. **EXTRACTING** — root-owned via toybox tar. Zstd is decompressed in-process to a sibling `.tar` first because piping data into a shell's stdin loses bytes (the shell pre-buffers past the script).
3. **CONFIGURING** — writes the same files the legacy create scripts do:
   - `/etc/resolv.conf` (8.8.8.8)
   - `/etc/pacman.conf` (`SigLevel=Never`, `DisableSandbox`, `#CheckSpace`, `IgnorePkg = linux …`)
   - `/etc/pacman.d/mirrorlist` (x86_64 only — ALARM ships its own)
   - `/etc/profile.d/00-path.sh` (PATH/TMPDIR/HOME)
   - `/etc/profile.d/01-tawc.sh` is *not* written here — `ChrootMounter`
     rewrites it on every chroot entry (Wayland env: `WAYLAND_DISPLAY`,
     `XDG_RUNTIME_DIR`, `LD_LIBRARY_PATH`, `HYBRIS_EGLPLATFORM`, `wayland-0`
     symlink) so env changes pick up without a reinstall, matching legacy
     `arch-chroot-run`.
4. **PACMAN_INIT** — `pacman-key --init`, `--populate archlinux` /
   `archlinuxarm` depending on arch. (Bind mounts are not a separate
   stage — they're set up inside the very `su` shell that runs each
   chroot command, see *Mount lifecycle* below.)
5. **PACMAN_INSTALL** — `pacman -Syu --noconfirm` then
   `pacman -S --noconfirm --needed base-devel git libtool wayland
   wayland-protocols pkg-config autoconf automake patchelf weston gtk3
   gtk3-demos`.

Re-running the install is safe: a known-good tarball is reused, and if
`/usr/bin/make` is already in the rootfs the keyring/install steps are
skipped.

## Mount lifecycle

Magisk runs each `su` invocation in a private mount namespace
(`unshare(CLONE_NEWNS)`), so any bind mounts performed by one `su` call
are torn down when that call exits. The legacy `arch-chroot-run`
accommodates this by combining mount setup and the chroot exec into a
single `su -c "..."` invocation; we follow exactly the same pattern.

`ChrootMounter.mountScript(rootfs)` returns a shell snippet that
performs the bind mounts; `ChrootRunner.run` concatenates it with the
chroot exec and hands the whole thing to one `Su.run`. There is no
separate `MOUNT` operation because there can't be — the mounts only
exist for the lifetime of that one shell. This avoids polluting the
global mount table with stale entries (and avoids the zygote-fork crash
that follows when `/data/data/<pkg>/...` has live bind mounts during
package fork).

`ChrootMounter.unmount` exists as a defensive cleanup — only relevant if
mounts somehow leaked into the global namespace (e.g. via a stray
`su --mount-master`). It `realpath`s the rootfs before scanning
`/proc/mounts`, because Kotlin's `File.absolutePath` returns the
`/data/user/0/...` symlink form while `/proc/mounts` reports the
canonical `/data/data/...` form — naive substring matching misses
every entry. The match is also a strict prefix check (`==` or starts
with `r"/"`) so paths containing `.` don't over-match other mounts.

Uninstall belt-and-braces: even after `unmount` reports OK, the
final delete uses `find <root> -xdev -depth -delete` (toybox `rm`
has no `--one-file-system`). If anything ever leaks past unmount,
`-xdev` keeps the delete from descending into a live `/dev`,
`/proc`, or `/sys` bind mount and unlinking host system files. The
historical bug here was an `rm -rf` that walked through a live
`/dev` bind mount and deleted host `/dev/socket` entries, which
crashed zygote — and zygote's `onrestart exec_background -- vdc
volume abort_fuse` then pegged multiple cores in a respawn loop.
Toybox `find -delete` prints every deleted path on stdout by
default; we redirect that to `/dev/null` and let only stderr
(actual errors) reach the log.

## Compositor socket

The compositor still puts its Wayland socket at
`/data/data/me.phie.tawc/wayland-0`. The mount snippet bind-mounts the
entire `/data/data/me.phie.tawc` directory at the matching path inside
the chroot, and `01-tawc.sh` symlinks it to `/tmp/wayland-0`. This
causes a benign path recursion (the rootfs lives inside the very dir we
mount), but no tool actually walks into it.

## CLI command interface

Every installation operation is reachable from the host so existing
scripted workflows can be ported off the legacy `arch-chroot-*` scripts.
Two flavours, picked per operation by what they need to do:

- **`am start`** into [ManageInstallationsActivity] — INSTALL and
  UNINSTALL. They need a foreground-service start, which background
  broadcast receivers can't reliably do on Android 14+ (BAL_BLOCK from
  cold). Activities launched by `am start` always have FGS-launch
  privileges, so the activity can start `InstallationService`
  immediately. The activity briefly surfaces, then the install runs to
  completion in the foreground service whether the activity stays open
  or not.
- **`am broadcast -W`** to `InstallationCommandReceiver` — LIST and
  RUN. Cheap and synchronous (LIST) or `goAsync()`'d onto a worker
  (RUN); both return their result via `am broadcast -W`'s data field.
  **Always use the explicit component form
  (`-n …/.install.InstallationCommandReceiver`)** — Android 14 silently
  drops implicit broadcasts to manifest-declared receivers when the
  sender isn't in the same package.

```sh
# Show what's installed.
adb shell am broadcast -W \
    -n me.phie.tawc/.install.InstallationCommandReceiver \
    -a me.phie.tawc.install.LIST

# Kick off a fresh install (works cold; the activity briefly surfaces).
adb shell am start \
    -n me.phie.tawc/.install.ManageInstallationsActivity \
    --es autoAction install --es id arch

# Tail the install log (download → extract → configure → pacman):
adb logcat -s tawc-install

# Run a command in the chroot. The receiver uses goAsync() and runs the
# command on a worker thread, so it can outlive the BroadcastReceiver's
# ~10s ANR budget; `am broadcast -W` blocks until the worker completes.
# Mounts come up inside this RUN's su shell and disappear with it (per
# Mount lifecycle above). Result data is returned via `am broadcast -W`'s
# data field.
adb shell am broadcast -W \
    -n me.phie.tawc/.install.InstallationCommandReceiver \
    -a me.phie.tawc.install.RUN --es id arch --es cmd 'uname -m'

# Tear down the install. Defensive unmount runs first; if anything
# can't be released, `rm -rf` is refused.
adb shell am start \
    -n me.phie.tawc/.install.ManageInstallationsActivity \
    --es autoAction uninstall --es id arch
```

## Required permissions

- **Magisk root.** The app needs `su` to mount/extract/chroot. On a real
  device the user accepts a Magisk prompt the first time; on the
  emulator the grant is by uid via the Magisk policies table.

- **`POST_NOTIFICATIONS`** for the foreground-service notification
  (Android 13+ requires runtime grant; the install runs without it but
  with no progress notification).

- **`INTERNET`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_DATA_SYNC`** —
  declared in the manifest, no runtime grant.

On the emulator the first two grants are applied automatically by
`bash client/start-emulator` after boot — see [emulator.md](emulator.md)
for details. Re-run the script after `adb install -r ...` to refresh
them. To do it by hand:

    uid=$(adb shell pm list packages -U me.phie.tawc | awk -F: '{print $3}')
    adb shell "su -c 'magisk --sqlite \"INSERT OR REPLACE INTO policies (uid,policy,until,logging,notification) VALUES($uid,2,0,1,0);\"'"
    adb shell pm grant me.phie.tawc android.permission.POST_NOTIFICATIONS

## Network security

`os.archlinuxarm.org` doesn't redirect to HTTPS for the bootstrap
tarball, so `res/xml/network_security_config.xml` carves out plaintext
HTTP for `archlinuxarm.org` and **only** that domain. Everything else
stays HTTPS-only.

## Android 14 FGS rules and why INSTALL/UNINSTALL aren't broadcasts

`startForegroundService()` from a background broadcast receiver is
blocked on Android 14+ (`mAllowStartForeground=false`), and Background
Activity Launches from the same context are also blocked
(`BAL_BLOCK`). That rules out a clean broadcast endpoint for INSTALL /
UNINSTALL, which need an FGS to run anything long-running.

`am start` directly into `ManageInstallationsActivity` is the
documented CLI for those operations: Activities launched by `am start`
have full FGS-launch privileges, so the activity can start
`InstallationService` immediately and the install runs to completion
whether the activity stays open or not.

If a no-UI INSTALL/UNINSTALL trigger is ever needed (e.g. headless
test workflows that can't run `am start`), the right answer is
WorkManager `OneTimeWorkRequest.setExpedited()` with a foreground info
— that's explicitly carved out of the FGS restriction. Not implemented
because every current caller can run `am start`.

## Future directions (designed-for, not implemented)

- **Other distros** — add `<Distro>Installer.kt` next to `ArchInstaller`,
  bump `Installation.distro`, and pick the installer in
  `InstallationService` based on metadata. Nothing else changes.
- **proot (rootless) installations** — add a `ProotMethod.kt` next to
  `ChrootMounter` / `ChrootRunner`, route through a strategy chosen
  from `Installation.method`. The `metadata.json` field exists for this.
- **Multiple installs** — vary the id passed in via the `--es id …`
  extra. The store/service/activity already carry the id end-to-end.

## Relationship to legacy scripts

The `client/arch-chroot-create*`, `arch-chroot-destroy`, and
`arch-chroot-run` scripts are intentionally untouched and continue to
operate on `/data/local/arch-chroot/`. They're the established way to
run integration tests today; the in-app installer is parallel
infrastructure that operates on `/data/data/me.phie.tawc/installations/<id>/`.
Porting the integration tests over to the new path is a separate task —
the broadcast `RUN` command is the bridge.
