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
#   - In-app Arch chroot installed at
#     /data/data/me.phie.tawc/distros/arch/ (install from the app's
#     home screen, or `am start -n me.phie.tawc/.install.InstallActivity
#     --es autoStart true --es id arch`)
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
    # apps:: tests — gtk3-demos provides gtk3-demo-application,
    # gtk4-demos provides gtk4-widget-factory; firefox + supertuxkart
    # are real-app tests on hardware buffers. supertuxkart is ~700 MB
    # because of bundled assets, so this initial install is a long one.
    gtk3 gtk3-demos gtk4-demos firefox supertuxkart
    # graphics:: tests
    mesa-utils weston vulkan-tools
)

echo "=== Installing chroot test deps: ${PKGS[*]} ==="
# `-Syu` (instead of plain `-S`): refresh the local DB in the same
# transaction we install in, so we never reference a `pkg.tar.xz` the
# mirror has already rolled forward of. `rm -rf /var/cache/pacman/pkg/*`
# afterwards drops the package cache — we never reinstall in place, so
# caching costs only disk. (pacman's own `-Scc --noconfirm` is a no-op
# for safety, see ArchPacmanCommon.installBasePackages.)
# Both halves match the install-time policy.
"$ROOT_DIR/client/tawc-chroot-run" \
    "pacman -Syu --noconfirm --needed ${PKGS[*]} && rm -rf /var/cache/pacman/pkg/*"

echo "=== Done ==="
