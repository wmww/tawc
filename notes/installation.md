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
      cache/install/                 # owned by BootstrapCache (see below):
      cache/install/bootstrap-<arch>.tar.zst    # canonical Arch x86_64 bootstrap (7-day TTL)
      cache/install/bootstrap-<arch>.tar.gz     # canonical ALARM aarch64 bootstrap (7-day TTL)
      cache/install/bootstrap-<arch>.tar.tmp                  # transient zstd decompression target (Archive owns lifecycle; sweep evicts unconditionally)
      cache/install/bootstrap-<arch>.tar.{zst,gz}.part        # transient Downloader in-flight file (sweep evicts unconditionally)
      distros/
        arch/
          metadata.json              # JSON: id, distro, arch, method, sourceUrl, state, failure?
          rootfs/                    # the chroot itself (what `arch-chroot` would chroot into)
          enter.sh                   # mount + chroot wrapper, regenerated on every chroot entry

`arch` is the default installation id. Multi-installation support is a
matter of varying the id; the on-disk layout, the [Installation] data
class, and [InstallationStore] already handle it.

## State machine

Every installation id is in exactly one state. The presence of
`<distros>/<id>/metadata.json` and its `state` field together encode
the whole machine.

```
                                              install
                              ┌─────────────────(refused)
                              │
                              │
   (no dir) ──install──►  INSTALLING ──success──► READY
       ▲                      │                     │
       │                      │ fail                │
       │                      ▼                     │
       │                   FAILED                   │
       │                      │                     │
       │                      │ uninstall           │ uninstall
       │                      ▼                     ▼
       │                  UNINSTALLING ◄────────────┘
       │                      │
       └──────────────────────┘
                  success
```

Transition table — rows are current state, cells say what each request does:

| from           | install      | uninstall      | op succeeds | op fails |
|----------------|--------------|----------------|-------------|----------|
| (no dir)       | → INSTALLING | (no-op)        | —           | —        |
| INSTALLING     | refused      | → UNINSTALLING | → READY     | → FAILED |
| READY          | refused      | → UNINSTALLING | —           | —        |
| UNINSTALLING   | refused      | restart op     | → (no dir)  | → FAILED |
| FAILED         | refused      | → UNINSTALLING | —           | —        |

Two consequences shape every other piece of the system:

1. **Install is always against an empty slot.** It only runs when no
   directory exists, so `Archive.extractAsRoot` never has to wipe and
   we never re-extract on top of a live chroot. To re-install,
   uninstall first.
2. **`<distros>/<id>/` is mutated only by the uninstall path
   ([RootfsCleaner]).** Uninstall is the one place that kills chroot
   processes, unmounts strictly, and `find -xdev -depth -delete`s the
   directory tree. Nothing else deletes anything under there, so the
   historical `rm -rf` walking through a live `/dev` bind mount is
   structurally impossible.

[InstallationService] is the gate that enforces the table — every
mutation goes through it, and it consults the on-disk state before
launching a job. Activities, broadcasts, and `am start --es autoStart`
are all just inputs; the service decides.

`FAILED` is a parking state: it carries a `failure` string and can only
be cleared by uninstalling the half-installed (or half-uninstalled)
remains. There is intentionally no "resume" or "repair" — the only
recovery is uninstall + install.

## Code layout (`me.phie.tawc.install`)

The package is split into three layers:

1. **Pipeline primitives** — distro-agnostic helpers used by every install.
2. **Distro abstraction** — per-(distro × Linux arch) policy in `distro/`.
3. **Service / state machine / UI** — gate, foreground service, activities.

