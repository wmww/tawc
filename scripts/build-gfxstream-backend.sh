#!/bin/bash
# Cross-compile gfxstream's host renderer (libgfxstream_backend.so) for
# Android via the NDK. Vulkan-only build — no GLES, no composer — to
# keep the dep surface minimal for the bridge bring-up.
#
# Output (per --abi):
#   build/gfxstream-android-<abi>/libgfxstream_backend.so
#   app/src/main/jniLibs/<jni-abi>/libgfxstream_backend.so
#   app/src/main/jniLibs/<jni-abi>/libc++_shared.so
#
# This is the "host" side of the gfxstream protocol from the bridge's
# perspective: it decodes guest Vulkan command streams and dispatches
# them via dlopen("libvulkan.so") + VK_USE_PLATFORM_ANDROID_KHR. Linked
# into the kumquat server (rutabaga_gfx) which the compositor app runs
# in-process.
#
# See notes/gfxstream-bridge.md.
#
# Usage:
#   scripts/build-gfxstream-backend.sh                   # default --abi=aarch64
#   scripts/build-gfxstream-backend.sh --abi=x86_64
#   scripts/build-gfxstream-backend.sh --abi=both
#   scripts/build-gfxstream-backend.sh --clean

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/deps.sh
source "$SCRIPT_DIR/lib/deps.sh"
GFXSTREAM_DIR="$(dep_dir gfxstream)"
PATCH_DIR="$REPO_DIR/deps/gfxstream-patches/gfxstream"

CLEAN=0
ABIS=""
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --abi=aarch64) ABIS="aarch64" ;;
        --abi=x86_64)  ABIS="x86_64" ;;
        --abi=both)    ABIS="aarch64 x86_64" ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done
[ -n "$ABIS" ] || ABIS="aarch64"

# ── NDK + API level ──
ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK_DIR="$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)"
[ -d "$NDK_DIR" ] || { echo "ERROR: no NDK at $ANDROID_HOME/ndk/*"; exit 1; }
NDK_BIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
# API 28 is the native floor for this backend; the app minSdk is higher
# but this keeps the backend's direct NDK symbol requirements explicit.
# Need ≥ 26 for AHardwareBuffer_*, ≥ 28 for vkGetMemoryAndroidHardwareBufferANDROID.
API=28

for tool in meson ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not on PATH" >&2; exit 1; }
done

# ── Vendored gfxstream + patches ──
dep_apply_patches gfxstream "$PATCH_DIR"

build_one() {
    local abi="$1"
    local triple jni_abi cpu_family
    case "$abi" in
        aarch64)
            triple=aarch64-linux-android
            jni_abi=arm64-v8a
            cpu_family=aarch64
            ;;
        x86_64)
            triple=x86_64-linux-android
            jni_abi=x86_64
            cpu_family=x86_64
            ;;
        *) echo "ERROR: bad abi: $abi" >&2; return 1 ;;
    esac

    local CC="$NDK_BIN/${triple}${API}-clang"
    local CXX="$NDK_BIN/${triple}${API}-clang++"
    [ -x "$CC" ] || { echo "ERROR: NDK clang missing at $CC"; return 1; }

    local OUT_DIR="$REPO_DIR/build/gfxstream-android-$abi"
    local BUILD_DIR="$GFXSTREAM_DIR/build-android-$abi"

    if [ "$CLEAN" = "1" ]; then
        echo "==> [$abi] wiping $OUT_DIR and $BUILD_DIR"
        rm -rf "$OUT_DIR" "$BUILD_DIR"
    fi

    mkdir -p "$OUT_DIR"

    # ── Header shims ──
    # NDK's sysroot doesn't ship cutils/native_handle.h (libcutils is part
    # of AOSP, not the NDK). gfxstream's vk_android_native_buffer_gfxstream.h
    # unconditionally includes it on Android. Make a shims dir holding only
    # the files we need from third_party/android/include — we deliberately
    # don't expose the android/, vndk/ subdirs there (they would shadow the
    # NDK's own android/* headers).
    local SHIMS_DIR="$OUT_DIR/include-shims"
    mkdir -p "$SHIMS_DIR/cutils"
    cp -f "$GFXSTREAM_DIR/third_party/android/include/cutils/native_handle.h" \
          "$SHIMS_DIR/cutils/"

    # ── Cross file ──
    # meson sees host_machine.system='android'; the patches conditionalise
    # off that. NDK clang does its own sysroot management — no -isystem needed.
    cat >"$OUT_DIR/cross.txt" <<EOF
[binaries]
c = ['$CC', '-I$SHIMS_DIR']
cpp = ['$CXX', '-I$SHIMS_DIR']
ar = '$NDK_BIN/llvm-ar'
strip = '$NDK_BIN/llvm-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = '$cpu_family'
cpu = '$cpu_family'
endian = 'little'
EOF

    # ── Configure + build ──
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        meson setup "$BUILD_DIR" "$GFXSTREAM_DIR" \
            --cross-file "$OUT_DIR/cross.txt" \
            -Dgfxstream-build=host \
            -Ddecoders=auto \
            --buildtype release \
            -Ddefault_library=shared
    fi
    ninja -C "$BUILD_DIR" host/libgfxstream_backend.so

    # ── Stage outputs ──
    cp "$BUILD_DIR/host/libgfxstream_backend.so" "$OUT_DIR/"

    # Stage as a real shared lib alongside libcompositor.so. The compositor
    # crate links against `gfxstream_backend` via rutabaga_gfx's gfxstream
    # feature (see compositor/Cargo.toml's kumquat_virtio dep), so
    # libcompositor.so picks up a DT_NEEDED on libgfxstream_backend.so. The
    # backend itself has DT_NEEDED on libc++_shared.so — both ride along
    # in nativeLibraryDir so the dynamic linker resolves them at app
    # startup.
    local JNILIBS_DIR="$REPO_DIR/app/src/main/jniLibs/$jni_abi"
    mkdir -p "$JNILIBS_DIR"
    cp "$OUT_DIR/libgfxstream_backend.so" "$JNILIBS_DIR/libgfxstream_backend.so"
    local LIBCPP="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/${triple}/libc++_shared.so"
    cp "$LIBCPP" "$JNILIBS_DIR/libc++_shared.so"

    echo "==> [$abi] built libgfxstream_backend.so:"
    ls -la "$OUT_DIR/libgfxstream_backend.so"
    file "$OUT_DIR/libgfxstream_backend.so"
    echo "==> [$abi] staged jniLibs:"
    ls -la "$JNILIBS_DIR/libgfxstream_backend.so" "$JNILIBS_DIR/libc++_shared.so"
}

for abi in $ABIS; do
    build_one "$abi"
done
