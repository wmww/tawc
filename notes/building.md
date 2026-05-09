# Building tawc

> **Source of truth for build dependencies and the fresh-system build flow.**
> Keep this file in sync with the `scripts/build-*.sh` scripts and Gradle config.
> Whenever you add or change a build-time dep — host package, vendored
> repo, env var, toolchain version — update this doc in the same change.
> CLAUDE.md instructs agents to consult and update this file when building.

This doc describes building on a Linux x86_64 host. macOS/Windows are not
supported as build hosts; nothing is fundamentally portable-hostile, just
untested.

## Quick reference

```bash
# One-time: install host deps (see "Host packages" below)

# Each iteration:
JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug
```

Vendored repos (`./deps/libxkbcommon/`, `./deps/libhybris/`, `./deps/android-headers/`)
are auto-cloned by their respective build scripts on first invocation —
no manual clone step is needed. Gradle drives those scripts ahead of the
Rust compositor build, so a fresh clone goes straight to `assembleDebug`.
To force a rebuild by hand, run `bash scripts/build-libxkbcommon.sh` or
`bash scripts/build-libhybris.sh`.

The result is `app/build/outputs/apk/debug/app-debug.apk`. Install
and launch as documented in CLAUDE.md's Quick Reference.

## Host packages

### Always required

| Component | Arch (`pacman -S`)                                     | Debian/Ubuntu (`apt install`)                        |
|-----------|--------------------------------------------------------|------------------------------------------------------|
| JDK 21    | `jdk21-openjdk`                                        | `openjdk-21-jdk`                                     |
| Rust      | `rustup` (then `rustup default stable`)                | `rustup` (via rustup.rs)                             |
| Cargo NDK | `cargo install cargo-ndk`                              | same                                                 |
| Android SDK + NDK | install Android Studio, or use `sdkmanager` directly. NDK version pinned in `app/build.gradle.kts` (currently 27.2.12479018). | same |
| Build basics | `base-devel`                                        | `build-essential pkg-config`                         |
| Meson + Ninja (libxkbcommon) | `meson ninja`                            | `meson ninja-build`                                  |
| Wayland host tools (libhybris cross-build) | `wayland wayland-protocols` | `libwayland-dev wayland-protocols`                   |
| Autotools (libhybris cross-build) | `autoconf automake libtool` | `autoconf automake libtool`                          |
| Vulkan headers (libhybris cross-build) | `vulkan-headers`        | `libvulkan-dev`                                      |
| nginx (dev-time mirror cache, optional) | `nginx`                       | `nginx`                                              |

JDK 26 is **not** supported — it crashes the Kotlin Gradle plugin (2.1.20).
Stick with 21.

`nginx` is only needed for the dev-time install caching proxy
(`scripts/cache-proxy.sh`, see notes/cache-proxy.md). Skip if you
don't iterate on installs.

### aarch64 glibc cross-toolchain (libhybris)

| Distro | Packages |
|--------|----------|
| Arch   | `aarch64-linux-gnu-gcc aarch64-linux-gnu-binutils aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers` |
| Debian/Ubuntu | `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-arm64-cross linux-libc-dev-arm64-cross` |
| Fedora | `gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu glibc-aarch64-linux-gnu` |

The toolchain produces aarch64 **glibc** binaries. We do **not** use the
NDK for libhybris because libhybris is glibc-side by design (its
`hooks.c` exports glibc-shaped symbols and is loaded by glibc Wayland
clients inside the chroot — see `notes/gpu-strategy.md`). The NDK
targets bionic and is the wrong toolchain.

For the rest of our native build (the Rust compositor, libxkbcommon),
the NDK is correct and we keep using it.

## Environment variables

```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
```

`ANDROID_NDK_HOME` is auto-detected by `scripts/build-libxkbcommon.sh`
(it falls back to `$ANDROID_HOME/ndk/<latest>`). `JAVA_HOME` must be
set explicitly when invoking `./gradlew` if your default JDK isn't 21.

## Vendored repos

All gitignored. The pinned commit + repo URL for every vendored git dep
lives in **[`deps/deps.list`](../deps/deps.list)** — single source
of truth. Build scripts source `scripts/lib/deps.sh` and call
`dep_ensure <name>`, which clones if missing and **errors loudly** if
the existing checkout is at the wrong commit (uncommitted edits are
silently tolerated as long as HEAD matches the pin).

