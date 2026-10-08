# Android Integration

## Wayland Socket Sharing

**With root (chroot):** The compositor creates a Unix socket at a known path and the
chroot client connects directly. Root bypasses SELinux MAC checks on `connect()`.
This is the current development approach.

**Without root (proot, future goal):** SELinux blocks cross-app `connect()` between
`untrusted_app` domains on Android 9+. Two viable solutions:

1. **Binder fd passing (preferred):** Compositor creates a `socketpair()`, passes one end
   to Termux via a ContentProvider or bound Service as a `ParcelFileDescriptor`. No
   `connect()` syscall occurs, so SELinux is never triggered.

2. **Shared UID:** `sharedUserId="com.termux"` makes both apps run as same UID/SELinux
   domain. Deprecated since API 33 but still functional. Limits distribution flexibility.

## Chroot Setup

Install (once, via the dev exec broker; progress streams to your TTY
and the in-app log screen opens automatically):
```bash
scripts/tawc-exec.sh --foreground-app --action install \
    --arg id=arch \
    --arg mirrorProxy=http://127.0.0.1:8080/proxy/
```

Then drive the chroot from the host with:
```bash
scripts/rootfs-run.sh                    # interactive shell
scripts/rootfs-run.sh '<command>'        # run a command and exit
```

`rootfs-run` routes through the dev exec broker's `RUNINSIDE` request
(`tawc-exec --in-rootfs <id>`). The broker reads the install's
recorded method from `metadata.json` and dispatches to the matching
[InstallationMethod.startInside], which builds the bind table and
chroot exec fresh in Kotlin on every call. There is no on-disk
wrapper script and no `adb shell su` in this path — chroot installs
fork `su` from inside the JVM. Generic TAWC Wayland env vars come
from `RootfsEnv.kt` via a `/usr/bin/env -i KEY=VAL …` wrapper around
the in-rootfs `bash -lc`, so nothing inside the rootfs needs to be
on disk between calls.

### Shell quoting

Commands sent through `tawc-exec --in-rootfs` are framed in the
broker wire protocol (length-prefixed argv), so quoting is not an
issue end-to-end. If you ever bypass the broker and use raw
`adb shell su -c '…'` directly, you'll need to handle the layered
quoting yourself:

**Critical quoting rule for `&&` / `||` in `su -c`:** When running compound
commands via adb, the outer shell (mksh) parses `&&` and `||` BEFORE `su` sees
them. This silently runs the second command as shell (uid 2000), not root:

```bash
# BROKEN: mksh splits at &&. cp runs as root, build runs as shell user.
adb shell su -c "cp /tmp/foo /chroot/tmp/ && /chroot/build.sh"

# CORRECT: inner quotes protect && from mksh.
adb shell "su -c 'cp /tmp/foo /chroot/tmp/ && /chroot/build.sh'"
```

Variable expansion like `$0` or `$KSH_VERSION` at any intermediate layer can
give misleading results. The `su` shell on Android is mksh (`/system/bin/sh`),
easily confused with the chroot's GNU bash.

## EGL Context and Surfaces

- An EGL context CAN move between threads (release on old, bind on new), but expensive
- One thread can render to multiple EGLSurfaces via `eglMakeCurrent` switches
- Each switch flushes the pipeline -- overhead per switch
- Recommended: single render thread, one context, switch surfaces per window
- `ASurfaceTransaction` + AHB avoids `eglMakeCurrent` overhead entirely (future opt)

## Multiple Activities

See [multi-activity.md](multi-activity.md) for the full per-window-task plan.
Background facts that informed it:

- All Activities in one app share the same process (single heap, static state, threads)
- One SurfaceView per Activity avoids Z-ordering issues
- Single background render thread maintains list of active surfaces
- Activity launch creates visual transitions -- suppress with
  `overridePendingTransition(0, 0)`
- Activities may be killed under memory pressure -- handle surface loss gracefully

## Kotlin App Structure

The Android app code (`app/src/main/java/me/phie/tawc/`) is split so that
everything talking to the Rust compositor lives in its own package, separate from
the rest of the app's UI/management features.

- `MainActivity.kt` — home screen hosting the intro / info panes or a
  distro's `DistroHome` (tab bar over apps or terminals; see "Home
  screen" below). Plain Android UI (no fullscreen, no Wayland).
- `OpenDistro.kt` — which install the home screen shows.
- `compositor/` — everything that interacts with the Rust compositor:
  - `CompositorActivity.kt` — fullscreen immersive Activity that owns the
    `SurfaceView`, dispatches touch/IME, and registers the test broadcast
    receiver. Started via Intent from `MainActivity`. Uses the
    `Theme.Tawc.Compositor` style.
  - `NativeBridge.kt` — JNI surface (matches Rust JNI symbols
    `Java_me_phie_tawc_compositor_NativeBridge_*` and `find_class
    "me/phie/tawc/compositor/NativeBridge"` in `compositor/src/lib.rs`).
  - `TawcInputConnection.kt` — IME bridge.
