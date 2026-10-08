# Editable package entries + per-entry graphics

Not started.

## Goal

- Long-press → **Edit** works on every launcher entry, not just
  personal ones in the managed dir.
- Editing a package-installed entry never touches the packaged file.
  Saving writes an **override copy** into the managed dir, and only one
  entry shows in the grid.
- When an entry has an override, the editor's toolbar shows **Reset**
  instead of Delete, and the confirm prompt says it restores the
  default.
- The editor gets a **Graphics** picker: `Default (<global pick>)`, then
  every backend this build ships (libhybris, libhybris+zink, gfxstream,
  CPU…). It uses the same list as Settings.

Read notes/launcher.md first, especially "Managed dir + .desktop
editor" and "Home-screen shortcuts". This plan builds on that section.

## What already exists (verified 2026-10-07)

- **Shadowing.** `launcher.rs::scan_entries` de-dups by id in
  `APPS_SUBDIRS` order, and that order is user-first. A
  `root/.local/share/applications/<id>.desktop` copy already hides the
  packaged entry with the same id. No scanner change is needed for "no
  duplicates".
- **Id-keyed state.** Hidden state (`Installation.hiddenDesktopIds`)
  and pinned shortcuts (`<installId>/<desktopId>`) key on the id. An
  override therefore inherits the entry's hide state and its pins.
- **Per-spawn backend.** `InstallationMethod.startInside`,
  `UserRootfsSession.runInside` and `TawcrootMethod`'s pty path already
  take an optional `graphics: GraphicsBackend?`. `null` means the
  global `Settings.graphicsBackend`.
- **Backends don't depend on the global pick.** Every enabled backend's
  files are installed into each rootfs no matter which one is selected.
  `MesaZinkInstallProvider` and `BridgeInstallProvider` gate only on
  `EnabledGraphicsBackends`, and the kumquat thread always starts when
  gfxstream is built. A per-app override therefore needs only the
  per-spawn env.

## Design

### 1. Scanner reports what an entry shadows

When the de-dup in `scan_entries` drops a duplicate, record the dropped
copy's path on the entry that won. Expose it as `shadows` in
`scan_json`: the packaged path, or an empty string. Add
`LauncherEntry.shadows` in Kotlin.

This is the only signal the UI needs:

- An entry is an **override** when it is managed and `shadows` is
  non-empty. It gets Reset.
- A **personal entry** is managed with an empty `shadows`. It gets
  Delete, as today.
- A **package entry** is not managed. Editing it creates an override.
- **Orphan overrides** need no special handling. If the package is
  removed, `shadows` goes empty and the entry turns into a personal
  entry with Delete. That is correct, since there is no default left to
  reset to.

No marker key goes into the file. Whether a file is an override is
derived from the filesystem every scan, so it can't go stale.

### 2. Edit on every entry

- `AppsPane.entryActionsFor`: drop the `isManaged` condition and keep
  `canEditEntries()`. The chroot gate stays as it is (see notes/launcher.md
  "Access model").
- `DesktopFileEditorActivity`: `EXTRA_PATH` may now be any scanned
  entry file.
  - Validate it: canonicalize, check it is under this rootfs, and check
    it is a regular file.
  - **Source** is the file to load.
  - **Target** is `managedDir/<source basename>`. When the source is
    already managed, source and target are the same file.

### 3. Patch instead of wholesale rewrite

Rebuilding a packaged entry from the five form fields would silently
drop `Name[de]`, `MimeType`, `StartupWMClass`, `Actions` and other keys.
Instead, `DesktopEntryFile` gains `patch(sourceText, draft): String`:

- Inside `[Desktop Entry]`, replace the values of `Name`, `Exec`,
  `Icon` and `Terminal` in place. Append any of these keys that are
  missing. Remove `Terminal` when it is false and was absent.
- When `Name` or `Comment` changes, also remove its locale variants
  (`Name[xx]=`). Otherwise the localized value would keep winning on
  device and the edit would look ignored.
- Keep every other line, group and comment byte for byte.

Use `patch` for every edit of an existing file, managed ones included.
`serialize` stays only for brand-new entries. This lets the
foreign-content warning (`Parsed.hasForeignContent`, the
`editor_foreign_warning` string and the non-UTF-8 check that feeds it)
be **deleted**: nothing is dropped any more. A non-UTF-8 source still
needs a guard. Refuse to edit it with a toast rather than patching
mojibake.

