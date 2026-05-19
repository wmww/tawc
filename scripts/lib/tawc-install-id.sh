#!/bin/bash
# Resolve the in-app install id. Sets TAWC_INSTALL_ID.
#
# Uses the caller's TAWC_INSTALL_ID, or auto-selects when exactly one
# /data/data/me.phie.tawc/distros/<id>/metadata.json exists.

set -euo pipefail

if [ -n "${TAWC_INSTALL_ID:-}" ]; then
    export TAWC_INSTALL_ID
    return 0 2>/dev/null || exit 0
fi

_pkg=me.phie.tawc
_distros=/data/data/$_pkg/distros

_lib_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
_repo_dir=$(cd "$_lib_dir/../.." && pwd)
TAWC_EXEC="${TAWC_EXEC:-$_repo_dir/scripts/tawc-exec.sh}"

_probe='for d in '"$_distros"'/*/metadata.json; do test -f "$d" && basename "$(dirname "$d")"; done; true'
_ids=$("$TAWC_EXEC" /system/bin/sh -c "$_probe" 2>/dev/null \
       | tr -d '\r' \
       | awk 'NF' \
       | sort -u)
if [ -n "$_ids" ]; then
    _count=$(printf '%s\n' "$_ids" | wc -l | tr -d '[:space:]')
else
    _count=0
fi

case "$_count" in
    1)
        export TAWC_INSTALL_ID="$_ids"
        ;;
    0)
        cat >&2 <<EOF
ERROR: no in-app install found at $_distros/*/.
       Install one with:
          scripts/tawc-exec.sh --foreground-app --action install --arg id=<id>
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