- `install/` — Kotlin implementation of the chroot install / run /
  destroy logic. The rootfs is stored under
  `/data/data/me.phie.tawc/distros/<id>/rootfs/` so uninstalling
  the app reclaims it. The host-side counterpart is
  `scripts/rootfs-run.sh`, which routes through the dev exec broker
  to the same [InstallationMethod.startInside]. See
  [installation.md](installation.md) for the package map, the
  broker `--action install/uninstall` CLI, and the Android 14 FGS
  rationale.

When adding new app features (settings, app launcher, …), put them in
their own packages under `me.phie.tawc.*` rather than mixing them into
the compositor or install packages.


## Home screen

Opening the app lands in the open distro's app list. `MainActivity`
(`singleTask`, `configChanges` for rotation, `adjustResize`) hosts
exactly one pane for the one open distro, plus a FAB. Panes are plain
view controllers, no Fragments; each supplies its own 48dp top row
(`paneTopRowHeightPx`), so there is no toolbar.

| Pane | When | Top row |
|---|---|---|
| Intro | no installs | `[≡] TAWC [⋮]`, logo, blurb, accent Install |
| Info (`DistroInfoView`) | open distro not READY | `[≡] <label> [⋮]`; state row links to the live op log |
| Home (`DistroHome`) | READY | `[≡] <label> [⋮]`, or `[≡][⊞][tabs…][+][⋮]` with terminals (`TerminalTabBar`) over the apps tab (`launcher/AppsPane`) or a terminal (`terminal/TerminalPane`) |

- **Open distro:** `Settings.openDistroId` (pref `open_distro`; the test
  store starts null). Always read through `OpenDistro.resolve` (stored
  id → first READY → first install → null, written back). Written by
  the drawer, `InstallActivity` on Install (a fresh install opens on its
  own progress), and command launches.
- **Tabs:** while terminal tabs exist, ⊞ (apps) is first, then the
  terminal tabs and `+`; otherwise the bar shows the distro label. Selection is in-memory activity state
  (kept across recreation, apps on a cold start, a distro switch or a
  finished install). Non-tawcroot installs show only the label. Tab lifecycle
  and keyboard rules: [terminal.md](terminal.md).
- **FAB:** apps tab of a tawcroot install only; always opens a new
  terminal tab (switching is the bar's job). Hides while the grid
  scrolls down, returns on scroll up.
- **⋮:** one `PopupMenu` per screen, top to bottom: tab items (apps:
  Show hidden (N) when N > 0, Add entry…; terminal: Close all, Keep
  awake), Settings (opened on that distro's card,
  `SettingsActivity.EXTRA_ID`), Run… (READY), Task manager, Distro info.
- **Drawer:** a checkable row per install (` · state` for non-READY,
  ` · N terminals` for live shells, re-read as the drawer opens), each
  with a trailing ⋮ (Settings, Run…, Distro info for *that* install, no
  switch); then Install new distro (no divider). The open distro's row has a
  neutral fill and a left accent strip (`drawable/nav_item_bg`).
  Opening the drawer drops the IME. Drawer and popups use
  `ThemeOverlay.Tawc.Surfaces`.
- **IME / bands:** the drawer root pads system bars + IME. On a home
  the status band continues the dark tab bar; the nav band is
  black under a terminal, else the window colour (`SystemBands`).
- **Back:** closes the drawer, else clears a non-empty apps search,
  else `moveTaskToBack` (intro: default).
- **Intents:** `EntryLauncher` sends the `.CommandLaunch` alias with
  `EXTRA_DISTRO` (+ `EXTRA_COMMAND`, `EXTRA_LABEL`) to open a terminal
  tab, for `Terminal=true` entries and the terminal built-ins (also via
  pinned shortcuts and `ShortcutLaunchActivity`); consumed once
  (`removeExtra`; `savedInstanceState` means restore). The session
  notification is a plain launch.
- **Settings:** the first card, titled with the open distro's label, holds
  its per-install settings (ando toggle, Manage binds; READY/FAILED only,
  else a one-line note), rebuilt in `onResume`. Omitted with no install.
  Every other card is global. `DistroInfoActivity` (same
  `DistroInfoView`) stays reachable from ⋮ for any install and holds
  Delete.
- Open ideas: a permanent drawer on wide screens.

## Audio

Audio forwarding from the rootfs to Android is not implemented yet. The current
plan is a PipeWire-first rootfs stack bridged through app-owned endpoints under
`/usr/share/tawc/`; see [audio.md](../plans/audio.md).
