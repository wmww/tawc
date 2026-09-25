# In-app installation system

The TAWC Android app includes a Kotlin re-implementation of the
install/run/destroy logic that previously lived only in the
`client/arch-chroot-*` shell scripts. This lets the app:

- ship a "Manage installations" screen as a first-class feature
- store the rootfs inside its private data dir, so uninstalling the app
  reclaims everything
- offer the same operations to adb / integration tests via the
  `scripts/rootfs-run.sh` host script (no broadcast surface)

The earlier `client/arch-chroot-*` scripts (which targeted
`/data/local/arch-chroot/`) have been deleted; their logic now lives
entirely in this Kotlin package. Rootfs entry goes through
[InstallationMethod.startInside] (see
[rootfs-sessions.md](rootfs-sessions.md)); there is no on-disk wrapper
script.

## Install methods

Three [InstallationMethod] implementations exist. **tawcroot is the
default and only officially supported method.** chroot and proot are
**not officially supported** — they're debug-build-only, kept around
purely for the dev loop, and never exposed to release users.

| key       | builds it ships in     | notes |
|-----------|------------------------|-------|
| tawcroot  | debug + release (default) | Custom systrap-based syscall emulator (`tawcroot/`). Rootless. The default for new installs and the only one users see in release builds. |
| proot     | debug only — **not officially supported** | Termux fork of proot, ptrace-based fake chroot. Rootless. Kept for performance comparisons + as a fallback during tawcroot bring-up. See [notes/proot.md](proot.md). |
| chroot    | debug only — **not officially supported** | Real `chroot(2)` via Magisk `su`. Fastest path with no syscall translation, but only works on rooted devices. See [notes/chroot.md](chroot.md). |

### Build-time selection

`app/build.gradle.kts` flips the `BuildConfig.METHOD_*_ENABLED` booleans
per build type. Defaults: debug ships all three for dev-loop coverage,
release ships only tawcroot. Override either side with
`-PtawcMethods=tawcroot[,proot[,chroot]]` (e.g. for testing the proot
path against a release-shaped APK). tawcroot must always be enabled.

The `EnabledMethods` helper exposes those booleans to the runtime:
`InstallationMethod.forKey` returns null for any method the build
doesn't ship, the install form hides the picker entirely when only one
method is enabled, and any APK that doesn't ship proot drops
`libproot.so` / `libproot-loader.so` at packaging time.

Distros are not build-gated the same way: every build ships all of
them, but the install form splits them into the two supported ones
and an "Other distros" expander (`Distro.supported`, see
[distro-options.md](distro-options.md) → *Which distros are
supported*).

## On-disk layout

Everything lives under the app's private data dir:

    /data/data/me.phie.tawc/
      cache/install/                 # owned by BootstrapCache (see below):
      cache/install/bootstrap-<cacheKey>.tar.zst    # canonical Arch x86_64 bootstrap (7-day TTL); cacheKey="arch-x86_64"
      cache/install/bootstrap-<cacheKey>.tar.gz     # canonical ALARM / Manjaro / Debian bootstrap (7-day TTL); cacheKey="arch-aarch64" / "manjaro-aarch64" / "debian-sid-aarch64"
      cache/install/bootstrap-<cacheKey>.tar.fifo                 # transient zstd→tar streaming pipe (Archive owns lifecycle; sweep evicts unconditionally)
      cache/install/bootstrap-<cacheKey>.tar.{zst,gz}.part        # transient Downloader in-flight file (sweep evicts unconditionally)
      distros/
        <id>/
          metadata.json              # JSON: schemaVersion, id, label?, distro, arch, method, installedAtMillis, installedAtAppVersionCode, sourceUrl, state, failure?, tawcStamp?, tawcInstalls?, externalBinds? (notes/external-binds.md)
          rootfs/                    # the chroot itself (what `arch-chroot` would chroot into)

The on-disk layout, the [Installation] data class, and
[InstallationStore] already handle multiple installs side-by-side; the
id is whatever the InstallActivity intent's `--es id <id>` extra (or
the home-screen install form's slugified label) supplied.

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
| CORRUPT        | refused      | wipe runs*     | → (no dir)  | stays CORRUPT |

\* The uninstall runs normally from CORRUPT, but the on-disk state
never moves — `setState` goes through `update()`, which refuses to
write through a CORRUPT record — so the slot reads CORRUPT until the
wipe removes the dir (and stays CORRUPT if the wipe fails or is
cancelled, rather than parking in FAILED).

CORRUPT is a pseudo-state, never written to disk: when
`metadata.json` exists but won't parse (corrupt file, or a
`schemaVersion` from a newer app — see "Schema versioning"),
`InstallationStore.list()`/`load()` return a synthetic marker record
(`Installation.corruptMarker`, id from the directory name, `failure`
= the parse error) instead of dropping the slot. The UI renders it
like FAILED (visible card, size probe, Delete; no run/launcher), and
the store refuses to write through it — `update()` no-ops and
`save()` throws on a CORRUPT record — so the unparseable file stays
byte-identical for manual recovery until the user uninstalls.
`TawcInstaller.installInto` also skips CORRUPT slots (the manifest of
previously-installed entries is unknowable).

Two consequences shape every other piece of the system:

1. **Install is always against an empty slot.** It only runs when no
   directory exists, so `Archive.extractAsRoot` never has to wipe and
   we never re-extract on top of a live chroot. To re-install,
   uninstall first.
2. **`<distros>/<id>/` is mutated only by the uninstall path
   ([RootfsCleaner]).** One engine for every install method: kill
   guest processes, unmount (chroot only), refuse if any mount
   remains under the install dir, then `find -xdev -depth -delete`.
   Nothing else deletes anything under there — enforced by
   `RootfsCleanerTripwireTest`, which scans the sources for stray
   recursive deletes — so the historical `rm -rf` walking through a
   live `/dev` bind mount is structurally impossible.

### Cancellation

The bound UI exposes a Cancel button on the in-progress panel
([OperationLogPanel]). The state-machine consequences:

- **Cancel install**: `INSTALLING → FAILED` (catch handler), then a
  follow-up uninstall fires automatically (`FAILED → UNINSTALLING →
  (no dir)`). The install gate guarantees an empty slot at install
  start, so the only files in the rootfs are bootstrap + freshly
  installed packages — no user data at risk. Confirm dialog before
  the cancel because the time loss is non-trivial.
- **Cancel uninstall**: `UNINSTALLING → FAILED`. The slot stays on
  disk in whatever partial state it was in; user can re-trigger
  uninstall to finish, or pull leftover files manually first. **No
  confirm dialog** — the user might be quickly aborting to save data
  they didn't mean to delete; another dialog in the way risks
  finishing the wipe before they can react.

The cancel mechanism has three layers:

1. **Coroutine cancellation** — [InstallationService] wraps the job
   body in `runInterruptible(Dispatchers.IO)` so a `Job.cancel()`
   translates into a thread interrupt. [Su.run],
   [Sh.run], and [Downloader.download] all honour
   thread interrupts (`destroyForcibly` of subprocess /
   `InterruptedIOException` from the HTTP read loop).
2. **Defensive process kill** — Magisk's `su` doesn't reliably
   forward signals to the remote shell's grandchildren (tar / pacman
   / find), so [InstallationService.runCancelKillScript] fires in
   parallel with the cancel: a /proc sweep that SIGKILLs anything
   chrooted into the rootfs (by `/proc/<pid>/root` dev:ino) or whose
   argv mentions the install path (catches host-side helpers).
3. **Two-pass wipe** — [RootfsCleaner.wipe] deletes the rootfs
   subtree first, then `metadata.json` + the container dir as a
   separate explicit step. A cancel mid-pass-1
   leaves `metadata.json` intact so the slot still shows up on the
   home screen and a follow-up uninstall picks up cleanly. Without
   this split, `find`'s readdir order between `rootfs/` and its
   sibling `metadata.json` is implementation-defined and the slot
   could become an orphan.

[InstallationService] is the gate that enforces the table — every
mutation goes through it, and it consults the on-disk state before
launching a job. The Install button, the Delete dialog, and the dev
exec broker's `install` / `uninstall` actions are all just inputs;
the service decides.

## Process discovery and cleanup

`ProcessScanner` is the shared process-discovery path for the task
manager and rootfs cleanup. App-uid methods (`tawcroot`, `proot`) are
scanned without `su`: `AppUidProcfsScanner` walks `/proc`, checks the
cheap `/proc/<pid>/{cwd,exe,root}` links first, then falls back to
executable file mappings in `/proc/<pid>/maps`. The maps fallback is
needed for tawcroot because the kernel's `exe` / `cmdline` can describe
the loader handoff rather than the guest ELF, while the loaded guest
binary still appears as an executable mapping under
`<distros>/<id>/rootfs`.