| Path                                  | Used by                                       |
|---------------------------------------|-----------------------------------------------|
| `./deps/libhybris/`                        | `scripts/build-libhybris.sh`              |
| `./deps/android-headers/`                  | `scripts/build-libhybris.sh`              |
| `./deps/libxkbcommon/`                     | `scripts/build-libxkbcommon.sh`                   |
| `./deps/proot/` (+ `./deps/proot-deps/talloc-*` tarball) | `scripts/build-proot.sh`                |
| `./deps/cleat/`                            | `tawcroot/build` (host + device test runners) |
| `./deps/xwayland-src/<lib>/` (~22 repos)   | `scripts/build-xwayland.sh`               |
| `./deps/smithay/`                     | Rust compositor (`scripts/setup-smithay.sh`; consumed via `[patch.crates-io]` path in `compositor/Cargo.toml`) |

Two tarball deps (`talloc`, `libmd`) are *not* in `deps.list` — their
version is baked into the URL inside the build script, so a version
bump auto-fetches a fresh extract.

### Bumping a dep

1. `cd <dep>; git checkout <new commit>; iterate; (push if needed)`
2. Edit `deps/deps.list` — bump the commit column.
3. On every checkout that needs to follow: `bash scripts/update-deps.sh`
   (or `bash scripts/update-deps.sh <name>` for a subset). This is the only
   command that mutates dep checkouts behind your back.

If you bumped commits but forgot to update `deps.list` (or the other
way round), the next build fails with a clear "dep is at the wrong
commit" error. There is intentionally no auto-update — silent drift
is what this whole system exists to prevent.

Vendoring (rather than fetching at build time) follows the same pattern
across all our cross-builds: deterministic builds, offline-capable,
no surprise tarball changes between runs.

## Per-component build instructions

Gradle invokes every cross-build below automatically before assembling
the APK — `./gradlew assembleDebug` from a fresh clone is enough.
Run the standalone scripts only when iterating on the component itself
(faster than a full Gradle round-trip).

### libxkbcommon (static .a → linked into compositor)

Cross-built once per ABI. NDK clang against bionic.

```bash
bash scripts/build-libxkbcommon.sh                  # aarch64 (default)
bash scripts/build-libxkbcommon.sh --abi=x86_64     # emulator
bash scripts/build-libxkbcommon.sh --abi=both
bash scripts/build-libxkbcommon.sh --clean          # wipe builddir(s)
```

Output: `deps/libxkbcommon/builddir{,-x86_64}/libxkbcommon.a`. Linked into
`libcompositor.so` via `compositor/build.rs`.

### libhybris (shared .so set → ships in APK as asset, future)

Cross-built once. aarch64-linux-gnu-gcc against glibc.

```bash
bash scripts/build-libhybris.sh           # incremental
bash scripts/build-libhybris.sh --clean   # distclean + rebuild
```

Output: `build/libhybris-aarch64/install/usr/local/lib/`, the
autotools install layout (`libhybris-common.so{,.1,.1.0.0}`,
`libEGL.so{,.1,.1.0.0}`, `libGLESv2.so{,.2,.2.0.0}`,
`libGLESv1_CM.so{,.1,.1.0.1}`, `libvulkan.so{,.1,.1.2.183}`,
`libsync.so{,.2,.2.0.0}`, plus the `libhybris/` plugin tree and
`libhybris/linker/q.so` for the Android 10+ bionic linker). On-device
this lands at `/usr/lib/hybris/` instead — see "/usr/lib/hybris" in
notes/installation.md and notes/wsi-layer.md for the env-var
overrides that paper over the build-time `/usr/local/lib` baking.

The build is in-tree (`builddir == sourcedir`) because some libhybris
subdirs reference wayland-scanner-generated headers via `-I`s that
only resolve when builddir == srcdir. `--clean` runs `make distclean`
on the source tree.

Bundled into the APK by the Gradle `packLibhybris` task as
`app/src/main/assets/libhybris/arm64-v8a.tar`. Extracted at
first compositor start by `CompositorService.ensureLibhybrisExtracted`,
and copied into each rootfs by `TawcInstaller`/`LibhybrisInstallProvider`
at install time and on first app start after an APK upgrade.
End-to-end automatic — no manual steps after `./gradlew assembleDebug`.

#### Why the cross-compile and not the NDK

