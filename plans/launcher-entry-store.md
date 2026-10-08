# Launcher entry store

Not started. Replaces the `.desktop` writing described in
notes/launcher.md ("Managed dir + .desktop editor") and the
per-entry fields in `metadata.json`.

## Goal

Everything TAWC writes about launcher entries goes in one TAWC-owned
store outside the rootfs:

- **Field overrides** on any scanned entry (packaged, `/usr/local`, or
  a `.desktop` file the user wrote by hand). Each field is overridden on
  its own; the rest still comes from the live file, so package upgrades
  still reach it.
- **TAWC shortcuts**: entries made in the app. They have no `.desktop`
  file.
- Hide, graphics and pointer settings, which move out of
  `metadata.json`.

TAWC no longer writes `.desktop` files or icon files into the rootfs. A
`.desktop` file the user makes by hand in any XDG dir is scanned like
any other file: the app can override fields on it but never edits it.
TAWC shortcuts and overrides only affect TAWC (its launcher, pins and
launches). A desktop environment running inside the guest won't see
them. That is accepted.

Overrides and shortcuts travel with export/import. Android home-screen
pins still don't, because Android's launcher owns them.

## Store

`<distros>/<id>/launcher/`, a sibling of `rootfs/` like `icon-cache/`.
It is app-owned, uninstall removes it, and it works for chroot installs
too.

```
launcher/
  entries.json
  icons/<name>.png
```

`entries.json`:

```json
{
  "version": 1,
  "migrated": 1,
  "overrides": {
    "firefox": { "name": "Firefox (CPU)", "graphics": "cpu", "hidden": true,
                 "env": { "MOZ_ENABLE_WAYLAND": "1" } }
  },
  "shortcuts": {
    "tawc:app:htop": { "name": "htop", "exec": "htop", "terminal": true,
                       "iconFile": "htop.png" }
  }
}
```

- **Fields** are the same for both maps, and all are optional:
  `name`, `comment`, `exec`, `terminal`, `icon` (theme name or in-rootfs
  path), `iconFile` (under `launcher/icons/`), `env` (a map applied at
  launch, not spliced into `exec`), `graphics`, `pointer`, `hidden`.
  Adding a field later is additive. An unknown field is kept on
  rewrite, so an older app doesn't drop a newer one's data. `version`
  newer than supported is refused, like `schemaVersion`.
- **Override semantics**: a field present replaces the scanned value.
  `env` is added on top of any `env` prefix in the entry's `Exec`. An
  override whose entry no longer exists is kept. It does nothing, and
  it comes back if the package is reinstalled, the way hide state works
  today.
- **Shortcut ids**: a new shortcut gets `tawc:app:<slug>` (`-2`, `-3`
  on collision). Scanned ids with the `tawc:` prefix are already
  dropped, so a new id can't collide with a scanned one. Migrated
  entries keep their old id (see Migration). If a shortcut and a
  scanned entry share an id, the shortcut wins, as the user dir did
  before.
- **Built-ins** (`tawc:term`, …) take `hidden` overrides only.
- **Writes**: `LauncherStore` (Kotlin) owns all writes. It does an
  atomic temp-file rename and read-modify-write under a per-install lock
  (the `InstallationStore.update` pattern), and each change is one
  helper such as `withOverride(id, field, value)`. The scan only reads
  the store.
- **Icon files**: a raster import is scaled to fit 256² and saved as
  PNG, as `IconImport` does today. An SVG is rasterized once at import
  by a new native call that reuses `icon_cache`'s resvg path. Storing
  only PNG keeps the "`iconPath` is a decodable PNG" contract with no
  cache. Unlike the rootfs imports, TAWC owns these files, so a save
  deletes any file no entry references any more.

## Merge (Rust)

`launcher.rs` reads `../launcher/entries.json` beside the rootfs
(`icon-cache/` is already found this way) and merges it into the scan:

1. Scan `.desktop` files as now, with de-dup and `shadows`.
2. Apply field overrides by id. Then add the shortcuts, which replace
   any scanned entry with the same id.
3. Resolve icons. `iconFile` resolves to `launcher/icons/<f>` after a
   check that the path stays inside that dir. `icon` goes through
   `IconResolver` as now.