The app-uid scanner matches exact rootfs paths and descendants, so a
guest with `cwd` at the rootfs root is visible. Orphan detection uses
the same rootfs boundary under `<distros>/<id>/rootfs` and reports a
missing install record as `(uninstalled: <id>)`. Real `chroot(2)`
processes are root-owned and hidden by Android procfs policy, so the
chroot path is isolated in `SuProcfsScanner`, which compares
`/proc/<pid>/root` dev:inode values via `su`.

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
| `Sh.kt`                        | Rootless sibling of `Su`: same stdin-script shape, as the app uid. Sets `TMPDIR` to the app cache dir (`java.io.tmpdir`) for every child — app processes have none, and mksh spills every heredoc to a temp file there, otherwise falling back to the app-unwritable `/data/local` (install died at Configure; wmww/tawc#13, #16). mksh silently ignores a `TMPDIR` that is not an existing writable dir, so `Sh` re-creates it per run. Guard: `test_prodenv_host_sh_heredoc`. |
| `Downloader.kt`                | HTTP downloader for bootstrap tarballs. Caches by content length. |
| `SignatureVerifier.kt`         | Sealed `BootstrapVerification` (`ResolvedAtInstallTime` / `Pgp` / `Sha256`) and `verify(...)`. Called from [Installer] between download and extract — see *Bootstrap integrity* below. Uses BouncyCastle (`bcpg-jdk18on` + `bcprov-jdk18on`) for the OpenPGP layer. Treat as load-bearing security code. |
| `Minisign.kt`                  | Minisign/signify (Ed25519, plain `Ed` and BLAKE2b-512-prehashed `ED`) detached-signature parsing + verification, on BouncyCastle's low-level `Ed25519Signer` / `Blake2bDigest`. Used by [VoidSha256Resolver] to authenticate Void's `sha256sum.txt` before any digest is read out of it. Also load-bearing security code. |
| `BootstrapCache.kt`            | Sole owner of `<cacheDir>/install/`. `download(arch, url, format, …)` is the single entry point: it mkdirs, fetches via [Downloader], and refreshes the file's mtime so the TTL counts from "last used" rather than "first downloaded". Also exposes `tempFifoFor(arch)` for [Archive]'s zstd→tar streaming FIFO so the transient lives in the cache dir under one owner. `sweepStale` runs a two-pass janitor: TTL eviction (7 days) for canonical `bootstrap-<cacheKey>.tar.{zst,gz}`; unconditional deletion of `*.fifo`, legacy `*.tmp`, `*.part` (transients are never valid across processes), and the legacy `*.md5.verified` sidecar left by pre-PGP ALARM installs. Also defines the `BootstrapFormat` enum. |
| `Archive.kt`                   | Tar extraction. Plain `.tar` / `.tar.gz` get handed to `toybox tar` directly; `.tar.zst` is streamed in-process through a named pipe (`bootstrap-<cacheKey>.tar.fifo`) so the ~700 MB plaintext never lands on disk. Never wipes — install only runs against an empty slot. |
| `RootfsCleaner.kt`             | The one and only delete path, for every install method: kill guest processes → unmount (chroot only) → refuse if any mount remains under the install dir → two-pass `find -xdev -depth -delete` (su-first for chroot, app-uid with one su retry otherwise). The chroot-only facts come from the metadata-recorded method key, not a live `InstallationMethod`, so disabled-method slots still wipe correctly. Used by uninstall; never by install. `RootfsCleanerTripwireTest` fails on recursive deletes elsewhere in the app sources. |
| `RootfsTmpSweeper.kt`          | Age-based sweep (3 days, lstat mtimes, never follows symlinks, skips `/tmp/.X11-unix`) of every install's `<rootfs>/tmp`, run from `TawcApplication`'s startup thread — see *Rootfs /tmp sweep* below. |
| `ChrootMounter.kt`             | Builds the bind-mount shell snippet (`mountScript`) used by [ChrootMethod.startInside], and provides defensive-cleanup `unmount` (used by [RootfsCleaner]). Mounts live inside a single `su` invocation's private namespace, not globally. |
| `Installer.kt`                 | Generic install/uninstall orchestrator. Drives `setState(INSTALLING) → BootstrapCache.download → Archive.extractAsRoot → distro.configure → distro.initPackageManager → distro.installBasePackages → setState(READY)`. Distro-agnostic; per-distro behaviour comes from the [Distro] passed in. |
| `distro/Distro.kt`             | Interface for a (distro × Linux arch). Owns `bootstrap` (URL/format/stripPrefix/verification), `cacheKey`, `basePackages`, the three policy hooks (`configure`, `initPackageManager`, `installBasePackages`), and `resolveBootstrap()` for distros with install-time URL/digest lookup (Manjaro/Void/Debian). Also defines `DistroBootstrap`. |
| `distro/DistroRegistry.kt`     | The only place that maps `(metadata.distro, metadata.arch)` → [Distro], `Build.SUPPORTED_ABIS` → installable [Distro] list, and the install activity's distro radio key → [Distro]. `availableForHost()` (supported-first) / `supportedForHost()` / `otherForHost()` / `defaultForHost()` / `forKey()`. |
| `distro/arch/ArchPacmanCommon.kt` | Helpers shared by every Arch / Manjaro flavour: pacman.conf munging (SigLevel/DisableSandbox/CheckSpace/IgnorePkg), mirrorlist write, the `pacman-key --init` boilerplate, and `pacman -Syu` / `pacman -S --needed`. Also exports the canonical `DEFAULT_BASE_PACKAGES` list. |
| `distro/arch/ArchLinuxX86_64.kt` | Arch Linux x86_64 (`pkgbuild.com` zstd bootstrap, `archlinux` keyring, geo-redirector mirrorlist). |
| `distro/arch/ArchLinuxArm.kt`  | Arch Linux ARM aarch64 (`archlinuxarm.org` gzip bootstrap, `archlinuxarm` keyring, curated multi-mirror list — see *ALARM mirror failover* below). |
| `distro/manjaro/GitHubReleaseResolver.kt` | Tiny `api.github.com /releases/latest` client. Returns `(browser_download_url, sha256)` for a named asset by reading the API response's `digest` field. |
| `distro/manjaro/ManjaroArm.kt` | Manjaro ARM aarch64 (`manjaro-arm/rootfs` GitHub release gz; `archlinuxarm manjaro manjaro-arm` keyring set). `resolveBootstrap` does the GitHub-API lookup at install time so we always pull the latest weekly tag with a verifiable SHA-256. |
| `distro/voidlinux/VoidCommon.kt` | Shared helpers for the two Void Linux flavours. `xbps-install -Suy xbps` then `xbps-install -uy` for the keyring/sync stage, followed by `xbps-install -y <packages>`. RSA package signatures are verified against the keys shipped in the bootstrap under `/var/db/xbps/keys/`. |
| `distro/voidlinux/VoidSha256Resolver.kt` | Fetches `sha256sum.txt` from `repo-default.voidlinux.org/live/current/` over HTTPS at install time, verifies its `sha256sum.sig` minisign signature against the release key for the image date ([Minisign] + [VoidReleaseKeys]), then parses out the latest `void-<arch>-ROOTFS-YYYYMMDD.tar.xz` filename + SHA-256 and hands them to the installer as a [BootstrapVerification.Sha256]. Fails closed if the signature is missing, bad, or no key for the image date can be obtained. |
| `distro/voidlinux/VoidReleaseKeys.kt` | Void's per-image-date minisign release public keys, bundled verbatim from `srcpkgs/void-release-keys/files/` in void-linux/void-packages, plus the `raw.githubusercontent.com` URL used to fetch a key for an image date newer than this APK. |
| `distro/voidlinux/VoidLinuxX86_64.kt` | Void Linux x86_64 (glibc). Bootstrap is the dated `tar.xz` ROOTFS published under `live/current/`. |
| `distro/voidlinux/VoidLinuxAarch64.kt` | Void Linux aarch64 (glibc). Same flow as the x86_64 flavour, different bootstrap URL and ABI. |
| `distro/apt/AptCommon.kt`      | Shared apt-family helpers: deb822 sources, apt.conf, dpkg `path-exclude`, apt-family `/etc/profile.d/tawc.sh`, shell-default stubs, `apt-get update`, `apt-get dist-upgrade`, and base package install. |
| `distro/debian/DebianDockerResolver.kt` | Pins the debuerreotype `dist-amd64` / `dist-arm64v8` branch tip to a commit SHA via the GitHub API, then fetches the OCI manifest at that commit and returns the `rootfs.tar.gz` URL plus layer SHA-256 — manifest and tarball are guaranteed to come from one tree state. |
| `distro/debian/DebianSid.kt`   | Debian sid x86_64 / aarch64. Suite-driven apt-family implementation; future Debian suites should mostly be additional data objects. |
| `util/HostArch.kt`             | `primaryAbi()` and `linuxArchFor(abi)` — the only place that knows the Android ABI ↔ Linux `uname -m` mapping. |
| `util/HumanSize.kt`            | Byte-count → "1.2 MiB" formatter for download progress. |
| `util/AppOwnership.kt`         | `chownAppDirNonRecursive` — resets a freshly-mkdir'd dir to app uid:gid so subsequent app-uid writes succeed. |
| `InstallProgress.kt`           | Stage enum + progress event used by the service. The pkg-manager-bootstrap stages are `PKG_KEYRING` and `PKG_INSTALL` (distro-agnostic names). |
| `InstallationService.kt`       | The state-machine gate. Foreground service that consults [InstallationStore], resolves the right [Distro] from [DistroRegistry], and exposes `progress` (StateFlow) + `log` (SharedFlow). |
| `InstallProgress.kt`'s `toOperationProgress` | Maps the install-specific `InstallStage` enum onto the generic `OperationStage`. Used by [InstallationService.publishProgress] when calling `op.publish(...)` on the per-job [me.phie.tawc.ops.MutableOperation]. |
| `InstallActions.kt`            | Broker action handlers (`install` / `uninstall`) registered from [TawcApplication.onCreate] (debug builds only). Validate args, call [InstallationService] companion-object helpers, open [LogScreenActivity] best-effort, and mirror the registered Operation's flows back to the broker socket until terminal. Host disconnect → `Operation.cancel()`. See `notes/exec-broker.md` for protocol. |
| `InstallActivity.kt`           | Install form (distro radio, free-form Label EditText with live slug-derived id hint, vertical method radio in `tawcroot (recommended) / proot / chroot (requires root)` order, "What's the difference?" link to [InstallMethodInfoActivity]) → Install button → calls [InstallationService.startInstall], opens [LogScreenActivity], and finishes itself. The Install button is disabled while the label is empty / unslugifiable / collides with an existing installation. The activity is `exported="false"` — there is no CLI launch path. |
| `InstallMethodInfoActivity.kt` | Read-only reference page describing the three install methods (tawcroot / proot / chroot). Linked from the install form's "What's the difference?" affordance so users can compare tradeoffs without leaving the app. |
| `DistroInfoActivity.kt`        | Per-distro detail page (id, label, registry-resolved distro/arch, method, source URL, installed-at, state/failure, full rootfs path) + an async `du -sk` size readout (only for `READY`) + a red Delete button (Are-You-Sure dialog → [InstallationService.startUninstall] + opens [LogScreenActivity]). The view is rebuilt in `onResume` so a returning trip from a cancelled uninstall (FAILED) refreshes the State row instead of showing the stale READY pre-uninstall snapshot. Reached from the home screen's ⋮ menu. Run moved to the home ⋮ menu (`RunCommandDialog.kt`). |

The `MainActivity` home screen shows the open installation (distro +
arch only — size lives on [DistroInfoActivity] because `du -sk` over a
multi-GB rootfs costs seconds via `su`); ⋮ → Distro info opens the
info page, which hosts the Delete button. See notes/android.md "Home
screen".

The non-compositor activities (`MainActivity`, `InstallActivity`,
`DistroInfoActivity`, [LogScreenActivity]) extend `AppCompatActivity`
and share a small `me.phie.tawc.ui.Scaffold` helper that builds a
`MaterialToolbar` (with a back/up arrow on child screens) plus a
content column. The theme is `Theme.Material3.DayNight.NoActionBar`
with a warm orange `colorPrimary` (`@color/tawc_accent`) for primary
buttons and `@color/tawc_danger` (red) for destructive ones; both have
night-mode variants in `res/values-night/`. The compositor activity
keeps the device-default theme — it draws its own surface and never
inflates Material widgets.

## Install pipeline

The state machine guarantees `ArchInstaller.install()` only ever runs
against a `(no dir)` slot — no wipe, no overlay, no resume. Stages
reported as `InstallProgress` to the UI and per-line logged to logcat
(`tawc-install`).

The pipeline below describes the **tarball** bootstrap flavor (every
distro's supported flavor today). Stages 2-4 are flavor-specific; see
*Bootstrap flavors* below for the `packages` alternative, which
replaces them and rejoins at stage 5.

1. **(state write)** — `setState(INSTALLING)` runs first; from here on
   the entry exists on disk and any failure parks it in `FAILED`.
2. **DOWNLOADING** — `BootstrapCache.download(arch, url, format, …)`
   handles the fetch, the cache filename, and the mtime touch. The TTL
   counts from "last used" rather than original download.
   - x86_64: `https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst` (toplevel `root.x86_64/` is flattened post-extract because toybox `tar` doesn't `--strip-components`)
   - aarch64: `https://fl.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz` (no wrapper dir)
3. **VERIFYING** — `SignatureVerifier.verify(...)` checks the
   tarball against the distro's [BootstrapVerification] before the
   extractor sees it. Throws on mismatch / forged blob and the
   install lands in `FAILED` before any byte hits the rootfs. See
   *Bootstrap integrity* below for what each distro declares.
4. **EXTRACTING** — root-owned via toybox tar. Zstd is streamed in-
   process through a named pipe (`bootstrap-<cacheKey>.tar.fifo` in the
   cache dir): a writer thread feeds zstd-decompressed bytes into the
   FIFO while tar reads `tar -xf <fifo>` as a positional argument
   (not stdin — the shell pre-buffers past the script body and tar
   would see garbage). The plaintext tar never lands on disk, so peak
   install-time disk usage stays at `compressed tarball + rootfs`.
   The destination is a freshly-mkdir'd directory; the install gate
   guarantees nothing else lives there.
5. **CONFIGURING** — writes the same files the legacy create scripts do, then
   strips the bootstrap of trees the chroot has no use for:
   - `/etc/resolv.conf` (8.8.8.8)
   - `/etc/pacman.conf` (`DisableSandbox`, `#CheckSpace`, `IgnorePkg = linux …`,
     plus a `tawc-no-extract` block of `NoExtract` patterns — see *Slimming policy*
     below; **upstream `SigLevel` is left intact** — see *Bootstrap integrity* below)
   - `/etc/pacman.d/mirrorlist` (x86_64: geo-routed `geo.mirror.pkgbuild.com`; aarch64: HTTPS-first curated multi-mirror list — see *ALARM mirror failover* below)
   - **Nothing under `/etc/profile.d/`.** PATH/TMPDIR/HOME and the
     Wayland/GL/X11 env all come from [RootfsEnv.kt] via `/usr/bin/env
     -i KEY=VAL …` on every spawn ([ChrootMethod], [ProotMethod],
     [TawcrootMethod]). No on-disk env state inside the rootfs that
     the app version would have to keep rewriting; env changes pick
     up next entry without a reinstall. (Interactive-shell cosmetics
     are the one exception, and they don't go through profile.d
     either: `ShellDefaults.configureScript` overwrites
     `/root/.bashrc` + `/root/.bash_profile` once at configure time
     with stubs that source the app-managed
     `/usr/lib/tawc/bashrc` — see [ShellDefaultsInstallProvider].)
   - `TawcInstaller.installInto` lays down the APK-shipped per-rootfs
     files as **real files** inside the rootfs, not symlinks. Same
     generic mechanism handles any future "ship file X into every
     rootfs" need. Providers see the install's method key, because the
     three whole app-owned dirs — `/usr/lib/hybris/{*.so,gl-shims/,libhybris/}`
     from `LibhybrisInstallProvider`, `/usr/lib/gfxstream/` from
     [BridgeInstallProvider], `/usr/lib/mesa-zink/` from
     [MesaZinkInstallProvider] (all TAWC-owned namespaces;
     `/usr/local/lib/` stays free for the user's own installs) — are
     copied only under proot/chroot and RO-**bound** under tawcroot
     (see *Copy vs bind* below). `/usr/share/glvnd/egl_vendor.d/00_libhybris.json`
     is copied under every method: it has to coexist with the distro's
     `50_mesa.json`.
     [AndoInstallProvider] ships the ando client at
     `/usr/local/bin/ando` (notes/ando.md).
     [ShellDefaultsInstallProvider] ships `/usr/lib/tawc/bashrc`
     (colored short PS1, `ls`/`grep` color aliases) — sourced by the
     one-time `/root/.bashrc` stub that `ShellDefaults.configureScript`
     writes during CONFIGURING; the user can remove that source line
     to opt out per-rootfs while the app-owned file stays updatable.
      `LD_LIBRARY_PATH` (set by [RootfsEnv]) is
      `/usr/lib/hybris/gl-shims:/usr/lib/hybris`. The source tree at
      `<filesDir>/libhybris/` is extracted from
      `assets/libhybris/<abi>.tar` by `CompositorService.ensureLibhybris‐
      Extracted` (called from both compositor service start and here).
     The set of files written into the rootfs is recorded in
     `metadata.json` (`tawcInstalls` array, `tawcStamp` matching
     `CompositorService.currentExtractStamp`); `TawcInstaller` is
     also called from `TawcApplication.onCreate` so an APK upgrade
     wipes the old set and re-copies fresh on first app start. See
     *Copy vs bind* below for the design call.
   - `rm -rf` of bootstrap cruft: `/boot`, `/usr/lib/firmware`,
     `/usr/lib/modules`, `/var/cache/pacman/pkg`, and the docs/locale
     trees under `/usr/share`. ~1.8 GB of immediate reclaim before
     pacman runs (see *Slimming policy* below).
6. **PKG_KEYRING** — `pacman-key --init`, then `--populate archlinux` /
   `archlinuxarm` (no `|| true` — populate must succeed since pacman
   is now run with the upstream default `SigLevel`), then `pacman -Rdd
   --noconfirm --noscriptlet` on the bootstrap-cruft package set
   (kernel, firmware split packages, mkinitcpio, console utilities,
   audit, network/disk userland, vim/nano, …) — files for these were
   already deleted in step 5; this just cleans up the local pacman DB
   so future operations don't think they're installed. (Bind mounts
   are not a separate stage — they're set up inside the very `su`
   shell that runs each chroot command, see *Mount lifecycle* below.)
7. **PKG_INSTALL** — `pacman -Syyu --needed --noconfirm <basePackages>`
   followed by a package-cache clear (a `tawc` pacman hook drops the
   cached `.pkg.tar.*`). The base set is deliberately **minimal**:
   currently just `inetutils` (for `hostname`); see
   `ArchPacmanCommon.DEFAULT_BASE_PACKAGES`. **No toolchain or desktop
   stack is installed at bootstrap** — the guest ships without
   `base-devel`/`gcc`/`make`, wayland, gtk, weston, autotools, etc.;
   the user installs whatever they need afterwards with `pacman -S`.
   The combined `-Syu --needed <pkgs>` form (rather than `-Syu`
   followed by `-S --needed`) avoids a version-skew window where the
   in-chroot DB is synced at T1 but the second `pacman` call at T2
   fetches a tarball the mirror has already rolled past. Every package
   is signature-verified against the keyring populated above.
8. **(state write)** — `setState(READY)` only after every step above
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

1. **TTL pass** — canonical `bootstrap-<cacheKey>.tar.{zst,gz}` files are
   evicted when their mtime is older than 7 days. Reuse refreshes the
   clock because `BootstrapCache.download` touches the mtime on every
   successful fetch (whether the bytes were freshly downloaded or served
   from a size-match cache hit inside [Downloader]). `setLastModifiedTime`
   is the NIO variant — `File.setLastModified` silently no-ops on some
   Android FS/SDK combos.
2. **Unconditional pass** — `bootstrap-<cacheKey>.tar.fifo` ([Archive]'s
   zstd→tar streaming FIFO; legacy `*.tmp` from older builds is also
   matched for safety) and `bootstrap-<cacheKey>.tar.{zst,gz}.part`
   ([Downloader]'s in-flight suffix) are deleted regardless of mtime,
   since neither is ever meaningful across processes — they only exist
   inside one `Archive.extractAsRoot` / `Downloader.download` call's
   `try/finally`. Anything found at app start is by definition stranded
   by a crash. (`File.isFile` is false for FIFOs, so the sweep filters
   on `!isDirectory` rather than `isFile`.)

During an x86_64 install the cache dir holds the ~200 MB compressed
`.tar.zst` plus a zero-byte `.tar.fifo` inode — the decompressed bytes
flow through the kernel pipe buffer straight into `tar` and never
materialise on disk. The FIFO is removed in [Archive.extractAsRoot]'s
`finally`; if that delete fails (e.g. FS error) the next sweep evicts
the leftover unconditionally.

The sweep is best-effort. There's a tiny window where it could race a
concurrent install — sweep stat()s the canonical tarball, decides to
evict, unlinks just before [Archive.extractAsRoot] runs `require(tarball.exists())`.
The install fails to `FAILED`; recovery is uninstall + retry. Practical
exposure is small because installs are user-initiated via `am start` and
sweep fires once at process cold start.

## Uninstall pipeline (the one and only wipe)

`<distros>/<id>/` is mutated by exactly one path:
`RootfsCleaner.wipe(store, id)`, called from `Installer.uninstall()`.
One engine for every install method — the two facts it branches on
(kernel mounts to tear down, root-owned guests; both chroot-only) are
derived from the method key recorded in `metadata.json`, not from a
live `InstallationMethod`, so a slot recorded against a now-disabled
method still wipes with the right guards (missing/corrupt metadata
degrades to the rootless path, whose su retry still clears root-owned
trees). `InstallationMethod` deliberately has no `wipe()`.
`RootfsCleanerTripwireTest` (JVM unit test) fails the build if a
recursive-delete primitive shows up anywhere else in the app sources.
The engine does, in order:

1. **(state write)** — `setState(UNINSTALLING)` (in
   `Installer.uninstall`, before the engine runs).
2. **containment** — the engine computes `<store.baseDir>/<id>` itself
   from a validated id, so a caller-side path bug can't hand it a
   parent dir.
3. **kill guest processes** — loops over scan → SIGKILL → short wait
   until the scan comes back empty or the sweep limit is reached. The
   app-uid scanner matches rootfs links/maps; the chroot branch compares
   `/proc/<pid>/root` dev:inode values via `su` (so it matches whether
   the kernel reports `/data/data/<pkg>/...` or `/data/user/0/<pkg>/...`).
   Repeated sweeps catch fork races and daemons that respawn on signal —
   the canonical offender is the `gpg-agent --daemon` that
   `pacman-key --init` detaches; left alive it holds FDs into the rootfs
   and races the delete, which on Android 14 spins vold's FUSE accounting
   into a `vdc volume abort_fuse` storm. The hazard is
   method-independent (a `setsid`'d tawcroot guest from a previous app
   process is just as live), so every method gets the sweep; the argv
   match on the install path additionally catches supervisors (proot
   tracer) and host-side helpers.
4. **strict unmount (chroot only)** — `ChrootMounter.unmount` runs via
   `su -mm` (Magisk's mount-master mode) so `umount` actually affects
   the global mount table; refuses with a non-zero exit if any mount
   remains under the rootfs.
5. **uniform mount gate (every method)** — read `/proc/self/mounts`
   and refuse to delete while any mount entry sits under the install
   dir; re-checked before every `su` escalation in the delete step
   (pass 1 takes minutes, and a root `find` ignores DAC). This — not
   `find -xdev` — is the load-bearing guard: `-xdev`
   only refuses to cross *filesystems*, and a bind mount with a
   same-filesystem source (anything under `/data`) has the same
   `st_dev` and gets walked straight through. For rootless methods a
   hit means something external leaked a mount; refusing loudly is
   correct. Covered by `tests/integration/tests/uninstall_wipe.rs`.
6. **`find -xdev -depth -delete` (rootfs subtree)** — pass 1 deletes
   everything under `<installDir>/rootfs/`. Never `rm -rf`. Toybox
   `rm` has no `--one-file-system`; `-xdev` is a free extra fence on
   top of the gate (the historical `rm` walking through a live `/dev`
   bind crashed zygote and pegged vold). Root-owned trees (chroot)
   delete via `su` directly; app-uid trees get
   `chmod -R u+rwX` first (mode-0500 bootstrap dirs) and one `su`
   retry for root-owned droppings from interleaved debug `su` use.
   This is the long step (multi-GB tree); a cancel from the UI lands
   here.
7. **explicit metadata + container teardown** — pass 2 removes the
   `tawcroot/` state dir (hardlink-emulation store, sibling of
   `rootfs/`, created lazily by the tawcroot method —
   `notes/tawcroot/link-emulation.md`), the `ando/` broker dir,
   `metadata.json.tmp`, `metadata.json`, then `rmdir`s the empty
   `<installDir>/`, in that order. Split out from pass 1 so a cancel
   mid-wipe leaves the slot recognisable on the home screen for
   follow-up. See *Cancellation* under *State machine* above.

On success the directory (including `metadata.json`) is gone and the id
is back to `(no dir)`. On any failure, the directory is left as-is and
`setState(FAILED)` records the reason; the only recovery is to call
uninstall again.

## Rootfs /tmp sweep

`/tmp` in every install is a plain flash directory (no init in the
rootfs, so nothing clears it; a tmpfs is unreachable rootlessly), and
[RootfsEnv]'s `TMPDIR=/tmp` + `XDG_RUNTIME_DIR=/tmp` make it
accumulate runtime sockets and gpg-agent state across sessions and
reboots. `RootfsTmpSweeper` (run from `TawcApplication`'s startup
thread, unit-tested in `RootfsTmpSweeperTest`) age-sweeps it; the
design constraints — why age-based rather than a full clear, why at
app start, the mtime-only residual risk for long-lived sockets, what
is never touched — live in its class kdoc.

## Mount lifecycle

Mount-namespace handling is chroot-only (tawcroot and proot don't
need real mounts) — see [notes/chroot.md](chroot.md) "Mount
lifecycle". The relevant cross-method invariant is that mounts only
exist for the lifetime of a single `startInside` shell and never
leak into the global mount table; uninstall ([RootfsCleaner]) is
defensive about cleaning up anything that does leak. The install
path never touches mounts.

## /usr/share/tawc

The compositor puts its Wayland socket at
`<appData>/share/wayland-0` and Xwayland's `xtmp/.X11-unix/` listening
socket dir at `<appData>/share/xtmp/.X11-unix/`. Each install method
bind-mounts JUST `<appData>/share/` at `/usr/share/tawc` inside the
rootfs (asymmetric bind on tawcroot/proot, real bind-mount on chroot).
[RootfsEnv] exports `WAYLAND_DISPLAY=/usr/share/tawc/wayland-0` so
wayland clients see the canonical in-rootfs path. Xwayland's X11
sockets get an additional asymmetric bind from
`<appData>/share/xtmp/.X11-unix` to `/tmp/.X11-unix`, since libxcb
hardcodes `/tmp/.X11-unix/X<N>` for the `:N` form of `$DISPLAY`.

We deliberately **don't** bind the whole `<appData>` tree the way an
earlier version did — that exposed the libhybris asset extract
(`<filesDir>/libhybris/`), the proot scratch dir, the bootstrap cache
(`<cacheDir>/install/...`), and every other piece of app-private
state to in-rootfs writes. A package install scriptlet hitting any of
those would corrupt host state shared across rootfses (and tawcroot
runs as the actual app uid, so file permissions don't help — the
rootfs has the same uid as the bind src files). Limiting the bind to
`<appData>/share/` keeps the cross-rootfs writable surface scoped to
"things the compositor explicitly publishes for clients."

## Copy vs bind

App-shipped files inside the rootfs split two ways:

- **Whole app-owned dirs** — `/usr/lib/hybris`, `/usr/lib/mesa-zink`,
  `/usr/lib/gfxstream`, each a `<filesDir>` asset extract with no
  distro-managed siblings. Under tawcroot these are **RO binds**
  (`TawcrootMethod.assetBinds`, `-b <filesDir>/<name>:<guest dir>:ro`).
  Saves ~30 MB per arm64 install, drops the per-upgrade copy churn,
  and makes the guest unable to corrupt its own GPU stack (writes get
  `EROFS`). proot and chroot still get copies — proot has no RO bind
  primitive.
- **Everything else** — copied under every method. The glvnd vendor
  JSON (`/usr/share/glvnd/egl_vendor.d/00_libhybris.json`), the
  `/usr/lib/hybris-vulkan-only/libvulkan.so.1` symlink, the ando
  client, `/usr/lib/tawc/bashrc`.
- **Android's bionic linker config** — not app-shipped at all, and
  copied from the host per *spawn* rather than per install. See "The
  bionic linker config" below.

Two properties decide which side a file lands on:

1. **Bind = replacement, not merge.** A single-file bind into a
   distro-managed dir like `/usr/share/glvnd/egl_vendor.d/` doesn't
   show up in `readdir` at the parent (tawcroot's `getdents` is a
   passthrough; the kernel only sees the on-disk dir). And a whole-dir
   bind would shadow files the distro package (e.g. libglvnd) wants
   to ship there, breaking package install. So anything that has to
   coexist with distro siblings stays a copy.
2. **RO binds exist only in tawcroot** — `-b SRC:DST:ro`, enforced
   centrally at the translation layer
   (notes/tawcroot/path-translation.md §"Read-only binds"). Without
   RO, a rootfs could write through the bind into shared host state,
   which is why proot/chroot keep copies (per-rootfs-owned, so one
   rootfs clobbering them affects nothing else).

Mechanics of the split: `TawcInstallProvider.entries(context,
methodKey)` gets the install's `Installation.method`, and the three
whole-dir providers return their tree only when the key isn't
`tawcroot`. Every spawn surface resolves the method from install
metadata, so a per-method manifest can't go stale via a cross-method
entry. On the bind side, tawcroot opens each bind src at startup and
`exit(93)`s if one is missing, while `TawcInstaller`'s stamp fast-path
skips extraction — so `assetBinds` gates each bind on the matching
`CompositorService.ensure*Extracted` call, per spawn (an asset probe
plus a stamp read, noise next to forking a login shell). No asset for
the ABI, or a build-disabled backend → the bind is simply omitted.
Two accepted consequences: the extract's `.version` stamp becomes
guest-visible inside each bound dir, and guest writes into those dirs
now fail `EROFS`.

`TawcInstaller` records its copies in `Installation.tawcInstalls` (a
list of `{src, dest, type=COPY|LINK}`) tagged with the
`tawcStamp` from `CompositorService.currentExtractStamp(context)` —
which combines `versionCode + lastUpdateTime` so every `adb install
-r` triggers a refresh, not just real version bumps. On app start,
`TawcApplication` calls `TawcInstaller.installAll` which walks every
slot; mismatched stamp → wipe the previous manifest's dests, run all
providers, copy/link, persist. Empty manifest (no libhybris on
x86_64) still records the stamp so subsequent starts hit the no-op
fast path. That flow is also the migration path in both directions:
the APK that introduced the tawcroot binds bumps the stamp, so the
first refresh wipes the recorded whole-dir copies and records the
smaller manifest; reverting bumps it again and re-lays full copies.

Verified on arm64 hardware (OnePlus 9, Android 14): the whole libhybris
integration module passes with `/usr/lib/hybris` bound RO — including
the `/usr/lib/hybris-vulkan-only/libvulkan.so.1` manifest LINK, which
resolves *through* the bind — and `00_libhybris.json` still coexists
with the distro's `50_mesa.json`. Wiping `<filesDir>/{libhybris,
mesa-zink,mesa-gfxstream}` under a stopped app is self-healing: the
next spawn re-extracts from the `assetBinds` gate instead of hitting
tawcroot's `exit(93)`.

## The bionic linker config

libhybris's vendored Q linker reads Android's boot-generated linker
config to build the bionic namespaces vendor GPU blobs dlopen across.
`LinkerConfig.install(rootfs)` copies that one file
(`/linkerconfig/ld.config.txt`, ~220 KB) into the rootfs at
`/usr/lib/hybris-config/ld.config.txt` at the top of every spawn, for
all three methods (`TawcrootMethod.prepareSpawn` — which covers both
`startInside` and the in-app terminal's `ptyShellExec` —
`ProotMethod.startInside`, `ChrootMethod.startInside`). The fork's
`kLdGeneratedConfigFilePath` (`hybris/common/q/linker.cpp`, the only
`/linkerconfig` reference in the whole fork) points at that path;
the two must be changed together.

**Why not the bind it replaced.** `/linkerconfig` used to be one of
`LIBHYBRIS_BIND_DIRS`. Android's real `/linkerconfig` is
`linkerconfig_file` on tmpfs, and AOSP's `domain.te` grants every
domain `linkerconfig_file:file r_file_perms` plus
`linkerconfig_file:dir search` — but no `getattr` on the dir. So
anything that stats the entries of `/` inside the rootfs got

    ls: cannot access '/linkerconfig': Permission denied
    d??????????   ? ?    ?       ?            ? linkerconfig

with `avc: denied { getattr } … tclass=dir` in the kernel log. Files
*inside* read fine, which is why nothing but `ls` ever noticed: hybris
`file_exists()`es and reads `ld.config.txt` and never touches the dir.
It took this long to surface because only *interactive* `ls` hits it —
coreutils takes the plain listing from `getdents` `d_type`, and stats
each entry only when color is on **and** `LS_COLORS` is set (the
sticky / other-writable / orphan classes aren't derivable from
`d_type`). Every interactive shell sets `LS_COLORS`; the broker and
test paths are non-interactive. `linker_config::test_ls_root_stats_
every_entry` (tests/integration) now encodes the interactive shape.

**Why `/usr/lib/hybris-config/`, not `/usr/lib/hybris/`** with the rest
of the hybris runtime: under tawcroot that dir is an RO bind of
`<filesDir>/libhybris` (see *Copy vs bind*), and a bind replaces
whatever the rootfs has underneath, so a rootfs-side copy there would
be invisible to the guest. A sibling dir keeps one code path for all
three methods.

**Why copying is not staler than binding.** Android regenerates
`/linkerconfig` only during boot and it is static for the rest of
uptime; a reboot kills the app and every guest; the hybris linker
reads the file once per process start. The copy is skipped when the
destination already matches the source's size and mtime, so steady
state costs two stats, and it is written via a temp file + rename so a
concurrent spawn can never read a torn file.

**Unconditional** — no ABI or emulator gate, so the emulator exercises
the same path phones do (see notes/emulator.md). On a pre-Android-11
host there is no `/linkerconfig` to copy and hybris falls back to
`init_default_namespace_no_config` plus the configured
`--with-default-hybris-ld-library-path`, exactly as it did before. A
copy failure is one warning log and the same fallback, never a failed
spawn.

One accepted behavior change: bound, the file was read-only; copied,
in-rootfs root can rewrite it. Not a new boundary — the guest already
owns its rootfs and `LD_LIBRARY_PATH`, and the config only shapes that
guest's own bionic namespaces.

The other libhybris binds (`/apex /vendor /system /system_ext`) can't
move the same way: their paths are baked into `ld.config.txt`'s own
namespace search paths, into vendor blobs' absolute-path dlopens, and
into libhardware's compiled-in `/vendor/lib64/hw`. They also don't
share the bug — their labels are world-readable; only the
boot-generated `linkerconfig_file` tmpfs denies dir `getattr`.

Verify the guest is reading the copy with
`HYBRIS_LD_DEBUG=1 <gl client>`: the hybris linker prints
`[ Reading linker config "/usr/lib/hybris-config/ld.config.txt" ]`
to stderr (`HYBRIS_LD_DEBUG`, not `HYBRIS_LOGGING_LEVEL` — it gates
the linker's own `INFO` macro via `g_ld_debug_verbosity`).

Verified 2026-08-10 on the OnePlus 9 (Android 14, which ships no
`/system/etc/ld.config.<arch>.txt` at all — `/linkerconfig` really is
the only source there) and on the x86_64 emulator: `ls -l /` exits 0
with no `linkerconfig` row under all three methods, the copy matches
the host file byte-count, `weston-simple-egl` under `HYBRIS_LD_DEBUG=1`
prints the read of the new path, and the whole libhybris integration
module (EGL / Vulkan / GTK / Firefox / SuperTuxKart hardware-buffer
smokes) still passes.

## CLI command interface

Install and uninstall are driven from the host through the **dev exec
broker** ([notes/exec-broker.md](exec-broker.md)). The broker is the
single host→app channel for everything: arbitrary command exec
(`tawc-exec /bin/sh -c …`) and structured app actions
(`tawc-exec --foreground-app --action install …`). Activities are pure viewers —
opening any in-app screen never side-effects.

- **Trigger surface (debug builds only):** the broker's `ACTION` header
  protocol. `install` and `uninstall` action handlers live in
  `me.phie.tawc.install.InstallActions`, registered from
  [TawcApplication.onCreate]. They validate args, call
  [InstallationService.startInstall] / [InstallationService.startUninstall],
  open [LogScreenActivity] (best-effort; BAL refusal is a no-op for
  headless tests), then mirror the operation's progress + log flows
  back to the broker socket until the op terminates. Host disconnect
  (`Ctrl-C` of `tawc-exec`) sets `ActionContext.cancelFlag`, the
  handler calls `Operation.cancel()`, and the service runs its normal
  cancel-cleanup path.
- **No broadcast / receiver surface.** Earlier versions used
  `am start … InstallActivity --es autoStart true` and an exported
  activity to receive the trigger; that conflated "open the page" with
  "run the mutation" and hit Android's recents-card replay every time
  the activity was reopened. The broker's `ACTION` form is debug-only
  by design, so a release APK has no CLI surface at all and a user's
  phone can't be tricked into installing through any CLI path.
- **Production:** the only triggers are the in-app `Install` button
  ([InstallActivity], form-only after the user fills it in) and the
  `Delete` confirm dialog on [DistroInfoActivity]. Both call
  [InstallationService] companion-object helpers directly.

```sh
# Kick off a fresh install. Streams progress + log to your TTY and
# opens the in-app log screen. Ctrl-C cancels.
scripts/tawc-exec.sh --foreground-app --action install \
    --arg id=arch \
    --arg mirrorProxy=http://127.0.0.1:8080/proxy/

# Or pick a different distro / pass extras forwarded as broker --arg
# flags:
scripts/tawc-exec.sh --foreground-app --action install \
    --arg id=arch \
    --arg method=proot \
    --arg distro=archlinuxarm \
    --arg mirrorProxy=http://127.0.0.1:8080/proxy/

# Debian sid via the (debug-only) packages bootstrap flavor — real
# debootstrap on-device against the signed archive; see "Bootstrap
# flavors":
scripts/tawc-exec.sh --foreground-app --action install \
    --arg id=sidpkg \
    --arg distro=debian-sid \
    --arg bootstrap=packages \
    --arg mirrorProxy=http://127.0.0.1:8080/proxy/

# Tear down. Same channel; the uninstall is also the only way to
# clear a `FAILED` install before trying again.
scripts/tawc-exec.sh --foreground-app --action uninstall \
    --arg id=arch
```

`--foreground-app` ensures `MainActivity` is foreground first (the broker
runs as a background thread inside the app process; Android-14
`mAllowStartForeground` rules block `startForegroundService` from a
fully-backgrounded app). `tawc-exec` already brings the app cold-up
when the process is dead; the foreground precondition matters only
when the app is alive but backgrounded.

Attaching to a running op from another shell:

```sh
# Generic viewer; pure viewer, no side-effects on launch.
adb shell am start \
    -n me.phie.tawc/me.phie.tawc.ops.LogScreenActivity \
    --es operationId 'install:arch'
```

Listing existing installations from the host: read the metadata
directly via the broker (no su / run-as needed).

```sh
. scripts/lib/select-device.sh
scripts/tawc-exec.sh /system/bin/sh -c 'ls /data/data/me.phie.tawc/distros/'
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
`scripts/emulator.sh start` after boot — see [emulator.md](emulator.md)
for details. Re-run the script after `adb install -r ...` to refresh
them. To do it by hand:

    uid=$(adb shell pm list packages -U me.phie.tawc | awk -F: '{print $3}')
    adb shell "su -c 'magisk --sqlite \"INSERT OR REPLACE INTO policies (uid,policy,until,logging,notification) VALUES($uid,2,0,1,0);\"'"
    adb shell pm grant me.phie.tawc android.permission.POST_NOTIFICATIONS

## Bootstrap flavors

A distro can implement more than one way to assemble its rootfs — a
**bootstrap flavor**. `DistroBootstrap` is a sealed interface:

- `TarballBootstrap` — today's path for every distro: download one
  archive, verify ([Bootstrap integrity](#bootstrap-integrity-load-bearing--do-not-weaken)),
  extract.
- `PackageBootstrap` — assemble from signed repo metadata + individual
  packages. Implemented for Debian sid only, via **real debootstrap
  run on-device** (`me.phie.tawc.install.pkgbootstrap.
  PackageBootstrapInstaller`). Its value is trust + freshness (the
  chain roots in an APK-shipped keyring and `Valid-Until` gives replay
  protection no tarball has); it is *slower* than the tarball.

`Distro.declaredBootstrapFlavors` maps `BootstrapFlavor` (`TARBALL`,
`PACKAGES`) to descriptors — everything a distro implements.
`Distro.bootstrapFlavors` is that filtered through
`EnabledBootstrapFlavors`, the **build-time gate** (BuildConfig
`BOOTSTRAP_PACKAGES_ENABLED`, `-PtawcBootstrapPackages=true|false`,
same shape as the `tawcMethods` gate): debug ships tarball+packages,
release ships tarball only and doesn't even pack the debootstrap
asset. Everything user-facing goes through the filtered map, so a
production APK has no flavor option at all — no radio row, no
selectable alternative.

`Distro.supportedFlavor` names the release-supported flavor (tarball
everywhere — the packages flavor is debug-only until it earns
promotion) and is also the default when no flavor is named, so every
install is a tarball install unless a dev picks otherwise. Selection:
broker `--arg bootstrap=packages`, or the dev-only radio row on the
install form (rendered only when the build ships >1 flavor for the
selected distro). `InstallationService` rejects flavors the distro
doesn't implement, flavors this build doesn't ship, and
`packages`×non-tawcroot method combinations (the flavor runs its
resolve/extract stages as a guest of the tawcroot method machinery).
The installed flavor is persisted as `Installation.bootstrapFlavor`
(missing field in old records = `tarball`; the value is written
explicitly on every save so old records converge).

### Packages flavor mechanics (Debian family)

Division of labour: **Kotlin owns trust, transport, environment,
progress; debootstrap owns all dpkg/apt logic** (dependency
resolution, unpack order, the dpkg database, maintainer scripts).
Nothing in Kotlin parses a Depends line or fabricates dpkg state.

1. **Trust phase** (Kotlin, before any downloaded code runs): fetch
   `dists/<suite>/InRelease`, verify the clearsign against the shipped
   `res/raw/debian_archive_keyring.asc` (`Clearsign.kt` — RFC 4880
   cleartext canonicalization, NOT the detached-sig path; vectors in
   `app/src/test/resources/debian-archive/`), enforce `Valid-Until`
   (reject, small fixed skew — this is the replay defence), fetch
   `Packages.xz` **by hash** out of the verified body (sid republishes
   several times a day; by-hash is the only race-free fetch), verify
   its digest, stream-parse to `name -> (Filename, Size, SHA256)`.
2. **Workspace guest**: `distros/<id>/bootstrap-work/` is laid out as
   a minimal rootfs — busybox-static plus libc6/libcrypt1/perl-base
   (debootstrap's pkgdetails needs perl; upstream has no shell
   fallback), all extracted in Kotlin from index-verified debs; shims
   for the exact paths `startInside` hardcodes (`/usr/bin/env` grows a
   `-C` shim because busybox env lacks it, `/bin/bash` aliases busybox
   ash); the vendored debootstrap tree from the APK asset
   (`packDebootstrap` Gradle task, pin in `deps/deps.list`); and a
   `file://` mirror of verified files. The method layer enters it with
   the same `runInside` every install step uses — no new execution
   machinery. Every guest command starts with an applet-farm preamble
   (`busybox --list` → symlinks) so ar/tar/wget/sha256sum exist as
   PATH commands.
3. **Resolve**: `debootstrap --print-debs` in the workspace guest
   (resolution needs only the index; verified to work against a
   `file://` mirror with an empty pool). Kotlin maps the ~76 names
   through the parsed index and downloads each deb into a
   content-addressed pool cache (`bootstrap-pkgs-<cacheKey>/<sha256>.deb`
   under `BootstrapCache`), SHA-256-verified against the index before
   use, then copies them into the workspace mirror. A pool 404 means
   the archive rotated mid-install: one clean re-resolve from a fresh
   `InRelease`, never mixing files from two index generations.
4. **Stage 1** `debootstrap --foreign --extractor=ar` in the workspace
   guest extracts into `bootstrap-work/rootfs/`, which is then moved
   to the final `rootfs/`. **Stage 2** `debootstrap --second-stage`
   runs inside the real rootfs (`runInside`, freshly-unpacked
   dpkg/bash; `/proc` from the per-spawn binds). The workspace is
   deleted on success; on failure `RootfsCleaner` reaps it at
   uninstall (explicit `bootstrap-work` entry in its pass 2).
5. Rejoin the flavor-agnostic tail: `configure → TawcInstaller →
   initPackageManager → installBasePackages`.

debootstrap runs with `--no-check-sig`. **That flag weakens nothing**:
verification already happened in Kotlin upstream of it — the local
mirror's `InRelease` was clearsign-verified against the shipped
keyring, the index hash-verified against the InRelease, and every pool
deb hash-verified against the index. debootstrap only ever reads that
pre-verified local mirror; there is no network fetch it could skip a
check on. Do not "fix" the flag away (it would just make debootstrap
re-verify files Kotlin already verified, with a gpgv it doesn't have),
and do not weaken the Kotlin phase because "debootstrap will check" —
it won't. See the hard rules below.

busybox quirk that cost a debugging session: Debian's busybox config
enables the standalone shell, so ash re-execs `/proc/self/exe` for
nearly every applet. tawcroot now substitutes the stashed guest exe
path on exec of `/proc/self/exe` (`exec_handler.c`), mirroring its
readlink synthesis — without that, every non-NOFORK applet died with
loader exit 68.

## Bootstrap integrity (load-bearing — do not weaken)

**Every byte we lay down inside the chroot's rootfs is run as root
inside that chroot, so the bootstrap-tarball trust path is the
single most security-relevant part of the install pipeline.** It is
specifically designed so that "there's a bug, just turn off the
check to keep going" is **never** the right move. Anyone touching
this code path is expected to keep the integrity barrier intact and
add to it, not weaken it.

Hard rules:

- **Every [DistroBootstrap] that reaches the verify stage must carry
  a concrete [BootstrapVerification].** There is deliberately no
  "skip verification" variant. Distros whose digest is only known at
  install time declare `BootstrapVerification.ResolvedAtInstallTime`
  in their static `bootstrap` field, and their `resolveBootstrap()`
  substitutes the real policy; the placeholder **fails closed** — if
  it ever reaches `SignatureVerifier.verify` (an override dropped or
  forgotten), the install throws instead of proceeding unverified.
  Before adding a new distro, find an upstream signature
  (`<tarball>.sig` is the convention), or at minimum a server-side
  digest to resolve at install time. A distro that genuinely cannot
  be verified would need a new, loudly-named variant added back —
  treat that as a security review, not a convenience.
- **No MD5 in the install path.** Both Arch bootstraps verify against
  a PGP key shipped in the APK; everything else is SHA-256. ALARM was
  the last MD5 holdout and moved to PGP in 2026-08 (see *ALARM
  bootstrap: from cross-mirror MD5 to PGP* below). Do not reintroduce
  a checksum-only or MD5 tier — the sealed class no longer has one.
- **`SigLevel = Never` is gone from `pacman.conf` and stays gone.**
  Pacman runs with the upstream default
  (`Required DatabaseOptional`); `pacman-key --populate <keyring>`
  is required to succeed; `pacman -Syu` verifies every package
  against the populated keyring. If `--populate` fails on a new
  Android version, fix it — don't paper over with `Never`.
- **`network_security_config.xml` is HTTPS-only.** No `cleartext`
  carve-outs. Both bootstrap fetches now go over TLS; if a future
  source is HTTP-only, mirror it via something with a valid cert
  rather than re-opening cleartext for the whole app.
- **The packages flavor's trust boundary is Kotlin, before any
  downloaded code runs.** Everything debootstrap ever reads — the
  `InRelease`, the package index, every deb, even the busybox/perl the
  workspace itself executes — is hash-verified against the
  shipped-key-verified `InRelease` first, in Kotlin
  (`PackageBootstrapInstaller`). debootstrap's `--no-check-sig` is
  therefore not a verification bypass and must never be "compensated
  for" by softening the Kotlin phase; conversely, no step of the
  Kotlin phase (clearsign verify, `Valid-Until` reject, by-hash index
  fetch, per-deb SHA-256) may be dropped on the theory that
  debootstrap re-checks — it doesn't. **No runtime key fetch, ever**:
  the Debian archive keyring ships in the APK
  (`res/raw/debian_archive_keyring.asc`); if the archive rotates all
  its keys, installs fail until a new APK ships — same intended
  failure mode as the Arch PGP paths.
- **Removing or downgrading these checks is a security regression
  even if it makes a CI failure go away.** Code review should treat
  changes that delete a `BootstrapVerification`, drop a `.sig` URL,
  re-add `SigLevel = Never`, or re-introduce a cleartext carve-out
  the same as deleting the password check on a login screen.

### What's verified today

| Bootstrap | Verification | Where |
| --- | --- | --- |
| Arch x86_64 (`geo.mirror.pkgbuild.com`, HTTPS) | PGP detached signature `<tarball>.sig`, against Pierre Schmitz's Arch developer key (`3E80 CA1A 8B89 F69C BA57 D98A 76A5 EF90 5444 9A5C`) shipped at `res/raw/arch_signing_key.asc` | `BootstrapVerification.Pgp` in `ArchLinuxX86_64.kt` |
| ALARM aarch64 (`fl.us.mirror.archlinuxarm.org`, HTTPS) | PGP detached signature `<tarball>.sig`, against the Arch Linux ARM Build System key (`68B3 537F 39A3 13B3 E574 D067 7719 3F15 2BDB E6A6`) shipped at `res/raw/archlinuxarm_signing_key.asc` — the same key `pacman-key --populate archlinuxarm` pins for packages | `BootstrapVerification.Pgp` in `ArchLinuxArm.kt` |
| Manjaro ARM aarch64 (`github.com/manjaro-arm/rootfs/releases`, HTTPS) | SHA-256 from the GitHub Releases REST API: `api.github.com/repos/manjaro-arm/rootfs/releases/latest` returns the asset's server-computed `digest: sha256:<hex>`. We fetch that JSON over HTTPS in `ManjaroArm.resolveBootstrap`, then verify the downloaded tarball's SHA-256 matches before extract | `BootstrapVerification.Sha256` (Manjaro path) in `ManjaroArm.kt` |
| Void Linux x86_64 / aarch64 glibc (`repo-default.voidlinux.org/live/current/`, HTTPS) | SHA-256 from upstream `sha256sum.txt`, **and** the minisign (Ed25519) signature `sha256sum.sig` over that manifest, checked against the per-image-date Void release key — bundled in `VoidReleaseKeys` from void-packages on GitHub, i.e. a second origin. `VoidSha256Resolver.resolveLatest` verifies the signature before trusting any digest from the manifest, then the tarball's SHA-256 is checked before extract | `Minisign.kt` + `VoidSha256Resolver.kt`, yielding `BootstrapVerification.Sha256` in `VoidLinux{X86_64,Aarch64}.kt` |
| Debian sid packages flavor (`deb.debian.org`, debug-only) | Clearsigned `dists/sid/InRelease` verified against the Debian Archive Automatic Signing Keys 12/bookworm + 13/trixie shipped at `res/raw/debian_archive_keyring.asc`; `Valid-Until` enforced (replay defence); `Packages.xz` fetched by-hash and digest-checked against the verified body; every `.deb` SHA-256-checked against the index before debootstrap sees it | `Clearsign.kt` / `DebianRelease.kt` / `PackageBootstrapInstaller.kt`; see *Bootstrap flavors* |
| Debian sid x86_64 / aarch64 (`raw.githubusercontent.com/debuerreotype/docker-debian-artifacts`, HTTPS) | SHA-256 from the official debuerreotype Docker artifact OCI manifest. We resolve the `dist-amd64` / `dist-arm64v8` branch tip to a commit SHA via the GitHub API, fetch `image-manifest.json` at that pinned commit, read the single gzip layer digest, then verify `rootfs.tar.gz` (fetched from the same commit) against it before extract. Commit-pinning closes the mutable-branch race; the trust profile is still a single HTTPS origin (digest and tarball from the same repo) plus OCI digest sanity check | `BootstrapVerification.Sha256` (Debian path) in `DebianSid.kt` / `DebianDockerResolver.kt` |
| In-chroot pacman packages | Default `SigLevel = Required DatabaseOptional`, against the keyring populated by `pacman-key --populate archlinux` / `archlinuxarm` / `archlinuxarm manjaro manjaro-arm` | `ArchPacmanCommon.kt` (the `Never` line was removed, `--populate` is no longer `\|\| true`'d) |

### Same-origin SHA-256 bootstraps (Manjaro / Debian): trust profile

Manjaro ARM sits below both Arch PGP paths. Upstream doesn't sign the
tarball at all, but it is hosted on GitHub Releases and the
GitHub API exposes a server-side SHA-256 of every release artifact in
the asset's `digest` field. We fetch that JSON over HTTPS to
`api.github.com` and use the digest to verify the tarball before
extract.

What this catches:

- Passive MITM (TLS handles this).
- Mid-download corruption / bit-flips (SHA-256 mismatch on the
  finished file).
- A redirect to a different host serving a different tarball (the
  digest-from-API and the actual-bytes don't match).

What it does **not** catch:

- A compromise of the manjaro-arm GitHub org pushing a malicious
  artifact: the API would return that artifact's matching digest, so
  our check would still pass. Same threat as any HTTPS-distributed
  artifact without a separate offline-key signature chain.

Weaker than `Pgp`: this rests on a single HTTPS endpoint's trust,
whereas a `.sig` is bound to a key in the APK and survives a
compromised origin. When upstream Manjaro starts shipping a detached
PGP signature we should switch over — ALARM did exactly that in
2026-08, so it's worth re-checking periodically rather than assuming
upstream never will.

The same analysis applies to Debian sid, with one aggravating detail:
its digest and tarball both come from the debuerreotype GitHub repo,
so it is a corruption/host-swap check, not an integrity barrier
against a compromised origin or a mis-issued cert. The branch tip is
commit-pinned via the GitHub API before either fetch (closes the
mutable-branch / force-push race), but Debian publishes no
out-of-band signature for these artifacts, so the origin itself
remains the trust root. Post-extract, apt verifies every package
against the debian-archive-keyring shipped in the bootstrap, so the
exposure is the bootstrap alone.

### Void: signed checksum manifest (minisign, second origin)

Void used to be in the same-origin bucket — `sha256sum.txt` and the
ROOTFS tarball both live on `repo-default.voidlinux.org/live/current/`,
so a compromised origin or a mis-issued cert served a matching pair.
It isn't any more: upstream publishes `sha256sum.sig` next to the
manifest, and we check it.

Mechanics (`Minisign.kt`, `VoidSha256Resolver.kt`, `VoidReleaseKeys.kt`):

- The format is **minisign**, not OpenBSD signify — the pubkey
  comments literally read "minisign public key <id>", and the current
  signature uses minisign's prehashed algorithm `ED` (Ed25519 over
  BLAKE2b-512 of the file). `Minisign` accepts plain `Ed` too, and
  verifies the trusted-comment global signature when present. The
  crypto is BouncyCastle's `Ed25519Signer` + `Blake2bDigest`; we do
  not shell out and do not depend on a `minisign` binary.
- Keys are **per image date**: the signature's trusted comment says
  "This key is only valid for images with date YYYYMMDD", and the
  matching pubkey is published as
  `srcpkgs/void-release-keys/files/void-release-<date>.pub` in
  void-linux/void-packages on **GitHub**. That's the point — the key
  origin is independent of voidlinux.org, so forging a bootstrap now
  requires compromising both — weaker than the Arch/ALARM PGP tier,
  where the key ships in the APK, but well above a single origin.
- All keys known at build time are bundled verbatim in
  `VoidReleaseKeys`, so the common case needs no extra fetch and no
  runtime trust in GitHub at all. If `live/current/` moves to an image
  date newer than the APK, we fetch that one key from
  `raw.githubusercontent.com` rather than bricking installs.
- **Fails closed** end to end: unfetchable `.sig`, malformed
  signature, key-id mismatch, bad signature, or an image date with no
  bundled key *and* no fetchable key all throw out of
  `resolveBootstrap`, before anything is downloaded.
- The image date used to pick the key comes from the manifest line we
  are about to trust, i.e. from unverified bytes. That's fine: it only
  selects *which* genuine upstream key we verify against, and every
  key at that URL is a real Void key. An attacker steering the choice
  still has to produce a signature under a key they don't have.
- Order of operations matters and is deliberate: parse → resolve key →
  verify signature over the raw manifest bytes → only then use the
  parsed filename/digest. The manifest bytes are kept undecoded for
  hashing so a charset round-trip can't perturb them.

Residual risk: **rollback**. An attacker with origin control can serve
an older, genuinely-signed Void release (manifest + sig + tarball all
consistent) instead of the current one. We accept any date with an
obtainable key, so this passes. It's a much weaker attack than
arbitrary content — the user gets a real, older Void rootfs, which
`xbps-install -Suy` then updates — and pinning a floor date would
break installs whenever a key lands in void-packages before or after
the image goes live. Not worth the machinery; recorded here so nobody
mistakes it for an oversight.

The upstream vectors (the real 20250202 `sha256sum.txt` +
`sha256sum.sig`) are checked into `app/src/test/resources/minisign/`
and exercised by `MinisignTest`, which also verifies every bundled key
parses — a typo in `VoidReleaseKeys` fails the unit tests rather than
an install.

### ALARM bootstrap: from cross-mirror MD5 to PGP

Until 2026-08 ALARM was the weak bootstrap: upstream published only an
`.md5` sidecar, so the trust root was mirror infrastructure (two
independently-operated HTTPS mirrors had to agree on the digest) rather
than a key we ship. That was also the last MD5 anywhere in the install
path.

Upstream now publishes a detached OpenPGP signature next to the
tarball. Verified 2026-08-10: `<tarball>.sig` is a binary detached
signature, RSA/SHA-512, issuer fingerprint
`68B3537F39A313B3E574D06777193F152BDBE6A6` — the
`Arch Linux ARM Build System <builder@archlinuxarm.org>` key
(rsa4096, created 2014-01-18, no expiry). Its signature timestamp
tracks the tarball's `Last-Modified`, so it is re-signed per rebuild
rather than being a stale one-off, and `ca.us` / `de3` serve it too, so
it comes from the publish pipeline and not one mirror's local addition.

The key was taken from two independent origins and both yielded that
fingerprint, byte-identical after a minimal export:

1. `keyserver.ubuntu.com`.
2. Upstream's own `archlinuxarm-keyring` package
   (`usr/share/pacman/keyrings/archlinuxarm.gpg`), whose
   `archlinuxarm-trusted` pins exactly that fingerprint and whose
   revocation list is empty.

It ships armored at `res/raw/archlinuxarm_signing_key.asc`. This is the
same key `pacman-key --populate archlinuxarm` trusts for packages once
the rootfs is up, so bootstrap and package trust now share one root.

Two consequences worth knowing:

- If ALARM rotates the build-system key, installs fail with "not
  present in shipped keyring" until a new APK ships. That is the
  intended failure mode and matches Arch x86_64 — do **not** soften it
  with a runtime key fetch, which hands the trust decision back to the
  network.
- The old `<tarball>.md5.verified` sidecar is gone, and with it the
  offline fallback that let a cached-but-unreachable-mirror ALARM
  install proceed on a previously verified digest. Verifying now always
  needs the 566-byte `.sig` fetch, so a fully offline re-install fails
  at the verify stage. Arch x86_64 has always behaved this way. Stale
  sidecars left by older installs are swept by
  `BootstrapCache.sweepStale` (its transient-name regex matches them).

### Verifier code

- `SignatureVerifier.kt` — sealed `BootstrapVerification`
  (`ResolvedAtInstallTime`, `Pgp`, `Sha256`) and
  `verify(context, tarball, verification)`.
  Called from `Installer.install` between [BootstrapCache.download]
  and [Archive.extractAsRoot]. Throws `IOException` on any failure
  and the install is parked in `FAILED` before any byte hits the
  rootfs.
- BouncyCastle (`bcpg-jdk18on`, `bcprov-jdk18on`) provides the OpenPGP
  layer; the duplicate `META-INF/versions/9/OSGI-INF/MANIFEST.MF`
  across the three jars is resolved with a `pickFirsts` rule in
  `app/build.gradle.kts`.
- `ShippedPgpKeysTest` parses the actual `res/raw` key files off disk
  and asserts their fingerprints, that every `Pgp.keyResource` a distro
  declares is registered in `SignatureVerifier.rawKeyResourceId` and
  present as a file, and that a signature whose issuer is absent from
  the declared ring throws. A key swapped, truncated, or shipped
  without its `when` entry fails the unit tests rather than a user's
  install.

## Slimming policy

The chroot is a Wayland-userland-only environment — no kernel runs
inside, no init system manages services, no user logs in
interactively. Anything in the bootstrap that exists to boot a real
Linux install or make a package manageable from a local console is
dead weight on Android's NAND. The slimming logic lives in
`distro/arch/ArchPacmanCommon.kt` (so both Arch flavours pay the same
cost) and runs in three pieces:

1. **Post-extract `rm -rf`** during `CONFIGURING` (`POST_EXTRACT_PURGE_PATHS`
   for whole directories, `POST_EXTRACT_PURGE_GLOBS` for shell-glob
   file patterns). Whole-tree reclaims: `/boot`, `/usr/lib/firmware`,
   `/usr/lib/modules`, `/var/cache/pacman/pkg`, the
   `man`/`info`/`doc`/`gtk-doc`/`help`/`locale`/`i18n` trees under
   `/usr/share`, and `/usr/share/gir-1.0` (GIR XML sources — runtime
   GI consumers read `.typelib` from `/usr/lib/girepository-1.0/`).
   File-glob reclaims: the multi-language gcc runtime libraries
   (`libgo`, `libgphobos`, `libgfortran`, `libgdruntime`, `libobjc`,
   `libgnat-*`, `libgnarl-*`) and the sanitizer runtimes
   (`libasan`/`libtsan`/`libubsan`/`liblsan`/`libhwasan`) — both `.so`
   and `.a` — plus a handful of large unused static archives
   (`libstdc++.a`, `libsupc++.a`, `libisl.a`, `libgio-2.0.a`,
   `libgprofng.a`, `libc.a`). The exact reclaim sizes live next to the
   constants in code; this note intentionally doesn't repeat them so
   the two can't drift.

2. **`NoExtract` patterns** appended to `/etc/pacman.conf` during
   `CONFIGURING` (`NO_EXTRACT_PATTERNS`). These keep future `pacman -S`
   calls (base-package install, test-deps install, libhybris build deps,
   …) from re-creating the trees we just deleted. Without them, every
   `pacman -S` would put back the docs/locales of every package it
   touches and the rootfs would slowly grow back to its bootstrap size.
   Patterns match file paths inside the package tarball (no leading
   slash); multiple `NoExtract = ` lines accumulate.

   Pacman's NoExtract uses fnmatch(3) without `FNM_PATHNAME`, so `*`
   matches `/`. A blanket `usr/lib/*.a` would therefore *also* block
   `usr/lib/gcc/<triple>/<ver>/libgcc.a` and break every `gcc` invocation
   with `cannot find -lgcc`. The static-archive entries are listed by
   exact filename for that reason.

3. **`pacman -Rdd --noconfirm --noscriptlet`** during `PKG_KEYRING`
   (`SHARED_CRUFT_PACKAGES` plus the per-arch kernel package). The
   files are already gone from step 1; this just cleans up the local
   pacman DB so future operations don't think the cruft is still
   installed. `-Rdd` skips dep checks (so `linux` doesn't drag the
   `base` metapackage with it), `--noscriptlet` skips the kernel's
   pre-remove hook (which tries to regenerate an initramfs that won't
   run anyway).

Plus `pacman -Scc --noconfirm` after every install transaction
(`installBasePackages`, integration-test package setup) to drop the
fetched `.pkg.tar.xz` files — we never reinstall in place, so caching
costs only disk.

Adding a new `NoExtract` pattern or `SHARED_CRUFT_PACKAGES` entry is
the right knob for general cruft-trimming. Adding the kernel package
name (which varies by arch) goes in the per-arch `ARCH_SPECIFIC_CRUFT`
list in `ArchLinuxX86_64.kt` / `ArchLinuxArm.kt`.

Two consequences of where each piece lives:

- **`configure()` is idempotent only by marker.** The `tawc-no-extract`
  block is appended once and rerunning `configure` short-circuits on
  the marker. Editing `NO_EXTRACT_PATTERNS` therefore does **not**
  converge an already-installed rootfs — uninstall + install is the
  only path. (Acceptable because the rootfs is cheap to rebuild and
  the gate refuses install on a non-empty slot, so there's no
  in-place "policy update" to worry about.)
- **Integration-test package setup upgrades only when packages are
  missing.** `scripts/run-integration-tests.sh` first queries the
  package manager for the required runtime set. If anything is absent,
  Arch/Manjaro use `pacman -Syu --needed <pkgs>` so the local DB is
  fresh in the same transaction the new packages come down — closes
  the version-skew window that bricked the install pipeline at one
  point.

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

The list lives in `ArchLinuxArm.MIRROR_LIST` and is now HTTPS-first:
`fl.us.` and `ca.us.` (the two ALARM mirrors with valid certs covering
their own hostnames) come first, then the geo-redirector and a few
regional HTTP mirrors as fallback. Pacman package signatures are
verified against the populated keyring (no more `SigLevel = Never`),
so HTTP for the fallback mirrors is defense-in-depth missing rather
than a hole. See *Bootstrap integrity* for the load-bearing rules.

## Android 14 FGS rules and the broker action path

`startForegroundService()` from a background broadcast receiver is
blocked on Android 14+ (`mAllowStartForeground=false`), and Background
Activity Launches from the same context are also blocked
(`BAL_BLOCK`). The broker action path sidesteps this by requiring the
app to be in the foreground when the action fires — `MainActivity`
(launched by the wrapper script) puts the process in the foreground
state, then the broker's accept thread is allowed to call
`startForegroundService` for [InstallationService].

`--foreground-app` does this before the action header is sent:

```sh
adb shell am start -n me.phie.tawc/.MainActivity   # bring foreground
scripts/tawc-exec.sh --action install --arg id=...  # then trigger
```

`tawc-exec` also auto-launches `MainActivity` when the app process
is dead; `--foreground-app` covers the case where the app is alive but
backgrounded. If the broker action runs against a truly backgrounded
app the FGS launch will throw `ForegroundServiceStartNotAllowedException`
and the action returns a clear error.

If a fully-headless INSTALL/UNINSTALL trigger is ever needed (e.g.
without an `am start` step), the right escape hatch is WorkManager
`OneTimeWorkRequest.setExpedited()` with a foreground info — that's
explicitly carved out of the FGS restriction. Not implemented because
the wrapper-script pattern works fine for every current caller and
keeps the trigger surface debug-only.

## Future directions (designed-for, not implemented)

- **Other distros** — add a new file in `distro/<family>/`
  implementing the `Distro` interface and append it to
  `DistroRegistry.all`. The generic `Installer` and the activities
  pick it up automatically. The `(distro, arch)` lookup in
  [DistroRegistry.forInstallation] is what dispatches to the right
  one at runtime.
- (Done) **proot (rootless) installations** — `ProotMethod.kt` ships
  alongside `ChrootMounter` / `ChrootRunner` and is routed via the
  `Installation.method` strategy field. **Superseded by tawcroot for
  release builds; see "Install methods" above.**
- **Multiple installs** — vary the id passed in via the `--es id …`
  extra. The store/service/activity already carry the id end-to-end.
  The `(distro, arch)` registry lookup means two installations of
  different Arch flavours on the same device (e.g. an x86_64
  emulator running both ALARM-via-qemu and Arch x86) is also
  unblocked once the install activity grows a "pick distro" UI.

## Upgrade policy

The chroot is treated as **immutable across app updates**. Once
`Distro.configure` has run during install, the rootfs carries that
version's policy (mirrorlist, `pacman.conf`, `NoExtract` patterns,
`POST_EXTRACT_PURGE_PATHS`, …) for the rest of its life. App updates
do *not* re-run `configure`, do *not* edit anything inside the rootfs,
and do *not* touch `metadata.json` other than the state-machine
transitions described above.

Why this is the right default:

- **No risk of an app update breaking a working chroot.** Users will
  have hand-installed packages, Firefox profiles, dotfiles, and
  whatever else inside the rootfs — none of that should be at the
  mercy of `configure()` changes shipped in a routine app update.
- **Most config changes aren't critical for keeping the chroot
  functional.** Slimming policy, mirror failover lists, and similar
  knobs improve fresh installs but the existing rootfs runs fine
  without them.
- **Anything truly critical to keeping a chroot working is rebuilt on
  every entry** by the install method's `startInside` (mount layout
  via `ChrootMounter`, env vars via `RootfsEnv`'s `env -i` wrapper).
  Those *do* pick up app updates without a reinstall.
- **Users who want the new config can uninstall + reinstall**, which
  is the only path the install gate allows for switching to a fresh
  rootfs anyway. They keep their old install until they decide.
- **Users who prefer to manually port a config change** can edit
  files inside the rootfs themselves; nothing the app does will
  fight them.

If a future app version genuinely needs to converge an existing
rootfs (e.g. a security-relevant fix to `pacman.conf`), add an
**idempotent `reconfigure` step** at that point — keyed on
[Installation.installedAtAppVersionCode] so it only touches rootfses
older than the version that introduced the fix, and explicit about
what it changes. Don't make `configure()` itself idempotent
speculatively; the marker-based short-circuit it has today is fine
for the install-only use case and a real reconfigure path will want
to think more carefully about what's safe to overwrite.

### Frozen identifiers (renaming breaks existing installs)

These strings are persisted in user-owned state (metadata.json,
inside rootfses, or Android's launcher pin store) and must be
treated as a frozen wire format once a release ships — renaming
one strands or breaks every existing install, and no app-side
migration can fully repair it:

- **Method keys** `"chroot"` / `"proot"` / `"tawcroot"`
  (`metadata.method`, resolved by [InstallationMethod.forKey]).
  Worst case of the lot: an unresolvable key makes launch silently
  no-op and makes uninstall fall back to `defaultForHost`, i.e.
  cleanup under the *wrong* method's assumptions.
- **Distro keys** `"arch"` / `"manjaro"` / `"void"` / `"debian-sid"`
  (`metadata.distro`, matched exactly in
  [DistroRegistry.forInstallation] together with the ABI in
  `metadata.arch`).
- **Pinned-shortcut format**: shortcut id `"<installId>/<desktopId>"`
  and the intent extras keys `"installId"` / `"desktopId"` /
  `"label"` ([EntryShortcuts], [ShortcutLaunchActivity]). Persisted
  by the system launcher, which the app cannot rewrite — a format
  change turns every existing pin into a dead icon.
- **`/usr/lib/tawc/bashrc`**: this absolute path is baked into the
  one-time user-owned `/root/.bashrc` stub at configure time
  ([ShellDefaults]); moving the app-owned file silently unsources
  shell defaults in every existing rootfs.
- **`/usr/local/bin/ando`** and the `ando` CLI surface — a public
  command users script against (notes/ando.md).
- Softer, prefs-only: `GraphicsBackend.key` values and the
  `tawc-settings` pref keys ([Settings]). Unknown values already
  fall back to defaults gracefully, so a rename only resets the
  user's choice — avoid anyway.

TAWC-owned rootfs paths shipped via [TawcInstaller]
(`/usr/lib/hybris/`, `/usr/lib/gfxstream/`, `/usr/lib/mesa-zink/`)
are *not* frozen at this level — the persisted install manifest
wipes old dests and lays new ones on upgrade — but user scripts may
reference them, so treat moves as user-visible changes.

### Schema versioning

`metadata.json` carries a `schemaVersion` field
([Installation.CURRENT_SCHEMA_VERSION], currently `1`) so that
forward-incompatible changes have a hook to dispatch on. The reader
([Installation.fromJson]) defaults missing `schemaVersion` to `1`
and missing `installedAtAppVersionCode` to `0`, so pre-versioning
records keep loading. A `schemaVersion` *newer* than
`CURRENT_SCHEMA_VERSION` is refused outright (the slot surfaces as
CORRUPT — see "State machine") rather than mis-parsed; that check in
`fromJson` is the dispatch site to grow a `when` at on bump. Unknown
enum values in v1 records are tolerated per-field instead: an
unrecognized `state` falls back to READY, an unrecognized
`tawcInstalls[].type` skips just that entry (worst case one stale
file after the next stamp refresh).

Bump `CURRENT_SCHEMA_VERSION` only when the new field can't safely
default — e.g. removing a field, renaming one whose old name is
still referenced elsewhere, or changing the meaning of an existing
value. Pure additive fields (new optional flag, new opt-in metadata)
do *not* warrant a bump; just add them with a sensible default in
`fromJson`.

When you do bump it, decide one of:

1. **Read-only tolerance:** old `schemaVersion` is still readable
   under the new code (`fromJson` handles both shapes). Preferred
   when possible — no migration needed.
2. **One-shot rewrite on load:** detect the old version in
   `fromJson`, transform fields, and `store.save` the upgraded
   record. Only safe if the transform is local to `metadata.json`
   and doesn't depend on rootfs state.
3. **Refuse + force reinstall:** if the old shape genuinely can't
   be carried forward, mark the slot `FAILED` with a clear failure
   string so the user uninstalls and reinstalls. Same UX as a
   bootstrap-verification failure today.

`installedAtAppVersionCode` exists alongside `schemaVersion` because
the schema number says "what fields are in this file," but the more
useful question for any real migration is "what code wrote this
rootfs." Two installs with the same `schemaVersion` can differ in
`installBasePackages` output, slimming policy, etc., so the version
code is what an `if (installedAtAppVersionCode < N)` check should
key on.

## Debian sid: keep full systemd out of the install set

The apt-family base list seeds `systemd-standalone-sysusers` +
`systemd-standalone-tmpfiles` (`AptCommon.DEFAULT_BASE_PACKAGES`).
Without a provider in the same install transaction, apt satisfies
dbus-daemon's `systemd | systemd-standalone-tmpfiles |
systemd-tmpfiles` alternative with its first branch — full systemd —
which drags in the tpm/libtss2 subtree and whose postinst
(machine-id setup, `systemctl enable`) has no reason to run in our
systemd-less rootfs. Historically it also could not run: systemd
≥260 raised its kernel baseline to 5.10 and its path machinery
(`chase()`/`xstatx_full`) hard-fails with EUNATCH ("Protocol driver
not attached") when `statx()` doesn't return
`STATX_MNT_ID`/`STATX_MNT_ID_UNIQUE` — bits that need kernel ≥5.8,
which 5.4-kernel phones can't provide, and the pre-260
`/proc/self/fdinfo` fallback was removed upstream. That broke the
2026-07 sid install at `apt base-package install failed (exit=100)`.

tawcroot now synthesizes `STATX_MNT_ID` from `/proc/self/fdinfo`
when the guest asks and the kernel can't deliver
(`syscalls_fs.c::tawcroot_statx_fill_mnt_id`), so sid's v261 systemd
tooling
(`systemd-sysusers`, `systemd-tmpfiles`, anything `chase()`-based —
these run from arbitrary package postinsts) works on old kernels.
The standalone seeding stays anyway: systemd-less by design, and a
smaller install. Under plain chroot/proot on a pre-5.8 kernel the
EUNATCH failures remain — nothing translates statx there.

## Known harmless install noise

Errors that appear in pacman/install logs under tawcroot, are
understood (or understood-enough), and have no practical fix worth
doing. Don't re-file these as issues.

- **`Cannot set file attributes for '/var/log/journal'`** plus
  pacman's `error: command failed to execute correctly` — the
  journald tmpfiles rule requests `FS_NOCOW_FL` (`+C`); ext4 on the
  Android data partition doesn't support it, so
  `ioctl(FS_IOC_SETFLAGS)` correctly returns EOPNOTSUPP.
  systemd-tmpfiles says `ignoring` but exits non-zero, which pacman
  reports as the hook failing. Only cost is a minor journal-rotation
  perf hint. Do **not** "fix" this by making tawcroot's ioctl handler
  return 0 — the flag genuinely isn't set on disk, and lying would
  hide real failures. If the noise ever matters, the clean fix is in
  the Kotlin install profile: mask
  `/usr/lib/tmpfiles.d/journal-nocow.conf` with an empty
  `/etc/tmpfiles.d/journal-nocow.conf` at install time.
- **`Failed to open path /dev/net/tun`** from tmpfiles — we don't
  stage `/dev/net/`, no target workload uses tun, and systemd never
  runs as init (tmpfiles only fires via the pacman ALPM hook).
  Cosmetic.
- **`Failed to check for chroot() environment: Function not
  implemented` → `Skipped: Current root is not booted.`** in
  `systemd-hook` post-transaction hooks (`systemctl daemon-reload`,
  `udevadm`, `sysctl --system`, …). The hook skips for the wrong
  reason (ENOSYS instead of a clean chroot detection), but the
  outcome is identical: systemd isn't PID 1 here, so the hook would
  no-op anyway. Root cause never pinned down. What's been ruled out:
  tawcroot *does* handle `statx` (`tawcroot/src/syscalls_fs.c`
  `handle_statx`, registered in the dispatch table), and the
  `/proc/1/sched` read plus `stat` compare in systemd's
  `running_in_chroot_or_offline()` go through handled paths that
  would fail with EACCES/ENOENT, not ENOSYS. Leading suspect: a
  newer-systemd syscall the 5.4 kernel lacks (e.g. `statmount(2)`/
  `listmount(2)`, kernel 6.8+, RET_ALLOWed by tawcroot → kernel
  ENOSYS) — i.e. a kernel-version/ABI mismatch, not a missing
  tawcroot handler, so there's nothing for us to do. If it ever
  needs confirming: `strace -f -e trace=!read,write` the
  systemd-hook invocation and find the syscall returning -38.

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

`scripts/rootfs-run.sh` is the host-driven counterpart to the
in-app launcher. Both route through the dev exec broker's
`RUNINSIDE` request type, which dispatches to
[InstallationMethod.startInside] — the single Kotlin entry point
where mount setup, env injection (via [RootfsEnv]'s `env -i` wrapper),
and chroot exec live (see
[rootfs-sessions.md](rootfs-sessions.md) and
[exec-broker.md](exec-broker.md)). Used by the integration tests
(`tests/integration/src/adb.rs`), `scripts/run-integration-tests.sh`,
and `scripts/run-integration-tests.sh`.
