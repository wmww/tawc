# In-app launcher

Per-distro app picker that reads `.desktop` files inside a chroot rootfs
and lets the user search + launch. Reached from the home screen card's
**Run** button.

## Pipeline

1. **MainActivity** card → `LauncherActivity` Intent with `EXTRA_ID =
   <installation id>`.
2. **LauncherActivity.loadApps()** → `NativeBridge.nativeLauncherScan(rootfs)`
   on `Dispatchers.IO`. Returns a JSON string.
3. **launcher.rs** walks `APPS_SUBDIRS` under the rootfs —
   `root/.local/share/applications` (the guest's XDG per-user dir;
   fake root, so `$HOME` is `/root`), `usr/local/share/applications`,
   `usr/share/applications`, flatpak/snap exports — parses each
   `.desktop` via the `freedesktop-desktop-entry` crate, filters
   non-Application / NoDisplay / Hidden / Exec-less entries, resolves
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
   bounded.
6. Tap or Enter → `EntryLauncher.launch(appContext, inst, entry)`, the
   shared dispatch point for every launch surface. `Terminal=true`
   entries on tawcroot installs open `TerminalActivity` as a command
   tab instead (see notes/terminal.md "Command sessions"); proot/chroot
   terminal entries fall through to the headless path with a logcat
   warn. Everything else runs
   `UserRootfsSession.runInside(rootfs, "<exec> </dev/null >/dev/null
   2>&1")` on its process-wide `LAUNCH_SCOPE` (Dispatchers.IO).
   `UserRootfsSession` starts `CompositorService` lazily and waits for
   the Wayland socket before spawning the Linux process. The Activity
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

Filtering is **Kotlin-side** (`LauncherActivity.applyFilter`), not in
`launcher.rs::scan_entries`:

- Hide state is per-install app metadata; the scanner takes only a
  rootfs path and shouldn't grow a metadata side-channel.
- `resolve_metadata_for_app_id` shares `scan_entries` for window
  icons/titles — a hidden app that is *running* must still resolve.

The ⋮ tonal icon button beside the search field opens a `PopupMenu`
with a checkable **"Show hidden (N)"** item (N counts hidden ids that
match actual entries) and — on editable methods — **"Add entry…"**
(the editor). Show-hidden is transient per-Activity state, not
persisted.
With it on, hidden entries render dimmed (alpha 0.5) in their normal
sort position and launch normally on tap. If every entry is hidden,
the empty-list message appends a "(N hidden)" hint.

Debug broker actions (notes/exec-broker.md): `launcher-list` returns
the post-filter list as JSON (optionally including hidden entries with
`showHidden=true`); `set-entry-hidden` performs the same metadata
write as the UI. Integration coverage: `launcher::` tests in
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
  Re-pinning an already-pinned id calls `updateShortcuts` (refreshes
  label/icon in place) + a toast instead of `requestPinShortcut` —
  the Pixel launcher does *not* dedupe a re-request; it happily adds
  a second workspace icon for the same id (verified on emulator).
- **Trampoline** (`ShortcutLaunchActivity`, non-exported —
  pinned-shortcut intents may target non-exported components of the
  publishing app; translucent DialogHost theme, `noHistory`,
  `excludeFromRecents`, `taskAffinity=""` so a tap doesn't yank the
  main tawc task forward): gate install exists + state READY → scan →
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
  every launcher shape; no/undecodable icon falls back to the tawc app
  icon. Geometry (`pinIconFit`) + id mapping are JVM-unit-tested
  (`EntryShortcutsTest`); pinning itself is a launcher-UI interaction,
  so end-to-end coverage is manual.

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
