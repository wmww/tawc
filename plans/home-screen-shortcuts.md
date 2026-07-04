# Home-screen shortcuts — pin launcher entries to the Android launcher

Let the user put individual Linux programs on the Android home screen.
**Depends on [launcher-entry-management.md](launcher-entry-management.md)**
(per-entry action menu + `EntryLauncher`). Independent of
[launcher-custom-programs.md](launcher-custom-programs.md), but both
route through `EntryLauncher`, so terminal-entry handling composes
automatically whichever lands first.

## Mechanism: pinned shortcuts

`ShortcutManagerCompat.requestPinShortcut` (androidx.core; minSdk is 29
so the API 26+ pinned-shortcut path is always available — verify
androidx.core is an explicit dep, it's already on the classpath via
`ViewCompat`). Rationale vs the alternatives:

- **Pinned shortcuts** render as first-class app icons, are unlimited,
  carry a per-shortcut icon/label, and their intents may target
  non-exported components of the publishing app — no new attack
  surface. Clear winner for "one icon per program".
- **App widget** (Termux:Widget style) needs an `AppWidgetProvider`,
  RemoteViews, and a config activity for strictly worse icon fidelity.
  A scrollable *list* widget is the one thing shortcuts can't do —
  possible follow-up, out of scope here.
- **`activity-alias`** is build-time-static; can't represent arbitrary
  user programs.
- **Dynamic shortcuts** (long-press tawc icon → recent programs) are a
  cheap follow-up once this lands; out of scope.

## Shortcut payload: reference, not command

The shortcut intent carries `(installId, desktopId)` — **never the
`Exec` string**. Re-resolving at tap time keeps shortcuts current when
the `.desktop` file changes (or gets edited by plan 3's editor) and
means the launcher's shortcut store never holds an executable command.

- Shortcut id: `"<installId>/<desktopId>"` — stable, so re-pinning the
  same entry updates the existing pin (`requestPinShortcut` with an
  existing id refreshes label/icon).
- Label: `entry.name` (fall back to id).

## Trampoline: `launcher/ShortcutLaunchActivity`

New activity, manifest-declared:

- `exported="false"` (pinned-shortcut intents may target non-exported
  components of the owning app), translucent theme, `noHistory`,
  `excludeFromRecents="true"`, `taskAffinity=""` so tapping a shortcut
  doesn't yank the main tawc task forward.

Flow in `onCreate`:

1. Read `installId` / `desktopId` extras; `InstallationStore.load`.
2. Gate: missing install or state != READY →
   `LaunchErrorActivity` ("<label> is not installed" / state message),
   finish.
3. Resolve the entry: `NativeBridge.nativeLauncherScan(rootfs)` on
   Dispatchers.IO, find by id. Scan cost at tap time is the same walk
   the launcher does on open — acceptable. Entry gone →
   `LaunchErrorActivity` ("app no longer installed"), finish.
4. `EntryLauncher.launch(applicationContext, inst, entry)`; finish.

A hidden (plan 1) entry still launches — hiding is a list-decluttering
feature, and an existing pin is explicit user intent.

## Pinning UI

Plan 1's entry action menu gains **"Add to home screen"**:

1. Build `ShortcutInfoCompat` (id, label, icon, intent with extras +
   `Intent.ACTION_VIEW` — an action is required on shortcut intents).
2. `isRequestPinShortcutSupported` false (rare: custom launchers) →
   toast and bail.
3. `requestPinShortcut` — the system sheet handles placement/confirm.

## Icon

- Decode `entry.iconPath` (`BitmapFactory`, same constraints as
  `IconLoader` — bounded sample size), center it onto a square bitmap
  with ~25% padding on a neutral solid background, and use
  `IconCompat.createWithAdaptiveBitmap` so it masks correctly on every
  launcher shape.
- No icon → fall back to the tawc app icon (`createWithResource`), so
  the pin is still recognizable.

## Stale shortcuts

Uninstalling a distro leaves its pins behind. MVP: the trampoline's
READY/missing gate turns a stale tap into a clear
`LaunchErrorActivity`, which is enough. Optional follow-up (not MVP):
the uninstall path calls
`ShortcutManagerCompat.getShortcuts(FLAG_MATCH_PINNED)`, filters ids
prefixed `"<installId>/"`, and `disableShortcuts` with a "distro
uninstalled" message — keeps uninstall code free of shortcut APIs for
now.

## Terminal entries

No special-casing here: dispatch goes through `EntryLauncher`, so
Terminal=true pins behave exactly like launching from the in-app list —
headless today, native terminal once
[launcher-custom-programs.md](launcher-custom-programs.md) lands.

## Tests

- Unit: shortcut-id mapping (`installId`/`desktopId` round-trip through
  intent extras), icon fallback selection.
- Manual on `.tawctarget` (pinning is a launcher-UI interaction; not
  automatable via the broker):
  - Pin a GUI app; tap with the app process dead → cold-start launch
    works.
  - Re-pin the same entry → existing pin updates, no duplicate.
  - Uninstall the distro; tap the stale pin → error dialog, no crash.
  - Emulator + physical device (different launchers).

## Doc updates (same change)

- notes/launcher.md: shortcut payload contract (reference not command),
  trampoline flow, stale-pin behavior.
- This plan is deleted and folded into notes/launcher.md when done.