Save rules for a package source (an override doesn't exist yet):

- If the patched text equals the source text, write nothing. A save
  that only changes graphics doesn't create a copy (see section 5).
- Otherwise, atomically write the target.

When an override is re-saved to exactly match its `shadows` file,
delete the override. The scan then shows the packaged entry again.

### 4. Reset vs Delete

- The toolbar action is chosen from what the launcher passed in. Add
  `EXTRA_SHADOWS` to the intent, or let the editor compare the source
  against `shadows`.
  - **Override:** show a reset icon. Add a new `ic_reset` vector based
    on Material "settings_backup_restore", tinted the same way as
    `ic_delete`. The confirm prompt reads: *"Reset 'Firefox' to
    default? Your edits are discarded."* Confirming deletes the
    managed copy and clears the graphics override.
  - **Package entry with only a graphics override:** also show Reset.
    It clears the override.
  - **Personal entry:** keep the trash icon and the delete prompt.
    Deleting also clears any graphics override for that id, so a
    future entry that reuses the slug doesn't inherit it.
  - **Package entry with nothing overridden:** show no toolbar action.
- New strings: `editor_reset`, `editor_reset_confirm`.

### 5. Graphics override

**Storage:** per-install metadata, not the `.desktop` file. Add
`Installation.entryGraphics: Map<String, String>` (desktop id →
`GraphicsBackend.key`), modeled on `hiddenDesktopIds`:

- The field is additive and serialized only when non-empty.
- There is no `schemaVersion` bump.
- It is written only through `InstallationStore.update` via a
  `withEntryGraphics(id, backend?)` helper.

Why metadata rather than an `X-TAWC-Graphics=` key in the file: a
graphics-only change shouldn't fork a packaged entry. A fork would
freeze that entry against package upgrades (see Trade-offs). Also, the
key would be meaningless to anything else in the rootfs that reads the
file.

**UI:** a "Graphics" radio group in the editor, under Terminal.

- The first item is `Default (<Settings.graphicsBackend.displayName>)`.
- Then one item per `EnabledGraphicsBackends.enabled`.
- Factor the radio-group builder out of
  `SettingsActivity.buildGraphicsBackendGroup` so both screens share it.
  The editor version takes the extra "Default (…)" row.
- A stored key that this build doesn't ship shows as Default.
- New entries default to Default.

**Launch:**

- `EntryLauncher.launch` resolves the backend with
  `inst.entryGraphics[entry.id]`, then a new nullable
  `GraphicsBackend.fromKeyOrNull`. That function returns null for
  unknown or disabled keys; don't use `fromKeyOrDefault`, which would
  turn "no override" into a hard pin.
- GUI entries pass the backend to `runInside(…, graphics)`.
- `Terminal=true` entries: add an `EXTRA_GRAPHICS` to
  `MainActivity.commandIntent`. Thread it through the command-tab spawn
  into `TawcrootMethod`'s pty exec, which already takes `graphics`.
- Pinned shortcuts go through `EntryLauncher` with a freshly loaded
  `Installation`, so they get the override without extra work.

**Scope:** only TAWC launches (the grid and pins) honour the override.
Running the same program by hand in a terminal uses the global backend.
Say so in the notes.

### 6. Debug surface + tests

- `launcher-list`: include `shadows` and `graphics` per entry.
- Broker: add `set-entry-graphics`, mirroring `set-entry-hidden`.
- JVM unit tests (`DesktopEntryFileTest`):
  - `patch` keeps foreign keys, groups and comments;
  - locale variants are dropped only for the key that changed;
  - `Terminal` is added and removed correctly;
  - an unchanged draft gives identical text.
- `Installation` tests: `entryGraphics` round-trips, and the empty map
  is omitted.
- Integration (`tests/integration/tests/launcher.rs`):
  - an override in the managed dir yields one entry with `shadows` set;
  - deleting it brings back the packaged entry with empty `shadows`;
  - `set-entry-graphics` shows up in `launcher-list`.
  - If a broker action can launch an entry, also assert the spawned
    env. Otherwise unit-test `EntryLauncher`'s backend resolution.
- Device check on `.tawctarget`, end to end:
  1. Edit Firefox's name.
  2. Confirm one tile.
  3. Reset.
  4. Confirm the original is back.
  5. Set CPU graphics and check that the window shows the magenta SHM
     tint.

### 7. Notes

Rewrite notes/launcher.md "Managed dir + .desktop editor" for the
override model, Reset, patch-on-save and the graphics override. Remove
the foreign-warning prose. Add a line to notes/gpu-strategy.md pointing
at the per-entry override.

## Trade-offs / open questions

- **Override snapshot goes stale.** A package upgrade that changes the
  packaged file (new `Exec` flags, renamed icon) doesn't reach an
  override. This is accepted: it is the standard XDG override model,
  and Reset is one tap away. Don't add a "the packaged entry changed"
  hint.
- **Nested app dirs.** Check how the `freedesktop-desktop-entry` crate
  derives `appid` for `usr/share/applications/<sub>/foo.desktop`. The
  override filename must give the same id, or the shadowing breaks.
- **apps-tab-home plan.** Its built-in entries (TAWC Term, Update
  packages, Add entry) are not `.desktop` files and stay non-editable.
  Whichever plan lands second keeps them out of the Edit gate.