| File | Role |
| --- | --- |
| `Installation.kt`              | Immutable metadata for one installed environment (id/distro/arch/method/state/failure). |
| `InstallationStore.kt`         | Filesystem layout + JSON metadata I/O. `setState` is the one entry point that writes the state field. |
| `Su.kt`                        | Wrapper around Magisk `su`. Pipes the script via stdin (no shell-quoting headaches), streams combined stdout/stderr line-by-line via a callback. |
| `Downloader.kt`                | HTTP downloader for bootstrap tarballs. Caches by content length. |
| `BootstrapCache.kt`            | Sole owner of `<cacheDir>/install/`. `download(arch, url, format, …)` is the single entry point: it mkdirs, fetches via [Downloader], and refreshes the file's mtime so the TTL counts from "last used" rather than "first downloaded". Also exposes `tempPlainTarFor(arch)` for [Archive]'s zstd-decompression target so the transient lives in the cache dir under one owner. `sweepStale` runs a two-pass janitor: TTL eviction (7 days) for canonical `bootstrap-<arch>.tar.{zst,gz}`; unconditional deletion of `*.tmp` and `*.part` (transients are never valid across processes). Also defines the `BootstrapFormat` enum. |
| `Archive.kt`                   | Tar extraction (decompresses zstd in-process to a sibling `.tar`, then hands it to `toybox tar` running as root). Never wipes — install only runs against an empty slot. |
| `RootfsCleaner.kt`             | The one and only delete path: kill chroot processes → unmount strictly → `find -xdev -depth -delete`. Used by uninstall; never by install. |
| `ChrootMounter.kt`             | Builds the bind-mount shell snippet, renders the canonical `enter.sh` (mount + chroot wrapper), and provides defensive-cleanup `unmount` (used by [RootfsCleaner]). Mounts live inside a single `su` invocation's private namespace, not globally. |
| `ChrootRunner.kt`              | Concatenates `ChrootMounter.mountScript(rootfs)` with the chroot exec into one `su -c` shell so the mounts and the command share that shell's mount namespace. Base64-encodes the command into the wrapper script to dodge quoting hell. |
| `Installer.kt`                 | Generic install/uninstall orchestrator. Drives `setState(INSTALLING) → BootstrapCache.download → Archive.extractAsRoot → distro.configure → writeEnterScript → distro.initPackageManager → distro.installBasePackages → setState(READY)`. Distro-agnostic; per-distro behaviour comes from the [Distro] passed in. |
| `distro/Distro.kt`             | Interface for a (distro × Linux arch). Owns `bootstrap` (URL/format/stripPrefix), `basePackages`, and the three policy hooks (`configure`, `initPackageManager`, `installBasePackages`). Also defines `DistroBootstrap`. |
| `distro/DistroRegistry.kt`     | The only place that maps `(metadata.distro, metadata.arch)` → [Distro] and `Build.SUPPORTED_ABIS` → host-default [Distro]. `defaultForHost()` is consulted by the service before any disk state is written, so an unsupported ABI is a clean reject rather than a half-installed FAILED slot. |
| `distro/arch/ArchPacmanCommon.kt` | Helpers shared by every Arch flavour: pacman.conf munging (SigLevel/DisableSandbox/CheckSpace/IgnorePkg), mirrorlist write, profile.d/00-path.sh, the `pacman-key --init` boilerplate, and `pacman -Syu` / `pacman -S --needed`. Also exports the canonical `DEFAULT_BASE_PACKAGES` list. |
| `distro/arch/ArchLinuxX86_64.kt` | Arch Linux x86_64 (`pkgbuild.com` zstd bootstrap, `archlinux` keyring, geo-redirector mirrorlist). |
| `distro/arch/ArchLinuxArm.kt`  | Arch Linux ARM aarch64 (`archlinuxarm.org` gzip bootstrap, `archlinuxarm` keyring, curated multi-mirror list — see *ALARM mirror failover* below). |
| `util/HostArch.kt`             | `primaryAbi()` and `linuxArchFor(abi)` — the only place that knows the Android ABI ↔ Linux `uname -m` mapping. |
| `util/HumanSize.kt`            | Byte-count → "1.2 MiB" formatter for download progress. |
| `util/AppOwnership.kt`         | `chownAppDirNonRecursive` — resets a freshly-mkdir'd dir to app uid:gid so subsequent app-uid writes succeed. |
| `InstallProgress.kt`           | Stage enum + progress event used by the service. The pkg-manager-bootstrap stages are `PKG_KEYRING` and `PKG_INSTALL` (distro-agnostic names). |
| `InstallationService.kt`       | The state-machine gate. Foreground service that consults [InstallationStore], resolves the right [Distro] from [DistroRegistry], and exposes `progress` (StateFlow) + `log` (SharedFlow). |
| `OperationLogPanel.kt`         | Reusable Android view (status line + progress bar + scrolling log) that binds to the service's `progress`/`log` flows. Used by both [InstallActivity] and [UninstallActivity] so the per-operation UI lives in one place. |
| `InstallActivity.kt`           | Install flow: read-only summary (Distro display name, Linux arch, install path) → Install button → swap to [OperationLogPanel] for live progress. Recognises `autoStart=true` to skip the form and start immediately. The button is disabled (with state-aware label) when the gate would refuse. |
| `UninstallActivity.kt`         | Uninstall flow: confirmation prompt → Uninstall button → swap to [OperationLogPanel]. Recognises `autoStart=true` to skip the confirmation. |
| `DistroInfoActivity.kt`        | Per-distro detail page (id, registry-resolved distro/arch, method, source URL, installed-at, state/failure, full rootfs path) + an async `du -sk` size readout (only for `READY`) + a red Uninstall button (which opens [UninstallActivity]). Reached from a tap on a home-screen row. |

