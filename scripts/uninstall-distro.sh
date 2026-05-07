#!/bin/bash
# Trigger an in-app distro uninstall via the dev exec broker.
#
# Usage:
#   bash scripts/uninstall-distro.sh <id>
#
# The host TTY shows progress + log lines; Ctrl-C cancels (no confirm
# — matches the in-app behaviour where the uninstall Cancel button is
# applied directly. The partial wipe is left in FAILED state on disk,
# see notes/installation.md).
#
# Requires .tawctarget set + debug APK installed.

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: bash scripts/uninstall-distro.sh <id>" >&2
    exit 2
fi

id="$1"

_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$_script_dir/lib/select-device.sh"
# shellcheck disable=SC1091
. "$_script_dir/lib/tawc-exec.sh"

adb shell am start -n me.phie.tawc/.MainActivity >/dev/null

exec "$TAWC_EXEC_BIN" --action uninstall --arg "id=$id"
