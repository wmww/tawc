#!/bin/bash
# Build the TAWC-DRI Phase 1 round-trip test binary on the phone
# inside the chroot. Run from the host. Pushes sources, compiles.
#
# Build deps (gcc, libxcb) are expected to already be installed in the
# chroot — `scripts/install-test-deps.sh` covers them via gtk3's
# transitive dependency on libxcb plus the existing gcc base-devel.
#
# Usage:
#   scripts/build-tawc-dri-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../scripts/lib/select-device.sh
source "$ROOT_DIR/scripts/lib/select-device.sh"
# shellcheck source=../scripts/lib/tawc-scratch.sh
source "$ROOT_DIR/scripts/lib/tawc-scratch.sh"
# shellcheck source=../scripts/lib/tawc-exec.sh
source "$ROOT_DIR/scripts/lib/tawc-exec.sh"
# shellcheck source=../scripts/lib/tawc-install-id.sh
source "$ROOT_DIR/scripts/lib/tawc-install-id.sh"

app_name="tawc-dri-test"
src_dir="$SCRIPT_DIR/$app_name"
chroot_root="/data/data/me.phie.tawc/distros/$TAWC_INSTALL_ID/rootfs"
build_dir="$chroot_root/tmp/$app_name"

echo "=== $app_name: pushing source ==="
adb push "$src_dir/$app_name.c" "$TAWC_SCRATCH/$app_name.c" >/dev/null
adb push "$src_dir/build.sh" "$TAWC_SCRATCH/$app_name-build.sh" >/dev/null
"$TAWC_EXEC_BIN" /system/bin/sh -c "mkdir -p $build_dir && \
                             cp $TAWC_SCRATCH/$app_name.c $build_dir/$app_name.c && \
                             cp $TAWC_SCRATCH/$app_name-build.sh $build_dir/build.sh"

echo "=== $app_name: building ==="
"$ROOT_DIR/scripts/tawc-rootfs-run.sh" "/bin/bash /tmp/$app_name/build.sh"

echo "=== $app_name: done ==="
echo "Binary (inside chroot): /tmp/$app_name/$app_name"
