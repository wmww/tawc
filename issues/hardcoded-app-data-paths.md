# Rootfs launch paths hardcode `/data/data/me.phie.tawc`

Some rootfs/chroot/proot/tawcroot paths still hardcode the canonical app data
directory:

- `ChrootMounter.kt`
- `ProotMethod.kt`
- `TawcrootMethod.kt`

The hardcoded form is usually correct for the primary user and is also the
canonical path that mount/procfs tools tend to report. But Android supports
multi-user/profile paths where `context.dataDir` / `applicationInfo.dataDir`
may differ, and lint correctly flags this as brittle.

This needs care because these paths cross the Android app, root shell,
rootfs bind mounts, and guest-visible environment. A mechanical replacement
could break unmount matching or rootfs paths that currently rely on the
canonical `/data/data/<pkg>` form.

Better fix:

- Introduce one app-data path helper that exposes both runtime dataDir and,
  where needed, the canonical `/data/data/<pkg>` alias.
- Use the runtime path for filesystem operations launched by the app uid.
- Keep or derive the canonical alias only where root/mount/procfs behavior
  requires it, with comments at those call sites.
- Test install, launch, process scanning, and uninstall on physical and
  emulator targets.

