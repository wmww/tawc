# One open distro: home screen and drawer

Multi-distro support stays, but the UI stops presenting it as the main
thing. One distro is "open" at a time; the home screen shows only that
distro; a navigation drawer switches. Most users have one install and
should never notice the multi-distro machinery.

Companion plan: [settings-distro-section.md](settings-distro-section.md)
(the per-distro card at the top of Settings). Do this one first: it
introduces the open-distro state the Settings card reads.

## Today

`MainActivity` lists every install as a card (label, distro line, state,
Terminal + gear icon buttons, search-apps stub), then three full-width
tonal buttons: Task manager, Settings, Install new distro (accent when
nothing is installed). The gear opens `DistroInfoActivity`, which holds
info rows, the ando toggle, Manage binds, Run, and Delete. Nothing in the
app knows which distro the user "uses"; every per-distro screen takes an
install id in its intent.

## Design

### Open-distro state

- `Settings.openDistroId: String?`, key `open_distro`, in the existing
  `tawc-settings` prefs. `TestStore` starts at null so `test_init` resets it.
- `OpenDistro.resolve(store): Installation?` (new small file next to
  `Settings.kt`): the stored id if it still exists, else the first READY
  install, else the first install of any state, else null. When the
  stored id is stale, write back the resolved one. Every screen that
  needs "the" distro calls this at `onResume`; none cache it.
- Writes: the drawer switch; `InstallActivity` on Install (the new slot
  becomes open, so the user comes back to its progress instead of the old
  distro); nothing on uninstall (resolve falls back on its own).
- The dev broker gets no new action unless a test needs one.

### Home screen (`MainActivity`)

Root becomes a `DrawerLayout` (already on the classpath through
Material) wrapping the existing scaffold column plus a `NavigationView`.
Add `Scaffold.buildDrawerScreen(...)` or extend `buildHomeScreen` with a
drawer; keep the imperative-Kotlin style, no XML layouts.

Toolbar: hamburger (new `ic_menu` drawable) on the left, app name
centered as now, `⋮` overflow on the right.

Body, with an open distro:

- The current card design, minus its two icon buttons: label, distro
  line, red state line when not READY, search-apps stub. Tapping the
  card's header opens `DistroInfoActivity`; give the header a trailing
  chevron or `ⓘ` glyph so the tap is discoverable.
- For INSTALLING / UNINSTALLING the state line is tappable and opens
  `LogScreenActivity.intentFor("install:<id>")` / `"uninstall:<id>"`.
  Only running ops have a log (`notes/log-screen.md`: no completed-runs
  history), so FAILED is not tappable; its failure text is on Distro
  info. Today there is no way back to a running install's log from the
  home screen except the notification.
- Floating action button, bottom-right, `ic_terminal`: opens
  `TerminalActivity` for the open distro. Shown only under today's gate
  (READY and tawcroot). This is the most-used action after launching an
  app and deserves the one prominent button.

Body, with no install: the existing empty-state text plus the accent
"Install new distro" button, as today. No FAB. Hamburger stays so the
chrome does not jump around after the first install.

`⋮` overflow menu (toolbar menu, not a custom popup):

- Run command… (READY only). Move `showRunDialog` out of
  `DistroInfoActivity` into a shared helper, e.g. `RunCommandDialog.kt`
  beside `RunCommandOp.kt`.
- Task manager
- Settings

Task manager and Settings could live in the drawer footer instead;
keeping them in `⋮` keeps the drawer single-purpose ("which distro"),
which is the point of the redesign.

### Drawer (`NavigationView`)

- Header: app name and icon.
- One checkable item per install, in `store.list()` order; the open one
  is checked. Subtitle-style state marker for non-READY installs (a
  NavigationView item has one line; append " · Installing…" to the
  title, or use a custom row view if that reads badly). Tapping sets
  `openDistroId`, closes the drawer, refreshes.
- Divider, then "Install new distro" with `ic_add`, always present.
- Back closes the drawer when open (`OnBackPressedCallback` registered
  while the drawer is open).

### Distro info (`DistroInfoActivity`)

Becomes read-mostly: the info rows, the size probe, and the red Delete
button. Run moves to `⋮` on the home screen; ando and Manage binds move
to Settings (companion plan). Keep `EXTRA_ID` so it can still be opened
for any install, but the home screen always opens it for the open one.

### Unchanged

- `TaskManagerActivity` keeps listing every install (and orphans); it is
  the one screen that is legitimately cross-distro. Optionally sort the
  open distro's group first.
- `LauncherActivity`, `TerminalActivity`, shortcuts, and the launcher's
  per-install hidden list all keep taking an install id.
- `InstallActivity` and the `LogScreenActivity` hand-off are unchanged
  apart from the open-distro write.

## Steps

1. `Settings.openDistroId` + `OpenDistro.resolve`; unit test the
   fallback order with an in-memory store.
2. Scaffold: drawer variant, `ic_menu` drawable, FAB helper in
   `Scaffold.kt` matching the existing button shape rules.
3. `MainActivity`: drawer, single card, FAB, `⋮` menu, empty state,
   state-line-to-log tap. Delete the per-card icon buttons and the three
   tonal buttons.
4. Move the run dialog to a shared helper; trim `DistroInfoActivity`
   (Run only; ando/binds go with the Settings plan).
5. `InstallActivity` sets the new slot open on Install.
6. Strings: `action_install_new_distro` stays; add menu/FAB labels.
   Remove `action_manage` if nothing else uses it.
7. Verify on the `.tawctarget` device: 0, 1, and 2 installs; switch
   while one is installing; uninstall the open distro and confirm the
   fallback; FAB gating on a non-tawcroot (debug) install.
8. Notes: update `notes/android.md` "Kotlin App Structure" and
   `notes/launcher.md` for the new entry points; add a short home-screen
   section wherever the drawer/open-distro state is described.

## Open questions

- **Embed the launcher in the home screen.** With one distro open, the
  search stub that forwards to `LauncherActivity` is a detour: the home
  screen could *be* the launcher for the open distro (search field at
  the top, app list below, drawer to switch). That is the natural end
  state of this redesign but changes `LauncherActivity`'s
  focus-on-entry / finish-on-launch behaviour and `EntryLauncher`'s
  callers, so it is left out of the steps above. Decide after step 7.
- Wide screens: a permanent (non-modal) drawer on tablets. Not now.
- Whether `⋮` should also carry "Distro info" for users who miss the
  card tap. Cheap to add; decide when looking at the built screen.
