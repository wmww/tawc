#!/bin/bash
# Cross-compile the ando guest client (static bionic) via the NDK and
# stage it at `app/src/main/jniLibs/<abi>/libando.so`. Same jniLib-
# extractor trick as tawcroot/proot: Android's extractor matches
# `lib*.so` filenames without validating ELF type, and
# `applicationInfo.nativeLibraryDir` files keep exec permission.
# AndoInstallProvider copies the binary into each rootfs at
# /usr/local/bin/ando.
#
# Static against bionic's libc.a: the binary runs as a guest under
# tawcroot (manual ELF loader), so a static link sidesteps per-distro
# glibc questions and the /system/bin/linker64 dependency.
#
# Usage:
#   tawcroot/ando/build.sh                  # current host's primary ABI
#   tawcroot/ando/build.sh --abi=aarch64
#   tawcroot/ando/build.sh --abi=x86_64
#   tawcroot/ando/build.sh --abi=both

set -euo pipefail

ANDO_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$ANDO_DIR/../.." && pwd)"
BUILD_ROOT="$REPO_DIR/build"
JNILIBS_DIR="$REPO_DIR/app/src/main/jniLibs"

# NDK lookup mirrors tawcroot/build.sh (we share the same NDK install).
find_ndk() {
    if [ -z "${ANDROID_NDK_HOME:-}" ]; then
        DEFAULT_SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
        if [ -d "$DEFAULT_SDK/ndk" ]; then
            ANDROID_NDK_HOME="$DEFAULT_SDK/ndk/$(ls -1 "$DEFAULT_SDK/ndk" | sort -V | tail -1)"
        fi
    fi
    if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
        echo "ERROR: ANDROID_NDK_HOME not set and no NDK found under \$ANDROID_HOME/ndk." >&2
        exit 1
    fi
    NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    [ -x "$NDK_BIN/llvm-strip" ] || { echo "ERROR: NDK toolchain missing at $NDK_BIN" >&2; exit 1; }
}

build_abi() {
    local abi="$1"
    local cc jni_abi
    case "$abi" in
        aarch64) cc="$NDK_BIN/aarch64-linux-android29-clang"; jni_abi="arm64-v8a" ;;
        x86_64)  cc="$NDK_BIN/x86_64-linux-android29-clang";  jni_abi="x86_64" ;;
        *) echo "internal error: unknown abi $abi" >&2; exit 1 ;;
    esac
    [ -x "$cc" ] || { echo "ERROR: missing toolchain: $cc" >&2; exit 1; }

    local out_dir="$BUILD_ROOT/ando-$abi"
    mkdir -p "$out_dir"
    local out="$out_dir/libando.so"

    echo "==> compiling ando ($abi)"
    "$cc" -O2 -Wall -Wextra -Werror -static -Wl,-z,max-page-size=16384 \
        "$ANDO_DIR/src/ando.c" -o "$out"
    "$NDK_BIN/llvm-strip" "$out"

    # Sanity: must be statically linked (no PT_INTERP) — the tawcroot
    # loader maps the binary itself, and a dynamic build would also need
    # /system/bin/linker64 visible in every rootfs.
    if "$NDK_BIN/llvm-readelf" -l "$out" | grep -q INTERP; then
        echo "ERROR: libando.so has PT_INTERP (not static)" >&2
        exit 1
    fi

    mkdir -p "$JNILIBS_DIR/$jni_abi"
    cp -f "$out" "$JNILIBS_DIR/$jni_abi/libando.so"
    echo "    staged at jniLibs/$jni_abi/libando.so ($(stat -c%s "$out") bytes)"
}

ABI=""
for arg in "$@"; do
    case "$arg" in
        --abi=aarch64|--abi=arm64) ABI="aarch64" ;;
        --abi=x86_64)              ABI="x86_64" ;;
        --abi=both)                ABI="both" ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done
if [ -z "$ABI" ]; then
    case "$(uname -m)" in
        x86_64|amd64) ABI="x86_64" ;;
        *)            ABI="aarch64" ;;
    esac
fi

find_ndk
case "$ABI" in
    both) build_abi aarch64; build_abi x86_64 ;;
    *)    build_abi "$ABI" ;;
esac
echo "==> ando build OK"
