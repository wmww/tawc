# In-app launcher

Per-distro app picker that reads `.desktop` files inside a rootfs and
lets the user search + launch: the home screen's apps tab
(`launcher/AppsPane`, for the open distro, notes/android.md "Home
screen"), plus pinned shortcuts.

## Pipeline

1. **MainActivity** shows a READY open distro's `DistroHome`, whose
   first tab is `AppsPane`.
2. **AppsPane.rescan()** — on every show, distro switch and resume, so
   packages installed from the terminal appear — →
   `LauncherEntry.list` (scan + built-ins, below) →
   `LauncherEntry.scan(rootfs)` on
   `Dispatchers.IO` — the shared wrapper around
   `NativeBridge.nativeLauncherScan` + JSON parse that every scan
   consumer (launcher list, shortcut trampoline, `launcher-list`
   broker action) goes through. Native failure = empty list.
3. **launcher.rs** walks `APPS_SUBDIRS` under the rootfs —
   `root/.local/share/applications` (the guest's XDG per-user dir;
   fake root, so `$HOME` is `/root`), `usr/local/share/applications`,
   `usr/share/applications`, flatpak/snap exports — resolving
   symlinks inside the rootfs (`resolve_in_rootfs`; LibreOffice ships
   absolute links into `/usr/lib/libreoffice/share/xdg/` that dangle
   on the host, and the editor's `DesktopEntryFile.fileInRootfs`
   mirrors this) — parses each
   `.desktop` via the `freedesktop-desktop-entry` crate, filters
   non-Application / NoDisplay / Hidden / Exec-less entries, and (in
   `scan_json`, not the entry walk — see "Icon resolution") resolves
   `Icon=` to an on-device PNG path. De-dup by id happens in walk
   order *before* the name sort, and `APPS_SUBDIRS` is ordered
   user-first, so a user's copy of an id shadows the packaged one
   ("hide the packaged entry behind my edited copy" works). Then
   sorted by localised name.
4. **LauncherEntry.parseList** turns the JSON into Kotlin records
   (`id, name, comment, exec, terminal, iconPath, path, shadows` —
   `path` is the absolute host path of the `.desktop` source file,
   `shadows` the lower-priority copy of the id the de-dup dropped; the
   editor uses both to tell overrides, personal and packaged entries
   apart).
5. **AppsPane** filters hidden entries + the search query, then
   renders rows (icon ImageView + name + comment). `IconLoader`
   async-decodes PNGs with `BitmapFactory.inSampleSize` keeping memory
   bounded, and holds them in a byte-bounded `LruCache` keyed by path; each rescan
   evicts bitmaps whose file mtime/size changed (`dropStale`), so an
   icon replaced in place updates. An entry with
   no resolvable icon gets `ic_terminal_fallback` or `ic_app_fallback`.
6. Tap or Enter → `EntryLauncher.launch(appContext, inst, entry)`, the
   shared dispatch point for every launch surface. `Terminal=true`
   entries on tawcroot installs open a new home terminal tab running
   the command instead (see notes/terminal.md "Command sessions"); proot/chroot
   terminal entries fall through to the headless path with a logcat
   warn. Everything else is a GUI launch behind a splash (see "Launch
   splash"): `EntryLauncher.launchGui` opens the splash task, then on
   its process-wide `LAUNCH_SCOPE` (Dispatchers.IO) reserves the host
   and starts `"export XDG_ACTIVATION_TOKEN=<t>
   DESKTOP_STARTUP_ID=<t>; <exec> </dev/null 2>&1"` via
   `UserRootfsSession.startInside`, reading the merged output through
   `MethodRunHelper.collectProcess` for the program's whole lifetime (a
   full pipe would block it).
   `EntryLauncher.guiCommand` prefixes a guest-side probe: if argv0
   resolves (`command -v` + `readlink -f`) next to
   `chrome_100_percent.pak`, or `/usr/lib/<name>/` has one, it appends
   `--no-sandbox` — Chromium-family apps (Chromium, ChatGPT) refuse to
   run as root without it and ignore `ELECTRON_DISABLE_SANDBOX`.
   `UserRootfsSession` holds a session reason while the process lives.
   The pane clears the query and drops the IME (a 500 ms debounce stops
   a hardware Enter's key event + editor action double-launching); the
   coroutine keeps blocking on the process for its lifetime, which
   pins one IO thread per running app. We can't `setsid -f` detach:
   proot's `--kill-on-exit` (kept on for pacman cleanup) SIGKILLs any
   backgrounded child when the launcher bash exits, so the app would
   die before it ever opened a Wayland window. Blocking for the
   program's lifetime is correct anyway — the program needs the JVM
   alive for the compositor's Wayland socket. Spawn failures go to the
   splash's log view, or with no splash up to `LaunchErrorActivity`
   from the application context.

## Launch splash

A GUI tap opens the window's own task at once, the way an Android
app's splash works: a `CompositorActivity` in launch mode
(`CompositorActivity.launchIntent`, `tawc://activity/<launchId>` plus a
`launchId` extra) shows the entry's icon and name (`LaunchSplash`, a
plain view over the SurfaceView, black like the compositor window) and
sets the recents label/icon up front. The program's first root window
maps into **this** Activity's host — no second task, no task switch —
and the splash fades out on that host's first frame with content.
Terminal entries and built-ins are unchanged (terminal tabs).

**Record** (`LaunchRegistry`, Kotlin, process-wide): `launchId`
(`launch-<random>`, also the host's `ActivityId`), desktop id, name,
icon path, terminal flag, and a capped output ring (1000 lines /
128 KiB) for the log view. Output is dropped once a window matches;
the pipe keeps draining. States (`reduce`, pure, `LaunchStateTest`):

| State | Means | Splash shows |
|---|---|---|
| Waiting | no window yet | icon + name |
| Handoff | exited 0, nothing left in its session: maybe handed to a running instance; 5 s grace | icon + name |
| TimedOut | 20 s and no window, still running | log, live; a late window still lands here |
| Exited(code) | exited without a window (non-zero, or after the timeout) | log, "exited with code N" |
| Failed(msg) | spawn threw (e.g. the fail-closed bind check) | log + message |
| Quit | Handoff grace ran out | closes with a toast |
| Detached | no host could be reserved | closes; the window opens as without a splash |
| Matched → Shown | window assigned → first frame rendered | fades out |

Exit 0 with processes left in the session (a launcher script that
backgrounds the program) keeps Waiting: `LaunchRegistry.sessionAlive`
scans `/proc` for the sid. Timers live in the registry, so a
configuration change doesn't reset them.

**Session id.** `startInside` runs tawcroot with `-s`, so it calls
`setsid()` before loading the guest: the session id is the spawned pid
(`MethodRunHelper.pidOf`, reflection). Verified on the emulator under
tawcroot: every guest process of a launch shares it.

**Reserved hosts** (Rust, `launch.rs`; JNI
`nativeReserveLaunchHost`/`nativeUpdateLaunch`/`nativeReleaseLaunchHost`
over the surface-event channel). Reserving needs a running compositor,
because the activation token comes from Smithay's `XdgActivationState`,
so `LaunchRegistry.reserve` starts it and waits (≤5 s, 3 tries) before
the spawn. `TawcState.pending_launches` holds `{host, desktop_id,
token, sid, exited}`; while any exists, `check_idle` never stops the
compositor. The splash's `onDestroy` (Back, swipe, Close — not a
configuration change) releases the reservation only; the program keeps
running and a later window gets a normal Activity. The host itself
enters `hosts` when the splash registers its surface, like any host;
it has no toplevels, so nothing finishes it for being empty.

**Matching** a root window to a launch, strongest first (pure policy in
`launch_match.rs`, host-testable with `rustc --edition 2021 --test
compositor/src/launch_match.rs`):

1. Session id at `new_toplevel`: the client's pid from
   `get_credentials`, field 6 of `/proc/<pid>/stat`. Synchronous, so
   the window never gets a throwaway Activity; covers wrapper scripts
   and forking launchers. Misses apps that `setsid` themselves.
2. X11 at `map_window_request`: the session of `_NET_WM_PID` (the
   Wayland client is Xwayland, so credentials don't help).
3. xdg-activation token (`xdg_activation_v1`, advertised for this):
   the launch's token, exported as `XDG_ACTIVATION_TOKEN` and
   `DESKTOP_STARTUP_ID`, on `activate` from a root toplevel matches
   across processes — the handoff to an already-running instance. That
   window already has an Activity, so it and everything on its host
   move to the launch host (`adopt_into_launch_host`) and the old
   Activity is finished; the brief flicker is accepted. Other tokens
   keep their usual meaning: bring the window's task to the front
   (`activateActivity` → `AppTask.moveToFront`), only for tokens
   requested from the foreground host. Unused client tokens expire
   after 60 s.
4. app_id / WM_CLASS vs. the desktop id (the icon-lookup comparison),
   only for launches whose session is unknown (proot/chroot) or gone,
   so a second window of a running app doesn't steal a fresh launch.
   Wayland checks on `set_app_id` and adopts like 3; X11 at map time.

A match removes the launch, calls `onLaunchMatched`, and arms a
one-shot first-frame check: after a render of that host with a
committed buffer on a root window, `onLaunchShown` hides the splash.
The reserved host usually has a size before the client connects, so a
matched toplevel is configured at once instead of waiting for an
Activity to spawn. Single-activity mode never claims a launch.

A splash task restored after process death (or reopened once closed)
has no record and finishes itself. Matched windows never need
`spawnActivity`, so Android's background-activity-launch block doesn't
apply to them. Unmatched windows from a client with no TAWC activity in
front (e.g. a `nohup` job after a swipe) can't get a `CompositorActivity`
and never show; that is Android policy, not a TAWC bug.

Debug broker: `launcher-launch` (the tap path, with a settable
`timeoutMs`) and `launch-state`; `query-state` reports
`pending_launches` and `host_windows` (`<host>:<windows>` per host).
Coverage: `tests/integration/tests/launch_splash.rs` (Wayland, wrapper,
backgrounding launcher, X11, failure log, timeout then late window,
token handoff via `wayland-debug-app activation-listener`, swipe
releases the reservation). Verified on the emulator 2026-10-08:
lxterminal (GTK3), xclock (X11) and Firefox each map into their splash
task with one host. No Qt app was checked: the emulator rootfs has no
Qt libraries (qv4l2 landed in the log view with the loader error). Not
done: a "Stop app" button on the timeout screen (killing the session).

## Hide / unhide + per-entry menu

Long-press on a row opens an action-list dialog (plain
`AlertDialog.setItems`, no Menu resources) built from a per-entry
`List<EntryAction>` (label + enabled + handler) in
`AppsPane.entryActionsFor` — append there to grow the menu.
Today's items: **Hide** on visible entries, **Unhide** on hidden ones,
**Add to home screen** (see "Home-screen shortcuts"), **Edit** on
every scanned entry (see "Managed dir + .desktop editor").

Hidden state lives in `Installation.hiddenDesktopIds` (ids =
`LauncherEntry.id`, filename minus `.desktop`), written only through
`InstallationStore.update` via `Installation.withEntryHidden`. The
field is additive with a safe default — no `schemaVersion` bump — and
serialized only when non-empty. Uninstall wipes `metadata.json`, so
hide state resets with the install; stale ids never match and are not
pruned.

Filtering is **Kotlin-side** (`LauncherEntry.filter`, a pure
unit-tested function driven from `AppsPane.applyFilter`), not
in `launcher.rs::scan_entries`:

- Hide state is per-install app metadata; the scanner takes only a
  rootfs path and shouldn't grow a metadata side-channel.
- `resolve_metadata_for_app_id` shares `scan_entries` for window
  icons/titles — a hidden app that is *running* must still resolve.

The pane is an Android-launcher-style grid under the home tab bar: an
always-visible search pill ("Search"; the distro name is in the bar), then icons in name
order with one-line, end-ellipsized names; descriptions are not shown.
Columns = width / 88dp (min 3). Bottom padding lets the last row scroll
clear of the FAB, which also hides while scrolling down. The search
field never takes focus on its own (the pane's column soaks up the
window's initial focus): only a tap or a printable hardware key with
nothing focused puts it there. Enter launches the top match; ✕ (shown
with a query), Back or a launch clears it and drops the IME. The ⋮ is
the home screen's one menu; on the apps tab it adds a
checkable **"Show hidden (N)"** item (N counts hidden ids that match
actual entries; omitted when N is 0) and, on editable methods, **"Add
entry…"** (the editor). Show-hidden is transient
per-pane state, not persisted.
With it on, hidden entries render dimmed (alpha 0.5) in their normal
sort position and launch normally on tap. With it off, a search whose
only app matches are hidden shows those (dimmed) instead of nothing.
If every entry is hidden,
the empty-list message appends a "(N hidden)" hint.

Debug broker actions (notes/exec-broker.md): `launcher-list` returns
the post-filter list as JSON (optionally including hidden entries with
`showHidden=true`), including the resolved `iconPath` so icon tests can
see what the scanner picked, plus `shadows` and the `graphics`/`pointer`
overrides; `set-entry-hidden`, `set-entry-graphics` and `set-entry-pointer` perform the same
metadata writes as the UI. Integration coverage: `launcher::` tests in
`tests/integration/tests/launcher.rs`.

## Built-in entries

`LauncherEntry.Builtin` entries are synthesized Kotlin-side
(`builtinsFor` + `withBuiltins`, unit-tested) after the scan, not
`.desktop` files. Ids carry a reserved `tawc:` prefix (scanned ids with
it are dropped), so hide state (`hiddenDesktopIds`) and pin ids work
unchanged and the query matches them like any entry. Edit is never
offered.

| Entry | Id | Action | Offered | Pin |
|---|---|---|---|---|
| TAWC Term | `tawc:term` | new shell tab | tawcroot | yes |
| Update packages | `tawc:update` | new command tab running `Distro.upgradeCommand` | tawcroot | yes |
| Add entry | `tawc:add-entry` | `DesktopFileEditorActivity` (new), for result | not chroot | no |

TAWC Term and Update packages sort by name with everything else; Add
entry sorts last whatever the query (`LauncherEntry.filter`). The
terminal ones draw their glyph (`ic_terminal`, `ic_update`) white on a
round black `builtin_icon_bg` tile — the same circle as
`ic_terminal_fallback` — in grid and pins alike, so TAWC Term looks
like any icon-less terminal entry; Add entry is a bare themed
`ic_add_entry` plus.
`launcher-list` includes them with `builtin: true`
(`launcher::test_builtin_entries_listed_and_hideable`).

## Managed dir + .desktop editor

`/root/.local/share/applications/` is the **managed dir** — the
package-manager boundary. `DesktopFileEditorActivity` (launched for
result from the launcher's "Add entry…" overflow item and per-entry
"Edit" action) only ever writes there. Edit is offered on every scanned
entry (not built-ins, not chroot); `/usr/share`, `/usr/local` and
flatpak files are never modified.

- **Overrides.** Saving an edit of a packaged entry writes a copy to
  `<managed>/<id>.desktop`; the scanner's user-first de-dup makes it
  hide the packaged one, and since it keeps the id it inherits hide
  state and pins. The id is derived like the scanner's crate does
  (`DesktopEntryFile.idFor`: path after the last `/applications/`,
  `/` → `-`), so a nested `applications/kde4/foo.desktop` becomes
  `kde4-foo.desktop`.
- **What an entry is**, derived from the scan every time (no marker in
  the file): the scanner reports `shadows`, the path of the copy the
  de-dup dropped. Managed + `shadows` = override; managed without =
  personal entry; not managed = packaged. An override whose package is
  removed loses `shadows` and becomes a personal entry.
- **Toolbar action.** Personal: trash, Delete. Override, or a packaged
  entry with only a graphics or pointer override: `ic_reset`, Reset ("…to
  default?"), which deletes the managed copy. Both also clear the id's
  graphics and pointer overrides, so a future entry reusing the slug
  doesn't inherit them. Packaged with nothing overridden: none.
- **Patch, don't rewrite.** Existing files go through
  `DesktopEntryFile.patch`: inside `[Desktop Entry]`, only changed keys
  among Name/Exec/Icon/Terminal/Comment are replaced in place or
  appended (empty Icon/Comment removes the key; a false Terminal is
  never added). A changed Name/Comment drops its `Name[xx]=` variants,
  which would otherwise keep winning. Everything else — locale keys,
  `MimeType`, `Actions`, other groups, comments — is kept byte for
  byte. `serialize` is for new entries only. Non-UTF-8 files are
  refused with a toast rather than patched.
- **Save rules.** Unchanged text writes nothing (so a graphics-only
  edit of a packaged entry doesn't fork it); an override patched back to
  exactly its packaged text is deleted. Writes are atomic
  (`atomicWriteText`).
- **Stale overrides** are accepted: a package upgrade doesn't reach an
  override (standard XDG model); Reset is one tap away.
- Editable check is Kotlin-side: `DesktopEntryFile.isManaged` prefixes
  `entry.path` against the managed dir. Both sides are canonicalized —
  Kotlin's `context.dataDir` is `/data/user/0/<pkg>` while the Rust
  scanner canonicalizes its walk roots to `/data/data/<pkg>`, so a
  naive prefix check never matches. The editor also checks
  `EXTRA_PATH` is a regular file inside the rootfs.
- Method gate: writes are plain app-uid file I/O, fine for
  tawcroot/proot but not chroot's root-owned rootfs (see "Access
  model") — chroot installs get no New/Edit entry points, consistent
  with the terminal gating.
- Form: Exec (required) + Name (blank = Exec, shown as the hint),
  Environment variables, Icon (see "Icon field" below), Terminal
  checkbox (checked by default for new entries — hand-made entries are
  usually CLI scripts), Override graphics, Override pointer emulation.
- **Environment variables** live in the file, the XDG way:
  `Exec=env K=V … command` (`DesktopEntryFile.splitExec`/`joinExec`),
  so they travel with the entry and other desktops see them too. Rows
  are `[NAME] [value] ✕` plus a "+ Add" button; fully blank rows are
  ignored, and an invalid name flags its field and disables Save.
  Split accepts `'…'`, `"…"` and `\x` quoting and `%%`; anything it
  can't represent (`env -i`, no command, unterminated quote) stays in
  Command untouched. Join double-quotes values that need it and doubles
  `%` (the scanner strips `%X` field codes). While command and
  variables are as loaded, Exec is written back verbatim, so an
  untouched entry still patches to identical text. Note the scanner
  collapses whitespace runs in Exec, quoted or not.
  `Comment=` has no field but is carried through.
- New file: `slugifyLabel`-style slug of Name + `.desktop`, `-2`/`-3`
  suffix on collision. Editing keeps the filename — it's the entry id.
- Closing: the toolbar shows ✕ rather than ←, since leaving discards
  the form. With unsaved changes (form fields + graphics vs. as
  opened), ✕, Back and the back gesture ask "Discard changes?"
  first; the `OnBackPressedCallback` is enabled only while dirty, so a
  clean form keeps predictive back.
- After save/delete/reset the launcher rescans (`RESULT_OK` →
  `loadApps()`).

### Graphics override

An "Override graphics" checkbox under Terminal; checking it reveals a
radio group of every backend this build ships (`ui/GraphicsBackendGroup`,
shared with Settings), preselected to the global pick. Unchecked = no
override. Stored per install, not in the file:
`Installation.entryGraphics` (id → `GraphicsBackend.key`, additive,
serialized only when non-empty, written via `withEntryGraphics`), so a
graphics-only change never forks a packaged entry.
`EntryLauncher.graphicsFor` resolves it with
`GraphicsBackend.fromKeyOrNull` (unknown/unshipped key → null = the
global setting) and passes it to `runInside`, or for `Terminal=true`
entries through `MainActivity.EXTRA_GRAPHICS` → `CommandTab` →
`TawcrootMethod.ptyShellExec`. Pins go through `EntryLauncher` with a
fresh `Installation`, so they follow it too. Only TAWC launches honour
it: running the same program from a terminal uses the global backend.
Verified on the emulator 2026-10-07 (Firefox with a CPU override spawns
with `LIBGL_ALWAYS_SOFTWARE=1` while the global pick is gfxstream).

### Pointer emulation override

Same shape below it: "Override pointer emulation" reveals
`ui/PointerEmulationGroup` (shared with Settings), stored in
`Installation.entryPointerEmulation` (id → `PointerEmulation.key`, via
`withEntryPointerEmulation`), resolved by `EntryLauncher.pointerEmulationFor`.
The launch hands it to the compositor with `nativeReserveLaunchHost`,
which applies it to the launched session and launch host — see
notes/input.md ("Pointer emulation"). GUI launches with a splash only;
terminal entries and launches without a reserved host get the global mode.

### Icon field

`[preview] [field ✕] / [Select] [Load]`. The field is a freeform
`Icon=` value (a name, or an in-rootfs absolute path); ✕ is a box-less
`TextInputLayout` with `END_ICON_CLEAR_TEXT`.

- **Preview**: what the grid would draw. `nativeResolveIcon(rootfs,
  value)` (the scan's resolver and SVG cache, one value) on IO,
  debounced 250 ms after typing and immediate on a Terminal toggle;
  empty/unresolved shows the grid's fallback glyph for the current
  Terminal state.
- **Select** → `IconPickerActivity`, a searchable grid (same pill and
  column math as `AppsPane`) of `nativeListIcons(rootfs)`:
  `[{name, user}]`, one entry per name. Cells resolve lazily through
  `nativeResolveIcon` (4 at a time, memoized per name for the
  activity), since rasterizing a whole theme up front takes seconds.
  Case-insensitive substring filter; `user` names (imports) first with
  no query. Picker renders land in the shared `icon-cache/`, so the
  next launcher scan prunes them; reopening the picker re-renders the
  visible cells (~8 ms each).
- **Load** → `OpenDocument(image/*)` → `IconImport`: rasters are
  decoded with `ImageDecoder`, scaled to fit 256² (never up) and
  re-encoded as PNG into
  `/root/.local/share/icons/hicolor/256x256/apps/`; SVGs (MIME or
  `.svg` name, ≤ 1 MiB like the cache's cap) are copied to
  `…/hicolor/scalable/apps/`. The name is `tawc-<slug of the document
  name>`, `-2`/`-3` on collision across both extensions, reusing a
  candidate whose bytes are identical. Written atomically
  (`atomicWriteBytes`). The field gets the plain name, so guest
  desktops reading the `.desktop` file find it too, and imports travel
  with the rootfs. Nothing deletes imports. Name logic:
  `IconImportTest`.

Serializer/patch/parse/id/slug logic is JVM-unit-tested
(`DesktopEntryFileTest`, `InstallationEntryGraphicsTest`); scan-dir,
precedence, `shadows` and the graphics override are integration-tested
through `launcher-list` (`tests/integration/tests/launcher.rs`).
Editor flows (edit packaged → one tile → Reset) verified on the
emulator 2026-10-07.

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
  A built-in id resolves before the scan, straight to
  `EntryLauncher`, which sends it through the `.CommandLaunch` path.
- A hidden entry still launches from its pin — hiding declutters the
  list; an existing pin is explicit user intent. Terminal entries get
  no special casing: dispatch goes through `EntryLauncher`, same as
  the in-app list.
- **Icon**: entry PNG decoded via `IconLoader.decode`, centered on a
  neutral square at 2/3 edge (adaptive-icon safe zone) and wrapped
  with `IconCompat.createWithAdaptiveBitmap` so it masks correctly on
  every launcher shape; no/undecodable icon falls back to the same
  glyph the grid cell uses (`ic_terminal_fallback` for `Terminal=true`,
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
2. Bare name → the theme walk over the icon bases, below.
3. `usr/share/pixmaps/<name>.{png,svg,svgz}` (legacy fallback).
4. `Icon=name.<ext>` strips known image extensions before the search.

The theme walk is spec-*shaped*, not the full fdo size-matching
algorithm — we want "largest sensible raster, else scalable":

- **Bases** (`ICON_BASES`): `/root/.local/share/icons` (the spec's
  `$XDG_DATA_HOME/icons`, where editor imports go), then
  `/usr/local/share/icons`, `/usr/share/icons`, and flatpak's
  `exports/share/icons`. A theme can span bases; its dirs from all
  bases are merged, and the user base wins a tie.
- **Theme order**: seeds `default`, `Adwaita`, `Papirus`, `breeze`,
  `hicolor`, each expanded breadth-first through its `index.theme`
  `Inherits=` (minimal line scan, stops at the second group header; no
  theme crate; read from the first base that has it). De-duplicated,
  missing themes dropped, `hicolor` forced last so an inherited parent
  still gets a look in. `default` leads so a distro/user-selected theme
  wins.
- **Per theme**: contexts `apps`, then `legacy`, then `categories` —
  generic names like `utilities-terminal` live outside `apps` in several
  themes — then every other context the theme has (`places`,
  `mimetypes`, …, by name), so an app's own icon beats a same-named
  generic one and the picker's every name resolves. Sizes
  `128, 96, 256, 64, 48`, then `scalable`, then `32, 24, 22, 16` (a
  vector icon beats a 16 px PNG blown up to a 56 dp row), then any other
  size dir (`512x512`, `36`, `48x48@2`, …) largest effective size
  first — Electron/flatpak apps often ship only 512. Both layouts are
  read: `<size>/<context>` (hicolor, Adwaita) and `<context>/<size>`
  (breeze), with numeric sizes spelled both `48x48` and bare `48`.
- Extensions per directory: `png`, then `svg`, then `svgz`. XPM is not
  searched — we can't decode it and only a couple of `NoDisplay` python
  entries still ship one.
- Each theme's two directory levels are read once per resolver
  (`ThemeDir::new`) into an ordered dir list, so the walk only stats
  dirs that exist instead of the full contexts × sizes × layouts ×
  extensions grid.
- **One walk, two consumers**: `IconResolver::sources` yields every
  source dir (theme dirs, pixmaps, symbolic dirs) in resolution order.
  `resolve` takes the first hit; `list` (the picker's
  `nativeListIcons`) collects every name from the same sources,
  `-symbolic` stripped for symbolic dirs and stems `resolve` would
  rewrite skipped, so a listed name resolves to the file the launcher
  would use (`launcher::test_listed_icons_resolve`). Dangling symlinks
  aren't listed.
- Why this exists: a user typing `folder` got nothing because it lives
  only in `Adwaita/scalable/places`, a context the walk used to skip.

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
- Written to `<hex>.<pid>-<seq>.tmp` then renamed — the launcher and the
  shortcut trampoline can scan concurrently, and picker cells render
  on several threads.
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
`<theme>/symbolic/<context>/<name>-symbolic.svg`, preferred contexts
first. It is
last because it loses the app's colours: Konsole on sid asks for
`utilities-terminal` and the rootfs ships only
`Adwaita/symbolic/legacy/utilities-terminal-symbolic.svg`.

Symbolic SVGs are a single near-black colour, so they vanish on a dark
background, and a cached PNG can't follow the app theme. They are baked
light-on-dark instead: the SVG is rendered at 60 % scale, its **alpha is
kept as a mask** and repainted white over a black rounded tile with the
same proportions as `ic_app_fallback`. Masking rather than
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
- Launching from the apps tab's search (one field for commands and
  apps).

None of these block today's "type-and-go" flow; revisit after dogfooding.
