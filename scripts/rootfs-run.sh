#!/bin/bash
# Run a command inside an installed rootfs through the dev exec broker.
#
# Usage: scripts/rootfs-run.sh [command]
# Set TAWC_INSTALL_ID=<id> when more than one install exists.
set -euo pipefail

_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
_root_dir=$(cd "$_script_dir/.." && pwd)
TAWC_EXEC="${TAWC_EXEC:-$_root_dir/scripts/tawc-exec.sh}"
# shellcheck disable=SC1091
. "$_script_dir/lib/tawc-install-id.sh"

INSTALL_ID="$TAWC_INSTALL_ID"

# Non-interactive `rootfs-run.sh "<cmd>"` mirrors stdio into the
# in-app log screen by default — the user's mental model is "I asked the
# phone to run this; show me what's happening on the phone too." Skip
# the panel for interactive shells (panel can't usefully render them)
# and when TAWC_OP_TITLE= is set to empty (escape hatch for callers
# that want pure stdio relay, e.g. nested rootfs-run inside a
# script that already shows its own panel). Default title trims long
# command lines so the toolbar stays readable.
default_title() {
    # 60-char total budget on the toolbar (install id prefix included).
    # Bash ${var:0:n} is character-indexed under a UTF-8 locale, which
    # any dev workstation will have — falls back to byte slicing under
    # LC_ALL=C, which is fine for the all-ASCII commands we expect.
    local prefix="$INSTALL_ID: "
    local cmd="$*"
    local budget=$((60 - ${#prefix}))
    if [ "$budget" -ge 8 ] && [ "${#cmd}" -gt "$budget" ]; then
        cmd="${cmd:0:$((budget - 3))}..."
    fi
    printf '%s%s' "$prefix" "$cmd"
}

if [ $# -gt 0 ]; then
    title="${TAWC_OP_TITLE-$(default_title "$@")}"
    if [ -n "$title" ]; then
        exec "$TAWC_EXEC" --in-rootfs "$INSTALL_ID" --op-title "$title" -- "$@"
    else
        exec "$TAWC_EXEC" --in-rootfs "$INSTALL_ID" -- "$@"
    fi
else
    exec "$TAWC_EXEC" --in-rootfs "$INSTALL_ID"
fi
