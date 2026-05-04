# Mirror cache (dev-time)

Host-side caching reverse proxy so the device fetches distro rootfs tarballs and package files through us. Eliminates the gigs-of-redownload pain on slow upstream mirrors during repeated install/uninstall test cycles. Side benefit: cache-forever gives bit-identical packages across runs (good for repro / bisect).

**Dev-time only** — never used in production builds.

## Architecture

- nginx caching reverse proxy on the host, listening on `127.0.0.1:8080`.
- Device reaches it via `adb reverse tcp:8080 tcp:8080` — works identically for emulator and physical, no IP discovery, no LAN exposure, no auth needed.
- URL format: `http://127.0.0.1:8080/proxy/<scheme>/<host>/<path>`. nginx rewrites that to `<scheme>://<host>/<path>` upstream and caches the response keyed by the upstream URL.
- The proxy has **no mirror list of its own** — the upstream URL is encoded in every request, so the same proxy handles all distros and any mirror. Single source of truth for mirrors stays in our existing per-distro install code.

## nginx config

`client/mirror-cache.conf`:

```nginx
worker_processes 1;
events { worker_connections 256; }

http {
  resolver 1.1.1.1 ipv6=off;
  proxy_cache_path cache/ keys_zone=m:50m max_size=50g inactive=365d;

  server {
    listen 127.0.0.1:8080;

    location ~ ^/proxy/(https?)/([^/]+)/(.*)$ {
      proxy_pass        $1://$2/$3$is_args$args;
      proxy_set_header  Host $2;
      proxy_cache       m;
      proxy_cache_key   "$1://$2/$3";
      proxy_cache_valid 200 206 365d;
      proxy_cache_lock  on;
    }
  }
}
```

- `resolver` is required because `proxy_pass` uses a variable (no startup-time DNS).
- `proxy_cache_lock on` coalesces parallel requests for the same URL → one upstream fetch even if several test runs race on the same `.deb`.
- `proxy_cache_valid 200 206 365d` caches **everything** including repo metadata (`Packages.gz`, `Release`, `core.db`, …) for a year. This is intentional — locked package versions across runs. To pull fresh state: `rm -rf build/mirror-cache/cache/` (whole) or delete a specific entry by md5 path under `cache/`.
- Range requests aren't cached by stock nginx. apt/pacman/xbps fall back to full GET fine; revisit with the `slice` module if it ever bites.

## Run script

`client/start-mirror-cache`:

```bash
#!/bin/bash
set -euo pipefail
PREFIX=$PWD/build/mirror-cache
mkdir -p "$PREFIX"/{cache,logs}
trap 'kill $(jobs -p) 2>/dev/null' EXIT INT TERM
nginx -p "$PREFIX" -c "$PWD/client/mirror-cache.conf" -g 'daemon off;' &
# Re-apply adb reverse on every device→device transition. track-devices streams
# one line per state change; falls back to a 2s poll loop on older adb.
adb track-devices | while read -r serial state _; do
  [[ ${state:-} == device ]] && adb -s "$serial" reverse tcp:8080 tcp:8080 || true
done
```

- `trap … EXIT` kills nginx when the script dies (Ctrl-C, terminal close, parent exit). Single-command lifecycle: run it, forget it, ^C when done.
- `nginx -g 'daemon off;'` keeps it foregrounded so the trap can hit it.
- Re-applying `adb reverse` on every device transition is idempotent and covers reconnects, reboots, AVDs (re-)starting, etc. Emulator and physical are treated identically.
- All state lives under `build/mirror-cache/` (gitignored with the rest of `build/`). Removing the directory fully resets.

## Install-side plumbing

- Install intent extra: `--es mirrorProxy http://127.0.0.1:8080/proxy/`. Optional; absent → install fetches direct, no proxy involvement.
- Kotlin install code stores `mirrorProxy` in `metadata.json`. When set, every upstream mirror URL the install code constructs gets rewritten as `<proxy>/<scheme>/<host>/<path>` — both for:
  - install-time fetches (rootfs tarball, debootstrap/xbps-install/pacstrap initial package set), and
  - the per-distro mirror config files we write into the rootfs (`/etc/apt/sources.list`, `/etc/xbps.d/*.conf`, `/etc/pacman.d/mirrorlist`).
- We already control all those mirror files per distro, so post-install `apt update` / package installs also flow through the proxy with no chroot post-processing.
- Single point of change in the install code: the helper that turns a canonical mirror URL into the URL we actually fetch / write.

## Toggle / UI

- Install intent extra `--es mirrorProxy <url>` is the primary path (used by tests and adb-driven workflows).
- InstallActivity also exposes a **"Use local proxy mirror"** checkbox, guarded by `BuildConfig.DEBUG` (entire UI element omitted from release builds). When checked, the activity sets `mirrorProxy` to the default `http://127.0.0.1:8080/proxy/` before kicking off the install — equivalent to passing the intent extra by hand.
- `metadata.json`'s `mirrorProxy` field handling is also `BuildConfig.DEBUG`-gated as belt-and-suspenders, so a release APK ignores the field entirely even if a malformed install intent supplies one.

## What gets cached

- Rootfs tarballs (Debian debootstrap output, Void `void-x86_64-ROOTFS-*.tar.xz`, Arch bootstrap, Manjaro ARM image, …) — the giant initial download.
- Per-package files (`.deb`, `.xbps`, `.pkg.tar.zst`).
- Repo metadata (`Packages.gz`, `Release`, `core.db`, `repodata/repomd.xml`, mirrorlist files, etc.).

URL-keyed; the same proxy serves all distros with no per-distro config.

## Security / safety

- Listens on `127.0.0.1` only (host loopback). Only adb-connected processes on the device reach it via `adb reverse`. Nothing on the LAN can hit the cache.
- Package signatures still validate end-to-end: signatures are over file contents, transport doesn't matter. HTTPS upstream is real (cert validation by nginx); the device sees plain HTTP from `127.0.0.1` and that's fine.

## Host deps

- nginx (single package on every distro). Add to `notes/building.md` host-deps list when implementing.
- adb (already required).

## Open work

- Implement `client/mirror-cache.conf`, `client/start-mirror-cache`, and the install-side mirror-URL rewrite helper.
- Pick first distro to wire up (probably Debian — biggest tarball, most-tested install path).
- After landing, document in `notes/building.md` ("if iterating on installs, run `bash client/start-mirror-cache &` and pass `--es mirrorProxy …` to install intents") and add the host-dep line.