libhybris is loaded by glibc Wayland clients in the chroot. Its
`hooks.c` exports glibc-shaped wrappers (e.g. `__sprintf_chk`,
`pthread_attr_setstackaddr`, `valloc`) for the bionic vendor blobs
it loads via its embedded Android linker (`libhybris/linker/q.so`).
Building with the NDK produces a bionic-linked binary that no glibc
client can `dlopen`. The NDK spike (which uncovered this) is recorded
in `issues/ship-libhybris-in-apk.md` and is not redoable — use the
glibc cross-toolchain.

#### Why a `-idirafter` hack appears in the build script

`server_wlegl.cpp` includes `<wayland-server.h>` unconditionally
(even when `--disable-wayland_serverside_buffers` is set), so the
wayland include dir has to be on the compiler's search path globally.
We add it via `-idirafter` rather than `-I` because `-I/usr/include`
shadows the cross-glibc's `stdint.h` with the host x86_64 version,
and host `bits/wordsize.h` gates LP64 on `__x86_64__` being defined
— compiling for aarch64 then collapses `uintptr_t` to `unsigned int`
(32-bit) and fails build with cast-precision errors. `-idirafter`
keeps wayland headers findable while letting the cross-glibc's
`stdint.h` win.

#### Why we skip `common/{mm,n,o}` linker subdirs

`common/Makefile.am` builds four bionic-linker plugin variants
(`mm`/`n`/`o`/`q`) corresponding to Android 6/7/8/10. We target
Android 10+ (matches our `minSdk=29`), so libhybris will only ever
load `q` at runtime. The legacy `mm` plugin doesn't compile clean
under gcc 15 (a `format string` mismatch the upstream code never
fixed); skipping it avoids the build break in code we don't ship.
The build script invokes `make` per-subdir to control this.

### Xwayland (binary + libs → ships in APK as asset)

Cross-built once. NDK clang against bionic — same toolchain as the
Rust compositor. Aarch64-only.

```bash
bash scripts/build-xwayland.sh           # incremental
bash scripts/build-xwayland.sh --clean   # wipe install + builddirs
bash scripts/build-xwayland.sh --only=libx11   # rebuild one stage
```

Output: `build/xwayland-aarch64/install/{bin/Xwayland,bin/xkbcomp,lib,share}`.
Gradle's `stageXwaylandJniLibs` task copies the binaries + `.so` deps
into `app/src/main/jniLibs/arm64-v8a/lib*.so` (so untrusted_app can
exec them out of `nativeLibraryDir`), and `packXwaylandShare` tars
the XKB data tree into `assets/xwayland/share.tar`.
`CompositorService.ensureXwaylandExtracted` extracts the share tar
and lays down `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` symlinks
into `nativeLibraryDir`.

Host packages (in addition to the always-required set above): `perl`
(needed by xorgproto/libxcb/font-util autotools macros). Everything
else (meson, ninja, autoconf, automake, libtool, pkg-config, python3)
is already required for libhybris.

Bionic-built (NDK), not glibc — see `notes/xwayland.md` "Why bionic"
for the rationale and the "Glibc alternative" section for the V4
toolchain swap that we tried and reverted.

### Rust compositor (.so → bundled in APK by Gradle)

NDK clang against bionic, via `cargo-ndk`. Invoked by Gradle
automatically; no separate command needed.

```bash
# Manual invocation (Gradle does this for you):
cd compositor && \
    cargo ndk --target arm64-v8a --platform 29 -- build --release
```

### proot (Termux fork → ships in APK as jniLib)

Cross-built once per ABI. NDK clang against bionic. Output:
`app/src/main/jniLibs/<abi>/libproot.so` + `libproot-loader.so`.
Auto-invoked by Gradle's `buildProot<Abi>` task; standalone:

```bash
bash scripts/build-proot.sh                # current host's primary ABI
bash scripts/build-proot.sh --abi=both     # both Android ABIs
bash scripts/build-proot.sh --clean        # wipe and rebuild
```

See [proot.md](proot.md) for why we use Termux's fork.

### tawcroot (systrap proot replacement → ships in APK as jniLib)

Cross-built once per ABI. NDK clang against bionic, static non-PIE
ET_EXEC, `-nostdlib` freestanding. Output:
`app/src/main/jniLibs/<abi>/libtawcroot.so`. Auto-invoked by
Gradle's `buildTawcroot<Abi>` task; standalone:

