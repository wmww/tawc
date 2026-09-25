# In-app launcher

Per-distro app picker that reads `.desktop` files inside a chroot rootfs
and lets the user search + launch. Reached from the home screen's
search stub (for the open distro, notes/android.md "Home screen") and
from pinned shortcuts.

## Pipeline

1. **MainActivity** search stub → `LauncherActivity` Intent with `EXTRA_ID =
   <installation id>`.
2. **LauncherActivity.loadApps()** → `LauncherEntry.scan(rootfs)` on
   `Dispatchers.IO` — the shared wrapper around
   `NativeBridge.nativeLauncherScan` + JSON parse that every scan
   consumer (launcher list, shortcut trampoline, `launcher-list`
   broker action) goes through. Native failure = empty list.
3. **launcher.rs** walks `APPS_SUBDIRS` under the rootfs —
   `root/.local/share/applications` (the guest's XDG per-user dir;
   fake root, so `$HOME` is `/root`), `usr/local/share/applications`,
   `usr/share/applications`, flatpak/snap exports — parses each
   `.desktop` via the `freedesktop-desktop-entry` crate, filters
   non-Application / NoDisplay / Hidden / Exec-less entries, and (in
   `scan_json`, not the entry walk — see "Icon resolution") resolves
   `Icon=` to an on-device PNG path. De-dup by id happens in walk
   order *before* the name sort, and `APPS_SUBDIRS` is ordered
   user-first, so a user's copy of an id shadows the packaged one
   ("hide the packaged entry behind my edited copy" works). Then
   sorted by localised name.
4. **LauncherEntry.parseList** turns the JSON into Kotlin records
   (`id, name, comment, exec, terminal, iconPath, path` — `path` is the
   absolute host path of the `.desktop` source file, kept so the UI can
   distinguish user-editable entries from distro-owned ones).
5. **LauncherActivity** filters hidden entries + the search query, then
   renders rows (icon ImageView + name + comment). `IconLoader`
   async-decodes PNGs with `BitmapFactory.inSampleSize` keeping memory
   bounded, and holds them in a byte-bounded `LruCache`. An entry with
   no resolvable icon gets `ic_terminal_fallback` or `ic_app_fallback`.
6. Tap or Enter → `EntryLauncher.launch(appContext, inst, entry)`, the
   shared dispatch point for every launch surface. `Terminal=true`
   entries on tawcroot installs open `TerminalActivity` as a command
   tab instead (see notes/terminal.md "Command sessions"); proot/chroot
   terminal entries fall through to the headless path with a logcat
   warn. Everything else runs
   `UserRootfsSession.runInside(rootfs, "<exec> </dev/null >/dev/null
   2>&1")` on its process-wide `LAUNCH_SCOPE` (Dispatchers.IO).
   `UserRootfsSession` holds a session reason while the process lives;
   the program's first Wayland/X11 connection starts the compositor. The Activity
   `finish()`es immediately; the coroutine keeps blocking in
   `runInside` for the program's lifetime, which pins one IO thread
   per running app. We can't `setsid -f` detach: proot's
   `--kill-on-exit` (kept on for pacman cleanup) SIGKILLs any
   backgrounded child when the launcher bash exits, so the app would
   die before it ever opened a Wayland window. Blocking for the
   program's lifetime is correct anyway — the program needs the JVM
   alive for the compositor's Wayland socket. Spawn failures surface
   via `LaunchErrorActivity` from the application context.

## Hide / unhide + per-entry menu

Long-press on a row opens an action-list dialog (plain
`AlertDialog.setItems`, no Menu resources) built from a per-entry
`List<EntryAction>` (label + enabled + handler) in
`LauncherActivity.entryActionsFor` — append there to grow the menu.
Today's items: **Hide** on visible entries, **Unhide** on hidden ones,
**Add to home screen** (see "Home-screen shortcuts"), **Edit** on
managed-dir entries (see "Managed dir + .desktop editor").

Hidden state lives in `Installation.hiddenDesktopIds` (ids =
`LauncherEntry.id`, filename minus `.desktop`), written only through
`InstallationStore.update` via `Installation.withEntryHidden`. The
field is additive with a safe default — no `schemaVersion` bump — and
serialized only when non-empty. Uninstall wipes `metadata.json`, so
hide state resets with the install; stale ids never match and are not
pruned.

