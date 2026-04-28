# Building tawc

> **Source of truth for build dependencies and the fresh-system build flow.**
> Keep this file in sync with the `client/build-*` scripts and Gradle config.
> Whenever you add or change a build-time dep — host package, vendored
> repo, env var, toolchain version — update this doc in the same change.
> CLAUDE.md instructs agents to consult and update this file when building.

This doc describes building on a Linux x86_64 host. macOS/Windows are not
supported as build hosts; nothing is fundamentally portable-hostile, just
untested.

## Quick reference

```bash
# One-time: install host deps (see "Host packages" below)

# One-time per fresh clone:
git clone https://github.com/wmww/libhybris.git           ./libhybris
git clone --branch halium-11.0 --depth 1 \
    https://github.com/Halium/android-headers.git         ./android-headers
bash client/build-libxkbcommon       # clones libxkbcommon, cross-builds .a
bash client/build-libhybris-aarch64  # cross-builds libhybris for aarch64 glibc

# Each iteration:
cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug
```

The result is `server/app/build/outputs/apk/debug/app-debug.apk`. Install
and launch as documented in CLAUDE.md's Quick Reference.

## Host packages

### Always required

| Component | Arch (`pacman -S`)                                     | Debian/Ubuntu (`apt install`)                        |
|-----------|--------------------------------------------------------|------------------------------------------------------|
| JDK 21    | `jdk21-openjdk`                                        | `openjdk-21-jdk`                                     |
| Rust      | `rustup` (then `rustup default stable`)                | `rustup` (via rustup.rs)                             |
| Cargo NDK | `cargo install cargo-ndk`                              | same                                                 |
| Android SDK + NDK | install Android Studio, or use `sdkmanager` directly. NDK version pinned in `server/app/build.gradle.kts` (currently 27.2.12479018). | same |
| Build basics | `base-devel`                                        | `build-essential pkg-config`                         |
| Meson + Ninja (libxkbcommon) | `meson ninja`                            | `meson ninja-build`                                  |
| Wayland host tools (libhybris cross-build) | `wayland wayland-protocols` | `libwayland-dev wayland-protocols`                   |
| Autotools (libhybris cross-build) | `autoconf automake libtool` | `autoconf automake libtool`                          |
| Vulkan headers (libhybris cross-build) | `vulkan-headers`        | `libvulkan-dev`                                      |

