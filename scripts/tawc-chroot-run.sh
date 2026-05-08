#!/bin/bash
# Enter the in-app Linux installation via the dev exec broker.
#
# All chroot work — mount setup, bind table, setsid, exec — lives in
# Kotlin behind the broker's RUNINSIDE request type
# (notes/exec-broker.md). The broker reads the install's recorded
# method from `metadata.json` and dispatches to the matching
# [InstallationMethod.startInside]. Identical Kotlin entrypoint as the
# in-app installer / launcher, so there's only one place "enter the
# chroot" semantics live.
#
# Requires:
#   - the `me.phie.tawc` debug-build APK installed (broker is
#     debug-build-only)
#   - the app launched at least once so the broker is bound (this
#     script auto-launches MainActivity if the broker isn't reachable)
#   - root via Magisk `su` for chroot installs only — the broker forks
#     `su` itself; nothing outside the JVM needs it.
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

if [ $# -gt 0 ]; then
    exec "$TAWC_EXEC_BIN" --in-chroot "$INSTALL_ID" -- "$@"
else
    exec "$TAWC_EXEC_BIN" --in-chroot "$INSTALL_ID"
fi
