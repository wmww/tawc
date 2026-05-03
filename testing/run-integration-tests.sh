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
#     (multiple targets: set TAWC_TARGET=device|emulator first)
#   - tawc app installed (this script reinstalls it during build).
#     libhybris ships inside the APK as an asset and is symlinked into
#     each rootfs at install time — no on-device libhybris build step.
#   - In-app Arch chroot installed at
#     /data/data/me.phie.tawc/distros/arch/. Install via:
#       adb shell am start -n me.phie.tawc/.install.InstallActivity \
#           --es autoStart true --es id arch
#   - Test-suite chroot packages installed (run
#     `bash testing/install-test-deps.sh` once per chroot install)
#   - JAVA_HOME set or java-21-openjdk installed at default path
#
# Usage:
#   bash testing/run-integration-tests.sh                       # everything
#   bash testing/run-integration-tests.sh <filter>              # libtest substring filter,
#                                                                 e.g. `<module>::` or `<module>::test_foo`
#   bash testing/run-integration-tests.sh --no-build [filter]   # skip rebuild/redeploy
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"

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

# shellcheck source=../client/select-device.sh
source "$ROOT_DIR/client/select-device.sh"

# Install id under /data/data/me.phie.tawc/distros/<id>/. Defaults to
# `arch` (the chroot/proot suite); set TAWC_INSTALL_ID=arch-tawcroot to
# target a parallel tawcroot install on the same device. Mirrors
# `client/tawc-chroot-run` and is exported so the cargo test harness
# (which reads it via tawc_integration::install_id) sees the same value.
INSTALL_ID="${TAWC_INSTALL_ID:-arch}"
export TAWC_INSTALL_ID="$INSTALL_ID"
INSTALL_DIR="/data/data/me.phie.tawc/distros/$INSTALL_ID"

echo "=== Checking adb connection ($ANDROID_SERIAL, install=$INSTALL_ID) ==="
adb get-state >/dev/null 2>&1 || { echo "ERROR: No adb device connected"; exit 1; }

if [ "$DO_BUILD" -eq 1 ]; then
    # Pick the right native ABI for the target. The Rust compositor links
    # against a locally-built static libxkbcommon per arch; building both
    # only works if both libxkbcommon trees exist.
    case "$ANDROID_SERIAL" in
        emulator-*) TAWC_ABIS="x86_64" ;;
        *)          TAWC_ABIS="arm64-v8a" ;;
    esac

    echo "=== Building compositor APK ($TAWC_ABIS) ==="
    cd "$ROOT_DIR/server"
    ./gradlew "-PtawcAbis=$TAWC_ABIS" assembleDebug --quiet
    cd "$ROOT_DIR"

    echo "=== Installing APK ==="
    adb install -r server/app/build/outputs/apk/debug/app-debug.apk

    echo "=== Verifying in-app install is present at $INSTALL_DIR ==="
    if ! adb shell "su -c 'test -x $INSTALL_DIR/enter.sh'" >/dev/null 2>&1; then
        cat >&2 <<EOF
ERROR: in-app install not found at $INSTALL_DIR/.

Install it from the host:
  adb shell am start -n me.phie.tawc/.install.InstallActivity \\
      --es autoStart true --es id $INSTALL_ID \\
      [--es method chroot|proot|tawcroot]

Then tail progress:
  adb logcat -s tawc-install
EOF
        exit 1
    fi

    echo "=== Pushing pidfile helper ==="
    adb push testing/tawc-pidfile-exec /data/local/tmp/
    adb shell "su -c 'cp /data/local/tmp/tawc-pidfile-exec $INSTALL_DIR/rootfs/tmp/tawc-pidfile-exec && chmod +x $INSTALL_DIR/rootfs/tmp/tawc-pidfile-exec'"
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

# Wait until the tawc process is alive AND the chroot-visible Wayland
# socket exists. Both matter: `am force-stop` leaves the unix-domain
# socket file behind on disk even though no process is listening, so
# the file alone would falsely indicate readiness.
COMPOSITOR_READY=0
for _ in $(seq 1 150); do
    if adb shell "pidof me.phie.tawc >/dev/null && \
                  su -c 'test -e $INSTALL_DIR/rootfs/tmp/wayland-0' && \
                  echo ready" 2>/dev/null | grep -q ready; then
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
# are NOT auto-installed — run `bash testing/install-test-deps.sh` once
# per chroot install.
cd "$SCRIPT_DIR/integration"
set +e
cargo test -- "${LIBTEST_ARGS[@]}"
TEST_EXIT=$?
set -e

echo "=== Stopping compositor ==="
adb shell am force-stop me.phie.tawc

exit $TEST_EXIT
