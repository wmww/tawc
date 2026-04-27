#!/bin/bash
# Install the chroot packages the integration test suite needs.
#
# Run once per chroot install. Idempotent (`pacman -S --needed`), so
# re-running is safe. The integration tests deliberately do NOT install
# anything at runtime — they assume these packages are present, so test
# runs aren't distro-specific and don't surprise you with package
# installs in the middle of a test.
#
# Prerequisites:
#   - Android device or emulator connected via adb (set TAWC_TARGET= if
#     multiple targets are connected)
#   - Arch Linux ARM chroot installed at /data/local/arch-chroot
#
# Usage:
#   bash testing/install-test-deps.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../client/select-device.sh
source "$ROOT_DIR/client/select-device.sh"

# Keep grouped/commented so it's obvious what each package is for.
PKGS=(
    # gtk4-debug-app build (compiled in chroot by ensure_debug_app)
    gtk4 pkg-config
    # apps:: tests
    gtk3 gtk3-demos
    # graphics:: tests
    mesa-utils weston vulkan-tools
)

echo "=== Pushing arch-chroot-run ==="
adb push "$ROOT_DIR/client/arch-chroot-run" /data/local/tmp/ >/dev/null

echo "=== Installing chroot test deps: ${PKGS[*]} ==="
adb shell "/system/bin/sh /data/local/tmp/arch-chroot-run 'pacman -S --noconfirm --needed ${PKGS[*]}'"

echo "=== Done ==="
