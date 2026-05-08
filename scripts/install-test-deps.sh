#!/bin/bash
# Install the chroot packages the integration test suite needs.
#
# Run once per chroot install. Idempotent: re-running just makes the
# package manager confirm everything's already there. The integration
# tests deliberately do NOT install anything at runtime — they assume
# these packages are present, so test runs aren't distro-specific and
# don't surprise you with package installs in the middle of a test.
#
# Distro-aware: reads `metadata.json` to dispatch between pacman
# (Arch / Manjaro) and xbps (Void) install paths. Same logical
# package set on each side, with names translated.
#
# Prerequisites:
#   - Android device or emulator connected via adb and selected via
#     ./.tawctarget or TAWC_TARGET=physical|emulator (see
#     scripts/lib/select-device.sh)
#   - In-app chroot installed at
#     /data/data/me.phie.tawc/distros/<id>/. Auto-targeted when exactly
#     one install is present; set TAWC_INSTALL_ID=<id> to pin one.
#
# Usage:
#   bash scripts/install-test-deps.sh
#   TAWC_INSTALL_ID=void bash scripts/install-test-deps.sh
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

TAWC_DISTROS_DIR="/data/data/me.phie.tawc/distros/$TAWC_INSTALL_ID"

# Read the distro key out of metadata.json via the broker (runs as app
# uid, can read the private data dir directly — works for every install
# method, no root needed).
DISTRO_KEY=$("$TAWC_EXEC_BIN" /system/bin/cat "$TAWC_DISTROS_DIR/metadata.json" \
    | tr -d '\r' \
    | sed -n 's/.*"distro"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    | head -n1)
if [ -z "$DISTRO_KEY" ]; then
    echo "ERROR: could not read distro key from $TAWC_DISTROS_DIR/metadata.json" >&2
    exit 1
fi
echo "=== Detected distro: $DISTRO_KEY ==="

case "$DISTRO_KEY" in
    arch|manjaro)
        # Pacman package set. Comments name the test cases each item
        # exists for; matches the fake-bwrap + Firefox-prefs install
        # below.
        PKGS=(
            # gtk4-debug-app build (compiled in chroot by ensure_debug_app)
            gtk4 pkg-config
            # apps:: tests — gtk3-demos provides gtk3-demo-application,
            # gtk4-demos provides gtk4-widget-factory; firefox + supertuxkart
            # are real-app tests on hardware buffers.
            gtk3 gtk3-demos gtk4-demos firefox supertuxkart
            # graphics:: tests
            mesa-utils weston vulkan-tools
            # apps::test_xwayland_xclock_renders_via_shm — pure-X11 client
            # exercising our bionic-built Xwayland (see notes/xwayland.md).
            xorg-xclock
            # apps::test_es2gears_x11_renders_via_ahb — real-app GLES-on-X11
            # client driving the libhybris X11 EGL platform plugin.
            mesa-demos
        )
        # `-Syu` (instead of plain `-S`): refresh local DB in the same
        # transaction we install in, so we never reference a `pkg.tar.xz`
        # the mirror has already rolled forward of. `rm -rf
        # /var/cache/pacman/pkg/*` afterwards drops the package cache —
        # we never reinstall in place. (pacman's own `-Scc --noconfirm`
        # is a no-op for safety, see ArchPacmanCommon.)
        INSTALL_CMD="pacman -Syu --noconfirm --needed ${PKGS[*]} && rm -rf /var/cache/pacman/pkg/*"
        ;;
    void)
        # Void package set. Logical match for the pacman list:
        #   gtk4              -> gtk4 + gtk4-devel
        #   gtk3              -> gtk+3 + gtk+3-devel
        #   gtk3-demos        -> gtk+3-demo (gtk3-demo, gtk3-widget-factory)
        #   gtk4-demos        -> gtk4-demo  (gtk4-demo, gtk4-widget-factory)
        #   mesa-utils        -> glxinfo    (Void splits this out of mesa-demos)
        #   mesa-demos        -> mesa-demos (es2gears_x11 lives here)
        #   vulkan-tools      -> Vulkan-Tools
        #   xorg-xclock       -> xclock
        # supertuxkart, weston, firefox, pkg-config keep their names.
        #
        # `-devel` packages bring the headers + `.pc` files we need
        # for gtk4-debug-app's `pkg-config --cflags --libs gtk4` step.
        # Arch's main packages ship them; Void splits them out.
        #
        # `mesa-dri` is the open-source GL stack (DRI drivers). On a
        # real ARM device it would sit dormant — libhybris hijacks the
        # GL/EGL entry points via `LD_LIBRARY_PATH=/usr/local/lib/gl-
        # shims:/usr/local/lib` and the bionic-Mesa wrapped behind it
        # provides every `libGL.so.1` / `libEGL.so.1` / `libGLESv2.so.2`
        # symbol. But `tawc-emu` doesn't ship libhybris on x86_64 (no
        # x86_64 GPU bionic blob to wrap; see notes/emulator.md), so on
        # the emulator gtk4 falls through to the chroot's real Mesa.
        # Without `mesa-dri` it has no driver to load, the GL init path
        # NULL-branches, and gtk4 segfaults inside libharfbuzz on the
        # first text reshape. Bundling it unconditionally costs ~50 MB
        # on a real device (which won't ever load it) — cheaper than
        # branching the test-deps list by ABI.
        #
        # `dejavu-fonts-ttf` covers the second NULL-deref: the Void
        # bootstrap ships fontconfig + freetype but no actual font
        # files, and harfbuzz NULL-derefs trying to shape with no
        # usable face. Arch's bootstrap already has Bitstream / Liberation.
        PKGS=(
            gtk4 gtk4-devel pkg-config
            gtk+3 gtk+3-devel gtk+3-demo gtk4-demo firefox supertuxkart
            glxinfo weston Vulkan-Tools
            xclock
            mesa-demos mesa-dri
            dejavu-fonts-ttf
        )
        # xbps quirk: `xbps-install -uy <pkgs>` updates only the listed
        # packages + their deps, NOT the whole system. Without an
        # in-between sysupgrade, fresh packages we install can pull a
        # new libuuid/libblkid that's SONAME-incompatible with the
        # already-installed util-linux, and xbps aborts with "in
        # transaction breaks installed pkg" (caught in the wild between
        # base install and a later test-deps run). So: full sysupgrade
        # first, then install the test packages.
        #
        # No cache wipe here — VoidCommon.installBasePackages already
        # cleared the cache at chroot-install time, and re-running this
        # script after a partial failure is a lot cheaper if the per-
        # package `.xbps` files haven't been blown away.
        INSTALL_CMD="xbps-install -Suy && xbps-install -y ${PKGS[*]}"
        ;;
    *)
        echo "ERROR: unsupported distro '$DISTRO_KEY' (expected arch / manjaro / void)" >&2
        exit 1
        ;;
