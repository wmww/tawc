#!/bin/bash
# Enter the in-app Linux installation via adb.
#
# Both install methods (chroot and proot) render their per-install
# `<distros>/<id>/enter.sh` script with the launcher logic baked in;
# this script just delivers the (base64-encoded) command to the right
# script via the right adb-side wrapper:
#
#   - chroot installs need root, so we go via `adb shell su -c '<enter.sh>
#     <b64>'`.
#   - proot installs run as the app uid, so we go via `adb shell run-as
#     me.phie.tawc <enter.sh> <b64>`. (Requires the APK to be debuggable
#     — true for the dev/debug build, not for release.)
#
# We read the install's recorded method from `metadata.json`. `run-as`
# works for both methods (since both are this app's own data dir), so
# the read itself doesn't need root. If `run-as` isn't available
# (release build) we fall back to `su` and let chroot vs proot dispatch
# the rest.
#
# Requires:
#   - the `me.phie.tawc` app installed
#   - an installation present at /data/data/me.phie.tawc/distros/<id>/
#     (install via `am start -n me.phie.tawc/.install.InstallActivity
#     --es autoStart true --es id <id> [--es method chroot|proot]`,
#     or the home-screen Install button).
#   - root via Magisk `su` granted to adb shell, OR a debuggable APK.
#
# Usage:
#   client/tawc-chroot-run                       # interactive shell
#   client/tawc-chroot-run "<command>"           # run a command and exit
#   TAWC_INSTALL_ID=<id> client/tawc-chroot-run  # pin a specific install
#                                                  (otherwise auto-detected
#                                                  if exactly one is present)
set -euo pipefail

# Honour TAWC_TARGET / .tawctarget so a multi-device dev loop picks the
# right one. This sets ANDROID_SERIAL which adb reads natively.
_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$_script_dir/select-device.sh"
# shellcheck disable=SC1091
. "$_script_dir/tawc-install-id.sh"

INSTALL_ID="$TAWC_INSTALL_ID"
PKG="me.phie.tawc"
INSTALL_DIR="/data/data/$PKG/distros/$INSTALL_ID"
ENTER="$INSTALL_DIR/enter.sh"
META="$INSTALL_DIR/metadata.json"

# Read the install method from metadata.json. Try `run-as` first (works
# for any debuggable build, no root needed), fall back to `su` for
# release builds with root. If neither works, the existence check below
# fires a more useful error than a silent failure here.
read_method() {
    local raw
    raw=$(adb shell "run-as $PKG cat $META 2>/dev/null || su -c 'cat $META' 2>/dev/null") || true
    # "method": "chroot" | "method": "proot". The metadata.json schema
    # has a single `method` field, but use awk + first-match-then-exit
    # rather than greedy sed so a future field whose name happens to
    # contain the substring "method" doesn't masquerade.
    echo "$raw" | awk -F'"' '/"method"[[:space:]]*:/{print $4; exit}'
}

METHOD=$(read_method)
if [ -z "$METHOD" ]; then
    cat >&2 <<EOF
ERROR: $META not found or unreadable on the device.

Did you install the chroot? From the host:
  adb shell am start -n me.phie.tawc/.install.InstallActivity \\
      --es autoStart true --es id $INSTALL_ID \\
      [--es method chroot|proot]

Tail progress:
  adb logcat -s tawc-install
EOF
    exit 1
fi

case "$METHOD" in
    chroot)
        # Same as before: chroot needs root, su -c quotes the b64 in.
        if [ $# -gt 0 ]; then
            cmd_b64=$(printf '%s' "$*" | base64 | tr -d '\n')
            exec adb shell "su -c '$ENTER $cmd_b64'"
        else
            exec adb shell -t "su -c '$ENTER'"
        fi
        ;;
    proot)
        # run-as me.phie.tawc switches to the app uid. Default TMPDIR
        # is unset and mksh falls back to /tmp / /data/local, neither
        # writable by app uid — its here-doc temp file then fails. cd +
        # set TMPDIR to the app's cache dir before exec'ing enter.sh.
        SCRATCH="/data/data/$PKG/cache/proot-tmp"
        if [ $# -gt 0 ]; then
            cmd_b64=$(printf '%s' "$*" | base64 | tr -d '\n')
            exec adb shell "run-as $PKG sh -c 'mkdir -p $SCRATCH && cd $SCRATCH && export TMPDIR=$SCRATCH && exec $ENTER $cmd_b64'"
        else
            exec adb shell -t "run-as $PKG sh -c 'mkdir -p $SCRATCH && cd $SCRATCH && export TMPDIR=$SCRATCH && exec $ENTER'"
        fi
        ;;
    tawcroot)
        # Same shape as proot — runs as app uid via run-as, scratch dir
        # under cacheDir. tawcroot doesn't need its own per-process tmp
        # dir (no extracted loader stub like proot), but mksh's
        # here-doc still wants TMPDIR pointed somewhere writable.
        SCRATCH="/data/data/$PKG/cache/tawcroot-tmp"
        if [ $# -gt 0 ]; then
            cmd_b64=$(printf '%s' "$*" | base64 | tr -d '\n')
            exec adb shell "run-as $PKG sh -c 'mkdir -p $SCRATCH && cd $SCRATCH && export TMPDIR=$SCRATCH && exec $ENTER $cmd_b64'"
        else
            exec adb shell -t "run-as $PKG sh -c 'mkdir -p $SCRATCH && cd $SCRATCH && export TMPDIR=$SCRATCH && exec $ENTER'"
        fi
        ;;
    *)
        echo "ERROR: unknown install method '$METHOD' in $META" >&2
        exit 1
        ;;
esac
