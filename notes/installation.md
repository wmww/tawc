# In-app installation system

The tawc Android app includes a Kotlin re-implementation of the chroot
install/run/destroy logic that previously lived only in the
`client/arch-chroot-*` shell scripts. This lets the app:

- ship a "Manage installations" screen as a first-class feature
- store the chroot inside its private data dir, so uninstalling the app
  reclaims everything
- offer the same operations to adb / integration tests via the
  `client/tawc-chroot-run` host script (no broadcast surface)

This is the *only* chroot system in the project. The earlier
`client/arch-chroot-*` scripts (which targeted `/data/local/arch-chroot/`)
have been deleted; their logic now lives in this package and the
auto-generated `enter.sh`.

## On-disk layout

Everything lives under the app's private data dir:

    /data/data/me.phie.tawc/
      cache/install/                 # downloaded bootstrap tarballs (kept across runs)
      distros/
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
| `ChrootMounter.kt`             | Builds the bind-mount shell snippet, renders the canonical `enter.sh` (mount + chroot wrapper), and provides defensive-cleanup `unmount`. Mounts live inside a single `su` invocation's private namespace, not globally. |
| `ChrootRunner.kt`              | Concatenates `ChrootMounter.mountScript(rootfs)` with the chroot exec into one `su -c` shell so the mounts and the command share that shell's mount namespace. Base64-encodes the command into the wrapper script to dodge quoting hell. |
| `ArchInstaller.kt`             | Stage machine: download → extract → configure → pacman init → pacman install. (No separate mount stage — see *Mount lifecycle*.) |
| `InstallProgress.kt`           | Stage enum + progress event used by the service. |
| `InstallationService.kt`       | Foreground service that runs install/uninstall in a coroutine and exposes `progress` (StateFlow) and `log` (SharedFlow). |
| `OperationLogPanel.kt`         | Reusable Android view (status line + progress bar + scrolling log) that binds to the service's `progress`/`log` flows. Used by both [InstallActivity] and [UninstallActivity] so the per-operation UI lives in one place. |
| `InstallActivity.kt`           | Install flow: read-only summary (distro, detected CPU arch, install path) → Install button → swap to [OperationLogPanel] for live progress. Recognises `autoStart=true` to skip the form and start immediately. |
| `UninstallActivity.kt`         | Uninstall flow: confirmation prompt → Uninstall button → swap to [OperationLogPanel]. Recognises `autoStart=true` to skip the confirmation. |
| `DistroInfoActivity.kt`        | Per-distro detail page (id/distro/arch/method/source URL/installed-at/full rootfs path) + an async `du -sk` size readout + Delete button (which opens [UninstallActivity]). Reached from a tap on a home-screen row. |

The `MainActivity` home screen lists the on-disk installations
(distro + arch only — size lives on [DistroInfoActivity] because
`du -sk` over a multi-GB rootfs costs seconds via `su` and would slow
down opening the launcher). Each row is tappable and opens the info
page; the page itself hosts the Delete button.

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
     symlink) so env changes pick up without a reinstall.
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
are torn down when that call exits. We accommodate this by combining
mount setup and the chroot exec into a single `su -c "..."` invocation
— that's what the on-disk `<installation-dir>/enter.sh` does.

`ChrootMounter.enterScript(rootfs)` renders the script body; the
installer writes it after extraction and `ChrootRunner.run` rewrites it
on every call so the mount logic is always current. There is no
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

Install and uninstall are driven from the host via `am start` into
[InstallActivity] / [UninstallActivity] with an `autoStart=true` extra.
Activities launched by `am start` have full FGS-launch privileges, so
the activity can immediately start `InstallationService` and the
operation runs to completion in the foreground service whether the
activity stays open or not.

There is intentionally **no broadcast/receiver surface** for running
arbitrary commands as root in the chroot. The app has Magisk root for
its own install/uninstall flow; exposing that as a public endpoint
would let any other app on the device get root execution inside the
chroot. If we ever need a no-UI handle for the test harness, build it
with auth baked in (signature permission or uid check) at that point.

```sh
# Kick off a fresh install (works cold; the activity briefly surfaces).
adb shell am start \
    -n me.phie.tawc/.install.InstallActivity \
    --es autoStart true --es id arch

# Tail the install log (download → extract → configure → pacman):
adb logcat -s tawc-install

# Tear down the install. Defensive unmount runs first; if anything
# can't be released, `rm -rf` is refused.
adb shell am start \
    -n me.phie.tawc/.install.UninstallActivity \
    --es autoStart true --es id arch
```

Listing existing installations from the host: read the metadata
directly with root.

```sh
adb shell "su -c 'ls /data/data/me.phie.tawc/distros/'"
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

`am start` directly into `InstallActivity` / `UninstallActivity` (with
`--es autoStart true`) is the documented CLI for those operations:
Activities launched by `am start` have full FGS-launch privileges, so
the activity can start `InstallationService` immediately and the
install runs to completion whether the activity stays open or not.

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

## Host-side bridge

`client/tawc-chroot-run` is the host-driven counterpart to in-app
`ChrootRunner.run`. Both invoke the same `<installation-dir>/enter.sh`
written by `ChrootMounter.enterScript`, so the mount + chroot logic is
defined exactly once (in Kotlin) and rendered to a script that adb
shell + su can replay. Used by the integration tests
(`testing/integration/src/adb.rs`), `testing/install-test-deps.sh`,
`testing/build-debug-app.sh`, and `client/build-libhybris`.
