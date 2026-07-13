# `pacman -Syu` usecase test can't run: frozen proxy db yields zero upgrades

The `cli-system-upgrade` usecase test needs a real upgrade transaction to
verify `pacman -Syu` doesn't brick the rootfs. On the current setup it
cannot be exercised: **there is nothing to upgrade.**

## Why

- The dev cache proxy caches every 200 for 365 days, including pacman repo
  dbs (`core.db`/`extra.db`/`alarm.db`) — see
  `issues/cache-proxy-stale-pacman-db.md`.
- The Arch Linux ARM install was bootstrapped *through* that frozen proxy,
  so its installed versions == the versions in the cached dbs.
- Therefore `pacman -Sy` re-fetches the same cached dbs and `pacman -Qu`
  reports 0 packages. `pacman -Syu --noconfirm` prints
  `there is nothing to do`.

This is structurally the *inverse* of `cache-proxy-stale-pacman-db.md`
(where the db is newer than the cached packages and downloads 404). Here
the db is exactly as old as the install, so nothing moves.

## Observed 2026-07-13 (physical 50f4ca18, Arch Linux ARM tawcroot)

```
pacman -Sy   → core/extra/alarm/aur downloaded, no errors
pacman -Qu   → 0
pacman -Syu --noconfirm → ":: Starting full system upgrade... there is nothing to do"
```

Rootfs stayed healthy throughout (455 pkgs before and after; bash 5.3.15,
git 2.55.0, pacman 7.1.0 all work; `/usr/lib/hybris` + `/usr/local/bin/ando`
intact; `/usr/share/man` still absent so slimming held; no `.pacnew`;
mirrorlist still 6 proxy lines; resolv.conf still `nameserver 8.8.8.8`).

## What's needed to run the test

Fresher repo dbs than the install, without breaking the "always use the
proxy" rule. Options, none of which a test agent can do unattended:

1. Whoever owns the proxy runs `scripts/cache-proxy.sh wipe '\.db$'` so the
   dbs track upstream while version-named packages stay cached. Then a
   fresh install (or the existing one) would have real upgrades. Risk: the
   `cache-proxy-stale-pacman-db.md` 404 (fresh db referencing packages
   upstream rotated but the proxy hasn't cached) can then bite mid-upgrade
   — and an interrupted `-Syu` is exactly the brick risk the test guards
   against, so this must be done deliberately, not blindly.
2. Give `\.db$`/`.db.sig` a short `proxy_cache_valid` (the real fix
   proposed in `cache-proxy-stale-pacman-db.md`), then re-run the test.

Until then the test is BLOCKED: it can confirm `-Syu` runs cleanly and
doesn't damage a current install, but it cannot exercise the actual
package-replacement / hook / setcap-scriptlet / `.pacnew` paths that are
the interesting failure surface.

## Note for whoever picks this up

CLAUDE.md forbids test agents from wiping `build/cache-proxy/cache/`
("ask the user"), so the wipe must be a human/user action.
