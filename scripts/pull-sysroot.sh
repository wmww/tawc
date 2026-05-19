#!/bin/bash
# Pull a curated sysroot out of an installed-on-device rootfs into
# build/<arch>-sysroot/. The aarch64 and x86_64 mesa-gfxstream cross-
# builds link against real distro .so files (libwayland-{client,server},
# libdrm, libudev, libffi) and need their headers — pulling from a live
# rootfs is the cheap-but-ugly way to get an ABI-accurate set without
# vendoring binaries.
#
# Hooks into the standard device selection (`.tawctarget` /
# `TAWC_TARGET`) and install-id resolution. Picks the rootfs ABI from
# the device's `ro.product.cpu.abi` (which always matches what the
# installed chroot was bootstrapped for; we don't run cross-arch
# rootfses).
#
# This whole "pull from a live device" arrangement is fragile — see
# issues/sysroot-pull-from-live-device.md for why and what would
# replace it. Until then, this script is the One Way to do it.
#
# Usage:
#   scripts/pull-sysroot.sh                # auto-pick ABI + install
#   TAWC_TARGET=emulator scripts/pull-sysroot.sh
#   TAWC_INSTALL_ID=arch scripts/pull-sysroot.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TAWC_EXEC="${TAWC_EXEC:-$SCRIPT_DIR/tawc-exec.sh}"

# shellcheck source=lib/select-device.sh
. "$SCRIPT_DIR/lib/select-device.sh"
# shellcheck source=lib/tawc-install-id.sh
. "$SCRIPT_DIR/lib/tawc-install-id.sh"

# Device ABI → sysroot dir name. Map adb-property ABI strings to the
# arch dir names the rest of the build expects.
DEVICE_ABI="$(adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
case "$DEVICE_ABI" in
    arm64-v8a) ARCH=aarch64 ;;
    x86_64)    ARCH=x86_64 ;;
    *)
        echo "ERROR: unsupported device ABI '$DEVICE_ABI' (need arm64-v8a or x86_64)" >&2
        exit 1
        ;;
esac

ROOTFS_DIR="/data/data/me.phie.tawc/distros/$TAWC_INSTALL_ID/rootfs"
DEST="$REPO_DIR/build/${ARCH}-sysroot"

echo "==> pulling $DEVICE_ABI sysroot from $TAWC_INSTALL_ID into build/${ARCH}-sysroot/"
rm -rf "$DEST"
mkdir -p "$DEST"

# Curated list — same set the old manual one-liner pulled. We tar on
# the device so we get real symlinks + permissions, then untar locally.
# `|| true` after the inner tar: some libs in the list aren't always
# installed (libdisplay-info on older Arch, etc.) and `tar -c` exits
# non-zero on missing inputs. The missing-lib stubs in
# build-mesa-gfxstream.sh handle the gap, so we don't actually need
# every entry — drop the exit code on the floor.
set +e
"$TAWC_EXEC" -- /system/bin/sh -c "
    cd '$ROOTFS_DIR' && tar -czf - \
        usr/include usr/lib/pkgconfig usr/share/pkgconfig \
        usr/lib/libwayland-* usr/lib/libdrm* usr/lib/libudev* usr/lib/libffi* \
        usr/lib/libstdc++.so.6 usr/lib/libgcc_s.so.1 \
        usr/lib/libdisplay-info* usr/lib/libvulkan.so.1 usr/lib/libxkbcommon* \
        2>/dev/null
    exit 0
" | tar -xzf - -C "$DEST"
set -euo pipefail

echo "==> sysroot pulled:"
du -sh "$DEST"
echo "    usr/lib entries:"
ls "$DEST/usr/lib" 2>/dev/null | head -10
echo "    (full list under $DEST/)"
