#!/bin/bash
# Build, install, and optionally launch the debug APK.
#
# Usage: scripts/app-build-install.sh [--no-build] [--no-launch] [--force-install] [--xwayland|--no-xwayland]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

DO_BUILD=1
DO_LAUNCH=1
FORCE_INSTALL=0
BUILD_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --no-build)  DO_BUILD=0 ;;
        --no-launch) DO_LAUNCH=0 ;;
        --force-install) FORCE_INSTALL=1 ;;
        --xwayland|--no-xwayland) BUILD_ARGS+=("$arg") ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# shellcheck source=lib/select-device.sh
source "$ROOT_DIR/scripts/lib/select-device.sh"

if [ "$DO_BUILD" -eq 1 ]; then
    "$ROOT_DIR/scripts/build-app.sh" --quiet "${BUILD_ARGS[@]}"
fi

APK="$ROOT_DIR/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK" ] || { echo "ERROR: missing $APK (drop --no-build?)" >&2; exit 1; }

apk_sha() {
    sha256sum "$APK" | awk '{print $1}'
}

installed_apk_sha() {
    local path
    path=$(adb shell pm path me.phie.tawc 2>/dev/null \
        | tr -d '\r' \
        | sed -n 's/^package://p' \
        | head -n1)
    [ -n "$path" ] || return 1
    adb shell "sha256sum '$path' 2>/dev/null | awk '{print \$1}'" \
        | tr -d '\r' \
        | head -n1
}

LOCAL_SHA="$(apk_sha)"
DEVICE_SHA="$(installed_apk_sha || true)"
if [ "$FORCE_INSTALL" -eq 0 ] && [ -n "$DEVICE_SHA" ] && [ "$LOCAL_SHA" = "$DEVICE_SHA" ]; then
    echo "=== APK already installed (sha256 match) ==="
else
    echo "=== Installing APK ==="
    adb install -r "$APK"
fi

if [ "$DO_LAUNCH" -eq 1 ]; then
    echo "=== Launching MainActivity ==="
    adb shell am force-stop me.phie.tawc
    adb shell am start -n me.phie.tawc/.MainActivity >/dev/null
fi
