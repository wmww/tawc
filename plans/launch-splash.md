# Launch splash

Not started. Tapping a GUI launcher entry opens the window's own task
at once, showing the app's icon until its first window maps into that
same task, the way an Android app's splash works. If no window appears
in time, or the program exits without one, the task switches to the
program's output log with a Close button.

Scope: GUI entries through `EntryLauncher.launch` (the apps grid and
pinned shortcuts). Terminal entries and built-ins already open a
terminal tab and are unchanged.

## UX

1. Tap → a `CompositorActivity` task opens immediately: entry icon
   centred on the window background, entry name below it, and
   `setTaskDescription(name, icon)` so the recents card is right from
   the start.
2. The first matching toplevel maps into **this** Activity's host. The
   splash overlay fades out over the first frame. No second task, no
   task switch.
3. Failure → the overlay switches to a log view: a status line, then the
   captured stdout/stderr (monospace, selectable, scrolls to the end),
   then **Close** (`finishAndRemoveTask`).
   - **Non-zero exit, no window**: show the log at once, with "exited
     with code N" as the status line.
   - **Exit 0, no window**: probably a handoff to an already-running
     instance. Wait a grace period (5 s) for a token-matched window
     (below). If nothing arrives, close quietly with a toast ("<App>
     exited without opening a window").
   - **Timeout** (20 s, process still alive): show the log, still live,
     with "<App> hasn't opened a window yet". The reservation stays, so
     a window that maps later still lands here and replaces the log.
4. Closing the task while the splash or log is up (Back, swipe, Close)
   drops the reservation only. The program keeps running, and a window
   it opens later gets a normal new Activity. Killing the program is
   out of scope for v1.

## Launch record (Kotlin)

`EntryLauncher` creates a `PendingLaunch`:
`launchId` (also the host's `ActivityId`, `launch-<random>`), entry
name, icon path, `Terminal` flag, desktop id, activation token, and once
spawned the process's session id. It is held in a small process-wide
`LaunchRegistry` keyed by `launchId`.

- **Spawn**: build the command as
  `export XDG_ACTIVATION_TOKEN=<t> DESKTOP_STARTUP_ID=<t>; <exec>`.
- **Output**: replace the `/dev/null` redirect with `2>&1` into the
  process pipe, read by `MethodRunHelper.collectProcess`. Lines go into
  a capped ring buffer (~1000 lines / 128 KiB) on the record. Keep
  draining for the program's whole lifetime: a full pipe would block
  the program, which is why the redirect exists today. Once the launch
  matches, drop the ring and just discard.
- **Session id**: `startInside` already wraps every launch in
  `/system/bin/setsid`, so every guest process of one launch shares a
  session. Read the sid from `/proc/<proc.pid()>/stat` right after
  start. Check whether toybox `setsid` forks (sid ≠ `proc.pid()`) or
  execs (sid == pid).
- `runInside` returning → `exited(code)`. A spawn failure keeps the
  current `LaunchErrorActivity` path only if the splash Activity isn't
  up yet; otherwise it goes to the splash's log view.

The splash Activity observes its record (a StateFlow: `Waiting`,
`Matched`, `Exited(code)`, `TimedOut`) by `launchId`.

## Splash host (Rust)

Today `DesktopRegistry::choose_host` always mints a fresh host and asks
Kotlin to `spawnActivity`. Add **reserved launch hosts**:

- New JNI `nativeReserveLaunchHost(launchId, sid, token, desktopId)` and
  `nativeReleaseLaunchHost(launchId)`. These go over a calloop channel
  like the other surface events. They are queued if the compositor
  isn't running yet. The launch already calls
  `CompositorService.ensureActivation`, and the splash can call
  `ensureRunning` since the app is about to connect anyway.
- `TawcState.pending_launches: Vec<PendingLaunch { host, sid, token,
  desktop_id }>`. The host enters `hosts` when the splash Activity
  registers its surface, the same as any host.
- While a pending launch exists, `check_idle` must not stop the
  compositor. Releasing the launch, or its host being destroyed,
  removes the pin.
- The cleanup pass's "host has no toplevels → `finishActivity`" rule
  must skip reserved hosts. They have never had a toplevel.
- With no toplevels assigned, the host renders nothing. Clear to the
  splash background colour, or keep the `SurfaceView` hidden behind the
  overlay until `Matched`. The overlay is a normal Android view, so the
  compositor never draws it.

### Matching a toplevel to a launch

On a match the toplevel is assigned to the reserved host with
`spawn_activity: false`. The launch becomes matched and is removed from
`pending_launches`; a reverse-JNI `onLaunchMatched(launchId)` tells
Kotlin. Child toplevels (dialogs, transients) keep riding on their
parent as today. Only root toplevels match.

Match points, in order of confidence:

1. **Session id at `new_toplevel`** (Wayland). Get the client PID from
   `client.get_credentials()` (same as `clipboard.rs`), read field 6
   (session) of its `/proc/<pid>/stat`, and compare it to the pending
   sids. This is synchronous, so the toplevel never gets a throwaway
   Activity. It covers wrapper scripts and forking launchers. It misses
   apps that call `setsid` themselves, which is rare.
2. **X11** (`map_window_request` / `assign_host_for_x11`): read the
   window's `_NET_WM_PID` with Smithay's `X11Surface::pid()`, then do
   the same sid check. The Wayland client here is Xwayland, so
   credentials don't help. A client that doesn't set `_NET_WM_PID`
   falls to 4.
3. **xdg-activation token**. Implement `xdg_activation_v1` (Smithay
   `XdgActivationState`) and register the launch token as an external
   token at reserve time. `activate(token, surface)` on a root toplevel
   matches even across processes. This is the handoff case: a second
   launch of an app whose running instance opens the window. The
   window may already have its own Activity by then. If so, reassign
   it to the reserved host and `finishActivity` the old one. A flicker
   is accepted here because this path is rare. Also use `activate` for
   its normal meaning: bring an existing host to the front.
4. **app_id / WM_CLASS == desktop id** (the `resolve_metadata_for_app_id`
   matching). This is the fallback when the sid is unavailable (debug
   methods, unreadable `/proc`). It runs on `set_app_id` / class set,
   and reassigns like 3 if the window already has its own Activity.
   Matches only a launch whose sid is unknown or which has exited, so
   two windows of an already-running app don't steal a fresh launch.

Unmatched new toplevels follow today's policy unchanged. Single-activity
mode skips the splash entirely, because launches never reserve a host.

The initial configure is already deferred until a host has a size. The
reserved host usually has one before the client connects, so matched
toplevels configure at once instead of waiting for an Activity to
spawn. The first window should show up faster than today.

## Kotlin Activity changes

- `CompositorActivity` gains a launch mode: `tawc://activity/<launchId>`
  plus a `launchId` extra. `EntryLauncher` starts it (with the usual
  new-document flags) instead of waiting for `spawnActivity`.
  `ShortcutLaunchActivity` gets this for free.
- Overlay view: icon (`IconLoader.decode`, falling back to the same
  `ic_app_fallback` / `ic_terminal_fallback` the grid uses), name, and
  the log panel. Reuse `OperationLogPanel` if its shape fits;
  otherwise use a plain `TextView` in a `ScrollView`.
- `onLaunchMatched` hides the overlay after the host's first rendered
  frame. Hiding it on match alone could flash the empty surface. Add a
  one-shot "first frame presented" callback for the host if none
  exists.
- `onDestroy` before a match → `nativeReleaseLaunchHost` plus a
  registry cleanup.
- Timers (timeout, exit grace) live in the registry, not the Activity,
  so a configuration change doesn't reset them.
- Recents: task description set from the entry up front. The existing
  window-metadata updates overwrite it on match, as today.

## Edge cases

- **Two launches of the same app**: distinct sids and tokens, so each
  splash gets its own window. Handoff launches (single-instance apps)
  match by token.
- **Splash closed, then the window appears**: the reservation is gone,
  so the window gets a normal new Activity.
- **App opens a window and keeps it unmapped** (some tray apps): this
  looks like a timeout and is handled as one.
- **Background launch**: the splash Activity is in front while the
  window maps, so `spawnActivity` isn't needed, and the background
  activity-launch block (issues/windows-cannot-open-with-no-tawc-activity-visible.md)
  doesn't apply to matched windows.
- **proot/chroot (debug only)**: best effort. PIDs are real, so the sid
  check may work; otherwise app_id matching applies.

## Tests

Integration (`tests/integration/tests/launcher.rs` or a new
`launch_splash.rs`), driven by a new debug broker action
`launcher-launch` (install id + desktop id, the same path as a tap) and
by `query-state` reporting pending launches and per-host toplevel
counts. Fixture `.desktop` entries in the managed dir:

- Wayland client mapping after a delay: one task, its toplevel on the
  launch host, no extra `CompositorActivity`.
- The same client behind a `sh -c 'client & wait'` wrapper: still
  matches by session.
- X11 client: matches by `_NET_WM_PID`.
- `sh -c 'echo boom >&2; exit 3'`: log state with "boom" and code 3.
- `sleep 999` with a short test timeout (settable through the broker):
  timed-out state, then a window opened later by the same session still
  lands on the host.
- Token handoff: a fixture that hands the token to an already-running
  client process, which calls `activate`.
- Swipe the splash (`am stack remove-task`): reservation released, and
  the compositor can idle-stop again.

Unit: match-order logic in Rust (pure function over pending launches +
client facts), and the Kotlin state machine for the record (exit /
grace / timeout transitions).

Verify on `.tawctarget` with Firefox (slow first launch under
libhybris), a GTK app, a Qt app and an X11 app.

## Phases

1. Output capture, launch registry and the splash/log UI, using a
   Kotlin-only splash and today's separate window Activity. Lands the
   UX and the failure paths early and on its own.
2. Reserved hosts + session matching (Wayland and X11). The window maps
   into the splash task. Idle-stop and cleanup exemptions.
3. xdg-activation: token registration, activate-based matching with
   reassignment, and activate-to-front.
4. app_id fallback, timeout tuning, docs (notes/launcher.md,
   notes/multi-activity.md).

## Open questions

- Does `CompositorActivity` work today if it starts before the
  compositor is running, e.g. opened from recents after a reclaim?
  Whatever handles that case should handle the splash too.
- Can the first-frame signal reuse an existing per-host render counter
  from `nativeQueryState`, or does it need a reverse-JNI callback?
- Should Close also offer "Stop app" (kill the session) on the timeout
  screen? Left out of v1.