esac

echo "=== Installing chroot test deps: ${PKGS[*]} ==="

# Install the packages first so /usr/lib/firefox/ and /usr/bin/bwrap
# exist as real files before we drop our overrides on top. Doing it in
# the other order (a) fails when /usr/lib/firefox doesn't exist yet,
# and (b) would have the package install clobber the fake-bwrap we
# just dropped.
"$ROOT_DIR/scripts/tawc-rootfs-run.sh" "$INSTALL_CMD"

# Drop in `tests/apps/fake-bwrap` over the chroot's `/usr/bin/bwrap`. The
# stock kernel on the test devices ships without `CONFIG_USER_NS`, so
# bubblewrap's `clone(NEWUSER)` always fails — and modern Arch GTK +
# Firefox both pull glycin, which in turn execs bwrap to sandbox each
# image loader. Without the replacement, every gtk-app integration
# test crashes on the first SVG icon. The replacement script walks
# bwrap's argv, skips the sandbox flags, and execs the COMMAND
# directly — fine on a private test rootfs, not safe in production.
HOST_BWRAP="$ROOT_DIR/tests/apps/fake-bwrap"
GUEST_BWRAP="$TAWC_DISTROS_DIR/rootfs/usr/bin/bwrap"
echo "=== Installing fake bwrap (no CONFIG_USER_NS workaround) ==="
adb push "$HOST_BWRAP" "$TAWC_SCRATCH/fake-bwrap"
# `install -m 0755` via the broker — runs as the app uid, which owns
# the rootfs tree, so no su needed.
"$TAWC_EXEC_BIN" /system/bin/sh -c "install -m 0755 $TAWC_SCRATCH/fake-bwrap $GUEST_BWRAP"

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
# read-back-into-shm. Chroot doesn't need these prefs (its GPU process
# spawns cleanly through real `chroot(2)`/`mount(2)` instead of
# `run-as`+ptrace) but flipping them there is harmless.
HOST_FF_CFG="$ROOT_DIR/tests/fixtures/firefox.cfg"
HOST_FF_AUTOCFG="$ROOT_DIR/tests/fixtures/firefox-autoconfig.js"
GUEST_FF_PREFIX="$TAWC_DISTROS_DIR/rootfs/usr/lib/firefox"
echo "=== Installing Firefox prefs (autoconfig) ==="
adb push "$HOST_FF_CFG" "$TAWC_SCRATCH/firefox.cfg"
adb push "$HOST_FF_AUTOCFG" "$TAWC_SCRATCH/firefox-autoconfig.js"
"$TAWC_EXEC_BIN" /system/bin/sh -c "install -m 0644 $TAWC_SCRATCH/firefox.cfg $GUEST_FF_PREFIX/firefox.cfg && \
                             install -m 0644 -D $TAWC_SCRATCH/firefox-autoconfig.js $GUEST_FF_PREFIX/defaults/pref/autoconfig.js"

echo "=== Done ==="
