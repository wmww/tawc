# Cache proxy (dev-time)

Host-side caching reverse proxy so the device fetches distro rootfs
tarballs and package files through us. Kills the gigs-of-redownload
pain on slow upstream mirrors during install/uninstall test cycles.
Side benefit: cache-forever gives bit-identical packages across runs
(good for repro / bisect).

**Dev-time only** — debug builds wire it up; release APKs ignore the
intent extra.

**Always run with the proxy when iterating on installs.** With
`--es mirrorProxy …` set, the rootfs's mirror config is rewritten to
point at `127.0.0.1:8080` exclusively — there is no fallback to direct
upstream. If the proxy isn't running, every install fails loudly with
`ConnectException`, which is the desired behaviour: silent fallback
would defeat the cache and make timing comparisons meaningless.

## Usage

```bash
# Terminal A: start the proxy (foregrounds; ^C to stop). State and
# cache live under build/cache-proxy/ — preserved across runs.
bash scripts/cache-proxy.sh run

# Terminal B: install with the proxy URL passed in.
bash scripts/install-distro.sh manjaro tawcroot \
    distro=manjaro \
    mirrorProxy=http://127.0.0.1:8080/proxy/
```

The script has subcommands — run `bash scripts/cache-proxy.sh` (no args)
or `… help` for the full list. Beyond `run`, useful ones are:

- `list` — per-host sizes + total (sorted by size desc).
- `list <regex>` — every cached URL matching `<regex>` (extended regex)
  with sizes + total. Use to find what's actually in the cache before
  selectively evicting.
