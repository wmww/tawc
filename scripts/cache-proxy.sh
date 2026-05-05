#!/bin/bash
# Dev-time caching reverse proxy for distro mirrors. See
# notes/cache-proxy.md.
#
# Foregrounds nginx (pid + cache under build/cache-proxy/) and re-applies
# `adb reverse tcp:8080 tcp:8080` for every device that comes online so
# the chroot's pacman/xbps/apt can reach the host loopback. Single-
# command lifecycle: run it, ^C when done. The cache itself is preserved
# across runs — wipe only on explicit user request.
#
# Idempotent against re-runs: a previous nginx is killed via its pidfile
# before we start a new one. adb reverse is also idempotent.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$REPO_ROOT/build/cache-proxy"
CONFIG="$REPO_ROOT/scripts/cache-proxy.conf"

mkdir -p "$PREFIX"/{cache,logs,tmp}

# If a previous instance is still around (orphaned shell, ^Z'd parent),
# clean it up before binding 127.0.0.1:8080 ourselves.
if [ -f "$PREFIX/logs/nginx.pid" ]; then
    old_pid=$(cat "$PREFIX/logs/nginx.pid" 2>/dev/null || true)
    if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
        echo "==> killing stale nginx pid=$old_pid"
        kill "$old_pid" 2>/dev/null || true
        # Wait briefly for the socket to free.
        for _ in 1 2 3 4 5; do
            kill -0 "$old_pid" 2>/dev/null || break
            sleep 0.2
        done
    fi
    rm -f "$PREFIX/logs/nginx.pid"
fi

ADB="${ADB:-adb}"
command -v "$ADB" >/dev/null || { echo "ERROR: adb not on PATH" >&2; exit 1; }
command -v nginx >/dev/null || { echo "ERROR: nginx not installed" >&2; exit 1; }

echo "==> starting nginx (prefix=$PREFIX, config=$CONFIG)"
echo "    proxy URL: http://127.0.0.1:8080/proxy/<scheme>/<host>/<path>"
echo "    cache:     $PREFIX/cache"
echo "    logs:      $PREFIX/logs"

# Bring nginx up in the foreground so the trap kills it on Ctrl-C / parent
# exit. Background it ourselves so the adb-reverse loop can run alongside.
nginx -p "$PREFIX/" -c "$CONFIG" -g 'daemon off;' &
NGINX_PID=$!

trap 'kill $NGINX_PID 2>/dev/null; wait 2>/dev/null; exit 0' EXIT INT TERM

# Wait for nginx to bind before announcing readiness.
for _ in $(seq 1 20); do
    if ss -ltn 2>/dev/null | grep -q '127\.0\.0\.1:8080\b' \
       || netstat -ltn 2>/dev/null | grep -q '127\.0\.0\.1:8080\b'; then
        break
    fi
    kill -0 "$NGINX_PID" 2>/dev/null || { echo "ERROR: nginx died — see $PREFIX/logs/error.log" >&2; exit 1; }
    sleep 0.1
done

# adb reverse is idempotent (re-applying when already in place is a
# no-op), so we just re-apply once per second. Cheap, and cuts out the
# version-skew minefield around `adb track-devices` — modern adb
# (platform-tools 35+) emits binary protobuf by default that's not
# parseable in shell. Polling tracks new connects, reboots, AVD
# (re)starts, and unplug→replug cycles uniformly.
#
# We log a one-liner only on **state transitions** (first time a device
# appears, or when it disappears) so the terminal isn't spammed with
# the same line every second.
echo "==> watching for devices (poll interval=1s)"
(
    declare -A seen=()
    while sleep 1; do
        cur=$("$ADB" devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1}')
        # Apply reverse for everything currently connected.
        for serial in $cur; do
            "$ADB" -s "$serial" reverse tcp:8080 tcp:8080 >/dev/null 2>&1 || {
                echo "WARN: adb reverse failed for $serial" >&2
                continue
            }
            if [ -z "${seen[$serial]:-}" ]; then
                echo "==> adb reverse tcp:8080 -> $serial"
                seen[$serial]=1
            fi
        done
        # Note disappearances so a plug-out + plug-in re-prints the
        # apply line for the same serial.
        for serial in "${!seen[@]}"; do
            if ! echo "$cur" | grep -qx "$serial"; then
                echo "==> $serial disconnected"
                unset 'seen[$serial]'
            fi
        done
    done
) &
POLL_PID=$!
trap 'kill $NGINX_PID $POLL_PID 2>/dev/null; wait 2>/dev/null; exit 0' EXIT INT TERM

echo "==> ready (Ctrl-C to stop)"
wait "$NGINX_PID"