Filtering is **Kotlin-side** (`LauncherEntry.filter`, a pure
unit-tested function driven from `LauncherActivity.applyFilter`), not
in `launcher.rs::scan_entries`:

- Hide state is per-install app metadata; the scanner takes only a
  rootfs path and shouldn't grow a metadata side-channel.
- `resolve_metadata_for_app_id` shares `scan_entries` for window
  icons/titles — a hidden app that is *running* must still resolve.

The search row is `←  [search field]  ⋮`, both buttons background-less
(`plainIconButton`) so they read as row chrome; the ← is the same mark as
a child screen's toolbar up arrow. The ← just `finish()`es:
system back is consumed by the soft keyboard first, so the popup window
needs its own dismiss.

The ⋮ button beside the search field opens a `PopupMenu`
with a checkable **"Show hidden (N)"** item (N counts hidden ids that
match actual entries) and — on editable methods — **"Add entry…"**
(the editor). Show-hidden is transient per-Activity state, not
persisted.
With it on, hidden entries render dimmed (alpha 0.5) in their normal
sort position and launch normally on tap. If every entry is hidden,
the empty-list message appends a "(N hidden)" hint.

Debug broker actions (notes/exec-broker.md): `launcher-list` returns
the post-filter list as JSON (optionally including hidden entries with
`showHidden=true`), including the resolved `iconPath` so icon tests can
see what the scanner picked; `set-entry-hidden` performs the same
metadata write as the UI. Integration coverage: `launcher::` tests in
`tests/integration/tests/launcher.rs`.

## Managed dir + .desktop editor

