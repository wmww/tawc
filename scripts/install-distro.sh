#!/bin/bash
# Trigger an in-app distro install via the dev exec broker. Replaces the
# old "am start … InstallActivity --es autoStart true" recipe (which
# conflated opening the activity with triggering the install).
#
# Usage:
#   bash scripts/install-distro.sh <id> [<method>] [extra=value …]
#     <id>      slot id under /data/data/me.phie.tawc/distros/
#     <method>  install method (tawcroot|proot|chroot, default tawcroot)
#     extras    extra key=value args forwarded as --arg, e.g.
#                 distro=archlinuxarm
#                 label='Arch'
#                 mirrorProxy=http://127.0.0.1:8080/proxy/
#
# Examples:
#   bash scripts/install-distro.sh arch proot
#   bash scripts/install-distro.sh test-slot tawcroot label='Test slot'
#
# The host TTY shows progress + log lines as the install runs; pressing
# Ctrl-C cancels (the broker socket close → action observes the cancel
# flag → calls Operation.cancel()). The in-app LogScreenActivity also
# opens automatically so on-device users can watch + cancel.
#
# Requires:
#   - .tawctarget set (or TAWC_TARGET=physical|emulator on the cmdline)
#   - debug APK installed (broker is debug-build only)
#   - cache proxy running (only when mirrorProxy=… is passed)

set -euo pipefail

if [ $# -lt 1 ]; then
    cat >&2 <<EOF
usage: bash scripts/install-distro.sh <id> [<method>] [key=value …]
       <method> defaults to tawcroot. See script header for examples.
EOF
    exit 2
fi

id="$1"; shift
method="${1:-tawcroot}"
case "$method" in
    tawcroot|proot|chroot) shift ;;
    *=*) method="tawcroot" ;;  # first positional was already a key=value pair
    *) echo "unknown method '$method' (try tawcroot, proot, or chroot)" >&2; exit 2 ;;
esac

_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$_script_dir/lib/select-device.sh"
# shellcheck disable=SC1091
. "$_script_dir/lib/tawc-exec.sh"

# Build extra --arg flags from positional key=value pairs.
extra_args=()
for kv in "$@"; do
    case "$kv" in
        *=*) extra_args+=(--arg "$kv") ;;
        *) echo "extra arg '$kv' must be in key=value form" >&2; exit 2 ;;
    esac
done

# Bring MainActivity foreground first. Android-14 FGS-from-bg rules can
# refuse a startForegroundService from a background process; the broker
# itself is a background thread, so the app being foreground is what
# guarantees the install service can come up. tawc-exec does this on
# its own when the app process is dead, but doesn't bring an existing
# background process to the foreground.
adb shell am start -n me.phie.tawc/.MainActivity >/dev/null

exec "$TAWC_EXEC_BIN" --action install \
    --arg "id=$id" \
    --arg "method=$method" \
    "${extra_args[@]}"