- `wipe <regex>` — delete every cache entry whose URL matches. No
  confirmation prompt — pair with `list <regex>` first to preview.
  Errors out on no match (so a stale regex doesn't silently no-op).

The proxy script polls `adb devices` once a second and applies
`adb reverse tcp:8080 tcp:8080` to every device that's currently
connected — works identically for emulator and physical, picks up
plug/unplug cycles and AVD restarts, and logs a one-line status
change on every connect/disconnect transition.

The cache is **persistent across runs** — `build/cache-proxy/` is
gitignored along with the rest of `build/`, but never auto-wiped. To
sweep it manually: `rm -rf build/cache-proxy/cache/`. For selective
evict use `bash scripts/cache-proxy.sh wipe <regex>` (URLs are matched
as extended regex). CLAUDE.md instructs agents not to wipe it without
explicit user request.

## Architecture

- nginx caching reverse proxy on the host, listening on `127.0.0.1:8080`.
- Device reaches it via `adb reverse tcp:8080 tcp:8080` — works
  identically for emulator and physical, no IP discovery, no LAN
  exposure, no auth needed.
- URL format: `http://127.0.0.1:8080/proxy/<scheme>/<host>/<path>`.
  nginx rewrites that to `<scheme>://<host>/<path>` upstream and
  caches the response keyed by the upstream URL (without query
  string — see below).
- The proxy has **no mirror list of its own** — the upstream URL is
  encoded in every request, so the same proxy handles all distros and
  any mirror. Single source of truth for mirrors stays in our existing
  per-distro install code.

## Files

- `scripts/cache-proxy.conf` — nginx config (single `server`, single
  `location` regex, all state under the prefix dir).
- `scripts/cache-proxy.sh` — foregrounds nginx, polls `adb devices`
  once a second to apply `adb reverse` to every connected device,
  traps cleanup on `EXIT|INT|TERM`.
- `app/src/main/java/me/phie/tawc/install/MirrorProxy.kt` — URL
  rewriter (`https://example.com/path` → `<base>/https/example.com/path`).
- `Installer` and each `Distro` take a `mirrorProxy: MirrorProxy?`
  parameter; when non-null, every install-time fetch and every
  package-mirror config file we write into the rootfs gets rewritten
  through it.

## Cache key, redirects, and 30x

```nginx
proxy_pass        $1://$2/$3$is_args$args;
proxy_cache_key   "$1://$2/$3";
proxy_cache_valid 200 206 365d;
proxy_redirect ~^(https?)://([^/]+)/(.*)$ /proxy/$1/$2/$3;
```

- `proxy_cache_key` strips the query, but `proxy_pass` keeps it. So
  GitHub release downloads (which 302 to
  `objects.githubusercontent.com?X-Amz-Signature=…`) hit one cache
  entry per path even though every request carries a fresh signature.
- `proxy_cache_valid 200 206 365d` caches **everything** including
  repo metadata (`Packages.gz`, `Release`, `core.db`, …) for a year.
  This is intentional — locked package versions across runs.
- `proxy_redirect` rewrites Location headers from upstream so 30x
  redirects bounce back through the proxy. Without this, the HTTP
  client would follow straight to the redirect target, bypass the
  cache, and the second install would re-download.
- `proxy_buffer_size 16k` — GitHub release-redirect headers are
  bigger than the 4 KiB default and overflow the buffer otherwise.
- 30x responses themselves are **not** cached. The redirect carries
  signed time-limited tokens that change per request; caching them
  would point old requests at expired tokens.
- `proxy_ssl_server_name on; proxy_ssl_name $2;` — variable-based
  `proxy_pass` defaults to no SNI, so Fastly-fronted mirrors (e.g.
  `repo-fastly.voidlinux.org`) reply 421 Misdirected Request because
  the served default cert doesn't cover the requested Host. Forcing
  SNI to match the proxied hostname fixes it.
- Range requests aren't cached by stock nginx. apt/pacman/xbps fall
  back to full GET fine; revisit with the `slice` module if it ever
  bites.

## Install-side plumbing

The `install` broker action accepts `--arg mirrorProxy=<url>` and
forwards it through `InstallationService.startInstall` to `Installer`'s
constructor; the in-app form's "Use cache proxy" checkbox feeds the
same path with the standard local URL.
`Installer.install`:

1. Rewrites the bootstrap tarball URL through the proxy before
   passing it to `BootstrapCache.download`. (`Installation.sourceUrl`
   in `metadata.json` still records the canonical upstream URL — the
   proxy URL is purely a wire detail.)
2. Forwards `mirrorProxy` to `Distro.configure`, which rewrites
   per-distro mirror config:
   - **Arch / Manjaro:** `Server = <url>` lines in
     `/etc/pacman.d/mirrorlist` (handled in
     `ArchPacmanCommon.configure` via the `SERVER_LINE_RE` regex).
   - **Void:** `repository=<url>` in `/etc/xbps.d/00-repository-main.conf`
     (handled in `VoidCommon.configure`).
   - Pacman's `$repo`/`$arch` substitutions and xbps's `aarch64`
     suffix happen after URL composition, so dollar signs survive
     verbatim through `MirrorProxy.wrap`.

The `mirrorProxy` value is **not** persisted in `metadata.json` —
this is dev-only and we don't want a release APK to accidentally
respect a stale field.

`InstallationService` gates `mirrorProxy` on `BuildConfig.DEBUG`
(set in `app/build.gradle.kts` via `buildFeatures { buildConfig = true }`),
so a release APK ignores the extra entirely. Debug builds log
`[install] using mirror proxy <base>` when one is in effect.

## What flows through the proxy

- Rootfs tarballs (Manjaro ARM, Arch ALARM, Arch x86, Void) — the
  giant initial download.
- Per-package files (`.pkg.tar.zst`, `.xbps`, `.deb`).
- Repo metadata (`Packages.gz`, `Release`, `core.db`, mirrorlist
  files, etc.).

URL-keyed; the same proxy serves all distros with no per-distro
config.

## What does **not** flow through the proxy

- PGP `.sig` files, ALARM `.md5` sidecars, and the GitHub Releases
  REST API (`api.github.com/repos/.../releases/latest` for Manjaro).
  These are tiny and proxying them would either undo the cross-mirror
  cross-check (in the case of `.md5` sidecars from independent
  hosts) or save no measurable time (REST API).
- Anything generated by the chroot itself — pacman keyring imports,
  e.g. — those are local files and never hit the network.

## Security / safety

- Listens on `127.0.0.1` only (host loopback). Only adb-connected
  processes on the device reach it via `adb reverse`. Nothing on the
  LAN can hit the cache.
- Package signatures still validate end-to-end: signatures are over
  file contents, transport doesn't matter. HTTPS upstream is real
  (cert validation by nginx); the device sees plain HTTP from
  `127.0.0.1` and that's fine.
- The `User-Agent: tawc-cache-proxy` header on the upstream side
  makes proxy traffic distinguishable in mirror logs (good citizenship).

## Known limitations

- The mirror config baked into the rootfs hard-codes the
  `127.0.0.1:8080` URLs. After a proxied install, running pacman /
  xbps inside the chroot **without** the proxy running will fail.
  Re-run `bash scripts/cache-proxy.sh run` (and let it apply
  `adb reverse`), or rewrite the mirrorlist by hand. We accept this
  for dev because the user opts in explicitly per install.
- Range requests (HTTP 206) aren't cached because stock nginx
  doesn't slice on `bytes=` — apt/pacman/xbps don't use them in
  practice, so this only matters for ad-hoc curl tests.

## Measured speedup

Verification run (Manjaro/tawcroot on physical aarch64; `mirror.alwyzon.net`
~180 KB/s upstream that day; bootstrap tarball was in cache for both runs):

| Stage          | Cold (160s) | Warm (62s) |
|----------------|-------------|------------|
| DOWNLOADING    | 0.005s      | 0.004s     |
| VERIFYING      | <0.1s       | <0.1s      |
| EXTRACTING     | 10.0s       | 9.9s       |
| CONFIGURING    | 0.9s        | 0.9s       |
| PKG_KEYRING    | 4.6s        | 4.5s       |
| PKG_INSTALL    | 144s        | 46s        |
| **Total**      | **160s**    | **62s**    |

Cache size after cold: 524 MB. After warm: also 524 MB (zero new
fetches — every `pacman -Syyu` request hit cache). PKG_INSTALL alone
is 3.1× faster; full install 2.6× faster. With a fully-cold cache
(no bootstrap tarball pre-warmed) the cold run would have included
another ~150 s for the tarball, pushing the speedup closer to ~3.5×
on a slow upstream day.

## Host deps

- `nginx` (single package on every distro). Listed in
  notes/building.md "Always required" as optional.
- `adb` (already required).
