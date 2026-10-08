#!/bin/bash
# Fail if any native lib in an APK has an ELF LOAD segment aligned below
# 16 KB. Such libs (and jniLib executables) won't load on 16 KB-page
# devices, and Android 15+ shows an "App Compatibility" warning for them.
#
# Usage: scripts/check-elf-alignment.sh [apk]   (default: debug APK)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APK="${1:-$ROOT_DIR/app/build/outputs/apk/debug/app-debug.apk}"
[ -f "$APK" ] || { echo "ERROR: no APK at $APK" >&2; exit 1; }

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
READELF="$(command -v llvm-readelf || ls "$ANDROID_HOME"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf 2>/dev/null | tail -1 || true)"
[ -x "$READELF" ] || { echo "ERROR: llvm-readelf not found" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
unzip -q "$APK" 'lib/*' -d "$tmp" 2>/dev/null || { echo "no native libs in $APK"; exit 0; }

bad=0
for so in "$tmp"/lib/*/*.so; do
    # Smallest LOAD p_align; every LOAD must be ≥ 16 KB.
    min="$("$READELF" -lW "$so" | awk '$1 == "LOAD" { a = strtonum($NF); if (m == "" || a < m) m = a } END { print m }')"
    if [ -n "$min" ] && [ "$min" -lt 16384 ]; then
        printf 'ERROR: %s: LOAD align %#x < 0x4000\n' "${so#"$tmp"/lib/}" "$min" >&2
        bad=1
    fi
done
[ "$bad" = 0 ] || { echo "Link with -Wl,-z,max-page-size=16384 (notes/building.md, \"16 KB page alignment\")." >&2; exit 1; }
echo "all native libs 16 KB-aligned"
