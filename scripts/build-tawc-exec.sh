#!/bin/bash
# Build the host-side tawc-exec helper into build/tawc-exec/.
# scripts/tawc-exec.sh calls this automatically; pass --clean to rebuild.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$ROOT_DIR/tools/tawc-exec"
OUT_DIR="$ROOT_DIR/build/tawc-exec"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$OUT_DIR" "$SRC_DIR/target"
fi

mkdir -p "$OUT_DIR"
# Use a separate target/ inside build/ so it isn't entangled with the
# compositor's target/ tree.
cargo build --release \
    --manifest-path "$SRC_DIR/Cargo.toml" \
    --target-dir "$OUT_DIR/target"

cp "$OUT_DIR/target/release/tawc-exec" "$OUT_DIR/tawc-exec"
echo "built: $OUT_DIR/tawc-exec"
