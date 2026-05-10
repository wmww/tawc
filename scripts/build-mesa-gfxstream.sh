#!/bin/bash
# Cross-compile Mesa's libvulkan_gfxstream.so + libvirtgpu_kumquat_ffi.a
# for aarch64 glibc.
#
# This is the chroot-side guest Vulkan driver for the gfxstream-bridge
# GPU path. See notes/gfxstream-bridge.md.
#
# Bridge architecture:
#   chroot: vulkan app -> libvulkan.so.1 -> libvulkan_gfxstream.so (this)
#                      -> kumquat protocol over Unix socket
#                      -> Android-side bridge (in-process compositor thread)
#                      -> system libvulkan.so + Adreno
#
# Why a separate libvulkan_gfxstream.so vs distro Mesa: distro Mesa
# packages already ship gfxstream-vk on aarch64 (Arch ARM, Debian) but
# WITHOUT `-Dvirtgpu_kumquat=true`. Without kumquat the driver only
# does DRM virtio-gpu transport, which we have no kernel for. We need
# the kumquat backend (Unix-socket transport, no kernel).
#
# Why we cargo-build the Rust pieces ourselves: Mesa's
# subprojects/packagefiles/*/meson.build hard-code `native: true` on
# every Rust crate's static_library. With `-Dvirtgpu_kumquat=true`
# meson tries to use the same proc-macro chain (cfg-if/syn/quote/
# proc-macro2/unicode-ident) for both build-machine targets (proc-
# macros) and host-machine targets (mesa3d_util etc.) and errors with
# "Tried to mix a build machine library with a host machine target".
# Plain cargo handles cross-builds + proc-macros transparently — so
# we patch Mesa with a `virtgpu_kumquat_external_ffi` option that
# resolves `dep_virtgpu_kumquat_ffi` via pkg-config from a separate
# cargo build instead. See deps/mesa-patches/mesa/.
#
# Output:
#   build/mesa-aarch64/install/usr/local/lib/libvulkan_gfxstream.so
#   build/mesa-aarch64/install/usr/local/share/vulkan/icd.d/gfxstream_vk_icd.aarch64.json
#
# Usage:
#   bash scripts/build-mesa-gfxstream.sh           # incremental
#   bash scripts/build-mesa-gfxstream.sh --clean   # wipe build tree first

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/deps.sh
source "$SCRIPT_DIR/lib/deps.sh"
MESA_DIR="$(dep_dir mesa)"
PATCH_DIR="$REPO_DIR/deps/mesa-patches/mesa"

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
RUST_TARGET="aarch64-unknown-linux-gnu"
command -v "$CC_BIN" >/dev/null || {
    echo "ERROR: $CC_BIN not on PATH (install aarch64 cross-toolchain — see notes/building.md)" >&2
    exit 1
}
for tool in meson ninja pkg-config cargo rustc wayland-scanner; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: '$tool' not on PATH. See notes/building.md." >&2
        exit 1
    }
done
rustup target list --installed 2>/dev/null | grep -q "^${RUST_TARGET}\$" || {
    echo "ERROR: rustup target ${RUST_TARGET} missing." >&2
    echo "       Install with: rustup target add ${RUST_TARGET}" >&2
    exit 1
}

# ── Vendored Mesa ──
dep_ensure mesa

