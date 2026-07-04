# Launcher entry management — hide/unhide + per-entry menu structure

Foundation plan for launcher UX work. Adds a per-entry long-press menu
and hidden-entry state to `LauncherActivity`, plus the structural pieces
(entry source path, shared launch dispatch, extensible menu) that
[home-screen-shortcuts.md](home-screen-shortcuts.md) and
[launcher-custom-programs.md](launcher-custom-programs.md) build on.
**Execute this plan first; the other two depend on it.**

## Scope

- Long-press context menu on launcher rows, built as an extensible
  action list so later plans can append items (Add to home screen, Edit).
- Hide / unhide entries, persisted per install.
- Launcher overflow menu with a "Show hidden" toggle.
- Structural: `.desktop` source path in the scan JSON; extract
  `launchEntry` dispatch into a shared helper; debug broker action for
  test visibility.

## Hidden state — metadata

New field on `Installation`
(app/src/main/java/me/phie/tawc/install/Installation.kt):

```kotlin
/** Desktop-entry ids the user hid from the launcher list. */
val hiddenDesktopIds: List<String> = emptyList(),
```

- Ids are `LauncherEntry.id` (filename minus `.desktop`), matching the
  Rust scanner's stable id.
- Additive JSON field with a safe default → **no `schemaVersion` bump**
  (per the comment in Installation.kt). Serialize only when non-empty,
  like `externalBinds`.
- All mutation through `InstallationStore.update(id) { ... }` — the
  locked read-modify-write is mandatory for existing records (see the
  `save` kdoc). Hide = `copy(hiddenDesktopIds = ... + entryId)`, unhide
  removes. No state gate needed beyond what `update` gives us: the
  launcher only opens for READY installs, and a lost race against
  uninstall just aborts the write.
- Uninstall wipes `metadata.json`, so hidden state resets with the
  install. Intended. Stale ids (app removed from the distro) are
  harmless — they just never match — so don't prune.

## Where filtering happens: Kotlin, not Rust

`launcher.rs::scan_entries` stays untouched by hide state. Reasons:

- Hide state is per-install Kotlin metadata; the scanner takes only a
  rootfs path and shouldn't grow a metadata side-channel.
- `resolve_metadata_for_app_id` shares `scan_entries` for window
  icons/titles — a user-hidden app that is *running* must still resolve.

`LauncherActivity` keeps `allEntries` complete and filters in
`applyFilter()` against the loaded `Installation.hiddenDesktopIds`
(reload the installation record on hide/unhide so the set is current).

## Structural: entry source path

`launcher.rs::Entry` gains `path`: the absolute host path of the parsed
`.desktop` file (available from the `Iter` walk). Thread it through
`scan_json` (`"path"`) and `LauncherEntry`. Unused by this plan's UI but
required by the custom-programs plan (editable iff under the managed
dir) and cheap to add while touching the JSON shape.

## Structural: shared launch dispatch

Extract the body of `LauncherActivity.launchEntry` into
`launcher/EntryLauncher.kt`:

```kotlin
object EntryLauncher {
    /** Fire-and-forget launch of [entry] in [inst]'s rootfs. */
    fun launch(appContext: Context, inst: Installation, entry: LauncherEntry)
}
```

It owns `LAUNCH_SCOPE`, the stdio-to-/dev/null redirect, and the
`LaunchErrorActivity` failure surface; `LauncherActivity` keeps the
double-launch guard + `finish()`. This becomes the single dispatch
point that the shortcut trampoline (plan 2) calls and the terminal
routing (plan 3) modifies — whichever lands later needs no changes in
the other.

## UI

- **Row long-press** → action menu. Given the app's hand-built-views
  style, a simple list dialog (AlertDialog with items) is enough — no
  Menu resources. Build it from a `List<EntryAction>` (label + enabled +
  handler) assembled per entry, so dependent plans append items
  conditionally.
  - This plan's items: **Hide** (visible entries) / **Unhide** (hidden
    entries, only reachable with show-hidden on).
- **Overflow menu**: small "⋮" tonal icon button beside the search
  field (same `tonalIconButton` style MainActivity uses), opening a
  menu with **"Show hidden (N)"** as a checkable item. Transient
  per-activity state, not persisted. Plan 3 adds "New program…" here.
- With show-hidden on, hidden entries render dimmed (alpha ~0.5) in
  their normal sort position and launch normally on tap.
- Empty-list case: if every entry is hidden, keep the
  "no launchable apps" message but append a "(N hidden)" hint.

## Test surface: broker action

Register a debug broker action `launcher-list <installId>` (via
`ActionRegistry`, like `InstallActions`) returning the post-filter entry
list as JSON (id, name, exec, terminal, path, hidden). Debug-only, same
gating as the rest of the exec broker. This gives integration tests a
query surface for this plan (hidden filtering) and plan 3 (scan dirs,
dedup precedence) without screenshot scraping, per the "prefer explicit
query/debug surfaces" rule.

## Tests

- App unit tests (`./gradlew :app:testDebugUnitTest`):
  - `Installation` JSON round-trip with `hiddenDesktopIds`; legacy
    record without the field parses to empty.
  - `LauncherEntry.parseList` with the new `path` field.
- Integration (via `launcher-list`): hide an id through metadata, assert
  it disappears from the list; unhide, assert it returns.
- Manual on `.tawctarget`: long-press hide, relaunch launcher, toggle
  show-hidden, unhide.

## Doc updates (same change)

- notes/launcher.md: menu structure, hidden-filter location (Kotlin,
  and why not Rust), the `path` field, `EntryLauncher`, the
  `launcher-list` broker action.
- This plan is deleted and folded into notes/launcher.md when done.
