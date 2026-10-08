#!/bin/bash
# Cross-compile OpenSSH's sftp-server (static bionic) via the NDK and
# stage it at `app/src/main/jniLibs/<abi>/libsftp-server.so` (same
# jniLib-extractor trick as ando). SftpServerInstallProvider copies it
# into each rootfs at /usr/lib/tawc/sftp-server, where remote access runs
# it through tawcroot for sftp/scp (notes/remote-access.md).
#
# Static for the same reason as ando: tawcroot's loader maps it, with no
# /system/bin/linker64 and no dependence on the distro's libc. Built
# without OpenSSL/zlib (sftp-server needs neither). Source: the pinned
# `openssh-portable` dep, copied into build/ so the checkout stays clean.
#
# Usage:
#   remote/sftp-server/build.sh [--abi=aarch64|x86_64|both]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$HERE/../.." && pwd)"
BUILD_ROOT="$REPO_DIR/build"
JNILIBS_DIR="$REPO_DIR/app/src/main/jniLibs"
# shellcheck source=../../scripts/lib/deps.sh
source "$REPO_DIR/scripts/lib/deps.sh"

find_ndk() {
    if [ -z "${ANDROID_NDK_HOME:-}" ]; then
        local sdk="${ANDROID_HOME:-$HOME/Android/Sdk}"
        if [ -d "$sdk/ndk" ]; then
            ANDROID_NDK_HOME="$sdk/ndk/$(ls -1 "$sdk/ndk" | sort -V | tail -1)"
        fi
    fi
    [ -n "${ANDROID_NDK_HOME:-}" ] || { echo "ERROR: ANDROID_NDK_HOME not set and no NDK found" >&2; exit 1; }
    NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    [ -x "$NDK_BIN/llvm-strip" ] || { echo "ERROR: NDK toolchain missing at $NDK_BIN" >&2; exit 1; }
}

build_abi() {
    local abi="$1" jni_abi
    case "$abi" in
        aarch64) jni_abi="arm64-v8a" ;;
        x86_64)  jni_abi="x86_64" ;;
        *) echo "internal error: unknown abi $abi" >&2; exit 1 ;;
    esac
    # A static binary never meets the device's libc, so compile against
    # the newest headers: libc.a has every symbol, and older API headers
    # hide declarations configure's link tests find.
    local cc
    cc="$(ls "$NDK_BIN"/"$abi"-linux-android*-clang | sort -V | tail -1)"

    local work="$BUILD_ROOT/sftp-server-$abi"
    rm -rf "$work"
    mkdir -p "$work"
    cp -a "$SRC/." "$work/"
    rm -rf "$work/.git"
    # Checkout mtimes are arbitrary; the release commit's generated files
    # are current by construction.
    touch "$work/configure" "$work/config.h.in"

    echo "==> configuring sftp-server ($abi)"
    # recallocarray: in libc.a but undeclared by bionic's headers.
    (cd "$work" && ./configure --host="$abi-linux-android" \
        CC="$cc" AR="$NDK_BIN/llvm-ar" RANLIB="$NDK_BIN/llvm-ranlib" \
        CFLAGS="-O2 -D__sentinel__=__sentinel__ -include $HERE/android-compat.h" \
        LDFLAGS="-static -Wl,-z,max-page-size=16384 -Wl,--trace-symbol=getpwuid,--trace-symbol=getpwnam,--trace-symbol=getgrgid,--trace-symbol=getgrnam,--trace-symbol=initgroups" \
        ac_cv_func_recallocarray=no \
        --without-openssl --without-zlib --without-pam --without-selinux \
        --without-security-key-builtin --disable-lastlog --disable-utmp \
        --disable-wtmp --disable-utmpx --disable-wtmpx \
        >"$work/configure.log" 2>&1) || { tail -20 "$work/configure.log" >&2; exit 1; }

    "$cc" -O2 -Wall -Wextra -Werror -c "$HERE/guest-passwd.c" -o "$work/guest-passwd.o"

    # openbsd-compat's getrrsetbyname (ssh's SSHFP lookups) needs resolver
    # internals bionic doesn't expose, and sftp-server doesn't use it.
    local compat
    compat="$(printf 'print:\n\t@echo $(OPENBSD)\n' | make -s -C "$work/openbsd-compat" -f Makefile -f - print 2>/dev/null \
        | tr ' ' '\n' | grep -v '^getrrsetbyname\.o$' | tr '\n' ' ')"

    echo "==> compiling sftp-server ($abi)"
    make -C "$work" -j"$(nproc)" sftp-server \
        OPENBSD="$compat" LIBS="$work/guest-passwd.o -ldl" >"$work/make.log" 2>&1 \
        || { grep -E 'error:|undefined' "$work/make.log" >&2 || true; exit 1; }

    local out="$work/sftp-server"
    # bionic's passwd/group code must not be linked: it ignores the
    # guest's /etc/passwd. lld's trace shows who defines each lookup; a
    # second definition from libc.a would also be a duplicate-symbol error.
    local sym
    for sym in getpwuid getpwnam getgrgid getgrnam initgroups; do
        if grep -E "libc\.a.*definition of $sym\$" "$work/make.log" >/dev/null; then
            echo "ERROR: bionic's $sym got linked in" >&2
            exit 1
        fi
    done
    "$NDK_BIN/llvm-strip" "$out"
    if "$NDK_BIN/llvm-readelf" -l "$out" | grep -q INTERP; then
        echo "ERROR: sftp-server has PT_INTERP (not static)" >&2
        exit 1
    fi

    mkdir -p "$JNILIBS_DIR/$jni_abi"
    cp -f "$out" "$JNILIBS_DIR/$jni_abi/libsftp-server.so"
    echo "    staged at jniLibs/$jni_abi/libsftp-server.so ($(stat -c%s "$out") bytes)"
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

dep_ensure openssh-portable
SRC="$REPO_DIR/deps/openssh-portable"
find_ndk
case "$ABI" in
    both) build_abi aarch64; build_abi x86_64 ;;
    *)    build_abi "$ABI" ;;
esac
echo "==> sftp-server build OK"
