#!/bin/bash
# Cross-compile libhybris for aarch64 Linux (glibc) on the host.
#
# Replaces the old in-chroot autotools build (build-libhybris-chroot.sh)
# so libhybris can ship inside the APK as an asset rather than be built
# on each device. See notes/building.md.
#
# libhybris MUST be glibc-linked: it is loaded by glibc Wayland clients
# inside the chroot, and its `hooks.c` calls glibc-internal symbols.
# Therefore we use the distro's aarch64-linux-gnu cross-compiler, NOT
# the Android NDK (which targets bionic).
#
# Output:
#   build/libhybris-aarch64/install/usr/lib/hybris/...
# matching the rootfs path LibhybrisInstallProvider copies into.
#
# Usage:
#   scripts/build-libhybris.sh           # incremental
#   scripts/build-libhybris.sh --clean   # wipe build tree first
#
# Build-time deps documented in notes/building.md ("Building libhybris").
# Keep that doc in sync with this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/deps.sh
source "$SCRIPT_DIR/lib/deps.sh"
LIBHYBRIS_DIR="$(dep_dir libhybris)"
ANDROID_HEADERS_DIR="$(dep_dir android-headers)"

CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done

# ── Toolchain ──
HOST_TRIPLE="aarch64-linux-gnu"
CC_BIN="${HOST_TRIPLE}-gcc"
CXX_BIN="${HOST_TRIPLE}-g++"
command -v "$CC_BIN" >/dev/null || {
    echo "ERROR: $CC_BIN not on PATH." >&2
    echo "       Install the aarch64 glibc cross-toolchain:" >&2
    echo "         Arch:   pacman -S aarch64-linux-gnu-{gcc,binutils,glibc,linux-api-headers}" >&2
    echo "         Debian: apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu" >&2
    echo "       See notes/building.md for the full list." >&2
    exit 1
}
command -v "$CXX_BIN" >/dev/null || {
    echo "ERROR: $CXX_BIN not on PATH (need the C++ frontend too)." >&2
    exit 1
}

# Host tools libhybris's autotools driver invokes during the build,
# plus patchelf for renaming the libhybris GLESv2 soname when building
# the GL shims.
for tool in autoreconf libtool wayland-scanner pkg-config make patchelf file; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: '$tool' not on PATH. See notes/building.md." >&2
        exit 1
    }
done

# Vendored sources. libhybris is our fork (`wmww/libhybris`), our changes
# stack as clean commits on top of upstream — see AGENTS.md "Libhybris
# fork" and `libhybris/TAWC_FORK.md`. android-headers is upstream Halium.
# Both pins live in `deps/deps.list`; `dep_ensure` clones if missing
# and errors loudly if the existing checkout is at the wrong commit.
dep_ensure libhybris
dep_ensure android-headers

# ── Build tree ──
# We build in-tree (BUILD_DIR == LIBHYBRIS_DIR/hybris) because some
# libhybris subdirs (egl/platforms/wayland) reference headers
# wayland-scanner generates into platforms/common with -I against the
# source path, which only works when builddir == srcdir. Out-of-tree
# would require makefile patches across multiple subdirs.
BUILD_DIR="$LIBHYBRIS_DIR/hybris"
OUT_DIR="$REPO_DIR/build/libhybris-aarch64"
PREFIX="$OUT_DIR/install"
PC_DIR="$OUT_DIR/pkgconfig"
STUB_DIR="$OUT_DIR/stubs"

if [ "$CLEAN" = "1" ]; then
    echo "==> distclean (in-tree)"
    if [ -f "$BUILD_DIR/Makefile" ]; then
        ( cd "$BUILD_DIR" && make distclean ) >/dev/null 2>&1 || true
    fi
    rm -rf "$OUT_DIR"
fi
mkdir -p "$OUT_DIR" "$PREFIX" "$PC_DIR" "$STUB_DIR"

# ── Logged build steps ──
# Run a noisy command, showing only a short tail on success but the full
# log on failure. Piping straight to `tail` (the old approach) could drop
# the actual error — e.g. a linker "cannot find .../crtbeginS.o" pushed
# out of view by trailing deprecation warnings.
BUILD_LOG="$OUT_DIR/build.log"
run_logged() {
    if "$@" >"$BUILD_LOG" 2>&1; then
        tail -n 5 "$BUILD_LOG"
    else
        echo "ERROR: command failed: $*" >&2
        echo "       full log ($BUILD_LOG) follows:" >&2
        cat "$BUILD_LOG" >&2
        exit 1
    fi
}

