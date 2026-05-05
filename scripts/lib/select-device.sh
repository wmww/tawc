#!/bin/bash
# Source from host scripts to pick which adb device to talk to.
# Sets ANDROID_SERIAL (which adb honors natively).
#
# Resolution order (first non-empty wins):
#   1. ANDROID_SERIAL already set by the caller -- used as-is, no
#      lookup. Lets the developer pin a specific serial when both
#      targets are connected.
#   2. TAWC_TARGET environment variable.
#   3. ./.tawctarget at the project root (one word, first line).
#   4. (none) -- treated as 'none', see below.
#
# Valid target values:
#   physical  -> first non-emulator adb device (a real phone).
#   emulator  -> first emulator-* adb device.
#   none      -> no target permitted; this script errors out. This is
#                 the safe default when the project has no .tawctarget,
#                 so on a fresh checkout no host script will silently
#                 talk to whatever happens to be plugged in.
#
# We never auto-pick a target just because exactly one is connected.
# `physical` requires a real device; `emulator` requires an emulator.
# If the requested kind is missing the script errors instead of falling
# back to the other kind. Ditto if .tawctarget says `emulator` and the
# AVD isn't running.
#
# To use a target other than the .tawctarget default for one command,
# set TAWC_TARGET on the command line (e.g.
# `TAWC_TARGET=emulator bash scripts/run-integration-tests.sh`). Don't
# rewrite .tawctarget for this -- it represents the user's standing
# choice, not a per-command override.

if [ -n "${ANDROID_SERIAL:-}" ]; then
    return 0 2>/dev/null || exit 0
fi

# Use namespaced var names: this file is sourced and shares scope with
# the caller, so plain `_script_dir` would clobber a same-named var
# already set above the source line (and several callers set one).
__sd_target="${TAWC_TARGET:-}"
if [ -z "$__sd_target" ]; then
    # This script lives at scripts/lib/select-device.sh; project root
    # (which contains .tawctarget) is two dirs up.
    __sd_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    __sd_tawctarget_file="$(dirname "$(dirname "$__sd_dir")")/.tawctarget"
    if [ -f "$__sd_tawctarget_file" ]; then
        __sd_target=$(head -n1 "$__sd_tawctarget_file" | tr -d '[:space:]')
    fi
fi
_target="$__sd_target"
[ -z "$_target" ] && _target=none

case "$_target" in
    none)
        echo "ERROR: target is 'none' (no .tawctarget, or set to 'none')." >&2
        echo "       Set TAWC_TARGET=physical|emulator or ANDROID_SERIAL=<serial>." >&2
        return 1 2>/dev/null || exit 1
        ;;
    emulator)
        _emu=$(adb devices | awk 'NR>1 && $2=="device" && $1 ~ /^emulator-/ {print $1; exit}')
        if [ -z "$_emu" ]; then
            echo "ERROR: target is 'emulator' but no emulator is connected." >&2
            echo "       Start one with: bash scripts/emulator.sh start" >&2
            echo "       (See notes/emulator.md for one-time AVD setup.)" >&2
            return 1 2>/dev/null || exit 1
        fi
        export ANDROID_SERIAL="$_emu"
        ;;
    physical)
        _dev=$(adb devices | awk 'NR>1 && $2=="device" && $1 !~ /^emulator-/ {print $1; exit}')
        if [ -z "$_dev" ]; then
            echo "ERROR: target is 'physical' but no real device is connected." >&2
            return 1 2>/dev/null || exit 1
        fi
        export ANDROID_SERIAL="$_dev"
        ;;
    *)
        if [ -n "${TAWC_TARGET:-}" ]; then
            _src="TAWC_TARGET"
        else
            _src="./.tawctarget"
        fi
        echo "ERROR: unknown target '$_target' from $_src" >&2
        echo "       (expected 'physical', 'emulator', or 'none')" >&2
        return 1 2>/dev/null || exit 1
        ;;
esac

if [ -n "${TAWC_VERBOSE_TARGET:-}" ]; then
    echo "==> using adb target: $ANDROID_SERIAL ($_target)" >&2
fi
