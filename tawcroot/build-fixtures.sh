#!/bin/bash
# Build the loader test fixtures (tawcroot/tests/integration/programs/) for an
# Android ABI via NDK. Used by `tawcroot/test.sh --device` to push
# guest binaries to the device and exercise `tawcroot --exec` against
# them.
#
# Outputs land in `build/tawcroot-<abi>/programs/`. Same naming as the
# host build's fixtures (`build/tawcroot-host/programs/`).
#
# Usage:
#   tawcroot/build-fixtures.sh <x86_64|aarch64>

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <x86_64|aarch64>" >&2
    exit 2
fi
ABI="$1"

TAWCROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$TAWCROOT_DIR/.." && pwd)"
PROG_DIR="$TAWCROOT_DIR/tests/integration/programs"
WRAP_SRC="$TAWCROOT_DIR/tests/handler/androidfilter/wrap.c"
OUT_DIR="$REPO_DIR/build/tawcroot-$ABI/programs"
mkdir -p "$OUT_DIR"

# NDK lookup mirrors scripts/build-proot.sh (we share the same NDK install).
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

case "$ABI" in
    x86_64)
        STATIC_ASM_SUFFIX="x86_64"
        CC="$NDK_BIN/x86_64-linux-android29-clang"
        ;;
    aarch64)
        STATIC_ASM_SUFFIX="aarch64"
        CC="$NDK_BIN/aarch64-linux-android29-clang"
        ;;
    *)
        echo "ERROR: unsupported abi: $ABI" >&2
        exit 1
        ;;
esac
[ -x "$CC" ] || { echo "ERROR: missing $CC" >&2; exit 1; }

# --- static fixtures (raw syscalls, freestanding) ---
#
# Source files are arch-specific .S because the syscall numbers and
# instruction set differ. If a fixture lacks a .S for the target arch
# we skip it here — but note test.sh hard-fails before pushing when any
# programs.list entry is missing, so a skipped fixture must be written
# (or delisted) before device tests run.
build_static() {
    local name="$1"
    local src="$PROG_DIR/${name}_${STATIC_ASM_SUFFIX}.S"
    if [ ! -f "$src" ]; then
        echo "  skip $name ($STATIC_ASM_SUFFIX source missing)"
        return
    fi
    local out="$OUT_DIR/$name"
    "$CC" -static -nostdlib -O2 -Wl,--build-id=none "$src" -o "$out"
    echo "  built $name"
}

# --- dynamic fixtures (link bionic via NDK) ---
build_dynamic() {
    local name="$1"
    local src="$PROG_DIR/${name}.c"
    if [ ! -f "$src" ]; then
        echo "  skip $name (no source)"
        return
    fi
    local out="$OUT_DIR/$name"
    "$CC" -O2 -Wl,--build-id=none "$src" -o "$out"
    echo "  built $name (dynamic, bionic)"
}

# --- dynamic fixture compiled with -fstack-clash-protection ---
build_dynamic_big_stack() {
    local out="$OUT_DIR/dynamic_big_stack"
    "$CC" -O2 -fstack-clash-protection -Wl,--build-id=none \
        "$PROG_DIR/dynamic_big_stack.c" -o "$out"
    echo "  built dynamic_big_stack (dynamic, bionic, -fstack-clash-protection)"
}

# --- androidfilter wrap (host-bionic helper used by handler/test_androidfilter.c) ---
build_wrap() {
    local out="$OUT_DIR/wrap"
    "$CC" -O2 -Wall -Wextra -Werror -Wl,--build-id=none "$WRAP_SRC" -o "$out"
    echo "  built androidfilter/wrap"
}

echo "==> building android-$ABI fixtures into $OUT_DIR"
build_static  static_exit42
build_static  static_argc_random
build_static  static_execve_exit42
build_static  static_open_creat_argv1
build_static  static_open_rdonly_argv1
build_static  static_sleep_open_creat_argv1
build_static  static_fork_open_argv1
build_static  static_small_stack_open_argv1
build_static  static_fork_exec_argv1
build_static  static_fork_closefrom_exec_argv1
build_static  static_vfork_exec_then_fork_exec_argv1
build_static  static_fexecve_argv1
build_static  static_unix_bind_argv1
build_static  static_check_proc_self_fd
build_static  static_check_proc_exe_argv0
build_static  static_io_uring_deny
build_dynamic dynamic_exit42
build_dynamic dynamic_argv_check
build_dynamic_big_stack
build_wrap

echo "==> done"
