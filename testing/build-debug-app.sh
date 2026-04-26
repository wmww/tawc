#!/bin/bash
# Build the GTK4 debug app on the phone inside the chroot.
# Run from the host. Pushes sources, installs deps if needed, compiles.
#
# Usage:
#   testing/build-debug-app.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

app_name="gtk4-debug-app"
pkg="gtk4"
src_dir="$SCRIPT_DIR/$app_name"
build_dir="/data/local/arch-chroot/tmp/$app_name"

echo "=== $app_name: pushing source ==="
adb push "$src_dir/$app_name.c" "/data/local/tmp/$app_name.c" >/dev/null
adb push "$src_dir/build.sh" "/data/local/tmp/$app_name-build.sh" >/dev/null
adb shell "su -c 'mkdir -p $build_dir && cp /data/local/tmp/$app_name.c $build_dir/$app_name.c && cp /data/local/tmp/$app_name-build.sh $build_dir/build.sh'"

echo "=== $app_name: ensuring build deps ($pkg, pkg-config) ==="
adb shell "/system/bin/sh /data/local/tmp/arch-chroot-run 'pacman -Q $pkg pkg-config >/dev/null 2>&1 || pacman -Sy --noconfirm $pkg pkg-config'"

echo "=== $app_name: building ==="
adb shell "/system/bin/sh /data/local/tmp/arch-chroot-run '/bin/bash /tmp/$app_name/build.sh'"

echo "=== $app_name: done ==="
echo "Binary (inside chroot): /tmp/$app_name/$app_name"
