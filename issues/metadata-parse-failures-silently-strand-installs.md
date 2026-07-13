# metadata.json parse failures silently strand installs

`InstallationStore.list()`/`load()` wrap `Installation.fromJson` in
`runCatching { … }.getOrNull()` (`InstallationStore.kt:83,92`) with no
logging. Any parse exception makes the install simply vanish: the UI
shows nothing, while the multi-GB rootfs sits orphaned on disk. That
converts any future schema mistake — or a corrupt metadata file —
into invisible data loss instead of a recoverable error state.

Known throw path today: `parseTawcInstalls` uses
`TawcInstall.Type.valueOf(...)` (`Installation.kt:242`), which throws
on an unknown type. If a future version adds a `Type` variant and the
user downgrades (or a future reader drops one), the whole record is
dropped. Contrast `state`, which is parsed defensively
(`runCatching { State.valueOf(it) }.getOrDefault(State.READY)`,
`Installation.kt:211-212`).

Related: `schemaVersion` is written and read but nothing dispatches on
it, so the migration hook described in notes/installation.md ("Schema
versioning") has no code site yet.

Why this matters *before* the first public release: whatever reading
behavior v1 ships is what v1 users' devices run forever. Tolerant
parsing and a visible corrupt-metadata state only help if they're in
the first APK real users install.

Possible fix:

- `list()`/`load()`: on parse failure, log and surface a
  "corrupt metadata" installation state (id from the directory name)
  instead of dropping the record — the user should see the slot and
  get an explicit recover/uninstall choice, not an empty list.
- Parse `TawcInstall.Type` defensively like `State` (skip or default
  unknown entries; skipping one manifest entry only means one stale
  file left behind on the next stamp refresh).
- Optionally wire an actual `schemaVersion` dispatch (even a no-op
  `when`) so the hook exists.