# ── Stub .so files ──
# libhybris's plugins DT_NEEDED-link to wayland-client/server/egl and
# vulkan; the chroot supplies the real implementations at load time.
# We only need stubs to keep ld.lld happy at link time. Each stub is an
# empty shared library with the correct SONAME so DT_NEEDED is recorded.
gen_stub() {
    local soname="$1"
    if [ ! -f "$STUB_DIR/$soname" ]; then
        echo "==> stub $soname"
        "$CC_BIN" -shared -nostdlib -Wl,-soname,"$soname" \
            -x c /dev/null -o "$STUB_DIR/$soname"
        # Provide an unversioned symlink so `-lwayland-client` resolves.
        local base="${soname%.so.*}.so"
        ln -sf "$soname" "$STUB_DIR/$base"
    fi
}
gen_stub libwayland-client.so.0
gen_stub libwayland-server.so.0
gen_stub libwayland-egl.so.1
gen_stub libvulkan.so.1
# X11 EGL platform plugin: chroot ships real libX11/libxcb at runtime,
# we just need empty SONAMEd stubs to satisfy the cross-link.
gen_stub libX11.so.6
gen_stub libxcb.so.1
gen_stub libX11-xcb.so.1

# ── Synthetic pkg-config files ──
# We point at the *host's* wayland/vulkan headers (plain C, ABI-clean)
# and our stub .so directory for the link step. The host's wayland-
# scanner and wayland-protocols XML are reused — they're build tools,
# not target binaries.
HOST_WAYLAND_INCLUDE="$(pkg-config --variable=includedir wayland-client 2>/dev/null || echo /usr/include)"
HOST_WAYLAND_DATADIR="$(pkg-config --variable=pkgdatadir wayland-client 2>/dev/null || echo /usr/share/wayland)"
HOST_WAYLAND_PROTOCOLS_DATADIR="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)"
HOST_WAYLAND_SCANNER="$(command -v wayland-scanner)"
HOST_VULKAN_INCLUDE="$(pkg-config --variable=includedir vulkan 2>/dev/null || echo /usr/include)"
[ -f "$HOST_VULKAN_INCLUDE/vulkan/vulkan.h" ] || {
    echo "ERROR: missing Vulkan headers at $HOST_VULKAN_INCLUDE/vulkan/vulkan.h" >&2
    echo "       Install vulkan-headers (Arch) or libvulkan-dev (Debian). See notes/building.md." >&2
    exit 1
}
# X11 / xcb headers — plain C, ABI-portable across glibc-aarch64 and host.
HOST_X11_INCLUDE="$(pkg-config --variable=includedir x11 2>/dev/null || echo /usr/include)"
HOST_XCB_INCLUDE="$(pkg-config --variable=includedir xcb 2>/dev/null || echo /usr/include)"

write_pc() {
    local name="$1" cflags="$2" libs="$3"
    cat >"$PC_DIR/$name.pc" <<EOF
Name: $name
Description: target-side $name (synthesised for aarch64 cross-build)
Version: 1.22.0
Cflags: $cflags
Libs: $libs
EOF
}
write_pc wayland-client "-I$HOST_WAYLAND_INCLUDE" "-L$STUB_DIR -lwayland-client"
write_pc wayland-server "-I$HOST_WAYLAND_INCLUDE" "-L$STUB_DIR -lwayland-server"
write_pc wayland-egl    "-I$HOST_WAYLAND_INCLUDE" "-L$STUB_DIR -lwayland-egl"
write_pc vulkan         "-I$HOST_VULKAN_INCLUDE"  "-L$STUB_DIR -lvulkan"
write_pc x11            "-I$HOST_X11_INCLUDE"     "-L$STUB_DIR -lX11"
write_pc xcb            "-I$HOST_XCB_INCLUDE"     "-L$STUB_DIR -lxcb"
write_pc x11-xcb        "-I$HOST_X11_INCLUDE"     "-L$STUB_DIR -lX11-xcb"

# wayland-scanner.pc and wayland-protocols.pc are read for variables
# (wayland_scanner, pkgdatadir) by libhybris's autotools, not for
# Cflags/Libs. Mirror the host pkg-config metadata.
cat >"$PC_DIR/wayland-scanner.pc" <<EOF
wayland_scanner=$HOST_WAYLAND_SCANNER
Name: wayland-scanner
Description: host wayland-scanner tool
Version: 1.22.0
EOF
cat >"$PC_DIR/wayland-protocols.pc" <<EOF
pkgdatadir=$HOST_WAYLAND_PROTOCOLS_DATADIR
Name: wayland-protocols
Description: host wayland-protocols data
Version: 1.32
EOF