JDK 26 is **not** supported — it crashes the Kotlin Gradle plugin (2.1.20).
Stick with 21.

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
export ANDROID_HOME=$HOME/android-sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
```

`ANDROID_NDK_HOME` is auto-detected by `client/build-libxkbcommon`
(it falls back to `$ANDROID_HOME/ndk/<latest>`). `JAVA_HOME` must be
set explicitly when invoking `./gradlew` if your default JDK isn't 21.

## Vendored repos

All gitignored. Each must exist locally before the matching build script
runs; the scripts fail fast with the clone command in the error message.

| Path                  | Source                                       | Pinned to              | Used by                           |
|-----------------------|----------------------------------------------|------------------------|-----------------------------------|
| `./libhybris/`        | https://github.com/wmww/libhybris            | tip of our fork        | `client/build-libhybris-aarch64`  |
| `./android-headers/`  | https://github.com/Halium/android-headers    | branch `halium-11.0`   | `client/build-libhybris-aarch64` |
| `./libxkbcommon/`     | https://github.com/xkbcommon/libxkbcommon    | tag `xkbcommon-1.7.0`  | `client/build-libxkbcommon` (auto-clones) |
| `./smithay/`          | https://github.com/Smithay/smithay (fork)    | per `compositor/Cargo.toml` | Rust compositor                |

Vendoring (rather than fetching at build time) follows the same pattern
across all our cross-builds: deterministic builds, offline-capable,
no surprise tarball changes between runs.

## Per-component build instructions

Run the cross-builds in any order. Gradle does **not** invoke them
yet (Phase 2 of `issues/ship-libhybris-in-apk.md`); for now treat them
as one-time setup steps after each fresh clone.

### libxkbcommon (static .a → linked into compositor)

Cross-built once per ABI. NDK clang against bionic.

```bash
bash client/build-libxkbcommon                  # aarch64 (default)
bash client/build-libxkbcommon --abi=x86_64     # emulator
bash client/build-libxkbcommon --abi=both
bash client/build-libxkbcommon --clean          # wipe builddir(s)
```

Output: `libxkbcommon/builddir{,-x86_64}/libxkbcommon.a`. Linked into
`libcompositor.so` via `compositor/build.rs`.

### libhybris (shared .so set → ships in APK as asset, future)

Cross-built once. aarch64-linux-gnu-gcc against glibc.

```bash
bash client/build-libhybris-aarch64           # incremental
bash client/build-libhybris-aarch64 --clean   # distclean + rebuild
```

Output: `build/libhybris-aarch64/install/usr/local/lib/`, mirroring the
on-device install layout (`libhybris-common.so{,.1,.1.0.0}`,
`libEGL.so{,.1,.1.0.0}`, `libGLESv2.so{,.2,.2.0.0}`,
`libGLESv1_CM.so{,.1,.1.0.1}`, `libvulkan.so{,.1,.1.2.183}`,
`libsync.so{,.2,.2.0.0}`, plus the `libhybris/` plugin tree and
`libhybris/linker/q.so` for the Android 10+ bionic linker).

The build is in-tree (`builddir == sourcedir`) because some libhybris
subdirs reference wayland-scanner-generated headers via `-I`s that
only resolve when builddir == srcdir. `--clean` runs `make distclean`
on the source tree.

Bundled into the APK by the Gradle `packLibhybris` task as
`server/app/src/main/assets/libhybris/arm64-v8a.tar`. Extracted at
first compositor start by `CompositorService.ensureLibhybrisExtracted`,
and symlinked into each rootfs at chroot install time by
`LibhybrisLinker.kt`. End-to-end automatic — no manual steps after
`./gradlew assembleDebug`.

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

### Rust compositor (.so → bundled in APK by Gradle)

NDK clang against bionic, via `cargo-ndk`. Invoked by Gradle
automatically; no separate command needed.

```bash
# Manual invocation (Gradle does this for you):
cd server/compositor && \
    cargo ndk --target arm64-v8a --platform 29 -- build --release
```

### APK assembly

```bash
cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug
```

Invokes the Rust compositor build, copies its output into
`jniLibs/arm64-v8a/`; runs `client/build-libhybris-aarch64` and packs
the result into `assets/libhybris/arm64-v8a.tar`; produces
`app/build/outputs/apk/debug/app-debug.apk`. Everything libhybris
needs ships inside this APK.

## Install and launch

```bash
adb install -r server/app/build/outputs/apk/debug/app-debug.apk && \
adb shell am force-stop me.phie.tawc && \
adb shell am start -n me.phie.tawc/.compositor.CompositorActivity \
    -d "tawc://activity/manual"
```

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
`server/app/src/main/assets/xkb/` and extracted to the app's data dir
(`files/xkb`) by `CompositorService.onCreate` before `nativeStartCompositor`.
Versioned via `files/xkb/.version`.

The data came from the chroot's `/usr/share/xkeyboard-config-2/`
(Arch Linux ARM `xkeyboard-config` package). To update:

```bash
adb shell "su -c 'cd /data/data/me.phie.tawc/distros/arch/rootfs/usr/share/xkeyboard-config-2 && tar cf /data/local/tmp/xkb-data.tar .'"
adb pull /data/local/tmp/xkb-data.tar /tmp/xkb-data.tar
rm -rf server/app/src/main/assets/xkb
mkdir -p server/app/src/main/assets/xkb
tar xf /tmp/xkb-data.tar -C server/app/src/main/assets/xkb/
rm /tmp/xkb-data.tar
adb shell "rm /data/local/tmp/xkb-data.tar"
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
bash testing/build-debug-app.sh                 # debug app on phone
cd testing/integration && cargo test -- --nocapture --test-threads=1
```
