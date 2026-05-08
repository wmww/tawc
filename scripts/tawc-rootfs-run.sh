#!/bin/bash
# Enter the in-app Linux installation via the dev exec broker.
#
# All rootfs-entry work — mount setup, bind table, setsid, exec —
# lives in Kotlin behind the broker's RUNINSIDE request type
# (notes/exec-broker.md). The broker reads the install's recorded
# method from `metadata.json` and dispatches to the matching
# [InstallationMethod.startInside]. Identical Kotlin entrypoint as
# the in-app installer / launcher, so there's only one place
# "enter the rootfs" semantics live.
#
# Requires:
#   - the `me.phie.tawc` debug-build APK installed (broker is
#     debug-build-only)
#   - the app launched at least once so the broker is bound (this
#     script auto-launches MainActivity if the broker isn't reachable)
#   - root via Magisk `su` for chroot-method installs only — the
#     broker forks `su` itself; nothing outside the JVM needs it.
#
# Usage:
#   scripts/tawc-rootfs-run.sh                       # interactive shell
#   scripts/tawc-rootfs-run.sh "<command>"           # run a command and exit
#   TAWC_INSTALL_ID=<id> scripts/tawc-rootfs-run.sh  # pin a specific install
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

# Non-interactive `tawc-rootfs-run.sh "<cmd>"` mirrors stdio into the
# in-app log screen by default — the user's mental model is "I asked the
# phone to run this; show me what's happening on the phone too." Skip
# the panel for interactive shells (panel can't usefully render them)
# and when TAWC_OP_TITLE= is set to empty (escape hatch for callers
# that want pure stdio relay, e.g. nested tawc-rootfs-run inside a
# script that already shows its own panel). Default title trims long
# command lines so the toolbar stays readable.
default_title() {
    # 60-char total budget on the toolbar (install id prefix included).
    # Bash ${var:0:n} is character-indexed under a UTF-8 locale, which
    # any dev workstation will have — falls back to byte slicing under
    # LC_ALL=C, which is fine for the all-ASCII commands we expect.
    local prefix="$INSTALL_ID: "
    local cmd="$*"
    local budget=$((60 - ${#prefix}))
    if [ "$budget" -ge 8 ] && [ "${#cmd}" -gt "$budget" ]; then
        cmd="${cmd:0:$((budget - 3))}..."
    fi
    printf '%s%s' "$prefix" "$cmd"
}

if [ $# -gt 0 ]; then
    title="${TAWC_OP_TITLE-$(default_title "$@")}"
    if [ -n "$title" ]; then
        exec "$TAWC_EXEC_BIN" --in-rootfs "$INSTALL_ID" --op-title "$title" -- "$@"
    else
        exec "$TAWC_EXEC_BIN" --in-rootfs "$INSTALL_ID" -- "$@"
    fi
else
    exec "$TAWC_EXEC_BIN" --in-rootfs "$INSTALL_ID"
fi