`/root/.local/share/applications/` is the **managed dir** — the
package-manager boundary. `DesktopFileEditorActivity` (launched for
result from the launcher's "Add entry…" overflow item and per-entry
"Edit" action) creates files only there, and only files there get the
Edit action; `/usr/local` stays read-only to the app (technically not
package-managed either, but `make install`-style entries there are
exactly the complex foreign files the editor shouldn't touch).

- Editable check is Kotlin-side: `DesktopEntryFile.isManaged` prefixes
  `entry.path` against the managed dir. Both sides are canonicalized —
  Kotlin's `context.dataDir` is `/data/user/0/<pkg>` while the Rust
  scanner canonicalizes its walk roots to `/data/data/<pkg>`, so a
  naive prefix check never matches.
- Method gate: writes are plain app-uid file I/O, fine for
  tawcroot/proot but not chroot's root-owned rootfs (see "Access
  model") — chroot installs get no New/Edit entry points, consistent
  with the terminal gating.
- Editor scope (`DesktopEntryFile`): Name + Exec (required), Icon
  (freeform `Icon=` value, resolved by `resolve_icon` on next scan),
  Terminal checkbox (checked by default for new entries — hand-made
  entries are usually CLI scripts). `Comment=` has no form field but is
  read and written back, so editing preserves an existing description.
  Saving
  writes the file wholesale (`Type=Application` + those keys); values
  lose embedded newlines, nothing else. Explicit non-goals: locale
  keys, actions, `%f` field codes, multiple groups — personal
  launchers, not production `.desktop` files.
- New file: `slugifyLabel`-style slug of Name + `.desktop`, `-2`/`-3`
  suffix on collision. Editing keeps the filename — it's the entry id,
  which pins and hidden-state reference. Delete is a toolbar trash
  action (confirmed), shown only when editing.
- Foreign files in the managed dir (unknown keys/groups): known keys
  load, and a notice warns that saving rewrites the file and drops the
  rest — a warning, not silent data loss.
- After save/delete the launcher rescans (`RESULT_OK` →
  `loadApps()`).

Serializer/parse/slug logic is JVM-unit-tested
(`DesktopEntryFileTest`); scan-dir + precedence + terminal-flag
behavior is integration-tested through `launcher-list`
(`tests/integration/tests/launcher.rs`). Editor flows verified
on-device 2026-07-04.

## Home-screen shortcuts (pinned)

Per-entry action **"Add to home screen"** pins the entry as an Android
pinned shortcut (`ShortcutManagerCompat.requestPinShortcut`, system
sheet handles placement; unsupported launchers get a toast). Code:
`EntryShortcuts` (build/pin + icon) and `ShortcutLaunchActivity` (tap
trampoline).

- **Payload is a reference, not a command**: the shortcut intent
  carries `(installId, desktopId, label)` — never the `Exec` string.
  The trampoline re-resolves the entry with a fresh
  `nativeLauncherScan` at tap time (same walk the launcher does on
  open), so pins stay current across `.desktop` edits and the system's
  shortcut store never holds an executable command.
- Shortcut id is `"<installId>/<desktopId>"`; install ids can't
  contain `/`, so `EntryShortcuts.splitShortcutId` is unambiguous.
  The id format and the `"installId"`/`"desktopId"`/`"label"` extras
  keys live in the system launcher's pin store across app updates —
  frozen wire format; see *Frozen identifiers* in
  notes/installation.md.
  Re-pinning an already-pinned id calls `updateShortcuts` (refreshes
  label/icon in place) + a toast instead of `requestPinShortcut` —
  the Pixel launcher does *not* dedupe a re-request; it happily adds
  a second workspace icon for the same id (verified on emulator).
- **Trampoline** (`ShortcutLaunchActivity`, non-exported —
  pinned-shortcut intents may target non-exported components of the
  publishing app; translucent DialogHost theme, `noHistory`,
  `excludeFromRecents`, `taskAffinity=""` so a tap doesn't yank the
  main TAWC task forward): gate install exists + state READY → scan →
  find by id → `EntryLauncher.launch`. Any gate failure shows
  `LaunchErrorActivity` instead of crashing, which is the whole
  stale-pin story: uninstalling a distro leaves pins behind, and a
  stale tap gets a clear error. (Optional follow-up if that annoys:
  uninstall could `disableShortcuts` ids prefixed `"<installId>/"`.)
- A hidden entry still launches from its pin — hiding declutters the
  list; an existing pin is explicit user intent. Terminal entries get
  no special casing: dispatch goes through `EntryLauncher`, same as
  the in-app list.
- **Icon**: entry PNG decoded via `IconLoader.decode`, centered on a
  neutral square at 2/3 edge (adaptive-icon safe zone) and wrapped
  with `IconCompat.createWithAdaptiveBitmap` so it masks correctly on
  every launcher shape; no/undecodable icon falls back to the same
  glyph the list row uses (`ic_terminal_fallback` for `Terminal=true`,
  `ic_app_fallback` otherwise) on a black backdrop — not the TAWC app
  icon, which would make a pinned icon-less app look like TAWC itself. Geometry (`pinIconFit`) + id mapping are JVM-unit-tested
  (`EntryShortcutsTest`); pinning itself is a launcher-UI interaction,
  so end-to-end coverage is manual.

## Icon resolution

`iconPath` is always a decodable PNG or empty — that contract is what
keeps all three Kotlin decoders (`IconLoader.decode`,
`EntryShortcuts.pinBitmap`, `CompositorActivity.decodeTaskIcon`)
single-format. SVG sources are rasterized on the Rust side rather than
handed to Kotlin.

Resolution is **lazy**. `scan_entries` keeps the raw `Icon=` value;
`scan_json` resolves every entry (it runs on `Dispatchers.IO`) and
`resolve_metadata_for_app_id` matches by id *first* and resolves only
the winning entry — that one runs on the compositor thread on first
window map, so it must not walk every icon in every rootfs.

`IconResolver` (built once per scan, holds the theme order and the
cache) searches, all rooted at the canonicalized rootfs:

1. Absolute `Icon=/foo/bar.png` → used directly. The value is
   guest-controlled and we now *parse* what we find, so the path is
   lexically normalized and rejected if `..` climbs out of the rootfs.
2. Bare name → the theme walk under `usr/share/icons`, below.
3. `usr/share/pixmaps/<name>.{png,svg,svgz}` (legacy fallback).
4. `Icon=name.<ext>` strips known image extensions before the search.

The theme walk is spec-*shaped*, not the full fdo size-matching
algorithm — we want "largest sensible raster, else scalable":

- **Theme order**: seeds `default`, `Adwaita`, `Papirus`, `breeze`,
  `hicolor`, each expanded breadth-first through its `index.theme`
  `Inherits=` (minimal line scan, stops at the second group header; no
  theme crate). De-duplicated, missing themes dropped, `hicolor` forced
  last so an inherited parent still gets a look in. `default` leads so a
  distro/user-selected theme wins.
- **Per theme**: contexts `apps`, then `legacy`, then `categories` —
  generic names like `utilities-terminal` live outside `apps` in several
  themes. Sizes `128, 96, 256, 64, 48`, then `scalable`, then
  `32, 24, 22, 16` (a vector icon beats a 16 px PNG blown up to a 56 dp
  row). Both layouts are tried: `<size>/<context>` (hicolor, Adwaita)
  and `<context>/<size>` (breeze), with numeric sizes spelled both
  `48x48` and bare `48`.
- Extensions per directory: `png`, then `svg`, then `svgz`. XPM is not
  searched — we can't decode it and only a couple of `NoDisplay` python
  entries still ship one.
- Each theme's immediate subdirectory names are read once
  (`read_subdir_names`), so the walk skips whole size tiers instead of
  stat'ing the full contexts × sizes × layouts × extensions grid.
  Adwaita on sid ships three size dirs, not ten.

### SVG cache

`icon_cache.rs` rasterizes SVG/SVGZ sources with `resvg` into
`<distros>/<id>/icon-cache/` — a sibling of the rootfs, so it is
app-owned (uninstall removes it, keys can't collide across installs) and
reachable without `app_paths`, which `nativeLauncherScan` may run
before.

- Key: hash of (rootfs-relative source path, mtime, len, render size,
  format version) → `<hex>.png`. A package upgrade changes mtime/len and
  lands on a new file.
- Rendered 192 px square, aspect-preserved, centred, transparent. That
  covers the ~56 dp row at 3×, the recents icon and the 2/3-safe-zone
  pin bitmap.
- Written to `<hex>.<pid>.tmp` then renamed — the launcher and the
  shortcut trampoline can scan concurrently.
- Guard rails, since the input is guest-controlled: sources over 1 MiB
  are skipped, parse+render runs under `catch_unwind`, and any failure
  leaves a zero-length `<hex>.fail` marker so a bad SVG isn't re-parsed
  on every scan. One `warn!` per scan with the failure count; never per
  icon.
- `scan_json` prunes cache files this scan didn't reference, skipping a
  scan that returned nothing (a transiently unreadable rootfs must not
  wipe the cache). `.tmp` files are only swept once older than a minute,
  so a concurrent scan's in-flight write survives.

Measured on the emulator sid install (13 entries, 6 SVG icons): cold
`launcher-list` 0.12 s, warm 0.07 s, both including the adb + broker
round trip. Roughly 8 ms per icon rendered, so even a full desktop
install stays well inside a second; parallelising the rasterize
(`std::thread::scope` over the SVG entries) is the lever if that ever
stops being true.

Entries that still resolve to nothing render the fallback glyph
(`ic_terminal_fallback` for `Terminal=true`, `ic_app_fallback` — a
neutral window mark — otherwise), not the TAWC logo. Recents keeps
`null` → the TAWC app icon: a TAWC-branded recents card is accurate,
not misleading.

### Symbolic last resort

After every theme, context and size has come up empty, the walk tries
`<theme>/symbolic/{apps,legacy,categories}/<name>-symbolic.svg`. It is
last because it loses the app's colours: Konsole on sid asks for
`utilities-terminal` and the rootfs ships only
`Adwaita/symbolic/legacy/utilities-terminal-symbolic.svg`.

Symbolic SVGs are a single near-black colour, so they vanish on a dark
background, and a cached PNG can't follow the app theme. They are baked
light-on-dark instead: the SVG is rendered at 60 % scale, its **alpha is
kept as a mask** and repainted white over a black rounded tile with the
same proportions as `ic_terminal_fallback`. Masking rather than
string-replacing the fill is deliberate — a symbolic icon's colour can
come from `fill`, `style`, a class or a `use` reference, so rewriting the
source is fragile. The cache key carries a `symbolic` bit.

Installing `breeze-icon-theme` still gives Konsole a real colour icon;
this is the answer for the case where nothing is installed.

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
