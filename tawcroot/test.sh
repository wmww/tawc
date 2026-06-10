#!/bin/bash
# Run the cleat-driven tawcroot test runner.
#
# A single binary at `build/tawcroot-host/tests` runs five layers of
# tests (per `notes/tawcroot.md` "Testing strategy"):
#
#   - tawcroot/tests/unit/         pure-function unit tests (cleat-direct)
#   - tawcroot/tests/hosted/       handler-logic tests, in-process against
#                                  the full production tree (host build is
#                                  ASan+UBSan; raw syscalls go through the
#                                  hosted shim + test hook)
#   - tawcroot/tests/handler/      handler/filter tests (fork tawcroot-testhost)
#   - tawcroot/tests/integration/  full-supervisor tests (fork production tawcroot)
#   - tawcroot/tests/diff/         (future) differential tests vs proot
#
# The existing three are registered through cleat and share one filter
# syntax, one exit code, one report. Positional args are full-match regexes
# against `module`, `name`, or `module::name` (e.g.
# `handler/test_foundation_smoke`, `rootfs_syscalls_smoke`, `.*smoke.*`).
# Multiple args are
# combined with OR. See cleat docs/test_framework.md.
#
# Modes:
#   - host (default) — runs the cleat binary directly on the dev box.
#   - device         — cross-builds the cleat orchestrator + tawcroot +
#                tawcroot-testhost + fixtures via the NDK, pushes the lot
#                to $TAWC_SCRATCH (see scripts/lib/tawc-scratch.sh), and runs
#                the orchestrator as the adb shell uid. Same four cleat layers
#                as host mode (unit / handler / integration), same filter
#                syntax, same exit code semantics — `--device` just changes
#                where the orchestrator runs. Selection within device mode
#                follows the rest of the tawc tree: `.tawctarget` /
#                `TAWC_TARGET=physical|emulator` (sourced from
#                `scripts/lib/select-device.sh` — the only tawc-app coupling
#                in this script).
#
# Usage:
#   tawcroot/test.sh                # everything, host
#   tawcroot/test.sh --device       # everything, device
#   tawcroot/test.sh foundation_smoke # filter (host)
#   tawcroot/test.sh --no-build     # reuse existing build/

set -euo pipefail

TAWCROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$TAWCROOT_DIR/.." && pwd)"

BUILD=1
MODE=host
MODE_EXPLICIT=0
PASSTHROUGH=()
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        --host)     MODE=host;   MODE_EXPLICIT=1 ;;
        --device)   MODE=device; MODE_EXPLICIT=1 ;;
        *)          PASSTHROUGH+=("$arg") ;;
    esac
done

# `TAWC_TARGET=physical|emulator` flips device mode on without `--device`,
# matching the convention other tawc scripts use. An explicit
# `--host` / `--device` flag wins over the env var.
if [ "$MODE_EXPLICIT" = "0" ]; then
    case "${TAWC_TARGET:-}" in
        physical|emulator)  MODE=device ;;
    esac
fi

# ---- host mode -----------------------------------------------------------
if [ "$MODE" = "host" ]; then
    if [ "$BUILD" = "1" ]; then
        "$TAWCROOT_DIR/build.sh" --abi=host
    fi
    BIN="$REPO_DIR/build/tawcroot-host/tests"
    [ -x "$BIN" ] || { echo "ERROR: $BIN missing — drop --no-build?" >&2; exit 1; }
    # The host tests binary is built with ASan+UBSan (see Makefile).
    # Callers can override by exporting their own ASAN_OPTIONS.
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_stack_use_after_return=1:strict_string_checks=1}"
    exec "$BIN" "${PASSTHROUGH[@]}"
fi

# ---- device mode ---------------------------------------------------------
# Cleat cross-compiles cleanly under the NDK (it's plain POSIX C + a
# vendored stc). We push the cross-built `tests` orchestrator alongside
# tawcroot / tawcroot-testhost / fixtures and run it as adb shell, exactly
# the same shape as host mode. PASSTHROUGH filters work everywhere; pass/fail
# detection is the orchestrator's exit code, not adb-grep heuristics.
# shellcheck disable=SC1091
source "$REPO_DIR/scripts/lib/select-device.sh"   # sets ANDROID_SERIAL (tawc-app glue)
# shellcheck disable=SC1091
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"    # sets TAWC_SCRATCH, ensures it

DEVICE_ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r\n')
case "$DEVICE_ABI" in
    arm64-v8a)  BUILD_ABI=aarch64 ;;
    x86_64)     BUILD_ABI=x86_64 ;;
    *) echo "ERROR: unsupported device ABI: '$DEVICE_ABI'" >&2; exit 1 ;;
esac

if [ "$BUILD" = "1" ]; then
    "$TAWCROOT_DIR/build.sh" "--abi=$BUILD_ABI" --testhost --tests
    "$TAWCROOT_DIR/build-fixtures.sh" "$BUILD_ABI"
fi

LOCAL_BIN="$REPO_DIR/build/tawcroot-$BUILD_ABI/libtawcroot-testhost.so"
LOCAL_PROD="$REPO_DIR/build/tawcroot-$BUILD_ABI/libtawcroot.so"
LOCAL_TESTS="$REPO_DIR/build/tawcroot-$BUILD_ABI/tests"
LOCAL_FIXTURES_DIR="$REPO_DIR/build/tawcroot-$BUILD_ABI/programs"
[ -f "$LOCAL_BIN" ]   || { echo "ERROR: $LOCAL_BIN missing — drop --no-build?" >&2; exit 1; }
[ -f "$LOCAL_PROD" ]  || { echo "ERROR: $LOCAL_PROD missing — drop --no-build?" >&2; exit 1; }
[ -f "$LOCAL_TESTS" ] || { echo "ERROR: $LOCAL_TESTS missing — drop --no-build?" >&2; exit 1; }

