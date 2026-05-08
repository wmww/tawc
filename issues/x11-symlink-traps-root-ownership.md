# `/tmp/.X11-unix` symlink can become unrecoverably root-owned under tawcroot

If `enter.sh` is ever invoked through `su -c` against a tawcroot install
(e.g. `adb shell su -c '/data/.../enter.sh ...'` for ad-hoc debugging),
the `etc/profile.d/01-tawc.sh` body runs as root, so the
`ln -sfn /data/data/me.phie.tawc/xtmp/.X11-unix /tmp/.X11-unix` lands
with uid=0 and the SELinux `app_data_file:s0` context **without** the
per-app categories.

Subsequent app-uid runs (the normal `run-as me.phie.tawc` path) then
get EACCES on `readlinkat("/tmp/.X11-unix")` because:

- `/tmp` is `1777` (sticky), so the app uid can't `unlink` a
  root-owned symlink to replace it via `ln -sfn`.
- The categoryless SELinux context blocks the app uid from reading the
  symlink even when DAC would allow it.

Result: every X11 client inside the chroot tries TCP `:0` instead of
the unix socket and dies with `xcb_connect failed (DISPLAY=:0)`. All
X11 integration tests turn red until the symlink is manually removed.

## Repro

```bash
# Healthy state.
adb shell su -c 'rm -f /data/data/me.phie.tawc/distros/void/rootfs/tmp/.X11-unix'
TAWC_TARGET=physical bash scripts/run-integration-tests.sh --no-build apps::test_eglx11
# -> ok

# Poison the state by exercising enter.sh as root.
adb shell "su -c '/data/data/me.phie.tawc/distros/void/enter.sh \$(echo -n true | base64 -w0)'"

# All X11 tests now fail.
TAWC_TARGET=physical bash scripts/run-integration-tests.sh --no-build apps::test_eglx11
# -> xcb_connect failed (DISPLAY=:0)

# Recovery.
adb shell su -c 'rm -f /data/data/me.phie.tawc/distros/void/rootfs/tmp/.X11-unix'
```

Workaround for now: never invoke `enter.sh` directly via `su -c`
against a tawcroot install — always go through
`scripts/tawc-rootfs-run.sh`, which dispatches by method and uses
`run-as` for tawcroot.

## Fix directions

- **`RootfsProfile.kt` profile body**: detect a root-owned `/tmp/.X11-unix`
  and decline to create the symlink (or fail loud) rather than silently
  inheriting the bad state. Probably wants `[ "$(stat -c %u /tmp/.X11-unix)" = "$(id -u)" ] || rm -f /tmp/.X11-unix` before the `ln -sfn`.
- **Or** change the symlink target site from `/tmp/.X11-unix` to a
  directory we own (`/data/data/me.phie.tawc/xtmp/.X11-unix` is already
  the canonical path) and rely on `XAUTHORITY`-style env var rather
  than the symlink. More invasive.
