#!/bin/bash
# Build, install, and optionally launch the debug APK.
#
# Usage: scripts/app-build-install.sh [--no-build] [--no-launch]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

DO_BUILD=1
DO_LAUNCH=1
for arg in "$@"; do
    case "$arg" in
        --no-build)  DO_BUILD=0 ;;
        --no-launch) DO_LAUNCH=0 ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# shellcheck source=lib/select-device.sh
source "$ROOT_DIR/scripts/lib/select-device.sh"

# Pick the right native ABI for the target. The Rust compositor links
# against a locally-built static libxkbcommon per arch; building both
# only works if both libxkbcommon trees exist.
case "$ANDROID_SERIAL" in
    emulator-*) TAWC_ABIS="x86_64" ;;
    *)          TAWC_ABIS="arm64-v8a" ;;
esac

if [ "$DO_BUILD" -eq 1 ]; then
    echo "=== Building APK ($TAWC_ABIS) ==="
    ( cd "$ROOT_DIR" && ./gradlew "-PtawcAbis=$TAWC_ABIS" assembleDebug --quiet )
fi

echo "=== Installing APK ==="
adb install -r "$ROOT_DIR/app/build/outputs/apk/debug/app-debug.apk"

if [ "$DO_LAUNCH" -eq 1 ]; then
    echo "=== Launching MainActivity ==="
    adb shell am force-stop me.phie.tawc
    adb shell am start -n me.phie.tawc/.MainActivity >/dev/null
fi
