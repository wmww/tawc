#!/bin/bash
# Enter the in-app Linux installation via adb.
#
# All install methods render their per-install
# `<distros>/<id>/enter.sh` script with the launcher logic baked in;
# this script just reads the recorded method from `metadata.json`,
# then dispatches:
#
#   - tawcroot / proot installs run as the app uid in the app's
#     untrusted_app SELinux domain — invoked via the dev exec broker
#     (see notes/exec-broker.md), which forks ProcessBuilder inside
#     the app process. Production-faithful, no `run-as` quirks.
#   - chroot installs need root (chroot(2) is privileged), so they
#     go via `adb shell su -c '<enter.sh> <b64>'`.
#
# Requires:
#   - the `me.phie.tawc` debug-build APK installed (broker is
#     debug-build-only)
#   - the app launched at least once so the broker is bound (this
#     script auto-launches MainActivity if the broker isn't reachable)
#   - root via Magisk `su` for chroot installs only
#
# Usage:
#   scripts/tawc-chroot-run.sh                       # interactive shell
#   scripts/tawc-chroot-run.sh "<command>"           # run a command and exit
#   TAWC_INSTALL_ID=<id> scripts/tawc-chroot-run.sh  # pin a specific install
#                                                  (otherwise auto-detected
#                                                  if exactly one is present)
set -euo pipefail

# Honour TAWC_TARGET / .tawctarget so a multi-device dev loop picks the
# right one. This sets ANDROID_SERIAL which adb reads natively.
_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$_script_dir/lib/select-device.sh"
# shellcheck disable=SC1091
. "$_script_dir/lib/tawc-exec.sh"
# shellcheck disable=SC1091
. "$_script_dir/lib/tawc-install-id.sh"

INSTALL_ID="$TAWC_INSTALL_ID"
PKG="me.phie.tawc"
INSTALL_DIR="/data/data/$PKG/distros/$INSTALL_ID"
ENTER="$INSTALL_DIR/enter.sh"
META="$INSTALL_DIR/metadata.json"

# Read the install method from metadata.json via the broker (runs as
# app uid, can read the private data dir directly).
read_method() {
    local raw
    raw=$("$TAWC_EXEC_BIN" /system/bin/cat "$META" 2>/dev/null) || true
    # "method": "chroot" | "method": "proot" | "method": "tawcroot".
    # First-match-then-exit so a future field whose name happens to
    # contain the substring "method" doesn't masquerade.
    echo "$raw" | awk -F'"' '/"method"[[:space:]]*:/{print $4; exit}'
}

METHOD=$(read_method)
if [ -z "$METHOD" ]; then
    cat >&2 <<EOF
ERROR: $META not found or unreadable on the device.

Did you install the chroot? From the host:
  bash scripts/install-distro.sh $INSTALL_ID [tawcroot|proot|chroot]

Progress streams to your TTY; the in-app log screen also opens.
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
    proot|tawcroot)
        # Both run as the app uid. Default TMPDIR is unset and mksh
        # falls back to /tmp / /data/local — neither writable by the
        # app uid, which breaks its here-doc temp-file path. `cd` and
        # set TMPDIR to a cache subdir first. (proot actually uses
        # TMPDIR for its own scratch; tawcroot doesn't but mksh's
        # here-doc path still wants somewhere writable.)
        SCRATCH="/data/data/$PKG/cache/$METHOD-tmp"
        # `/system/bin/sh $ENTER` (not `exec $ENTER` directly): SELinux's
        # untrusted_app domain doesn't allow execve of files in app
        # data_file, but reading them does work — so the inner sh
        # interprets enter.sh as text rather than the kernel exec'ing
        # the script's #! line itself.
        if [ $# -gt 0 ]; then
            cmd_b64=$(printf '%s' "$*" | base64 | tr -d '\n')
            exec "$TAWC_EXEC_BIN" --cwd "$SCRATCH" --env "TMPDIR=$SCRATCH" --env "PATH=/system/bin:/system/xbin" \
                /system/bin/sh -c "mkdir -p $SCRATCH && cd $SCRATCH && exec /system/bin/sh $ENTER $cmd_b64"
        else
            exec "$TAWC_EXEC_BIN" --cwd "$SCRATCH" --env "TMPDIR=$SCRATCH" --env "PATH=/system/bin:/system/xbin" \
                /system/bin/sh -c "mkdir -p $SCRATCH && cd $SCRATCH && exec /system/bin/sh $ENTER"
        fi
        ;;
    *)
        echo "ERROR: unknown install method '$METHOD' in $META" >&2
        exit 1
        ;;
esac
