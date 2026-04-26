#!/bin/bash
# Run the tawc integration test suite.
#
# Builds all components (compositor APK, debug app, libhybris on device),
# deploys to the target, then runs the cargo integration tests. Both
# apps and input groups by default; pass arguments to narrow
# down to a single group or a single test name.
#
# Prerequisites:
#   - Android device or emulator connected via adb with root (su) access
#     (multiple targets: set TAWC_TARGET=device|emulator first)
#   - Arch Linux ARM chroot installed at /data/local/arch-chroot
#   - libhybris already built and installed in the chroot (run
#     `bash client/build-libhybris` once); skipped on emulator
#   - JAVA_HOME set or java-21-openjdk installed at default path
#
# Usage:
#   bash testing/run-integration-tests.sh                   # everything
#   bash testing/run-integration-tests.sh apps              # only the apps group
#   bash testing/run-integration-tests.sh input             # only the input group
#   bash testing/run-integration-tests.sh test_firefox_launches_with_hardware_buffers
#                                                           # one test by name
#   bash testing/run-integration-tests.sh input test_text_input_and_backspace
#                                                           # one test in a specific group
#   bash testing/run-integration-tests.sh --no-build apps   # skip the rebuild/redeploy phase
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"

# Parse args: optional --no-build, optional group, optional test-name filter.
DO_BUILD=1
GROUP=""
TEST_FILTER=""
for arg in "$@"; do
    case "$arg" in
        --no-build|-n)
            DO_BUILD=0
            ;;
        apps|input)
            if [ -n "$GROUP" ]; then
                echo "ERROR: group already set to '$GROUP', got '$arg'" >&2
                exit 2
            fi
            GROUP="$arg"
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

echo "=== Checking adb connection ($ANDROID_SERIAL) ==="
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

    echo "=== Pushing arch-chroot-run ==="
    adb push client/arch-chroot-run /data/local/tmp/

    echo "=== Pushing pidfile helper ==="
    adb push testing/tawc-pidfile-exec /data/local/tmp/
    adb shell "su -c 'cp /data/local/tmp/tawc-pidfile-exec /data/local/arch-chroot/tmp/tawc-pidfile-exec && chmod +x /data/local/arch-chroot/tmp/tawc-pidfile-exec'"

    case "$ANDROID_SERIAL" in
        emulator-*)
            echo "=== Skipping libhybris build (emulator has no Android GPU drivers) ==="
            ;;
        *)
            echo "=== Building libhybris + GL shims (if sources changed) ==="
            bash client/build-libhybris --if-needed
            ;;
    esac
fi

# Build the cargo invocation. `--test <group>` selects a specific tests/*.rs
# binary; the trailing positional arg after `--` is libtest's test-name
# substring filter.
CARGO_ARGS=()
if [ -n "$GROUP" ]; then
    CARGO_ARGS+=(--test "$GROUP")
fi
LIBTEST_ARGS=(--nocapture --test-threads=1)
if [ -n "$TEST_FILTER" ]; then
    LIBTEST_ARGS+=("$TEST_FILTER")
fi

if [ -n "$GROUP" ] && [ -n "$TEST_FILTER" ]; then
    echo "=== Running integration test: $GROUP::$TEST_FILTER ==="
elif [ -n "$GROUP" ]; then
    echo "=== Running integration tests: $GROUP group ==="
elif [ -n "$TEST_FILTER" ]; then
    echo "=== Running integration tests matching: $TEST_FILTER ==="
else
    echo "=== Running integration tests (all groups) ==="
fi
# Note: debug app build + deps are handled by the Rust test harness
# (chroot::ensure_debug_app) with freshness caching.
cd "$SCRIPT_DIR/integration"
set +e
cargo test "${CARGO_ARGS[@]}" -- "${LIBTEST_ARGS[@]}"
TEST_EXIT=$?
set -e

echo "=== Stopping compositor ==="
adb shell am force-stop me.phie.tawc

exit $TEST_EXIT
