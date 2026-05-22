# Building tawc

> **Source of truth for build dependencies and the fresh-system build flow.**
> Keep this file in sync with the `scripts/build-*.sh` scripts and Gradle config.
> Whenever you add or change a build-time dep — host package, vendored
> repo, env var, toolchain version — update this doc in the same change.
> AGENTS.md instructs agents to consult and update this file when building.

This doc describes building on a Linux x86_64 host. macOS/Windows are not
supported as build hosts; nothing is fundamentally portable-hostile, just
untested.

## Quick reference

```bash
# One-time: install host deps (see "Host packages" below)

# Each iteration:
scripts/build-app.sh
```

Vendored git repos listed in `deps/deps.list` are auto-cloned by their
respective build/setup scripts on first invocation - no manual clone step
is needed. Gradle drives those scripts ahead of APK assembly, so a fresh
clone goes straight to `scripts/build-app.sh`. To force a component rebuild by
hand, use the matching script under `scripts/` or `tawcroot/build.sh`.

The result is `app/build/outputs/apk/debug/app-debug.apk`. Install
and launch as documented in AGENTS.md's Common Commands.

## Host packages

### Always required

| Component | Arch (`pacman -S`)                                     | Debian/Ubuntu (`apt install`)                        |
|-----------|--------------------------------------------------------|------------------------------------------------------|
| JDK 21    | `jdk21-openjdk`                                        | `openjdk-21-jdk`                                     |
| Rust      | `rustup` (then `rustup default stable`)                | `rustup` (via rustup.rs)                             |
| Rust Android targets (`error[E0463]: can't find crate for \`core\`` if missing) | `rustup target add aarch64-linux-android` (add `x86_64-linux-android` for emulator builds). The kumquat server is a Cargo dep of the compositor crate (`target_os="android"`-gated), so the same target also covers the gfxstream-bridge build; no extra toolchain. | same |
| Rust glibc targets (`build-mesa-gfxstream.sh` cross-builds Mesa's gfxstream-vk Rust pieces) | `rustup target add aarch64-unknown-linux-gnu` (and `rustup target add x86_64-unknown-linux-gnu` for the emulator bridge) | same |
| `bindgen` (Mesa's gfxstream-vk meson Rust bindings) | `cargo install bindgen-cli` | same |
| Cargo NDK (cargo subcommand — `cargo build` will fail with `error: no such command: ndk` if missing) | `cargo install cargo-ndk` | same |
| Android SDK + NDK | install Android Studio, or use `sdkmanager` directly. Android platform API 36 is required by `compileSdk`; NDK version pinned in `app/build.gradle.kts` (currently 27.2.12479018). | same |
| Build basics | `base-devel`                                        | `build-essential pkg-config curl libarchive-tools`   |
| Meson + Ninja (libxkbcommon) | `meson ninja`                            | `meson ninja-build`                                  |
| Wayland host tools (libhybris cross-build) | `wayland wayland-protocols` | `libwayland-dev wayland-protocols`                   |
| Host sysroot + test app builds | `curl libarchive wayland` | `curl libarchive-tools libwayland-dev` |
| Autotools (libhybris cross-build) | `autoconf automake libtool` | `autoconf automake libtool`                          |
| Vulkan headers (libhybris cross-build) | `vulkan-headers`        | `libvulkan-dev`                                      |
| `patchelf` (libhybris GL shims) | `patchelf`                  | `patchelf`                                           |
| nginx (dev-time mirror cache, optional) | `nginx`                       | `nginx`                                              |

JDK 26 is **not** supported for running this Gradle build — Gradle 8.12's
embedded Kotlin stack crashes while parsing Java version `26.0.1`. The repo
pins the Gradle daemon to Java 21 in `gradle/gradle-daemon-jvm.properties`, so
direct `./gradlew ...` works when a JDK 21 install is available even if the
shell's default `java` is newer.

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

### x86_64 glibc compiler (mesa-gfxstream for the emulator)

`scripts/build-mesa-gfxstream.sh --abi=x86_64` cross-builds the
chroot-side gfxstream Vulkan ICD for the AVD's x86_64 rootfs. Since
the build host is also x86_64-glibc, this is technically a "native"
build — the system `gcc`/`g++` (Arch: `base-devel`; Debian/Ubuntu:
`build-essential`; Fedora: `gcc gcc-c++`) is the right compiler. The
script prefers the triple-prefixed names (`x86_64-linux-gnu-gcc`,
which Debian ships by default) when present and falls back to plain
`gcc` otherwise. No separate cross-toolchain is needed.

### Host sysroots (per-ABI)

Both `--abi=aarch64` and `--abi=x86_64` cross-builds of
`build-mesa-gfxstream.sh` link `libvulkan_gfxstream.so` against a
small distro sysroot under `build/sysroots/<distro>-<arch>/`. The
canonical builder is:

```bash
scripts/build-host-sysroot.sh --abi=aarch64 --distro=arch --profile=prod
scripts/build-host-sysroot.sh --abi=x86_64 --distro=arch --profile=prod
```

`build-mesa-gfxstream.sh` runs this automatically when its production
sysroot is missing or lacks Mesa's required Wayland protocol XMLs.
`tests/apps/Makefile` uses the same script with `--profile=full`, which
pulls the Cairo/Wayland/X11 header and pkg-config closure needed to
build test clients on the host. There is no device-rootfs sysroot pull
path anymore.

Default distro is Arch (`TAWC_SYSROOT_DISTRO=arch`). `void` support uses
`xbps-install` when that host tool is available. The builder keeps a
compatibility link at `build/<arch>-sysroot` for older build consumers.
For non-production profiles (`--profile=full`, used by test apps), distro
package downloads go through the dev mirror cache by default
(`http://127.0.0.1:8080/proxy/`); run `scripts/cache-proxy.sh run` first
or set `TAWC_MIRROR_PROXY` explicitly. Pacman repo databases are fetched
directly on each sysroot build so stale cached metadata cannot reference
package archives that have already rolled off the mirror.

## Environment variables

```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
```

`scripts/build-app.sh` sets `JAVA_HOME` and `ANDROID_HOME` to the defaults
above when they are unset. `ANDROID_NDK_HOME` is auto-detected by
`scripts/build-libxkbcommon.sh`
(it falls back to `$ANDROID_HOME/ndk/<latest>`). Direct `./gradlew` invocations
use the repo's Gradle daemon JVM pin and require JDK 21 to be installed.

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
| `./deps/cleat/`                            | `tawcroot/build.sh` (host + device test runners) |
| `./deps/xwayland-src/<lib>/` (~22 repos)   | `scripts/build-xwayland.sh`               |
| `./deps/smithay/`                     | Rust compositor (`scripts/ensure-deps.sh smithay`; consumed via `[patch.crates-io]` path in `compositor/Cargo.toml`) |
| `./deps/mesa/`                             | `scripts/build-mesa-gfxstream.sh` (gfxstream-vk and Mesa-Zink assets) |
| `./deps/gfxstream/`                        | `scripts/build-gfxstream-backend.sh`      |
| `./deps/rutabaga_gfx/`                     | `scripts/ensure-deps.sh --patches rutabaga_gfx deps/rutabaga-patches/rutabaga_gfx`; Rust compositor kumquat server dep |

Two tarball deps (`talloc`, `libmd`) are *not* in `deps.list` — their
version is baked into the URL inside the build script, so a version
bump auto-fetches a fresh extract.

### Bumping a dep

1. `cd <dep>; git checkout <new commit>; iterate; (push if needed)`
2. Edit `deps/deps.list` — bump the commit column.
3. On every checkout that needs to follow: `scripts/update-deps.sh`
   (or `scripts/update-deps.sh <name>` for a subset). This is the only
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
the APK — `scripts/build-app.sh` from a fresh clone is enough.
Run the standalone scripts only when iterating on the component itself
(faster than a full Gradle round-trip).

### libxkbcommon (static .a → linked into compositor)

Cross-built once per ABI. NDK clang against bionic.

```bash
scripts/build-libxkbcommon.sh                  # aarch64 (default)
scripts/build-libxkbcommon.sh --abi=x86_64     # emulator
scripts/build-libxkbcommon.sh --abi=both
scripts/build-libxkbcommon.sh --clean          # wipe builddir(s)
```

Output: `deps/libxkbcommon/builddir{,-x86_64}/libxkbcommon.a`. Linked into
`libcompositor.so` via `compositor/build.rs`.

### libhybris (shared .so set → ships in APK as asset)

Cross-built once. aarch64-linux-gnu-gcc against glibc.

```bash
scripts/build-libhybris.sh           # incremental
scripts/build-libhybris.sh --clean   # distclean + rebuild
```

Output: `build/libhybris-aarch64/install/usr/lib/hybris/`, the
on-device install layout (`libhybris-common.so{,.1,.1.0.0}`,
`libEGL.so{,.1,.1.0.0}`, `libGLESv2.so{,.2,.2.0.0}`,
`libGLESv1_CM.so{,.1,.1.0.1}`, `libvulkan.so{,.1,.1.2.183}`,
`libsync.so{,.2,.2.0.0}`, plus the `libhybris/` plugin tree and
`libhybris/linker/q.so` for the Android 10+ bionic linker, and the
generated `gl-shims/` directory). `scripts/build-libhybris.sh` configures
`--prefix=/usr/lib/hybris --libdir=/usr/lib/hybris`, so libhybris's
RUNPATH, plugin dirs, and linker plugin dir already match the rootfs
copy location. No `HYBRIS_*_DIR` env overrides are needed.

The build is in-tree (`builddir == sourcedir`) because some libhybris
subdirs reference wayland-scanner-generated headers via `-I`s that
only resolve when builddir == srcdir. `--clean` runs `make distclean`
on the source tree.

Bundled into the APK by the Gradle `packLibhybris` task as
`app/src/main/assets/libhybris/arm64-v8a.tar`. Extracted at
first compositor start by `CompositorService.ensureLibhybrisExtracted`,
and copied into each rootfs by `TawcInstaller`/`LibhybrisInstallProvider`
at install time and on first app start after an APK upgrade.
End-to-end automatic — no manual steps after `scripts/build-app.sh`.

#### Why the cross-compile and not the NDK

libhybris is loaded by glibc Wayland clients in the chroot. Its
`hooks.c` exports glibc-shaped wrappers (e.g. `__sprintf_chk`,
`pthread_attr_setstackaddr`, `valloc`) for the bionic vendor blobs
it loads via its embedded Android linker (`libhybris/linker/q.so`).
Building with the NDK produces a bionic-linked binary that no glibc
client can `dlopen` — the spike that uncovered this is not worth
redoing; use the glibc cross-toolchain.

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

### libvulkan_gfxstream.so (Mesa gfxstream-vk → ships in APK as asset, gfxstream-bridge GPU path)

Cross-built once per enabled ABI. `aarch64` uses the same
`aarch64-linux-gnu` toolchain as libhybris; `x86_64` uses the host
glibc compiler. Builds with `-Dvirtgpu_kumquat=true` enabled — Mesa
patches in `deps/mesa-patches/mesa/` add a meson option that
sidesteps the in-tree Rust subproject build (which doesn't
cross-compile cleanly) by linking to a separately-cargo-built
`libvirtgpu_kumquat_ffi.a` via pkg-config. Output .so is ~7MB.

Pre-req: make sure the host sysroot exists. The Mesa build script does
this automatically, but the standalone command is:

```bash
scripts/build-host-sysroot.sh --abi=aarch64 --profile=prod
scripts/build-mesa-gfxstream.sh
scripts/build-mesa-gfxstream.sh --abi=x86_64
scripts/build-mesa-gfxstream.sh --clean   # wipe builddir
```

Output: `build/mesa-<arch>/install/usr/lib/gfxstream/libvulkan_gfxstream.so`
+ `.../gfxstream_vk_icd.<arch>.json` (co-located, no separate
`share/vulkan/icd.d/` - `VK_ICD_FILENAMES` points at it explicitly).
Bundled into the APK by Gradle's `packMesaGfxstream<Abi>` and laid into
every rootfs by `BridgeInstallProvider`. The same script also builds
the optional Mesa-Zink tarball consumed by `libhybris-zink` unless
Gradle passes `--no-zink` via `-PtawcGraphics=...`.
Mesa's `wayland-protocols` XML comes from the pinned
`deps/xwayland-src/wayland-protocols` checkout, not the host sysroot.
That keeps Mesa's generated protocol inputs in sync with the Mesa
source even when distro sysroot packages lag.

### Xwayland (binary + libs → ships in APK as asset)

Cross-built once. NDK clang against bionic — same toolchain as the
Rust compositor. Aarch64-only. APK builds include it by default when
`arm64-v8a` is enabled; pass `-PtawcXwayland=false` to Gradle or
`--no-xwayland` to `scripts/build-app.sh` / `scripts/app-build-install.sh`
to skip building, packaging, extracting, and spawning it.

```bash
scripts/build-xwayland.sh           # incremental
scripts/build-xwayland.sh --clean   # wipe install + builddirs
scripts/build-xwayland.sh --only=libx11   # rebuild one stage
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

`cargo-ndk` is a cargo subcommand that has to be installed once per
user (`cargo install cargo-ndk` — also listed in the Host packages
table). Without it, the Rust build fails with `error: no such command:
ndk`.

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
scripts/build-proot.sh                # current host's primary ABI
scripts/build-proot.sh --abi=both     # both Android ABIs
scripts/build-proot.sh --clean        # wipe and rebuild
```

See [proot.md](proot.md) for why we use Termux's fork.

### tawcroot (systrap proot replacement → ships in APK as jniLib)

Cross-built once per ABI. NDK clang against bionic, static non-PIE
ET_EXEC, `-nostdlib` freestanding. Output:
`app/src/main/jniLibs/<abi>/libtawcroot.so`. Auto-invoked by
Gradle's `buildTawcroot<Abi>` task; standalone:

```bash
tawcroot/build.sh --abi=aarch64      # explicit Android ABI
tawcroot/build.sh --abi=both         # both Android ABIs
tawcroot/build.sh --abi=host         # native glibc, runs on dev box
tawcroot/build.sh --testhost         # also build testhost twin
tawcroot/build.sh --tests            # also build cleat orchestrator
```

See [tawcroot.md](tawcroot.md) for the design.

### APK assembly

```bash
scripts/build-app.sh
```

Builds `arm64-v8a` by default, or `x86_64` when `ANDROID_SERIAL` or
`.tawctarget` points at an emulator. Use `--abi=arm64-v8a`,
`--abi=x86_64`, or `--abi=both` to override.

Invokes the Rust compositor build, copies its output into
`jniLibs/<abi>/`; applies Smithay/rutabaga setup; cross-builds proot
(when enabled) and tawcroot; builds the gfxstream host backend for each
enabled ABI; builds/packs Mesa gfxstream-vk and optional Mesa-Zink
assets; builds/packs libhybris for arm64; builds/packs Xwayland for
arm64 unless `--no-xwayland` is passed; then produces
`app/build/outputs/apk/debug/app-debug.apk`.
Everything the supported install/runtime paths need ships inside this
APK.

## Install and launch

```bash
scripts/app-build-install.sh
```

Picks the device from `.tawctarget` / `TAWC_TARGET` via
`scripts/lib/select-device.sh`, builds through `scripts/build-app.sh`,
installs, force-stops, and launches `MainActivity` (which starts
`CompositorService`). Flags: `--no-build` to reuse the existing APK;
`--no-launch` to install without starting (used by
`run-integration-tests.sh`).

Note: `am start` directly into `.compositor.CompositorActivity` does
not work — go through `MainActivity` (the script does this).

After reinstalling, the compositor restarts with a new Wayland socket.
Any running chroot clients (Firefox, etc.) will be connected to the
old socket and show black screens — kill and relaunch them.

Installing or upgrading the APK causes the next app start to re-extract
bundled runtime assets and re-run `TawcInstaller` against existing
rootfs metadata when the `tawcStamp` changes. Libhybris is copied as
real files into each rootfs at `/usr/lib/hybris/`; gfxstream and
Mesa-Zink use the same provider/manifest mechanism under their own
`/usr/lib/...` namespaces.

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
scripts/run-integration-tests.sh           # package setup, deploy, cargo test
```
