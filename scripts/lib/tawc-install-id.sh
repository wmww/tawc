#!/bin/bash
# Source from host scripts to resolve which in-app install (under
# /data/data/me.phie.tawc/distros/<id>/) the rest of the script will
# operate on. Sets and exports TAWC_INSTALL_ID.
#
# Resolution order:
#   1. TAWC_INSTALL_ID already set by the caller -- used as-is.
#   2. Otherwise, list /data/data/me.phie.tawc/distros/*/metadata.json
#      on the device:
#        - exactly one match -> use that id.
#        - zero matches      -> error: nothing installed.
#        - multiple matches  -> error: ambiguous, list them and ask the
#          caller to pin one with TAWC_INSTALL_ID=<id>.
#
# We deliberately do NOT auto-pick when there's more than one install
# (mirrors the .tawctarget policy in select-device.sh: if the choice
# isn't obvious, refuse rather than guess and get it wrong).
#
# Caller contract:
#   - adb on PATH, ANDROID_SERIAL already selected (source
#     scripts/lib/select-device.sh first).
#   - Read the resolved id back from $TAWC_INSTALL_ID.

if [ -n "${TAWC_INSTALL_ID:-}" ]; then
    export TAWC_INSTALL_ID
    return 0 2>/dev/null || exit 0
fi

_pkg=me.phie.tawc
_distros=/data/data/$_pkg/distros

# Run the probe inside the app via the dev exec broker — that gets us
# read access to the app's private data dir without root and without
# `run-as`'s SELinux domain quirks. See notes/exec-broker.md.
_lib_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tawc-exec.sh
. "$_lib_dir/tawc-exec.sh"

_probe='for d in '"$_distros"'/*/metadata.json; do test -f "$d" && basename "$(dirname "$d")"; done'
_ids=$("$TAWC_EXEC_BIN" /system/bin/sh -c "$_probe" 2>/dev/null \
       | tr -d '\r' \
       | awk 'NF' \
       | sort -u)
_count=$(printf '%s' "$_ids" | grep -c '^')

case "$_count" in
    1)
        export TAWC_INSTALL_ID="$_ids"
        ;;
    0)
        cat >&2 <<EOF
ERROR: no in-app install found at $_distros/*/.
       Install one with:
         bash scripts/install-distro.sh <id> [tawcroot|proot|chroot]
EOF
        return 1 2>/dev/null || exit 1
        ;;
    *)
        {
            echo "ERROR: multiple installs found at $_distros/:"
            printf '         - %s\n' $_ids
            echo "       Pick one with TAWC_INSTALL_ID=<id>."
        } >&2
        return 1 2>/dev/null || exit 1
        ;;
esac

if [ -n "${TAWC_VERBOSE_INSTALL:-}" ]; then
    echo "==> using install id: $TAWC_INSTALL_ID" >&2
fi