# ── Apply patches (xwayland-style) ──
# Sentinel encodes the patch hash; if the patch set changes we re-apply.
PATCH_HASH="$(cat "$PATCH_DIR"/*.patch 2>/dev/null | sha1sum | cut -c1-12)"
PATCH_SENTINEL="$MESA_DIR/.tawc-patches-applied-$PATCH_HASH"
if [ ! -f "$PATCH_SENTINEL" ]; then
    rm -f "$MESA_DIR"/.tawc-patches-applied-*
    git -C "$MESA_DIR" reset --hard --quiet HEAD
    git -C "$MESA_DIR" clean -fdx --quiet
    for p in "$PATCH_DIR"/*.patch; do
        [ -f "$p" ] || continue
        echo "==> patch mesa: $(basename "$p")"
        ( cd "$MESA_DIR" && patch -p1 --no-backup-if-mismatch < "$p" >/dev/null )
    done
    touch "$PATCH_SENTINEL"
fi

# ── Build tree ──
OUT_DIR="$REPO_DIR/build/mesa-aarch64"
PREFIX="$OUT_DIR/install"
PC_DIR="$OUT_DIR/pkgconfig"
STUB_DIR="$OUT_DIR/stubs"
BUILD_DIR="$MESA_DIR/build-aarch64"
CARGO_DIR="$OUT_DIR/cargo"

if [ "$CLEAN" = "1" ]; then
    echo "==> wiping $OUT_DIR and $BUILD_DIR"
    rm -rf "$OUT_DIR" "$BUILD_DIR"
fi

mkdir -p "$OUT_DIR" "$PREFIX" "$PC_DIR" "$STUB_DIR" "$CARGO_DIR"

# ── Cargo build of virtgpu_kumquat_ffi ──
# This sidesteps the meson Rust-subproject mess; we hand the resulting
# static lib + Mesa's existing virtgpu_kumquat_ffi.h to meson via
# pkg-config and the patched `-Dvirtgpu_kumquat_external_ffi=true`.
mkdir -p "$CARGO_DIR/.cargo"
cat >"$CARGO_DIR/.cargo/config.toml" <<EOF
[target.${RUST_TARGET}]
linker = "$CC_BIN"
EOF
echo "==> cargo build virtgpu_kumquat_ffi (target=${RUST_TARGET}, release)"
( cd "$MESA_DIR" && \
  CARGO_TARGET_DIR="$CARGO_DIR/target" \
  CARGO_HOME="$CARGO_DIR" \
  cargo build -p virtgpu_kumquat_ffi --target="$RUST_TARGET" --release )

KUMQUAT_FFI_A="$CARGO_DIR/target/${RUST_TARGET}/release/libvirtgpu_kumquat_ffi.a"
KUMQUAT_FFI_INC="$MESA_DIR/src/virtio/virtgpu_kumquat_ffi/include"
[ -f "$KUMQUAT_FFI_A" ] || { echo "ERROR: cargo did not produce $KUMQUAT_FFI_A" >&2; exit 1; }
[ -f "$KUMQUAT_FFI_INC/virtgpu_kumquat_ffi.h" ] || { echo "ERROR: header missing at $KUMQUAT_FFI_INC" >&2; exit 1; }

# ── Stub .so files for runtime deps ──
# Same trick as build-libhybris.sh: the chroot supplies the real
# implementations at load time, we just need stubs to record DT_NEEDED.
# Mesa references some interface symbols (wl_*_interface) at link time
# from wayland-scanner-generated protocol files, so we copy real aarch64
# .so files for libwayland-{client,server} when available — the
# build/aarch64-sysroot pull from the device's installed rootfs is the
# convenient source. Pure stubs (libudev, libffi) are fine.
gen_stub() {
    local soname="$1"
    if [ ! -f "$STUB_DIR/$soname" ]; then
        "$CC_BIN" -shared -nostdlib -Wl,-soname,"$soname" \
            -x c /dev/null -o "$STUB_DIR/$soname"
    fi
    local base="${soname%.so.*}.so"
    ln -sf "$soname" "$STUB_DIR/$base"
}

# Real aarch64 .so for symbol-rich deps (wayland generates .data.rel
# entries that reference wl_*_interface — empty stub fails to link).
SYSROOT_LIB="$REPO_DIR/build/aarch64-sysroot/usr/lib"
copy_or_stub() {
    local soname="$1"
    if [ -f "$SYSROOT_LIB/$soname" ]; then
        cp -L "$SYSROOT_LIB/$soname" "$STUB_DIR/$soname"
        local base="${soname%.so.*}.so"
        ln -sf "$soname" "$STUB_DIR/$base"
    else
        gen_stub "$soname"
    fi
}
copy_or_stub libwayland-client.so.0
copy_or_stub libwayland-server.so.0
copy_or_stub libdrm.so.2
copy_or_stub libudev.so.1
copy_or_stub libffi.so.8

# ── Synthetic pkg-config files ──
# Headers come from the host (wayland-client.h, xf86drm.h, vulkan/*.h
# are ABI-portable C). Library .so paths point to our stub dir for
# runtime DT_NEEDED entries. virtgpu_kumquat_ffi points at the cargo
# static lib + Mesa's own header dir (in-tree, untouched).
HOST_WAYLAND_INC="$(pkg-config --variable=includedir wayland-client)"
HOST_WAYLAND_PROT_DATADIR="$(pkg-config --variable=pkgdatadir wayland-protocols)"
HOST_WAYLAND_SCANNER="$(command -v wayland-scanner)"

write_pc() {
    local name="$1" cflags="$2" libs="$3" version="${4:-1.0.0}"
    cat >"$PC_DIR/$name.pc" <<EOF
Name: $name
Description: $name (cross stub)
Version: $version
Cflags: $cflags
Libs: $libs
EOF
}

write_pc wayland-client "-I$HOST_WAYLAND_INC" "-L$STUB_DIR -lwayland-client" "1.25.0"
write_pc wayland-server "-I$HOST_WAYLAND_INC" "-L$STUB_DIR -lwayland-server" "1.25.0"
write_pc libdrm "-I/usr/include -I/usr/include/libdrm" "-L$STUB_DIR -ldrm" "$(pkg-config --modversion libdrm)"
write_pc libudev "-I/usr/include" "-L$STUB_DIR -ludev" "$(pkg-config --modversion libudev)"
write_pc libffi "-I$(pkg-config --variable=includedir libffi)" "-L$STUB_DIR -lffi" "$(pkg-config --modversion libffi)"

# virtgpu_kumquat_ffi: static lib from cargo + header from Mesa source.
# Static archive built by cargo pulls in libstd's stubs for libgcc_s,
# pthread, dl, util, m, c, rt — those are fine because libvulkan_gfxstream.so
# already DT_NEEDED's libstdc++ / libm / libc and the chroot has them.
write_pc virtgpu_kumquat_ffi \
    "-I$KUMQUAT_FFI_INC" \
    "$KUMQUAT_FFI_A -lpthread -ldl -lutil -lrt -lm" \
    "0.1.76"

cat >"$PC_DIR/wayland-scanner.pc" <<EOF
wayland_scanner=$HOST_WAYLAND_SCANNER
Name: wayland-scanner
Description: host wayland-scanner
Version: 1.25.0
Cflags:
Libs:
EOF

cat >"$PC_DIR/wayland-protocols.pc" <<EOF
pkgdatadir=$HOST_WAYLAND_PROT_DATADIR
Name: wayland-protocols
Description: host wayland-protocols
Version: 1.45
Cflags:
Libs:
EOF

# ── meson cross file ──
# `-idirafter /usr/include` makes the host's headers available WITHOUT
# clobbering the cross-toolchain's own /usr/aarch64-linux-gnu/include
# (which has the right stdint.h with 64-bit uintptr_t). `idirafter`
# searches last so the cross-toolchain's headers win for arch-sensitive
# files; the host's headers are only consulted for things the cross
# toolchain doesn't ship (wayland-client.h, xf86drm.h, vulkan/*.h).
# `-isystem /usr/include` would put it BEFORE the cross gcc's targets
# and break uintptr_t — confirmed by `-fpermissive` cast errors on
# DrmVirtGpuDevice.cpp during initial attempts.
cat >"$OUT_DIR/cross.txt" <<EOF
[binaries]
c = ['$CC_BIN', '-idirafter', '/usr/include']
cpp = ['$CXX_BIN', '-idirafter', '/usr/include']
ar = '${HOST_TRIPLE}-ar'
strip = '${HOST_TRIPLE}-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PC_DIR'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

# ── Configure + build ──
# `-Dvirtgpu_kumquat=true -Dvirtgpu_kumquat_external_ffi=true` enables
# kumquat without pulling Mesa's Rust subprojects into the meson graph.
# Required by patch deps/mesa-patches/mesa/02-meson-external-kumquat-ffi.patch.
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    PKG_CONFIG_LIBDIR="$PC_DIR" meson setup "$BUILD_DIR" "$MESA_DIR" \
        --cross-file "$OUT_DIR/cross.txt" \
        -Dvulkan-drivers=gfxstream \
        -Dgallium-drivers= \
        -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Degl=disabled \
        -Dglx=disabled -Dvideo-codecs= \
        -Dvirtgpu_kumquat=true \
        -Dvirtgpu_kumquat_external_ffi=true \
        -Dplatforms=wayland \
        -Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dshared-glapi=disabled -Dllvm=disabled -Dxmlconfig=disabled \
        -Ddisplay-info=disabled \
        --buildtype release -Ddefault_library=shared --prefix /usr/local
fi
ninja -C "$BUILD_DIR"

# ── Stage outputs ──
mkdir -p "$PREFIX/usr/local/lib" "$PREFIX/usr/local/share/vulkan/icd.d"
cp "$BUILD_DIR/src/gfxstream/guest/vulkan/libvulkan_gfxstream.so" \
   "$PREFIX/usr/local/lib/"
cp "$BUILD_DIR/src/gfxstream/guest/vulkan/gfxstream_vk_icd.aarch64.json" \
   "$PREFIX/usr/local/share/vulkan/icd.d/"

echo "==> built libvulkan_gfxstream.so:"
ls -la "$PREFIX/usr/local/lib/libvulkan_gfxstream.so"
echo "==> ICD JSON:"
cat "$PREFIX/usr/local/share/vulkan/icd.d/gfxstream_vk_icd.aarch64.json"
