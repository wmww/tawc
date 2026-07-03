# Cache proxy serves stale pacman dbs, breaking fresh Arch installs

The dev cache proxy caches every 200 response for 365 days
(`scripts/cache-proxy.conf` `proxy_cache_valid 200 206 365d`),
including pacman repo databases (`core.db` / `extra.db`). Arch mirrors
rotate package files out as versions bump, so once the cached db ages
past upstream's package set, any in-rootfs `pacman -Syyu`/`-S` 404s on
files the stale db references and the install fails:

```
pacman: error: failed retrieving file 'mesa-1:26.0.6-1-x86_64.pkg.tar.zst'
        from 127.0.0.1:8080 : The requested URL returned error: 404
FAILED: pacman -Syyu --needed install failed (exit=1)
```

Observed 2026-06-09 on the x86_64 emulator: cached `extra.db` listed
mesa `1:26.0.6-1`; upstream geo.mirror.pkgbuild.com had `1:26.1.2-1`.
This blocks any manual fresh install through the proxy, and in-rootfs
`pacman -Syu` runs such as the run-integration-tests.sh test-deps
install step. (The `external_binds` integration test used to be
affected too; it was deleted 2026-07 — tests no longer install
anything through the proxy.)

Workaround: whoever runs the proxy wipes the cached dbs (cheap, few MB;
packages stay cached):

```
scripts/cache-proxy.sh wipe '\.db$'
```

Note the proxy may be running from a different checkout — the cache
lives under that checkout's `build/cache-proxy/cache/`.

Possible real fix: give `\.db$` (and `.db.sig`) requests a short
`proxy_cache_valid` (e.g. minutes-hours) in `scripts/cache-proxy.conf`
via a `map`/separate `location`, so dbs track upstream while packages
stay cached for a year. Package files are version-named and immutable,
so the long TTL is only wrong for the dbs.