4. The JSON gains `source` (`desktop` | `shortcut`), `overridden` (a
   list of field names), the packaged value of each overridden field
   (for the editor's hints and per-field reset), and `hidden`, `env`,
   `graphics` and `pointer`.

Hiding is still filtered Kotlin-side (`LauncherEntry.filter`), so a
running hidden app still resolves. `resolve_metadata_for_app_id` uses
the same merge, so recents show the overridden name and icon.
Shortcuts there match only through the launch splash, since they have
no `StartupWMClass`.

Doing the merge in Rust lets every consumer get merged entries from one
call: the launcher list, the pin trampoline, the `launcher-list` broker
action and window metadata.

## Editor

`DesktopFileEditorActivity` becomes an entry editor that writes only to
the store.

- **Scanned entry**: each field shows its effective value. A changed
  field becomes an override, with a small reset control beside it.
  Clearing it falls back to the packaged value, shown as the hint.
  The toolbar action **Reset** drops every override for the id; hide
  state is kept.
- **Shortcut**: the same form, with **Delete**. "Add entry…" makes a
  shortcut. Terminal stays checked by default.
- Env rows edit `env` directly. The `splitExec`/`joinExec` round-trip
  goes away, except in migration.
- No `.desktop` method gate: the store is app-owned, so chroot installs
  get the editor as well. Launching from them still has the chroot
  issues described under "Access model".

Code to delete: `DesktopEntryFile.patch`/`serialize`, the managed-dir
and `isManaged` logic, the override fork/unfork save rules, and
`IconImport`'s rootfs writes.

## Launch

`EntryLauncher` reads `env`, `graphics` and `pointer` from the merged
entry instead of from `Installation`. `env` is exported in the command
prefix before `exec`, so both GUI and terminal launches get it.
`graphicsFor` and `pointerEmulationFor` take the entry instead of
`inst`.

## Migration

Migration is one-time and idempotent per install. It runs from
`LauncherStore` the first time an install's store is loaded, so it
also covers a v3 export imported later. When it finishes it sets
`"migrated": 1`.

1. **metadata.json fields**: `hiddenDesktopIds`, `entryGraphics` and
   `entryPointerEmulation` move into `overrides`. The store is written
   first, then the fields are removed from the record and from
   `Installation`. (`rewrite` already keeps them, so an imported
   format-1 export takes the same path.)
2. **Managed dir** (`/root/.local/share/applications/*.desktop`): a
   file is treated as TAWC-written only if it parses as UTF-8 and
   `[Desktop Entry]` holds nothing beyond what the editor wrote
   (`Type`, `Version`, `Name`, `Exec`, `Icon`, `Terminal`, `Comment`).
   That leaves out locale keys, extra groups and `Actions`.
   - With `shadows` (an override, which only ever existed in dev
     builds): store the fields that differ from the shadowed file as
     overrides.
   - Without `shadows` (a v3 personal entry): it becomes a shortcut
     under its old id (the filename slug), so existing home-screen
     pins keep working. That id format is frozen; see notes/installation.md
     "Frozen identifiers".
   - An `env` prefix in `Exec` is split into `env`.
   - `Icon=tawc-*` names found in `…/icons/hicolor/{256x256,scalable}/apps/`
     are copied into `launcher/icons/` (SVGs rasterized) and become
     `iconFile`. The rootfs copies stay in place: they are harmless,
     and a hand-made entry could reference them.
   - The `.desktop` file is deleted only after the store write
     succeeds.
   Files that don't match are left alone and keep working as hand-made
   entries.

## Export / import

- `ExportRule` adds `launcher/` (both `entries.json` and `icons/`), and
  `importRejection` allows canonical `launcher/…` names.
- `DistroArchive.FORMAT` goes to 2. An older app's importer would
  reject the unknown top-level dir partway through, so the version
  check makes it fail cleanly up front instead. Format-1 archives still
  import, and step 1 of the migration covers them.
- On import the store needs no rewrite, because it holds no
  install-specific paths. Make sure that stays true: `iconFile` is
  relative and `icon` paths are guest paths.
- Custom (loose tarball) imports start with an empty store.

## Debug broker

- `launcher-list`: returns merged entries with the new fields.
- `set-entry-hidden`, `set-entry-graphics` and `set-entry-pointer` are
  replaced by `set-entry-override {id, field, value|null}` and
  `put-shortcut` / `delete-shortcut`. They are debug-only, so update
  the tests rather than keep aliases.

## Phases

1. `LauncherStore` and its schema, plus migration step 1. `EntryLauncher`
   and `AppsPane` read from the store. JVM tests for parse, unknown-field
   round-trip and migration.
2. Rust merge in `scan_json` and `resolve_metadata_for_app_id`, plus
   the new JSON fields. Host-side tests where pure, and integration
   tests through `launcher-list`.
3. Editor rewrite: overrides, shortcuts, the `icons/` dir and the SVG
   rasterize call. Delete the `.desktop` writing code.
4. Migration step 2 (managed dir). Integration test that seeds a
   v3-shaped personal file plus a hand-made file with extra keys, then
   checks that only the first is converted and that a pin id for it
   still resolves.
5. Export format 2. Integration test: export → import keeps overrides,
   shortcuts and icon files, and a format-1 export with metadata fields
   still migrates.
6. Notes: rewrite notes/launcher.md's editor, hide and override
   sections. Update notes/installation.md (export format, the
   `metadata.json` fields removed, and frozen ids: the `tawc:app:`
   prefix and the `entries.json` keys). Delete this plan.

Verify on the `.tawctarget` device: override a packaged app's name and
graphics, then upgrade the package and check that the override survives
and other fields follow the upgrade. Then make a shortcut, pin it, and
do an export/import round trip.
