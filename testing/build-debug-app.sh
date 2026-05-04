#!/bin/bash
# Build the GTK4 debug app on the phone inside the chroot.
# Run from the host. Pushes sources, compiles.
#
# Build deps (gtk4, pkg-config) are expected to already be installed in
# the chroot — run `testing/install-test-deps.sh` once per chroot install.
#
# Usage:
#   testing/build-debug-app.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../client/select-device.sh
source "$ROOT_DIR/client/select-device.sh"
# shellcheck source=../client/tawc-scratch.sh
source "$ROOT_DIR/client/tawc-scratch.sh"
# shellcheck source=../client/tawc-install-id.sh
source "$ROOT_DIR/client/tawc-install-id.sh"

app_name="gtk4-debug-app"
src_dir="$SCRIPT_DIR/$app_name"
chroot_root="/data/data/me.phie.tawc/distros/$TAWC_INSTALL_ID/rootfs"
build_dir="$chroot_root/tmp/$app_name"

echo "=== $app_name: pushing source ==="
adb push "$src_dir/$app_name.c" "$TAWC_SCRATCH/$app_name.c" >/dev/null
adb push "$src_dir/build.sh" "$TAWC_SCRATCH/$app_name-build.sh" >/dev/null
adb shell "su -c 'mkdir -p $build_dir && cp $TAWC_SCRATCH/$app_name.c $build_dir/$app_name.c && cp $TAWC_SCRATCH/$app_name-build.sh $build_dir/build.sh'"

echo "=== $app_name: building ==="
"$ROOT_DIR/client/tawc-chroot-run" "/bin/bash /tmp/$app_name/build.sh"

echo "=== $app_name: done ==="
echo "Binary (inside chroot): /tmp/$app_name/$app_name"
