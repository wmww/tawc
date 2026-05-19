#!/bin/bash
# Build the host-side exec helper if needed, then run it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$ROOT_DIR/build/tawc-exec/tawc-exec"
SRC="$ROOT_DIR/tools/tawc-exec"

needs_build() {
    [ ! -x "$BIN" ] && return 0
    [ -n "$(find "$SRC/src" "$SRC/Cargo.toml" "$SRC/Cargo.lock" \
        -newer "$BIN" -print -quit 2>/dev/null)" ]
}

if needs_build; then
    "$SCRIPT_DIR/build-tawc-exec.sh" >&2
fi

need_target=1
if [ $# -eq 0 ]; then
    need_target=0
else
    for arg in "$@"; do
        case "$arg" in
            -h|--help|help) need_target=0 ;;
        esac
    done
fi

case "$need_target" in
    0) ;;
    1)
        # shellcheck source=lib/select-device.sh
        . "$SCRIPT_DIR/lib/select-device.sh"
        ;;
esac

exec "$BIN" "$@"
