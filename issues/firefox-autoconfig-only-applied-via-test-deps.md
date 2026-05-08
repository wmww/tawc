# Firefox autoconfig prefs only applied via the test-deps script

`tests/fixtures/firefox.cfg` and `tests/fixtures/firefox-autoconfig.js` set the
Firefox prefs that keep WebRender on in the parent process and force
dmabuf/AHB output. Without them, Firefox under tawc falls back to the
GTK/cairo software renderer and the compositor sees only `wl_shm`
commits — magenta-tinted, no AHB.

Today these prefs are only deployed by `scripts/install-test-deps.sh`
into `/usr/lib/firefox/{firefox.cfg, defaults/pref/autoconfig.js}` of
the chroot rootfs. End users following the documented install flow
(Install button or `am start … --es id arch`) get a Firefox that
silently renders SHM regardless of method.

## Symptoms an end user would see

- Firefox launches fine
- Compositor is magenta-tinted on every Firefox frame
- Performance is noticeably worse than chrome with WebRender
- No error visible anywhere — `gfxPlatform` decides once, silently,
  on first GPU process spawn failure (proot) or just defaults to
  off (chroot if WebRender not auto-detected)

This is more visible under proot (the GPU-process fork-server can't
launch, so gfxPlatform definitively disables HW accel) but the chroot
path benefits from the same prefs — `widget.dmabuf.force-enabled=true`
in particular keeps Firefox emitting `zwp_linux_dmabuf_v1` frames
instead of read-back-into-shm.

## What the fix looks like

Move the autoconfig + .cfg pair into the in-app install pipeline so
they ship with every install, not just integration-test rootfses.
Three places make sense:

1. **As an APK asset extracted in `Distro.configure` (or a successor
   `firefox.configure` step).** Cleanest, since it follows the
   pattern of `LibhybrisLinker` / `ChrootMounter.mountScript` writing
   things into the rootfs at install time. Requires the prefs to be
   carried alongside the APK (~1 KB).
2. **Lazily, the first time Firefox is launched.** Hook the
   `tawc-rootfs-run` path or a future per-app launcher so Firefox-
   targeted invocations idempotently materialise the autoconfig
   files before exec'ing. Avoids paying disk for users who never
   install Firefox.
3. **Distro-level `pacman.conf` post-transaction hook.** After
   `pacman -S firefox`, run a script that drops the prefs in. Most
   "right" answer architecturally — package install hook. Requires
   pacman alpm hook plumbing.

(1) is closest to today's chroot-side `LibhybrisLinker` pattern and
fits the in-app install flow without extra moving parts. (2) is the
cleanest for the universal-fix story (works for Chromium / other
sandboxed renderers too if we extend it) but is more code.

## Related to but distinct from
[firefox-sandbox-env-vars-in-universal-proot-profile.md](firefox-sandbox-env-vars-in-universal-proot-profile.md)

The MOZ_DISABLE_*_SANDBOX env vars are about *running* Firefox
subprocesses without crashing under proot's seccomp tracer. The
autoconfig prefs are about *rendering* Firefox output through the
hardware-buffer path. Both are Firefox-specific Firefox-fixes; both
are currently in the wrong layers (env vars in universal profile.d,
autoconfig in test-only setup script).

## References

- `scripts/install-test-deps.sh` (lines installing firefox.cfg /
  autoconfig.js — the canonical reference for what we should be
  doing in production too)
- `tests/fixtures/firefox.cfg`, `tests/fixtures/firefox-autoconfig.js`
- `notes/firefox.md` (background on Firefox-under-tawc rendering)
