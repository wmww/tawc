# In-app launcher

Per-distro app picker that reads `.desktop` files inside a chroot rootfs
and lets the user search + launch. Reached from the home screen card's
**Run** button.

## Pipeline

1. **MainActivity** card → `LauncherActivity` Intent with `EXTRA_ID =
   <installation id>`.
2. **LauncherActivity.loadApps()** → `NativeBridge.nativeLauncherScan(rootfs)`
   on `Dispatchers.IO`. Returns a JSON string.
3. **launcher.rs** walks `usr/share/applications` +
   `usr/local/share/applications` + flatpak/snap exports under the
   rootfs, parses each `.desktop` via the `freedesktop-desktop-entry`
   crate, filters non-Application / NoDisplay / Hidden / Exec-less
   entries, resolves `Icon=` to an on-device PNG path, sorts by
   localised name, de-dups by id.
4. **LauncherEntry.parseList** turns the JSON into Kotlin records.
5. **LauncherActivity** renders rows (icon ImageView + name + comment).
   `IconLoader` async-decodes PNGs with `BitmapFactory.inSampleSize`
   keeping memory bounded.
6. Tap or Enter → `InstallationMethod.runInside(rootfs, "setsid -f sh -c
   '<exec>' </dev/null >/dev/null 2>&1")` on the process-wide
   `LAUNCH_SCOPE`. Activity `finish()`es immediately; the spawned program
   keeps running because the scope outlives the Activity.

## Icon resolution

Search order in `launcher.rs::resolve_icon`, all rooted at the rootfs:

1. Absolute path `Icon=/foo/bar.png` → use directly if PNG.
2. Bare name `Icon=firefox` → search
   `usr/share/icons/<theme>/<size>/apps/<name>.png` for
   themes = `Adwaita`, `Papirus`, `breeze`, `hicolor`,
   sizes = `128`, `96`, `256`, `64`, `48` (mid-size first because list
   rows render at ~56 dp).
3. `usr/share/pixmaps/<name>.png` (legacy fallback).
4. `Icon=name.<ext>` strips known image extensions before the search.

PNG-only by design: `BitmapFactory` doesn't decode SVG/XPM, and shipping
a path Kotlin can't open just produces broken rows. SVG-only icons (some
modern GNOME apps) end up empty; the row renders without an icon. SVG
support would mean adding either a Rust SVG renderer (`resvg`, heavy) or
the `AndroidSVG` jar — defer until users ask.

`index.theme` `Inherits=` chains aren't parsed: hicolor catches almost
everything in practice and the spec walker would 5× the search cost.
Revisit if real-app testing turns up missing icons that hicolor doesn't
cover.

## Access model

The rootfs lives at `/data/data/me.phie.tawc/distros/<id>/rootfs/`,
owned by the app uid for `proot` and `tawcroot` installs — Kotlin can
`BitmapFactory.decodeFile` directly.

For `chroot` installs the rootfs is uid-0-owned (see
`InstallationStore.computeSizeBytes` for the `su` retry pattern). Icon
paths returned by `launcher.rs` would need a privileged read step
that's not wired up today; the Rust scanner itself runs as the app uid
through `nativeLauncherScan` and may even fail to enumerate `.desktop`
files on a chroot rootfs. Testing hasn't surfaced this because nobody's
been running chroot installs lately. TODO: gate the home-screen Run
button on `inst.method != chroot` until we add a privileged-read path,
or copy icons into an app-uid-readable cache at install time.

## Future UX

- Pinning / favourites at the top.
- Frecency ranking (track per-app launch counts in a small SQLite).
- Window-list integration: show running Wayland windows alongside apps
  to switch.
- Recently-launched section.

None of these block today's "type-and-go" flow; revisit after dogfooding.