The `MainActivity` home screen lists the on-disk installations
(distro + arch only — size lives on [DistroInfoActivity] because
`du -sk` over a multi-GB rootfs costs seconds via `su` and would slow
down opening the launcher). Each row is tappable and opens the info
page; the page itself hosts the Uninstall button.

The non-compositor activities (`MainActivity`, `InstallActivity`,
`UninstallActivity`, `DistroInfoActivity`) extend `AppCompatActivity`
and share a small `me.phie.tawc.ui.Scaffold` helper that builds a
`MaterialToolbar` (with a back/up arrow on child screens) plus a
content column. The theme is `Theme.Material3.DayNight.NoActionBar`
with a yellow-orange `colorPrimary` (`@color/tawc_accent`) for primary
buttons and `@color/tawc_danger` (red) for destructive ones; both have
night-mode variants in `res/values-night/`. The compositor activity
keeps the device-default theme — it draws its own surface and never
inflates Material widgets.

## Install pipeline

The state machine guarantees `ArchInstaller.install()` only ever runs
against a `(no dir)` slot — no wipe, no overlay, no resume. Stages
reported as `InstallProgress` to the UI and per-line logged to logcat
(`tawc-install`):

1. **(state write)** — `setState(INSTALLING)` runs first; from here on
   the entry exists on disk and any failure parks it in `FAILED`.
