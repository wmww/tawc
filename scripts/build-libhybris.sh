#!/bin/bash
# Cross-compile libhybris for aarch64 Linux (glibc) on the host.
#
# Replaces the old in-chroot autotools build (build-libhybris-chroot.sh)
# so libhybris can ship inside the APK as an asset rather than be built
# on each device. See issues/ship-libhybris-in-apk.md.
#
# libhybris MUST be glibc-linked: it is loaded by glibc Wayland clients
# inside the chroot, and its `hooks.c` calls glibc-internal symbols.
# Therefore we use the distro's aarch64-linux-gnu cross-compiler, NOT
# the Android NDK (which targets bionic).
#
# Output:
#   build/libhybris-aarch64/install/usr/local/{lib,share}/...
# matching the layout of the existing on-device install.
#
# Usage:
#   bash scripts/build-libhybris.sh           # incremental
#   bash scripts/build-libhybris.sh --clean   # wipe build tree first
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
for tool in autoreconf libtool wayland-scanner pkg-config make patchelf; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: '$tool' not on PATH. See notes/building.md." >&2
        exit 1
    }
done

# Vendored sources. libhybris is our fork (`wmww/libhybris`), our changes
# stack as clean commits on top of upstream — see CLAUDE.md "Libhybris
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
if [ ! -x "$LIBHYBRIS_DIR/hybris/configure" ] || [ "$CLEAN" = "1" ]; then
    echo "==> autogen.sh (NOCONFIGURE=1)"
    ( cd "$LIBHYBRIS_DIR/hybris" && NOCONFIGURE=1 ./autogen.sh ) 2>&1 | tail -5
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
if [ ! -f "$BUILD_DIR/Makefile" ] || [ "$CLEAN" = "1" ]; then
    echo "==> configure (host=$HOST_TRIPLE prefix=/usr/lib/hybris)"
    ( cd "$BUILD_DIR" && \
      ./configure \
        --host="$HOST_TRIPLE" \
        --prefix=/usr/lib/hybris \
        --libdir=/usr/lib/hybris \
        --with-android-headers="$ANDROID_HEADERS_DIR" \
        --enable-arch=arm64 \
        --enable-wayland \
        --enable-x11 \
        --disable-wayland_serverside_buffers \
        --enable-adreno-quirks \
        --enable-property-cache \
        --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64 \
      ) 2>&1 | tail -20
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
COMMON_TOPLEVEL=( "make -C common SUBDIRS=. libhybris-common.la" )
COMMON_INSTALL=( "make -C common install-libLTLIBRARIES" )
DIRS="include properties libsync platforms hardware ui gralloc egl glesv1 glesv2 hwc2 vulkan utils"

echo "==> make"
echo "    -- common (top-level + q linker only)"
make -C "$BUILD_DIR/common" -j"$(nproc)" SUBDIRS=. libhybris-common.la 2>&1 | tail -5
make -C "$BUILD_DIR/common/q" -j"$(nproc)" 2>&1 | tail -5
for dir in $DIRS; do
    if [ -d "$BUILD_DIR/$dir" ] && [ -f "$BUILD_DIR/$dir/Makefile" ]; then
        echo "    -- $dir"
        make -C "$BUILD_DIR/$dir" -j"$(nproc)" 2>&1 | tail -5
    fi
done

echo "==> install (DESTDIR=$PREFIX)"
make -C "$BUILD_DIR/common" install-libLTLIBRARIES DESTDIR="$PREFIX" 2>&1 | tail -3 || true
make -C "$BUILD_DIR/common/q" install DESTDIR="$PREFIX" 2>&1 | tail -3 || true
for dir in $DIRS; do
    if [ -d "$BUILD_DIR/$dir" ] && [ -f "$BUILD_DIR/$dir/Makefile" ]; then
        make -C "$BUILD_DIR/$dir" install DESTDIR="$PREFIX" 2>&1 | tail -3 || true
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
"$CC_BIN" -shared -fPIC \
    -o "$SHIM_DIR/libGLESv2.so.2" \
    "$REPO_DIR/deps/libhybris-shims/libglesv2-shim.c" \
    -L"$SHIM_DIR" -l:libGLESv2_hybris.so \
    -Wl,-rpath,/usr/lib/hybris/gl-shims \
    -Wl,--no-as-needed \
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
    -L"$SHIM_DIR" -l:libGL.so.1 \
    -Wl,-rpath,/usr/lib/hybris/gl-shims \
    -Wl,--no-as-needed \
    -Wl,-soname,libGL.so.1

echo "==> done. Output in $LIB_DIR"