# ── Toolchain env ──
export CC="$CC_BIN"
export CXX="$CXX_BIN"
export AR="${HOST_TRIPLE}-ar"
export STRIP="${HOST_TRIPLE}-strip"
export RANLIB="${HOST_TRIPLE}-ranlib"
export LD="${HOST_TRIPLE}-ld"
export NM="${HOST_TRIPLE}-nm"
export OBJDUMP="${HOST_TRIPLE}-objdump"
export PKG_CONFIG_PATH="$PC_DIR"
export PKG_CONFIG_LIBDIR="$PC_DIR"

# Some libhybris .cpp files (platforms/common/server_wlegl.cpp) include
# wayland-server.h unconditionally even when --disable-wayland_server‐
# side_buffers is set; in the old chroot build the host's wayland-server
# headers were on the default include path, which masked the issue.
# Replicate that on the host build, but use -idirafter so the wayland
# include path is consulted *after* the cross-compiler's own system
# headers — otherwise the host's x86_64 stdint.h shadows the cross-
# glibc's, and uintptr_t collapses to 32-bit because the host's
# bits/wordsize.h gates LP64 on __x86_64__ being defined.
export CPPFLAGS="-idirafter $HOST_WAYLAND_INCLUDE"

# ── autogen.sh (regen configure if needed) ──
if [ ! -x "$BUILD_DIR/configure" ] || [ "$CLEAN" = "1" ]; then
    echo "==> autogen.sh (NOCONFIGURE=1)"
    run_logged env -C "$BUILD_DIR" NOCONFIGURE=1 ./autogen.sh
fi

# ── configure ──
# `--prefix=/usr/lib/hybris --libdir=/usr/lib/hybris` is what makes
# libhybris bake the on-device install path into its .so files
# directly:
#
#   - libtool stamps DT_RUNPATH=$libdir into every shared library, so
#     libhybris-common's DT_NEEDED libs (libEGL → libhybris-common,
#     etc.) resolve through /usr/lib/hybris on the device without an
#     LD_LIBRARY_PATH override.
#   - The PKGLIBDIR macro (consumed by hybris/{egl,vulkan}/ws.c to
#     dlopen `eglplatform_<name>.so` / `vulkanplatform_<name>.so`) is
#     `$(pkglibdir)/` = `$libdir/$PACKAGE/` = `/usr/lib/hybris/libhybris/`.
#   - LINKER_PLUGIN_DIR (common/hooks.c → bionic linker plugin dlopen)
#     is `$(libdir)/libhybris/linker` = `/usr/lib/hybris/libhybris/linker`.
#
# Both line up with [LibhybrisInstallProvider]'s GUEST_PLUGIN_DIR /
# `…/linker`, so the Kotlin side doesn't need HYBRIS_*_DIR env-var
# overrides anymore. (LD_LIBRARY_PATH is still needed for the
# by-name `dlopen("libEGL.so")` first-level lookup since we don't
# ship --enable-glvnd; see notes/wsi-layer.md.)
CONFIGURE_ARGS=(
    --host="$HOST_TRIPLE"
    --prefix=/usr/lib/hybris
    --libdir=/usr/lib/hybris
    --with-android-headers="$ANDROID_HEADERS_DIR"
    --enable-arch=arm64
    --enable-wayland
    --enable-x11
    --disable-wayland_serverside_buffers
    --enable-adreno-quirks
    --enable-mali-quirks
    --enable-property-cache
    --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64
)

# The build tree is in-tree (`deps/libhybris/hybris/`), so leftover
# configure state survives a host-side build/ wipe and can be stale in
# ways that break the build or misplace artefacts:
#   - changed configure args (e.g. an old --prefix makes `make install`
#     land binaries at the old path, tripping the verify step below with
#     all libs "missing");
#   - a host cross-gcc upgrade (libtool bakes absolute paths to the old
#     gcc's CRT objects, e.g. `.../aarch64-linux-gnu/15.2.0/crtbeginS.o`,
#     so every link fails with "cannot find crtbeginS.o").
# So stamp a fingerprint of both after each configure, and reconfigure
# from scratch whenever the recorded one doesn't match.
FINGERPRINT="crt=$("$CC_BIN" -print-file-name=crtbeginS.o) args=${CONFIGURE_ARGS[*]}"
STAMP="$BUILD_DIR/.tawc-configure-stamp"
NEED_CONFIGURE=0
if [ "$CLEAN" = "1" ] || [ ! -f "$BUILD_DIR/Makefile" ]; then
    NEED_CONFIGURE=1
elif [ "$(cat "$STAMP" 2>/dev/null)" != "$FINGERPRINT" ]; then
    echo "==> stale build tree (configure args or cross-gcc changed) — distclean + reconfigure"
    ( cd "$BUILD_DIR" && make distclean ) >/dev/null 2>&1 || true
    NEED_CONFIGURE=1