# On-device layout — must match the -D paths baked into LOCAL_TESTS by
# build_tests in tawcroot/build.sh (TAWCROOT_TESTHOST_BIN, TAWCROOT_PROD_BIN,
# TAWCROOT_*_BIN, TAWCROOT_ANDROID_FILTER_WRAP, TAWCROOT_TEST_TMPDIR).
TESTS=$TAWC_SCRATCH/tests
TR=$TAWC_SCRATCH/tawcroot-testhost
PROD=$TAWC_SCRATCH/tawcroot
FIXTURES=$TAWC_SCRATCH/programs
TMPDIR_ON_DEVICE=$TAWC_SCRATCH/tt-rootless

# $TAWC_SCRATCH is shell-uid-writable (it's under /data/local/tmp), so a
# single rootless `adb push` lands the binary at its final path with
# execute bits preserved from the host — no `su cp` second hop needed.
adb shell "mkdir -p $FIXTURES $TMPDIR_ON_DEVICE" >/dev/null
push_one() {
    local local_path="$1" device_path="$2"
    adb push "$local_path" "$device_path" > /dev/null
}

push_one "$LOCAL_BIN"   "$TR"
push_one "$LOCAL_PROD"  "$PROD"
push_one "$LOCAL_TESTS" "$TESTS"

# Cross-check programs.list against locally-built binaries before
# pushing. A missing fixture (typical cause: build-fixtures bailed
# mid-list because of an aarch64 .S that doesn't assemble; less
# common: a manual `rm`) otherwise surfaces as
# `test_true(build_rootfs())` panicking inside an unrelated test —
# test_prod_features.c shares one build_rootfs across every test
# in the file, so one missing fixture takes out tests that don't
# even reference it. Fail loudly with the actual list of missing
# fixtures so the operator knows what to rebuild.
PROGRAMS_LIST="$TAWCROOT_DIR/tests/integration/programs/programs.list"
MISSING_FIXTURES=()
while IFS= read -r name; do
    [[ -z "$name" || "$name" =~ ^[[:space:]]*# ]] && continue
    name="$(echo "$name" | awk '{print $1}')"
    if [ ! -f "$LOCAL_FIXTURES_DIR/$name" ]; then
        MISSING_FIXTURES+=("$name")
    fi
done < "$PROGRAMS_LIST"
# `wrap` (handler/test_androidfilter's host-bionic helper) is built by
# build-fixtures.sh but lives outside programs.list — give it the same
# fail-loudly treatment instead of an unrelated-looking runtime error.
if [ ! -f "$LOCAL_FIXTURES_DIR/wrap" ]; then
    MISSING_FIXTURES+=("wrap")
fi
if [ "${#MISSING_FIXTURES[@]}" -gt 0 ]; then
    {
        echo "ERROR: ${#MISSING_FIXTURES[@]} fixture(s) missing from $LOCAL_FIXTURES_DIR/:"
        for n in "${MISSING_FIXTURES[@]}"; do echo "  - $n"; done
        echo "Rebuild with: tawcroot/build-fixtures.sh $BUILD_ABI"
    } >&2
    exit 1
fi

for f in "$LOCAL_FIXTURES_DIR"/*; do
    [ -f "$f" ] || continue
    push_one "$f" "$FIXTURES/$(basename "$f")"
done

# Cleat tests create rootfs trees under TAWCROOT_TEST_TMPDIR. Use a shell-owned
# directory so prior root-run leftovers under the old `tt` path do not matter.
# The FIFO/mknod checks are host-only: Android shell SELinux denies mknod on
# shell_data_file, and that one syscall is not worth making device tests need
# root.
adb shell "rm -rf $TMPDIR_ON_DEVICE/tawcroot-test-rootfs-* && mkdir -p $TMPDIR_ON_DEVICE"

# Forward PASSTHROUGH filters verbatim — the cross-compiled cleat
# orchestrator parses them with the same grammar as the host build.
PT_QUOTED=""
for a in "${PASSTHROUGH[@]}"; do
    # Single-quote each arg for the adb shell; literal `'` in args
    # would need escaping but cleat filters never contain one.
    PT_QUOTED+=" '$a'"
done

echo "=== running cleat tests on $ANDROID_SERIAL ($BUILD_ABI) ==="
# adb shell doesn't always forward inner exit codes reliably across vendor
# shells; capture the orchestrator's exit explicitly via a sentinel line and
# grep for `__exit=0`. Output streams live so cleat's coloured pass/fail stays
# visible.
TMPLOG=$(mktemp)
trap 'rm -f "$TMPLOG"' EXIT
adb shell "cd $TAWC_SCRATCH && $TESTS$PT_QUOTED; echo __exit=\$?" \
    | tee "$TMPLOG"
got=$(grep -oE '__exit=[0-9]+' "$TMPLOG" | tail -1 | cut -d= -f2 || true)
if [ "${got:-}" != "0" ]; then
    echo "FAIL: cleat orchestrator exit code = ${got:-<missing>}" >&2
    exit 1
fi