2. **DOWNLOADING** — `BootstrapCache.download(arch, url, format, …)`
   handles the fetch, the cache filename, and the mtime touch. The TTL
   counts from "last used" rather than original download.
   - x86_64: `https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst` (toplevel `root.x86_64/` is flattened post-extract because toybox `tar` doesn't `--strip-components`)
   - aarch64: `http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz` (no wrapper dir; HTTP — see *Network security* below)
3. **EXTRACTING** — root-owned via toybox tar. Zstd is decompressed
   in-process to a sibling `.tar` first because piping data into a
   shell's stdin loses bytes (the shell pre-buffers past the script).
   The destination is a freshly-mkdir'd directory; the install gate
   guarantees nothing else lives there.
4. **CONFIGURING** — writes the same files the legacy create scripts do:
   - `/etc/resolv.conf` (8.8.8.8)
   - `/etc/pacman.conf` (`SigLevel=Never`, `DisableSandbox`, `#CheckSpace`, `IgnorePkg = linux …`)
   - `/etc/pacman.d/mirrorlist` (x86_64: geo-routed `geo.mirror.pkgbuild.com`; aarch64: a curated multi-mirror list — see *ALARM mirror failover* below)
   - `/etc/profile.d/00-path.sh` (PATH/TMPDIR/HOME)
   - `/etc/profile.d/01-tawc.sh` is *not* written here — `ChrootMounter`
     rewrites it on every chroot entry (Wayland env: `WAYLAND_DISPLAY`,
     `XDG_RUNTIME_DIR`, `LD_LIBRARY_PATH`, `HYBRIS_EGLPLATFORM`, `wayland-0`
     symlink) so env changes pick up without a reinstall.
5. **PACMAN_INIT** — `pacman-key --init`, `--populate archlinux` /
   `archlinuxarm` depending on arch. (Bind mounts are not a separate
   stage — they're set up inside the very `su` shell that runs each
   chroot command, see *Mount lifecycle* below.)
6. **PACMAN_INSTALL** — `pacman -Syu --noconfirm` then
   `pacman -S --noconfirm --needed base-devel git libtool wayland
   wayland-protocols pkg-config autoconf automake patchelf weston gtk3
   gtk3-demos`.
7. **(state write)** — `setState(READY)` only after every step above
   succeeds.

The downloaded tarball persists in `cache/install/` across runs; the
*next* install of the same arch reuses it as a download cache hit. The
rootfs itself does not — every install is a fresh extraction, by
construction.

[BootstrapCache] enforces a 7-day idle TTL on top of `cacheDir`. Without
that, Android only evicts under storage pressure (or when the user hits
"Clear Cache" in app info), so a 200 MB tarball can squat indefinitely
on a phone with free space, competing with caches from apps the user
actually opens. [TawcApplication.onCreate] runs `sweepStale()` on a
background thread on every cold start. The sweep is a two-pass janitor:

1. **TTL pass** — canonical `bootstrap-<arch>.tar.{zst,gz}` files are
   evicted when their mtime is older than 7 days. Reuse refreshes the
   clock because `BootstrapCache.download` touches the mtime on every
   successful fetch (whether the bytes were freshly downloaded or served
   from a size-match cache hit inside [Downloader]). `setLastModifiedTime`
   is the NIO variant — `File.setLastModified` silently no-ops on some
   Android FS/SDK combos.
2. **Unconditional pass** — `bootstrap-<arch>.tar.tmp` ([Archive]'s
   zstd-decompression transient) and `bootstrap-<arch>.tar.{zst,gz}.part`
   ([Downloader]'s in-flight suffix) are deleted regardless of mtime,
   since neither is ever meaningful across processes — they only exist
   inside one `Archive.extractAsRoot` / `Downloader.download` call's
   `try/finally`. Anything found at app start is by definition stranded
   by a crash.

During an x86_64 install the cache dir briefly *triples* in apparent
size: ~200 MB compressed `tar.zst`, plus the ~800 MB decompressed `.tmp`
that [Archive] writes for toybox tar to consume. The `.tmp` is deleted
in [Archive.extractAsRoot]'s `finally`; if that delete fails (e.g. FS
error) the next sweep evicts the leftover unconditionally.

The sweep is best-effort. There's a tiny window where it could race a
concurrent install — sweep stat()s the canonical tarball, decides to
evict, unlinks just before [Archive.extractAsRoot] runs `require(tarball.exists())`.
The install fails to `FAILED`; recovery is uninstall + retry. Practical
exposure is small because installs are user-initiated via `am start` and
sweep fires once at process cold start.

## Uninstall pipeline (the one and only wipe)

`<distros>/<id>/` is mutated by exactly one path:
`RootfsCleaner.wipe(installDir)`, called from
`ArchInstaller.uninstall()`. It does, in order:

1. **(state write)** — `setState(UNINSTALLING)`.
2. **kill chroot processes** — sweeps `/proc/<pid>/root` and
   `/proc/<pid>/cwd`, comparing dev:inode against the rootfs (so it
   matches whether the kernel reports `/data/data/<pkg>/...` or
   `/data/user/0/<pkg>/...`). Sends SIGKILL twice with a beat between
   to catch daemons that respawn on signal — the canonical offender is
   the `gpg-agent --daemon` that `pacman-key --init` detaches; left
   alive it holds FDs into the rootfs and races the delete, which on
   Android 14 spins vold's FUSE accounting into a `vdc volume
   abort_fuse` storm.
3. **strict unmount** — `ChrootMounter.unmount` runs via `su -mm`
   (Magisk's mount-master mode) so `umount` actually affects the
   global mount table; refuses with a non-zero exit if any mount
   remains under the rootfs.
4. **`find -xdev -depth -delete`** — never `rm -rf`. Toybox `rm` has
   no `--one-file-system`, and a single missed unmount could
   otherwise let `rm` walk through a live `/dev` bind and unlink host
   nodes (the historical bug that crashed zygote and pegged vold).
   `-xdev` is the belt-and-braces against that.

On success the directory (including `metadata.json`) is gone and the id
is back to `(no dir)`. On any failure, the directory is left as-is and
`setState(FAILED)` records the reason; the only recovery is to call
uninstall again.

## Mount lifecycle

Magisk's `su` inherits the **calling** process's mount namespace by
default — bind mounts done inside one `Su.run` would persist into the
app's namespace and pile up across calls. The recursive
`/data/data/<pkg> → <rootfs>/data/data/<pkg>` bind in particular is
the smoking gun: it makes `find -xdev` walk back into itself ("loop
detected") and the uninstall delete fails on a tree it created
moments earlier. To keep each invocation isolated, [Su.run] wraps the
non-mount-master path in `unshare -m` so every script gets its own
private mount namespace that's torn down when the script exits. The
canonical chroot entry point is `<installation-dir>/enter.sh`, which
combines mount setup and the chroot exec into one shell — the mounts
exist for the lifetime of that shell and never leak.

`ChrootMounter.enterScript(rootfs)` renders the script body;
`ArchInstaller.install` writes it after extraction and
`ChrootRunner.run` rewrites it on every call so the mount logic is
always current. There is no separate `MOUNT` operation because there
can't be — the mounts only exist for the lifetime of that one shell.
This avoids polluting the global mount table with stale entries (and
avoids the zygote-fork crash that follows when `/data/data/<pkg>/...`
has live bind mounts during package fork).

The install path **never** touches mounts. It can't possibly delete
through one because it doesn't delete at all (the gate guarantees an
empty slot). Mount cleanup belongs to the uninstall path
([RootfsCleaner]); see *Uninstall pipeline* above.

`ChrootMounter.unmount` `realpath`s the rootfs before scanning
`/proc/mounts`, because Kotlin's `File.absolutePath` returns the
`/data/user/0/...` symlink form while `/proc/mounts` reports the
canonical `/data/data/...` form — naive substring matching misses
every entry. The match is also a strict prefix check (`==` or starts
with `r"/"`) so paths containing `.` don't over-match other mounts.

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
# If the id is already in any state but `(no dir)` the request is
# refused at the service gate and the activity logs the rejection — the
# rootfs is *never* re-extracted on top of an existing one.
adb shell am start \
    -n me.phie.tawc/.install.InstallActivity \
    --es autoStart true --es id arch

# Tail the install log (download → extract → configure → pacman):
adb logcat -s tawc-install

# Tear down the install. RootfsCleaner kills chroot processes,
# unmounts strictly, and `find -xdev -depth -delete`s the dir. The
# uninstall is also the only way to clear a `FAILED` install before
# trying again.
adb shell am start \
    -n me.phie.tawc/.install.UninstallActivity \
    --es autoStart true --es id arch
```

`autoStart` fires once per launch, gated on `savedInstanceState ==
null`. A process-death recreation of the activity does not re-trigger;
a fresh `am start` (which delivers a new launch intent) does. Combined
with the service-level gate, even a leaked auto-start is at worst a
no-op rejection.

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

## ALARM mirror failover

The aarch64 install path replaces ALARM's shipped one-Server mirrorlist
(just `http://mirror.archlinuxarm.org/$arch/$repo`, the geo-IP redirector)
with a curated list of explicit mirrors. The geo-IP redirector load-
balances across regional mirrors, and any single regional mirror can be
mid-sync at the moment pacman happens to hit it — observed failure mode
is a stable 404 on a small handful of `*.pkg.tar.xz` files mid-
transaction (e.g. `bubblewrap-0.11.2-1-aarch64.pkg.tar.xz`). With one
Server line pacman has no fallback and the whole transaction aborts;
with several explicit mirrors it skips past the stale one to the next.
The list lives in `ArchInstaller.configure` and intentionally puts the
geo-redirector first so the common case still uses the closest mirror.

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

- **Other distros** — add a new file in `distro/<family>/`
  implementing the `Distro` interface and append it to
  `DistroRegistry.all`. The generic `Installer` and the activities
  pick it up automatically. The `(distro, arch)` lookup in
  [DistroRegistry.forInstallation] is what dispatches to the right
  one at runtime.
- **proot (rootless) installations** — add a `ProotMethod.kt` next to
  `ChrootMounter` / `ChrootRunner`, route through a strategy chosen
  from `Installation.method`. The `metadata.json` field exists for this.
- **Multiple installs** — vary the id passed in via the `--es id …`
  extra. The store/service/activity already carry the id end-to-end.
  The `(distro, arch)` registry lookup means two installations of
  different Arch flavours on the same device (e.g. an x86_64
  emulator running both ALARM-via-qemu and Arch x86) is also
  unblocked once the install activity grows a "pick distro" UI.

## Refactor history

- **2026-04-27** — split `ArchInstaller` into a generic [Installer]
  pipeline plus a `distro/` package (`Distro` interface,
  `DistroRegistry`, two Arch implementations sharing
  `ArchPacmanCommon`). Pipeline stages renamed
  `PACMAN_INIT`/`PACMAN_INSTALL` →
  `PKG_KEYRING`/`PKG_INSTALL`. UI labels now use
  `Distro.displayName` + `Distro.linuxArch` instead of the raw
  Android ABI. Unsupported-ABI installs are now refused at the
  service gate before any disk state is written. Generic helpers
  (`HostArch`, `HumanSize`, `AppOwnership`) moved to
  `install/util/`. See `notes/distro-abstraction.md` for the full
  design rationale.

## Host-side bridge

`client/tawc-chroot-run` is the host-driven counterpart to in-app
`ChrootRunner.run`. Both invoke the same `<installation-dir>/enter.sh`
written by `ChrootMounter.enterScript`, so the mount + chroot logic is
defined exactly once (in Kotlin) and rendered to a script that adb
shell + su can replay. Used by the integration tests
(`testing/integration/src/adb.rs`), `testing/install-test-deps.sh`,
`testing/build-debug-app.sh`, and `client/build-libhybris`.
