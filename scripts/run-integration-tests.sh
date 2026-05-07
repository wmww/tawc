#!/bin/bash
# Run the tawc integration test suite.
#
# Builds all components (compositor APK including libhybris asset, debug
# app), deploys to the target, then runs the cargo integration tests.
# Pass an optional libtest substring filter to narrow the run; pass
# --no-build to skip the rebuild/redeploy phase.
#
# Prerequisites:
#   - Android device or emulator connected via adb with root (su) access
#     and selected via ./.tawctarget or TAWC_TARGET=physical|emulator
#     (see scripts/lib/select-device.sh -- no auto-fallback to a single
#     connected target).
#   - tawc app installed (this script reinstalls it during build).
#     libhybris ships inside the APK as an asset and is symlinked into
#     each rootfs at install time — no on-device libhybris build step.
#   - At least one in-app install present at
#     /data/data/me.phie.tawc/distros/<id>/. The suite auto-targets it
#     when there's exactly one; with multiple, set TAWC_INSTALL_ID=<id>
#     explicitly. Install via:
#       bash scripts/install-distro.sh <id> [tawcroot|proot|chroot] \
#           [distro=<distro>]
#   - Test-suite chroot packages installed (run
#     `bash scripts/install-test-deps.sh` once per chroot install)
#   - JAVA_HOME set or java-21-openjdk installed at default path
#
# Usage:
#   bash scripts/run-integration-tests.sh                       # everything
#   bash scripts/run-integration-tests.sh <filter>              # libtest substring filter,
#                                                                 e.g. `<module>::` or `<module>::test_foo`
#   bash scripts/run-integration-tests.sh --no-build [filter]   # skip rebuild/redeploy
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

DO_BUILD=1
TEST_FILTER=""
for arg in "$@"; do
    case "$arg" in
        --no-build|-n)
            DO_BUILD=0
            ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *)
            if [ -n "$TEST_FILTER" ]; then
                echo "ERROR: test filter already set to '$TEST_FILTER', got '$arg'" >&2
                exit 2
            fi
            TEST_FILTER="$arg"
            ;;
    esac
done

# shellcheck source=../scripts/lib/select-device.sh
source "$ROOT_DIR/scripts/lib/select-device.sh"
# shellcheck source=../scripts/lib/tawc-scratch.sh
source "$ROOT_DIR/scripts/lib/tawc-scratch.sh"
# shellcheck source=../scripts/lib/tawc-exec.sh
source "$ROOT_DIR/scripts/lib/tawc-exec.sh"
# shellcheck source=../scripts/lib/tawc-install-id.sh
source "$ROOT_DIR/scripts/lib/tawc-install-id.sh"

# tawc-install-id.sh exported TAWC_INSTALL_ID (auto-detected when unset
# and exactly one install is present; errors if 0 or >1). The cargo
# test harness reads the same env var via tawc_integration::install_id.
INSTALL_ID="$TAWC_INSTALL_ID"
INSTALL_DIR="/data/data/me.phie.tawc/distros/$INSTALL_ID"

echo "=== Checking adb connection ($ANDROID_SERIAL, install=$INSTALL_ID) ==="
adb get-state >/dev/null 2>&1 || { echo "ERROR: No adb device connected"; exit 1; }

if [ "$DO_BUILD" -eq 1 ]; then
    # Build + install the APK. We skip the launch — this script does its
    # own force-stop + am start + readiness wait below, so the
    # compositor lifetime brackets the cargo run cleanly.
    bash "$ROOT_DIR/scripts/app-build-install.sh" --no-launch

    echo "=== Verifying in-app install is present at $INSTALL_DIR ==="
    if ! "$TAWC_EXEC_BIN" /system/bin/sh -c "test -x $INSTALL_DIR/enter.sh" >/dev/null 2>&1; then
        cat >&2 <<EOF
ERROR: in-app install not found at $INSTALL_DIR/.

Install it from the host:
  bash scripts/install-distro.sh $INSTALL_ID [tawcroot|proot|chroot] \\
      [distro=<distro>]