fi
if [ "$NEED_CONFIGURE" = "1" ]; then
    echo "==> configure (host=$HOST_TRIPLE prefix=/usr/lib/hybris)"
    rm -f "$STAMP"
    run_logged env -C "$BUILD_DIR" ./configure "${CONFIGURE_ARGS[@]}"
    printf '%s\n' "$FINGERPRINT" >"$STAMP"
fi

# ── Build & install ──
# Per-subdir to match build-libhybris-chroot.sh's behaviour: the
# top-level make sometimes errors on a subdir we don't use but keeps
# going when invoked per-dir.
#
# common/ has four legacy bionic-linker plugin subdirs (mm/n/o for
# Android 6/7/8) that we don't need — at runtime, libhybris loads only
# the `q` plugin on Android 10+. Skipping them avoids unrelated build
# failures in code we'd never run.
#
# Install failures must propagate (no `|| true` swallowing), otherwise
# the verify step below trips on the missing artefacts and hides the
# underlying error.
DIRS="include properties libsync platforms hardware ui gralloc egl glesv1 glesv2 hwc2 vulkan utils"
JOBS="$(nproc)"

echo "==> make + install (DESTDIR=$PREFIX)"
echo "    -- common (top-level + q linker only)"
run_logged make -C "$BUILD_DIR/common" -j"$JOBS" SUBDIRS=. libhybris-common.la
run_logged make -C "$BUILD_DIR/common" install-libLTLIBRARIES DESTDIR="$PREFIX"
run_logged make -C "$BUILD_DIR/common/q" -j"$JOBS"
run_logged make -C "$BUILD_DIR/common/q" install DESTDIR="$PREFIX"
for dir in $DIRS; do
    if [ -f "$BUILD_DIR/$dir/Makefile" ]; then
        echo "    -- $dir"
        run_logged make -C "$BUILD_DIR/$dir" -j"$JOBS"
        run_logged make -C "$BUILD_DIR/$dir" install DESTDIR="$PREFIX"
    fi
done

# ── Verify ──
LIB_DIR="$PREFIX/usr/lib/hybris"
echo "==> verify"
MISSING=""
for lib in \
    libhybris-common.so.1.0.0 \
    libEGL.so.1.0.0 \
    libGLESv2.so.2.0.0 \
    libGLESv1_CM.so \
    libvulkan.so.1 \
    libsync.so.2.0.0 \
    libhybris/eglplatform_wayland.so \
    libhybris/eglplatform_x11.so \
    libhybris/vulkanplatform_wayland.so \
    libhybris/linker/q.so
do
    [ -f "$LIB_DIR/$lib" ] || MISSING="$MISSING $lib"
done
if [ -n "$MISSING" ]; then
    echo "ERROR: missing built libraries:$MISSING" >&2
    exit 1
fi

# Sanity-check ELF arch and that we got a glibc binary, not bionic.
elf_class=$(file "$LIB_DIR/libhybris-common.so.1.0.0")
case "$elf_class" in
    *"aarch64"*) ;;
    *) echo "ERROR: libhybris-common.so is not aarch64: $elf_class" >&2; exit 1 ;;
esac
if ! "${HOST_TRIPLE}-readelf" -d "$LIB_DIR/libhybris-common.so.1.0.0" \
        | grep -q 'NEEDED.*libc\.so\.6'; then
    echo "ERROR: libhybris-common.so doesn't NEEDED libc.so.6 (glibc); wrong libc?" >&2
    exit 1
fi

# ── GL shims ──
# Self-contained shim directory that goes alongside libhybris in the
# install tree. [LibhybrisInstallProvider] copies it into each rootfs
# at `/usr/lib/hybris/gl-shims/`, and [RootfsEnv] puts that path on
# LD_LIBRARY_PATH ahead of `/usr/lib/hybris` so `dlopen("libGL.so.1")`
# and `dlopen("libGLESv2.so.2")` land on our shims instead of glvnd /
# Mesa. See deps/libhybris-shims/libgl-shim.c for why.
#
# Layout:
#   gl-shims/
#     libGLESv2_hybris.so   -- libhybris's real GLESv2, soname renamed
#     libGLESv2.so.2        -- shim: GLX-NULL stubs + DT_NEEDED to ↑
#     libGLESv2.so          -- symlink → libGLESv2.so.2
#     libGL.so.1            -- symlink → libGLESv2.so.2 (yes, the shim)
#     libGL.so              -- shim: GLX-NULL stubs + DT_NEEDED libGL.so.1
#                              (so -lGL link-time finds it; soname libGL.so.1)
echo "==> GL shims"
SHIM_DIR="$LIB_DIR/gl-shims"
rm -rf "$SHIM_DIR"
mkdir -p "$SHIM_DIR"