```bash
bash tawcroot/build --abi=aarch64      # explicit Android ABI
bash tawcroot/build --abi=both         # both Android ABIs
bash tawcroot/build --abi=host         # native glibc, runs on dev box
bash tawcroot/build --testhost         # also build testhost twin
bash tawcroot/build --tests            # also build cleat orchestrator
```

See [tawcroot.md](tawcroot.md) for the design.

### APK assembly

```bash
JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug
```

Invokes the Rust compositor build, copies its output into
`jniLibs/<abi>/`; cross-builds proot and tawcroot and stages the
result alongside; runs `scripts/build-libhybris.sh` and packs the
result into `assets/libhybris/arm64-v8a.tar`; runs
`scripts/build-xwayland.sh`, stages binaries + `.so` deps as
`jniLibs/arm64-v8a/lib*.so` and the XKB data tree as
`assets/xwayland/share.tar`; produces
`app/build/outputs/apk/debug/app-debug.apk`. Everything libhybris,
proot, tawcroot, and Xwayland need ships inside this APK.

## Install and launch

```bash
bash scripts/app-build-install.sh
```

Builds, installs, force-stops, and launches `MainActivity` (which
starts `CompositorService`). Picks the device from `.tawctarget` /
`TAWC_TARGET` via `scripts/lib/select-device.sh`. Flags: `--no-build`
to reuse the existing APK; `--no-launch` to install without starting
(used by `run-integration-tests.sh`).

Note: `am start` directly into `.compositor.CompositorActivity` does
not work — go through `MainActivity` (the script does this).

After reinstalling, the compositor restarts with a new Wayland socket.
Any running chroot clients (Firefox, etc.) will be connected to the
old socket and show black screens — kill and relaunch them.

Installing or upgrading the APK causes the next compositor start to
re-extract `assets/libhybris/<abi>.tar` into `filesDir/libhybris/`
(versioned by `longVersionCode`). Existing chroots' symlinks already
point at that path, so they automatically pick up the new libhybris
tree without re-running the chroot installer.

## Device setup

SELinux enforcing mode is supported. `ChrootMounter` applies the needed
SELinux policy rule (`type_transition magisk tmpfs file appdomain_tmpfs`)
via `magiskpolicy --live` on every chroot entry.

## Vendored xkb data

The compositor needs xkeyboard-config data for `libxkbcommon` to load
keymaps. This is **not** built — it's a pure data drop, vendored in
`app/src/main/assets/xkb/` and extracted to the app's data dir
(`files/xkb`) by `CompositorService.onCreate` before `nativeStartCompositor`.
Versioned via `files/xkb/.version`.

The data came from the chroot's `/usr/share/xkeyboard-config-2/`
(Arch Linux ARM `xkeyboard-config` package). To update:

```bash
adb shell mkdir -p /data/local/tmp/tawc-dev
adb shell "su -c 'cd /data/data/me.phie.tawc/distros/arch/rootfs/usr/share/xkeyboard-config-2 && tar cf /data/local/tmp/tawc-dev/xkb-data.tar .'"
adb pull /data/local/tmp/tawc-dev/xkb-data.tar /tmp/xkb-data.tar
rm -rf app/src/main/assets/xkb
mkdir -p app/src/main/assets/xkb
tar xf /tmp/xkb-data.tar -C app/src/main/assets/xkb/
rm /tmp/xkb-data.tar
adb shell "rm /data/local/tmp/tawc-dev/xkb-data.tar"
```

## Chroot package gotchas

- **Always `pacman -Syu` before installing GTK4 (or anything else recent).**
  Plain `pacman -S gtk4` installs the current gtk4 package but does **not**
  upgrade already-installed deps like `glib2`. GTK4 4.22 references
  `g_get_monotonic_time_ns`, which only exists in `glib2` >= 2.88 — if the
  chroot still has an older glib2 (e.g. 2.86.4), `gtk4-demo` will fail with
  `symbol lookup error: /usr/lib/libgtk-4.so.1: undefined symbol:
  g_get_monotonic_time_ns` on the first lazy PLT resolution. `pacman -Syu`
  (or `pacman -Sy gtk4` to at least pull a fresh package db) fixes it.

## Debug app & integration tests

See [testing.md](testing.md) for full details.

```bash
bash scripts/build-debug-app.sh                 # debug app on phone
cd tests/integration && cargo test -- --nocapture --test-threads=1
```
