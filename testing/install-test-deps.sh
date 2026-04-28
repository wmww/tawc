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

# Pacman first so /usr/lib/firefox/ and /usr/bin/bwrap exist as real
# files before we drop our overrides on top. Doing it in the other
# order (a) fails when /usr/lib/firefox doesn't exist yet, and (b)
# would have pacman's bubblewrap install clobber the fake-bwrap we
# just dropped.
#
# `-Syu` (instead of plain `-S`): refresh the local DB in the same
# transaction we install in, so we never reference a `pkg.tar.xz` the
# mirror has already rolled forward of. `rm -rf /var/cache/pacman/pkg/*`
# afterwards drops the package cache — we never reinstall in place, so
# caching costs only disk. (pacman's own `-Scc --noconfirm` is a no-op
# for safety, see ArchPacmanCommon.installBasePackages.)
"$ROOT_DIR/client/tawc-chroot-run" \
    "pacman -Syu --noconfirm --needed ${PKGS[*]} && rm -rf /var/cache/pacman/pkg/*"

# Drop in `testing/fake-bwrap` over the chroot's `/usr/bin/bwrap`. The
# stock kernel on the test devices ships without `CONFIG_USER_NS`, so
# bubblewrap's `clone(NEWUSER)` always fails — and modern Arch GTK +
# Firefox both pull glycin, which in turn execs bwrap to sandbox each
# image loader. Without the replacement, every gtk-app integration
# test crashes on the first SVG icon. The replacement script walks
# bwrap's argv, skips the sandbox flags, and execs the COMMAND
# directly — fine on a private test rootfs, not safe in production.
HOST_BWRAP="$ROOT_DIR/testing/fake-bwrap"
GUEST_BWRAP="/data/data/me.phie.tawc/distros/arch/rootfs/usr/bin/bwrap"
echo "=== Installing fake bwrap (no CONFIG_USER_NS workaround) ==="
adb push "$HOST_BWRAP" /data/local/tmp/fake-bwrap
adb shell "su -c 'install -m 0755 /data/local/tmp/fake-bwrap $GUEST_BWRAP'"

# Firefox autoconfig. Without this, Firefox 150 on the test devices
# tries to spawn a separate GPU process for WebRender. Under proot the
# fork-server path that GPU process startup goes through never lands a
# running child (Mozilla's gfxPlatformGtk gives up after a single
# silent failure and disables hardware acceleration for the rest of
# the session), so chrome falls back to GTK's cairo software renderer
# and the compositor sees only `wl_shm` commits — magenta-tinted, no
# AHB. Forcing WebRender on AND running it in the parent process
# (no GPU process) keeps the EGL/AHB path active. `widget.dmabuf.
# force-enabled=true` is what tells Firefox to import its WebRender
# output as a `zwp_linux_dmabuf_v1` buffer (i.e. bind it as an AHB
# via libhybris's `android_wlegl`) instead of falling back to
# read-back-into-shm. (The earlier Firefox bionic-CFI crash that
# landed us in this code path is gone now that libhybris hooks
# `__cfi_slowpath{,_diag}` at symbol-resolution time — see
# `libhybris/TAWC_FORK.md`.) Chroot doesn't need these prefs (its
# GPU process spawns cleanly through real `chroot(2)`/`mount(2)`
# instead of `run-as`+ptrace) but flipping them there is harmless.
HOST_FF_CFG="$ROOT_DIR/testing/firefox.cfg"
HOST_FF_AUTOCFG="$ROOT_DIR/testing/firefox-autoconfig.js"
GUEST_FF_PREFIX="/data/data/me.phie.tawc/distros/arch/rootfs/usr/lib/firefox"
echo "=== Installing Firefox prefs (autoconfig) ==="
adb push "$HOST_FF_CFG" /data/local/tmp/firefox.cfg
adb push "$HOST_FF_AUTOCFG" /data/local/tmp/firefox-autoconfig.js
adb shell "su -c 'install -m 0644 /data/local/tmp/firefox.cfg $GUEST_FF_PREFIX/firefox.cfg && \
                  install -m 0644 -D /data/local/tmp/firefox-autoconfig.js $GUEST_FF_PREFIX/defaults/pref/autoconfig.js'"

echo "=== Done ==="
