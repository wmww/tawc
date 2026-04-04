#!/bin/bash
# Run the full tawc integration test suite.
#
# Builds all components (compositor APK, WSI layer, memfd shim, debug app),
# deploys to the phone, and runs the Cargo integration tests.
#
# Prerequisites:
#   - Android device connected via adb with root (su) access
#   - Arch Linux ARM chroot installed at /data/local/arch-chroot
#   - JAVA_HOME set or java-21-openjdk installed at default path
#
# Usage: bash testing/run-integration-tests.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"

echo "=== Checking adb connection ==="
adb get-state >/dev/null 2>&1 || { echo "ERROR: No adb device connected"; exit 1; }

echo "=== Setting SELinux permissive ==="
adb shell su -c "setenforce 0"

echo "=== Building compositor APK ==="
cd "$ROOT_DIR/server"
./gradlew assembleDebug --quiet
cd "$ROOT_DIR"

echo "=== Installing APK ==="
adb install -r server/app/build/outputs/apk/debug/app-debug.apk

echo "=== Pushing arch-chroot-run ==="
adb push client/arch-chroot-run /data/local/tmp/

echo "=== Pushing pidfile helper ==="
adb push testing/tawc-pidfile-exec /data/local/tmp/
adb shell "su -c 'cp /data/local/tmp/tawc-pidfile-exec /data/local/arch-chroot/tmp/tawc-pidfile-exec && chmod +x /data/local/arch-chroot/tmp/tawc-pidfile-exec'"

echo "=== Building WSI layer ==="
bash client/tawc-wsi/build

echo "=== Building memfd shim ==="
bash client/memfd-selinux-shim/build

echo "=== Running integration tests ==="
# Note: debug app build + deps are handled by the Rust test harness
# (chroot::ensure_debug_app) with freshness caching.
cd "$SCRIPT_DIR/integration"
cargo test -- --nocapture --test-threads=1
TEST_EXIT=$?

echo "=== Stopping compositor ==="
adb shell am force-stop me.phie.tawc

exit $TEST_EXIT