cp "$LIB_DIR/libGLESv2.so.2.0.0" "$SHIM_DIR/libGLESv2_hybris.so"
patchelf --set-soname libGLESv2_hybris.so "$SHIM_DIR/libGLESv2_hybris.so"

# libGLESv2.so.2 — shim wrapping the renamed real lib.
#
# -Wl,--no-as-needed must come *before* the -l: that it applies to:
# --as-needed/--no-as-needed are positional in GNU ld, and the shim
# references no symbol from the real lib (the GLES entry points are
# resolved by dlsym at runtime, not by the linker), so the default
# --as-needed silently drops it and records no DT_NEEDED at all. The
# result is a libGLESv2.so.2 that dlopens fine but resolves zero GLES
# symbols, which Firefox WebRender reports as "gGLGetString: not found"
# and then aborts on.
"$CC_BIN" -shared -fPIC \
    -o "$SHIM_DIR/libGLESv2.so.2" \
    "$REPO_DIR/deps/libhybris-shims/libglesv2-shim.c" \
    "$REPO_DIR/deps/libhybris-shims/glx-stubs.c" \
    -Wl,--no-as-needed \
    -L"$SHIM_DIR" -l:libGLESv2_hybris.so \
    -Wl,-rpath,/usr/lib/hybris/gl-shims \
    -Wl,--version-script="$REPO_DIR/deps/libhybris-shims/glx-stubs.map" \
    -Wl,-soname,libGLESv2.so.2
ln -sf libGLESv2.so.2 "$SHIM_DIR/libGLESv2.so"

# libGL.so.1 is a symlink to the GLESv2 shim (forwards GLES, exposes
# NULL-GLX). libGL.so is a separately-built shim that DT_NEEDEDs
# libGL.so.1 — soname libGL.so.1 ensures `-lGL` consumers record
# libGL.so.1 as DT_NEEDED.
ln -sf libGLESv2.so.2 "$SHIM_DIR/libGL.so.1"
"$CC_BIN" -shared -fPIC \
    -o "$SHIM_DIR/libGL.so" \
    "$REPO_DIR/deps/libhybris-shims/libgl-shim.c" \
    "$REPO_DIR/deps/libhybris-shims/glx-stubs.c" \
    -Wl,--no-as-needed \
    -L"$SHIM_DIR" -l:libGL.so.1 \
    -Wl,-rpath,/usr/lib/hybris/gl-shims \
    -Wl,--version-script="$REPO_DIR/deps/libhybris-shims/glx-stubs.map" \
    -Wl,-soname,libGL.so.1

check_glx_export() {
    local so="$1"
    local sym="$2"
    if ! "${HOST_TRIPLE}-readelf" -Ws "$so" | awk -v sym="$sym" '
        $4 == "FUNC" && $5 == "GLOBAL" && $8 ~ ("^" sym "(@@|$)") { found = 1 }
        END { exit found ? 0 : 1 }
    '; then
        echo "ERROR: $so does not export $sym" >&2
        exit 1
    fi
}

for shim in "$SHIM_DIR/libGLESv2.so.2" "$SHIM_DIR/libGL.so"; do
    check_glx_export "$shim" glXGetCurrentContext
    check_glx_export "$shim" glXGetCurrentDisplay
    check_glx_export "$shim" glXChooseFBConfig
    check_glx_export "$shim" glXGetProcAddressARB
done

# The GLES symbols are deliberately *not* re-exported: glibc's dlsym
# searches a handle's dependency closure, so the DT_NEEDED is what makes
# them resolve. That makes DT_NEEDED the thing worth checking — a
# "does it export glGetString" readelf check would fail on a correct
# shim and pass on a broken one.
check_needed() {
    local so="$1"
    local lib="$2"
    if ! "${HOST_TRIPLE}-readelf" -d "$so" | grep -qF "Shared library: [$lib]"; then
        echo "ERROR: $so is missing DT_NEEDED $lib" >&2
        exit 1
    fi
}

check_needed "$SHIM_DIR/libGLESv2.so.2" libGLESv2_hybris.so
# libGL.so DT_NEEDEDs libGL.so.1, but that is a symlink to libGLESv2.so.2
# and a DT_NEEDED records the resolved library's SONAME, not the path used
# on the command line.
check_needed "$SHIM_DIR/libGL.so" libGLESv2.so.2

echo "==> done. Output in $LIB_DIR"