Progress streams to your TTY; the in-app log screen also opens.
EOF
        exit 1
    fi

    echo "=== Pushing pidfile helper ==="
    adb push tests/apps/tawc-pidfile-exec "$TAWC_SCRATCH/tawc-pidfile-exec"
    # `cp` + chmod via the broker — runs as the app uid, which owns the
    # rootfs tree. No su required.
    "$TAWC_EXEC_BIN" /system/bin/sh -c "cp $TAWC_SCRATCH/tawc-pidfile-exec $INSTALL_DIR/rootfs/tmp/tawc-pidfile-exec && chmod +x $INSTALL_DIR/rootfs/tmp/tawc-pidfile-exec"

    # Pre-build the tawcroot device test bundle so the
    # `tawcroot::test_tawcroot_device_suite` integration case can run
    # `tawcroot/test --device --no-build` and skip a redundant
    # cross-compile.
    case "$ANDROID_SERIAL" in
        emulator-*) TAWCROOT_ABI=x86_64 ;;
        *)          TAWCROOT_ABI=aarch64 ;;
    esac
    echo "=== Building tawcroot device tests ($TAWCROOT_ABI) ==="
    bash tawcroot/build "--abi=$TAWCROOT_ABI" --testhost --tests
    bash tawcroot/build-fixtures "$TAWCROOT_ABI"
fi

# Launch the compositor once for the whole suite. Tests assert it is
# running rather than starting it themselves, so the suite gets a single
# clean compositor lifetime instead of N partial ones. Force-stop first
# so a leftover wayland-0 socket from a previous run is cleared before
# the new compositor binds. The Service is `exported="false"`, so we
# launch MainActivity — its onCreate calls startForegroundService.
echo "=== Starting compositor ==="
adb shell "am force-stop me.phie.tawc"
sleep 0.3
adb shell "am start -n me.phie.tawc/.MainActivity" >/dev/null

# Wait until the tawc process is alive, the wayland socket exists, AND
# the calloop event loop is dispatching. The compositor binds the
# socket as its last setup step (see compositor/src/event_loop.rs::run)
# so a fresh "Entering calloop event loop" log line confirms both — but
# `am force-stop` leaves the previous run's socket file behind, so the
# stat alone would falsely match a stale socket while the new
# compositor is still in early init. The logcat probe disambiguates.
adb logcat -c >/dev/null 2>&1 || true
COMPOSITOR_READY=0
for _ in $(seq 1 150); do
    # Wayland socket lives in the app's private data dir; probe via
    # the broker (runs as the app uid).
    if adb shell 'pidof me.phie.tawc >/dev/null' 2>/dev/null && \
       "$TAWC_EXEC_BIN" /system/bin/sh -c "test -e /data/data/me.phie.tawc/wayland-0" 2>/dev/null && \
       adb logcat -d -s tawc-native 2>/dev/null | grep -q "Entering calloop event loop"; then
        COMPOSITOR_READY=1
        break
    fi
    sleep 0.1
done
if [ "$COMPOSITOR_READY" -ne 1 ]; then
    echo "ERROR: compositor did not become ready within 15s" >&2
    adb shell am force-stop me.phie.tawc || true
    exit 1
fi

LIBTEST_ARGS=(--nocapture --test-threads=1)
if [ -n "$TEST_FILTER" ]; then
    LIBTEST_ARGS+=("$TEST_FILTER")
    echo "=== Running integration tests matching: $TEST_FILTER ==="
else
    echo "=== Running integration tests ==="
fi
# Note: the debug app build is handled by the Rust test harness
# (chroot::ensure_debug_app) with freshness caching. Chroot packages
# are NOT auto-installed — run `bash scripts/install-test-deps.sh` once
# per chroot install.
cd "$ROOT_DIR/tests/integration"
set +e
cargo test -- "${LIBTEST_ARGS[@]}"
TEST_EXIT=$?
set -e

echo "=== Stopping compositor ==="
adb shell am force-stop me.phie.tawc

exit $TEST_EXIT
