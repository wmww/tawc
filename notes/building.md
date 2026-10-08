# Building TAWC

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
| Rust      | `rustup` — the version is pinned by `rust-toolchain.toml` at the repo root (currently 1.93.0), so rustup installs it on first build; no `rustup default` needed | `rustup` (a real Debian/Ubuntu package since trixie; rustup.rs works too) |
| Rust Android targets (`error[E0463]: can't find crate for \`core\`` if missing) | Both Android targets are listed in `rust-toolchain.toml`, so rustup adds them automatically. Manually: `rustup target add aarch64-linux-android` (add `x86_64-linux-android` for emulator builds). The kumquat server is a Cargo dep of the compositor crate (`target_os="android"`-gated), so the same target also covers the gfxstream-bridge build; no extra toolchain. | same |
| Rust glibc targets (`build-mesa-gfxstream.sh` cross-builds Mesa's gfxstream-vk Rust pieces) | `rustup target add aarch64-unknown-linux-gnu` (and `rustup target add x86_64-unknown-linux-gnu` for the emulator bridge) | same |
| `bindgen` (Mesa's gfxstream-vk meson Rust bindings) | `cargo install bindgen-cli` | same |
| Cargo NDK (cargo subcommand — `cargo build` will fail with `error: no such command: ndk` if missing) | `cargo install cargo-ndk --version 4.1.2 --locked` (known-good; `cargo install cargo-ndk` for latest) | same |
| Android SDK + NDK | install Android Studio, or use `sdkmanager` directly. Android platform API 36 is required by `compileSdk`; NDK version pinned in `app/build.gradle.kts` (currently 27.2.12479018). The SDK's `cmdline-tools` (for `apkanalyzer`, used by `scripts/check-no-dev-code.sh` on the release APK) and `build-tools` (zipalign/apksigner/aapt2) are both needed for `scripts/build-release-apk.sh`. | same |
| Build basics | `base-devel`                                        | `build-essential pkg-config curl libarchive-tools`   |
| Meson + Ninja (libxkbcommon) | `meson ninja`                            | `meson ninja-build`                                  |
| `bison` (libxkbcommon's meson build and xkbcomp's `AC_PROG_YACC`) | in `base-devel`         | `bison`                                              |
| Wayland host tools (libhybris cross-build) | `wayland wayland-protocols` | `libwayland-dev libwayland-egl-backend-dev wayland-protocols` (Arch's `wayland` carries `wayland-egl-backend.h`; Debian splits it out, and libhybris's wayland EGL platform includes it) |
| Host sysroot + test app builds | `curl libarchive wayland` | `curl libarchive-tools libwayland-dev` |
| Autotools (libhybris cross-build) | `autoconf automake libtool` | `autoconf automake libtool libtool-bin` (Debian ships the `libtool` binary itself in `libtool-bin`, and `build-libhybris.sh`/`build-xwayland.sh` check for it) |
| `ltdl.m4` autoconf macros (libffi's `LT_SYS_SYMBOL_USCORE`, in the Xwayland dep chain) | in `libtool` | `libltdl-dev`                                        |
| Vulkan headers (libhybris cross-build) | `vulkan-headers`        | `libvulkan-dev`                                      |
| X11/xcb headers (libhybris's X11 EGL platform, `eglplatform_x11.so`) | `libx11 libxcb`   | `libx11-dev libx11-xcb-dev libxcb1-dev`              |
| `patchelf` (libhybris GL shims) | `patchelf`                  | `patchelf`                                           |
| `file` (libhybris build verify step) | `file`                 | `file`                                               |
| nginx (dev-time mirror cache, optional) | `nginx`                       | `nginx`                                              |
| podman (release builds + the F-Droid rig, rootless) | `podman`      | `podman`                                             |
| `apksigcopier` (release signing check) | `apksigcopier`             | `apksigcopier`                                       |

On a clean Debian trixie the whole set is one line — the same set the
F-Droid recipe installs, verified by building a release APK from scratch
in a container with none of it preinstalled:

```bash
apt install git openjdk-21-jdk rustup build-essential make pkg-config curl \
    libarchive-tools file meson ninja-build bison autoconf automake libtool \
    libtool-bin libltdl-dev perl python3 python3-libxml2 xsltproc libexpat1-dev \
    libwayland-dev libwayland-bin libwayland-egl-backend-dev wayland-protocols \
    libvulkan-dev patchelf libx11-dev libx11-xcb-dev libxcb1-dev \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu libc6-arm64-cross linux-libc-dev-arm64-cross
cargo install cargo-ndk --version 4.1.2 --locked
```

Three of those are easy to miss when the packaging differs from Arch's,
and each one fails deep into a cross-build rather than up front:
`bison` and `libtool-bin` (Arch folds both into `base-devel`),
`libwayland-egl-backend-dev` and `libltdl-dev` (Arch keeps both inside
`wayland` and `libtool`), and the `-dev` split for X11/xcb, which Arch
does not have at all.

That line covers the `libhybris,cpu` graphics set. The gfxstream and
libhybris-zink backends additionally need `bindgen` and the Mesa/sysroot
tooling described below; `proot` builds its own talloc.

JDK 26 is **not** supported for running this Gradle build — Gradle 8.12's
embedded Kotlin stack crashes while parsing Java version `26.0.1`. The repo
pins the Gradle daemon to Java 21 in `gradle/gradle-daemon-jvm.properties`, so
direct `./gradlew ...` works when a JDK 21 install is available even if the
shell's default `java` is newer.

`nginx` is only needed for the dev-time install caching proxy
(`scripts/cache-proxy.sh`, see notes/cache-proxy.md). Skip if you
don't iterate on installs.

`podman` and `apksigcopier` are only needed to cut a release or test the
F-Droid recipe — see "F-Droid buildserver rig" below. Skip for ordinary
dev builds. Release signing also needs build-tools 35 or newer, for
`apksigner --alignment-preserved`.

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

Gradle needs an Android SDK location. Set one of:

- repo-local `local.properties` with `sdk.dir=/path/to/Android/Sdk`
- `ANDROID_HOME=/path/to/Android/Sdk`
- `ANDROID_SDK_ROOT=/path/to/Android/Sdk`

`local.properties` is gitignored because the path is host-specific. On this
dev machine it is:

```properties
sdk.dir=/home/ai/Android/Sdk
```

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

`SOURCE_DATE_EPOCH` is exported onto every Gradle `Exec` task (so every
native build script and cargo run inherits it) for reproducible release
builds. `scripts/lib/repro.sh` is the single source of truth: it derives
the epoch from HEAD's commit time (0 outside a git checkout), and its
`repro_tar` wrapper packs the asset tars with fixed entry order, owner,
modes and mtime. Gradle reads both back via `scripts/lib/repro.sh
--epoch` / `--tar-args`. Set `SOURCE_DATE_EPOCH` yourself to override.
See [reproducible-builds.md](reproducible-builds.md).

## Vendored repos

All gitignored. The pinned commit + repo URL for every vendored git dep
lives in **[`deps/deps.list`](../deps/deps.list)** — single source
of truth. Build scripts source `scripts/lib/deps.sh` and call
`dep_ensure <name>`, which clones if missing and **errors loudly** if
the existing checkout is at the wrong commit (uncommitted edits are
silently tolerated as long as HEAD matches the pin).

On top of that, any `dep_ensure`/`dep_apply_patches` call first verifies
*every* existing checkout against its pin (missing checkouts are skipped —
they're cloned on demand), so a build that touches one dep fails on drift
in any other. Gradle builds also run `scripts/ensure-deps.sh --verify-all`
via the never-up-to-date root `verifyDeps` task, wired to every module's
`preBuild`, which catches drift even when every dep-consuming task is
cached — including a termux-derived module built alone. The settings-eval
`ensure-deps.sh termux-app` runs through `providers.exec`, so the
configuration cache records its output as an input and re-verifies pins
even on cache-hit builds.

Dep-built artifacts also track checkout *content*: every dep-artifact
Gradle task (`buildLibhybris`, `buildXwayland*`, …) declares
`scripts/ensure-deps.sh --tree-state <dep|dest-prefix/>...` — HEAD plus
a hash of tracked-file edits per consumed dep — as an input property,
so local edits in a dep tree, and their later discard by
`update-deps.sh`, rebuild the artifact instead of letting it ship
stale. A missing checkout fingerprints as "clean at pin" (what a fresh
clone produces), so deleting a drifted checkout also triggers a
rebuild. Untracked files are deliberately not fingerprinted — they
survive `dep_reset`, so they can't be silently discarded; when
iterating on one, run the build script directly. Expect one extra
(script-incremental) task re-run after a build that itself rewrites
tracked dep files (patch-set change, first autotools regen) — the
fingerprint settles on the next build.

`update-deps.sh` additionally `git clean -fdx`s a checkout when its pin
actually moves, so stale in-tree configure state (and untracked WIP)
doesn't leak across commits; a same-pin reset leaves untracked files
alone.

| Path                                  | Used by                                       |
|---------------------------------------|-----------------------------------------------|
| `./deps/libhybris/`                        | `scripts/build-libhybris.sh`              |
| `./deps/android-headers/`                  | `scripts/build-libhybris.sh`              |
| `./deps/libxkbcommon/`                     | `scripts/build-libxkbcommon.sh`                   |
| `./deps/proot/` (+ `./deps/proot-deps/talloc-*` tarball) | `scripts/build-proot.sh`                |
| `./deps/cleat/`                            | `tawcroot/build.sh` (host + device test runners) |
| `./deps/termux-app/`                       | Gradle included projects `:terminal-emulator` + `:terminal-view` (in-app terminal; ensured at settings-evaluation time by `settings.gradle.kts`) |
| `./deps/xwayland-src/<lib>/` (~22 repos)   | `scripts/build-xwayland.sh`               |
| `./deps/smithay/`                     | Rust compositor (`scripts/ensure-deps.sh smithay`; consumed via `[patch.crates-io]` path in `compositor/Cargo.toml`) |
| `./deps/mesa/`                             | `scripts/build-mesa-gfxstream.sh` (gfxstream-vk and Mesa-Zink assets) |
| `./deps/gfxstream/`                        | `scripts/build-gfxstream-backend.sh`      |
| `./deps/rutabaga_gfx/`                     | `scripts/ensure-deps.sh --patches rutabaga_gfx deps/rutabaga-patches/rutabaga_gfx`; Rust compositor kumquat server dep |
| `./deps/openssh-portable/`                 | `remote/sftp-server/build.sh` (remote access sftp-server) |

Two tarball deps (`talloc`, `libmd`) are *not* in `deps.list` — they
ship as release tarballs, not git repos, so their pin is a
version + sha256 pair in the build script itself:

| Dep     | Pinned in                    | Variables                              |
|---------|------------------------------|----------------------------------------|
| `libmd` | `scripts/build-xwayland.sh`  | `LIBMD_VERSION`, `LIBMD_SHA256`        |
| `talloc`| `scripts/build-proot.sh`     | `TALLOC_VERSION`, `TALLOC_SHA256`      |

Both go through `dep_fetch_tarball <url> <sha256> <dest>` from
`scripts/lib/deps.sh`, which downloads when the file is absent and
verifies the hash every time — so a truncated or tampered cached
download fails as loudly as a fresh bad one. To bump either, change the
version and the hash together; the mismatch error tells you the hash it
actually got.

The *extracts* are still not re-verified after unpacking: hand edits or
corruption in a `talloc-*`/`libmd` tree are invisible until the next
version bump (accepted — delete the extract to force a clean re-fetch).

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

### debootstrap (asset tar, no compilation)

`deps/debootstrap` (upstream salsa.debian.org, pinned in
`deps/deps.list`) is packed verbatim — entry script, `functions`,
`scripts/` with symlinks preserved — into
`app/build/generated/tawc-assets/debootstrap/debootstrap/debootstrap.tar`
by the `packDebootstrap` Gradle task. No host deps beyond `tar`.
Consumed at runtime by the Debian *packages* bootstrap flavor
(notes/installation.md "Bootstrap flavors"), which extracts it into the
per-install bootstrap workspace.

The generated dir is registered as an assets srcDir only on the build
types that ship the packages flavor — debug by default, never release
— so a production APK carries no debootstrap, and the pack task (plus
the `deps/debootstrap` checkout it needs) is skipped entirely when no
variant wants it. `-PtawcBootstrapPackages=true|false` overrides both
build types; see [installation.md](installation.md) "Bootstrap
flavors". Bump the pin deliberately via
`scripts/update-deps.sh debootstrap` and re-run the on-device packages
install afterwards — debootstrap is unpatched by design; if a local
patch ever becomes necessary, fork like libhybris rather than sedding
at build time.

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
then exposed in each rootfs at `/usr/lib/hybris/` — bound RO under
tawcroot, copied by `TawcInstaller`/`LibhybrisInstallProvider` under
proot/chroot (at install time and on first app start after an APK
upgrade).
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

#### Why `--no-as-needed` is baked into `CC`/`CXX`

Debian's gcc (the F-Droid image, so every release APK) links with
`--as-needed` by default; Arch's cross gcc doesn't. Under it the GL
shims lost their `DT_NEEDED` on `libGLESv2_hybris.so`, so
`libGL.so.1`/`libGLESv2.so.2` exported no GL symbols and every GTK app
and Firefox crashed (v4 release candidate). It has to be in `CC`/`CXX`:
libtool places `LDFLAGS` after the libraries, too late to apply. When
comparing a container build against a local one, diff `readelf -d`
`NEEDED` lists across the libhybris tar first.

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
Bundled into the APK by Gradle's `packMesaGfxstream<Abi>` and exposed
in every rootfs at `/usr/lib/gfxstream/` (tawcroot RO bind;
`BridgeInstallProvider` copy under proot/chroot). The same script also builds
the optional Mesa-Zink tarball consumed by `libhybris-zink` unless
Gradle passes `--no-zink` via `-PtawcGraphics=...`. Passing
`--no-gfxstream` builds only Mesa-Zink; passing both `--no-gfxstream`
and `--no-zink` is rejected because there is no Mesa output to build.
Mesa's `wayland-protocols` XML comes from the pinned
`deps/xwayland-src/wayland-protocols` checkout, not the host sysroot.
That keeps Mesa's generated protocol inputs in sync with the Mesa
source even when distro sysroot packages lag.

### Xwayland (binary + libs → ships in APK as asset)

Cross-built per APK ABI. NDK clang against bionic — same toolchain as
the Rust compositor. APK builds include it by default for every enabled
ABI; pass `-PtawcXwayland=false` to Gradle or
`--no-xwayland` to `scripts/build-app.sh` / `scripts/app-build-install.sh`
to skip building, packaging, extracting, and spawning it.

```bash
scripts/build-xwayland.sh           # incremental
scripts/build-xwayland.sh --abi=x86_64
scripts/build-xwayland.sh --clean   # wipe install + builddirs
scripts/build-xwayland.sh --only=libx11   # rebuild one stage
```

Output: `build/xwayland-<abi>/install/{bin/Xwayland,bin/xkbcomp,lib,share}`.
Gradle's `stageXwaylandJniLibs<Abi>` task copies the binaries + `.so` deps
into `app/src/main/jniLibs/<abi>/lib*.so` (so untrusted_app can
exec them out of `nativeLibraryDir`), and `packXwaylandShare` tars
the XKB data tree into `assets/xwayland/share.tar`.
`CompositorService.ensureXwaylandExtracted` extracts the share tar
and lays down `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` symlinks
into `nativeLibraryDir`.

Host packages (in addition to the always-required set above): `perl`
(needed by xorgproto/libxcb/font-util autotools macros), expat
headers (`expat` / `libexpat1-dev`, for the native wayland-scanner
below), and libltdl's autoconf macros (`libtool` / `libltdl-dev`, for
libffi's `LT_SYS_SYMBOL_USCORE`). Everything else (meson, ninja, autoconf, automake, libtool,
pkg-config, python3) is already required for libhybris.

The build does **not** use the host's `wayland-scanner`. The
`wayland-scanner` stage builds it from our own pinned libwayland tree
into `build/xwayland-<abi>/native/`, and `native.ini` (a meson native
machine file, generated next to `android-cross.ini`) puts that prefix
ahead of the host pkg-config path so every `native: true` scanner
lookup resolves there. This is not just tidiness: libwayland's own
cross build does `dependency('wayland-scanner', native: true, version:
meson.project_version())`, and meson reads a bare `version:` as `==`,
so a host wayland package that differs from our pin at all — e.g. host
1.26.0 vs pinned 1.25.0 — fails the build outright. Bumping the pin to
chase the host is not a fix; the next host upgrade (or any builder on
an older distro) breaks it again.

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

The compositor links the `remote/` crate (`tawc_remote`, remote access;
see [remote-access.md](remote-access.md)) as a path dependency, so
`remote/src` and `remote/Cargo.toml` are inputs of
`buildRustLibrary<Abi>`, and its crates are pinned in
`compositor/Cargo.lock` like the rest (the F-Droid recipe needs nothing
extra). TLS is rustls with the `ring` backend only: `aws-lc-rs` would
need cmake under cargo-ndk. `remote/` keeps its own `Cargo.lock` for
host tests (`cd remote && cargo test`); unlike the compositor it builds
and tests on the host.

### sftp-server (OpenSSH → ships in APK as jniLib)

Static bionic `sftp-server` for remote access sftp/scp, from the pinned
`openssh-portable` dep (a release tag: its generated `configure` is used,
no autotools needed). Needs only the NDK and `make`. The source is copied
into `build/sftp-server-<abi>/` so the checkout stays clean. Gradle's
`buildSftpServer<Abi>`; standalone:

```bash
remote/sftp-server/build.sh [--abi=aarch64|x86_64|both]
```

Output: `app/src/main/jniLibs/<abi>/libsftp-server.so`, installed into each
rootfs at `/usr/lib/tawc/sftp-server`. The bionic workarounds and the
passwd-lookup guard are in [remote-access.md](remote-access.md)
"sftp-server".

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

The host test build (`--abi=host`, driven by `tawcroot/Makefile`) is
sanitized: the cleat `tests` orchestrator gets ASan+UBSan, the host
`tawcroot`/`tawcroot-testhost` binaries get trap-mode UBSan. Needs
gcc's `libasan`/`libubsan` runtimes, which ship with the same
distro gcc packages listed above — no extra host package. The NDK
cross-builds (the shipped artifacts) are untouched.

See [tawcroot](tawcroot/README.md) for the design.

### ando client (→ ships in APK as jniLib)

The guest-side client for the ando broker (`ando <cmd>` — run an
Android command from inside the rootfs; see [ando.md](ando.md)).
Single-file C, NDK clang `-static` against bionic's libc.a (static so
the tawcroot loader needs no `/system/bin/linker64` or per-distro
glibc). Output: `app/src/main/jniLibs/<abi>/libando.so`; installed
into each rootfs at `/usr/local/bin/ando` by `AndoInstallProvider`.
Auto-invoked by Gradle's `buildAndo<Abi>` task; standalone:

```bash
tawcroot/ando/build.sh --abi=aarch64
tawcroot/ando/build.sh --abi=x86_64
tawcroot/ando/build.sh --abi=both
```

### APK assembly

```bash
scripts/build-app.sh
```

Builds `arm64-v8a` by default, or `x86_64` when `ANDROID_SERIAL` or
`.tawctarget` points at an emulator. Use `--abi=arm64-v8a`,
`--abi=x86_64`, or `--abi=both` to override.

Invokes the Rust compositor build, copies its output into
`jniLibs/<abi>/`; applies Smithay setup and, when gfxstream is enabled,
rutabaga setup; cross-builds proot (when enabled) and tawcroot; builds
the gfxstream host backend for each enabled ABI only when the gfxstream
backend is enabled; builds/packs Mesa gfxstream-vk and/or Mesa-Zink
assets when their backends are enabled; builds/packs libhybris for
arm64; builds/packs Xwayland for arm64 unless `--no-xwayland` is passed;
then produces
`app/build/outputs/apk/debug/app-debug.apk`.
Everything the supported install/runtime paths need ships inside this
APK.

Graphics backend builds are controlled by Gradle's
`-PtawcGraphics=libhybris,libhybris-zink,gfxstream,cpu`, or by the
wrapper flags `--no-gfxstream` and `--no-mesa`. Disabling gfxstream
also disables the compositor crate's kumquat/gfxstream Cargo feature
and drops `libgfxstream_backend.so`; disabling both gfxstream and
libhybris-zink skips `scripts/build-mesa-gfxstream.sh` entirely.
`scripts/build-release-apk.sh` defaults to `libhybris,cpu` so
production APKs do not ship libhybris-zink or gfxstream/kumquat unless
`--graphics=...` or `TAWC_RELEASE_GRAPHICS` opts them back in.

`-PtawcAllFilesAccess=false` strips `MANAGE_EXTERNAL_STORAGE` from the
manifest (build-type overlay `app/src/overlays/no-all-files-access/`)
for distribution channels that can't carry it; the app hides the
external-binds UI when the permission is absent. See
[external-binds.md](external-binds.md).

#### 16 KB page alignment

Every jniLib must have ELF LOAD segments aligned to ≥16 KB, or it
won't load on 16 KB-page devices (Pixel dev option, newer hardware),
and Android 15+ shows an "App Compatibility" dialog on first launch.
NDK r27 still links at 4 KB by default (r28+ defaults to 16 KB), so
every NDK link passes `-Wl,-z,max-page-size=16384`: the Xwayland
meson/autotools flags, the gfxstream backend cross file, proot
(`LDFLAGS` + `LOADER_LDFLAGS`), tawcroot, ando, sftp-server, and
termux's `libtermux.so` (`APP_SUPPORT_FLEXIBLE_PAGE_SIZES` set from the
root `build.gradle.kts`). cargo-ndk output and `libc++_shared.so` are
already 16 KB. Guest-side assets (libhybris, Mesa) use GNU ld's 64 KB
aarch64 default.

`scripts/build-app.sh` ends with `scripts/check-elf-alignment.sh`,
which fails on any under-aligned lib in the APK. Incremental builds
can keep stale 4 KB libs; `--clean` the offending build script.
Libs are stored compressed and extracted (`useLegacyPackaging`), so
zip-level 16 KB alignment doesn't matter.

### F-Droid buildserver rig (release builds + recipe testing)

```bash
scripts/fdroid/prepare.sh verify     # stage the local tree
scripts/fdroid/run.sh                # build it in F-Droid's image (hours)
scripts/fdroid/lint.sh               # lint/rewritemeta/schema the recipe
scripts/fdroid/clean.sh              # drop all rig state (GBs, re-downloads)
```

Runs `fdroid build` on `fdroid/me.phie.tawc.yml` inside F-Droid's own
buildserver image (`registry.gitlab.com/fdroid/fdroidserver:buildserver-trixie`)
under rootless podman, as close as it gets to how fdroiddata's CI does
it. State — container storage, the fdroiddata checkout, a mirror of this
repo, the caches and logs — lives under `build/fdroid/`.

Two modes:

- `verify` builds the working tree (dirty included, snapshotted into the
  mirror without touching any ref here) with `fdroid build --test`, and
  throws the APK away. Use it to check the recipe still builds.
- `release` builds a clean tree from a freshly cloned mirror, keeps the
  result, and copies it to
  `app/build/outputs/apk/release/me.phie.tawc_<version>-unsigned.apk`.
  This is the APK a release ships: building it in F-Droid's image at
  F-Droid's path is what makes their independent rebuild of the tag come
  out byte-identical, so they can distribute the maintainer-signed APK.
  `scripts/fdroid/compare-apks.py A.apk B.apk` is the entry-by-entry
  check. See [reproducible-builds.md](reproducible-builds.md).

`run.sh --cpuset-cpus <spec>` and `--fresh-cache` vary core count and
cache warmth, which is how a build gets checked for determinism.

### Working on the fdroiddata recipe

The recipe lives at `fdroid/me.phie.tawc.yml` here and is submitted from
a branch (`me.phie.tawc`) on the maintainer's fork,
`gitlab.com/sphi/fdroiddata`. To update it:

1. Copy `fdroid/me.phie.tawc.yml` over `metadata/me.phie.tawc.yml` in
   the fork checkout, then run `fdroid rewritemeta me.phie.tawc` in the
   buildserver image — fdroiddata CI enforces canonical formatting and
   strips every comment. The repo copy keeps its comments; the fork copy
   is the canonical one.
2. Rebase onto current `upstream/master` before submitting.
3. Keep it a single commit, message `New app: TAWC (me.phie.tawc)`,
   authored by the maintainer, with no trailers — that is fdroiddata's
   house style for new-app MRs.

Two traps, both hit for real:

- **GitLab refuses pushes from a shallow clone** ("shallow update not
  allowed"), and `--unshallow` on fdroiddata pulls its entire history.
  Use a blobless clone instead: `git clone --filter=blob:none` gives the
  full commit graph with blobs on demand, is not shallow, and lands in
  ~190 MB. `build/fdroiddata-full` is that clone.
- In a blobless clone with both `origin` (the fork) and `upstream`
  remotes, checking out `upstream/master` makes git lazily fetch blobs
  from the `origin` promisor, which does not have them:
  `fatal: git upload-pack: not our ref …`. Harmless when only committing
  a new file (the commit is built from the index), but verify the result
  with `git diff --name-status upstream/master HEAD` before pushing.

`fdroid checkupdates` writes `AutoName:` into the recipe when it is
absent and the CI job then fails its own `git diff --exit-code`, so that
field has to be present and correct.

### Third-party license text (checked-in asset)

```bash
scripts/gen-third-party-licenses.sh
```

Regenerates `app/src/main/assets/licenses.json`, the data behind
Settings > About > "Licenses" (`LicensesActivity` and
`LicenseSectionActivity`). The distributed APK is GPLv3 (termux-shared's
extra-keys widget), and the permissive licenses on everything else
require their notices to ship with the binary — this asset is how both
obligations are met. See [licensing.md](licensing.md).

Regular builds never run it: the output is checked in. Re-run it after
changing a Gradle dependency, a `deps/` pin, or a compositor crate, and
commit the result. Inputs, all read from the working tree:

- `LICENSE` / `LICENSE.MIT` — the GPLv3 text and TAWC's own terms
- `deps/**/{LICENSE,COPYING}*` — vendored native and Java sources
- `cargo metadata` for `compositor/`, with per-crate texts read out of
  the local `~/.cargo` registry checkout
- `./gradlew :app:dependencies --configuration releaseRuntimeClasspath`
  for Maven artifacts, mapped to licenses by the `GRADLE_LICENSES`
  table in the script
- `licenses/` — checked-in texts for the few artifacts whose license
  lives only in a POM or on a project website, or that are checked in
  directly (the Hack terminal font)

So it needs populated dep checkouts and a warm cargo registry. It fails
loudly rather than silently omitting a component: an unmapped Maven
coordinate or a vendored checkout with no license file is an error, so
new dependencies have to be classified before the file regenerates.

Output shape matters for the UI. Components are grouped by license
*family* so the index screen is ~14 rows rather than one endless page,
and identical texts are shared (deduplicated on a whitespace-normalized
key, so indentation differences don't scatter one Apache-2.0 into a
dozen entries). Texts are never rewritten for grouping — only the key is
normalized. Hard-wrapped prose is reflowed into single paragraphs so
Android can rewrap it to the screen; blocks that don't look like prose
stay verbatim and render monospace.

### App icon (checked-in generated assets)

**`app/icon.svg` is the source of truth.** Every other form of the mark is
generated from it by `scripts/gen-icon.sh`:

| Generated file | Where it shows up |
|----------------|-------------------|
| `app/src/main/res/drawable/ic_launcher_foreground.xml` | foreground layer of the adaptive launcher icon (`mipmap-anydpi-v26/ic_launcher.xml`) — the home screen, the app switcher, and pinned Linux-app shortcuts (`EntryShortcuts` falls back to `R.mipmap.ic_launcher`) |
| `app/src/main/res/drawable/ic_tawc_logo.xml` | the mark at full size, no safe-zone scale; the home drawer header and intro pane logo (`MainActivity`) |
| `app/src/main/res/values/icon_colors.xml` | `tawc_icon_bg`, the adaptive icon's background layer |
| `fastlane/metadata/android/en-US/images/icon.png` | the F-Droid store listing (512×512) |

`mipmap-anydpi-v26/ic_launcher.xml` is hand-written — it only wires the two
layers together and has no artwork in it.

To change the icon:

1. Edit `app/icon.svg` in Inkscape.
2. Run `scripts/gen-icon.sh`.
3. Commit the SVG and all four generated files together.

```bash
scripts/gen-icon.sh           # regenerate everything
scripts/gen-icon.sh --check   # verify the checked-in files match the SVG
scripts/gen-icon.sh --size=N  # store icon at another size
```

Nothing runs the generator automatically: the app build must not depend on
an SVG rasteriser, and the icon changes about once a year. `--check` is the
guard against the checked-in files drifting from the source; it needs no
rasteriser for the vector outputs.

Constraints on the SVG, all enforced by the script — it fails naming what
it could not translate rather than quietly dropping it from the icon:

- **Paths only.** The translation to Android's vector format is structural
  (`pathData` is SVG path syntax), so shapes, strokes and text have to
  become paths first: in Inkscape, select all, then Path > Object to Path
  (and Path > Stroke to Path for strokes). Empty text frames left behind by
  that conversion are ignored.
- **Flat fills.** No gradients or patterns.
- **Group transforms** may be translate/scale/rotate, or a matrix with no
  rotation or skew; each becomes a nested Android `<group>`.
- **The background colour is the SVG page colour** (Inkscape: Document
  Properties > Background), not a drawn rectangle — that is what
  `icon_colors.xml` is generated from.

The safe-zone scale (0.60) lives in the script. Android masks the outer
edge of an adaptive icon away, so the foreground has to sit inside the
central safe zone; the store PNG uses the same scale so it matches what
launchers actually draw.

### Store metadata (checked-in assets)

`fastlane/metadata/android/en-US/` holds the F-Droid store listing in the
standard fastlane layout — F-Droid reads it straight out of the source
repo, so it ships by being committed, not by being built:

| File                              | Limit  | Notes                                    |
|-----------------------------------|--------|------------------------------------------|
| `title.txt`                       | 50     | keep in sync with the `app_name` string  |
| `short_description.txt`           | 80     | one line, shown in listings              |
| `full_description.txt`            | 4000   | the listing body                         |
| `changelogs/<versionCode>.txt`    | 500    | one per release; `1.txt` for `v1`        |
| `images/icon.png`                 | 512×512| generated, see above                     |
| `images/phoneScreenshots/*.png`   | —      | ordered by filename                      |

Each release needs a new `changelogs/<versionCode>.txt` — that is the only
recurring F-Droid chore once the recipe is merged (see
[release.md](release.md)).

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

**App IDs: dev vs production.** Debug builds are
`me.phie.tawc.dev`, labelled "TAWC (dev)" with a purple launcher icon; release
builds (the ones the user signs) are `me.phie.tawc`. A phone holding a
release-signed `me.phie.tawc` can't take a debug-signed build under the
same ID without an uninstall (which deletes its distros), so dev
iteration always uses the `.dev` ID beside it. Each ID has its own data
dir (`/data/data/<id>/`), so install a distro into the dev app first.
Only run one of the two compositors at a time.

Mechanics: the debug `applicationIdSuffix` comes from
`-PtawcAppIdSuffix` (default `.dev`; empty for the plain ID). Host
tooling reads `TAWC_PACKAGE` (default `me.phie.tawc.dev`, resolved by
`scripts/lib/tawc-package.sh`, and `app_package()` in the integration
crate): `build-app.sh`, `app-build-install.sh`, `tawc-exec.sh` /
`rootfs-run.sh`, the install-id lookup and the integration suite all
follow it. The exec broker socket is `<id>.exec`. `TAWC_PACKAGE=me.phie.tawc`
builds a debug APK under the plain ID — only for a device without a
release-signed install, and only when asked. The dev label lives in
`app/src/debug/res/values/strings.xml`; the purple launcher foreground
`app/src/debug/res/drawable/ic_launcher_foreground.xml` is generated by
`scripts/gen-icon.sh`. Only the launcher icon is purple — in-app icons and
colours match release.

Note: `am start` directly into `.compositor.CompositorActivity` does
not work — go through `MainActivity` (the script does this).

After reinstalling, the compositor restarts with a new Wayland socket.
Any running chroot clients (Firefox, etc.) will be connected to the
old socket and show black screens — kill and relaunch them.

Installing or upgrading the APK causes the next app start to re-extract
bundled runtime assets and re-run `TawcInstaller` against existing
rootfs metadata when the `tawcStamp` changes. Under tawcroot the
libhybris / gfxstream / Mesa-Zink trees are RO-bound from the extract,
so they track the APK with no per-rootfs copy; proot/chroot rootfses
get real-file copies under the same `/usr/lib/...` namespaces via the
provider/manifest mechanism. See notes/installation.md "Copy vs bind".

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

## App unit tests

Host-side JUnit tests for the Kotlin app live in `app/src/test/`
(currently metadata/JSON parsing — see
[external-binds.md](external-binds.md)):

```bash
./gradlew :app:testDebugUnitTest
```

Deps: `junit:junit` plus the real `org.json:json` artifact, which
shadows the throw-on-use stubs in the mockable android.jar so
`JSONObject`-based code runs off-device.
