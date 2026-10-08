# Multi-Activity Window Manager

Each Wayland toplevel that the chroot user sees becomes its own Android
task — one card per window in the system app switcher, the way Chrome's
"merge tabs and apps" mode shows each tab.

Status: phases 0-7 shipped (2026-04-26). Polish (task labels, icons,
refused-close handling, settings UI) is the remaining work. The design
intent is preserved below; the "As built" section captures where the
implementation diverged.

## Goal

Make each window the chroot user sees show up as a separate task in the Android
recents list, the way Chrome's "merge tabs and apps" mode shows each tab as a
separate task. Tapping a card brings that window forward; swiping it away
closes that window.

## Non-goals (for the first cut)

- Freeform / floating / split-screen behaviour. We render one window per task
  fullscreen for now. Freeform is left to a later pass once the assignment
  layer exists.
- Per-Activity DPI / scale / size differences. All Activities will use the
  primary display's geometry until a real reason to diverge appears.
- Z-ordering between windows inside the compositor. Android decides which task
  is foreground; we render only the foreground task each frame.
- Perfect lifecycle semantics ("save changes?" prompts, refused close, etc).
  The first implementation does something reasonable and well-defined; we
  refine later.

## Bootstrap: there is no "primary" CompositorActivity

`MainActivity` is the launcher entry. It does not start the compositor
on open, and neither does anything else: the first client connection
does (notes/architecture.md, "Compositor lifecycle"). Once a Wayland client maps a toplevel, every
`CompositorActivity` instance corresponds 1:1 to a real Wayland
toplevel (spawned by the policy via reverse-JNI, killed by
`finishActivity` when the toplevel dies). The recents view shows
`MainActivity` (the launcher card) plus one card per window. There is
no separate "compositor" / "primary" task to confuse the user.

For tests, `run-integration-tests.sh` launches the compositor before
`cargo test` and `compositor::is_running` checks `pidof me.phie.tawc`
AND the chroot-side socket symlink, so a leftover socket file from a
prior `am force-stop` doesn't get mistaken for a live compositor.

## Why not just keep the compositor in an Activity?

Today the compositor thread is started by `CompositorActivity.surfaceCreated`
and would die with the Activity. That breaks the moment we start spawning
Activities per window: the policy says "new toplevel → spawn Activity," so
the compositor must already be running (Wayland socket open, ready to accept
the chroot client's connection) before the first Activity exists, and must
outlive any single Activity dying.

(Historical design. Today the service is bound only, not foreground and
not started; the foreground service is `SessionService` —
[session-service.md](session-service.md).)

Resolution: introduce `CompositorService` (an Android foreground Service) that:

- Runs the Rust compositor thread (`nativeStartCompositor()`) on
  `onCreate`. Wayland socket is created here, decoupled from any Activity's
  lifetime.
- Holds the Activity registry used by activity-scoped JNI reverse-call
  hooks (`HashMap<ActivityId, WeakReference<CompositorActivity>>`).
- Posts a sticky notification (foreground-service requirement on
  Android 8+).
- Started by `MainActivity` (or by the first `CompositorActivity` if
  launched directly from recents after a system reclaim) via
  `startForegroundService` + `bindService`.

`CompositorActivity` instances bind to the Service to register their
`activityId`/`Surface` and forward events. The Service is the single owner
of the activity registry on the Kotlin side.

The "compositor thread" / native side does not change shape — it just no
longer terminates when a CompositorActivity is destroyed.
`nativeOnSurfaceCreated`/`Destroyed` are replaced by per-activity variants
(see below).

This is phase 0 of the implementation plan.

## Architectural shape

There is exactly one Rust compositor process, and it owns both the
toplevel-to-Activity assignment table **and** the policy that decides what
goes where. Kotlin is purely the Android-side mechanism: it spawns Activities
when told and forwards surface/input events tagged by `activityId`. There
is no assignment table on the Kotlin side.

This split is deliberate:

- The information the policy needs (parent toplevel, `app_id`, `title`,
  alive/dead, currently-foreground host) is all Wayland state that already
  lives in `TawcState`. Round-tripping it to Kotlin would just shadow it.
- "Reverting to single-Activity" becomes one branch in the Rust policy
  function, not a Kotlin refactor.
- Future policy refinements ("group toplevels of the same `app_id` into one
  Activity," "split this dialog out into its own card") are pure Rust
  changes.

### Rust: `OutputHost` per Activity, plus an assignment table

Pre-refactor, `RenderState` held a single `egl_surface` and the calloop
data wrapper held a single `output_size`. Both move into a per-host record:

```rust
struct OutputHost {
    activity_id: ActivityId,
    egl_surface: Option<EGLSurface>,    // None until Activity registers its Surface
    native_window: *mut c_void,
    physical_size: Size<i32, Physical>,
    logical_size: (i32, i32),
    foreground: bool,                   // last reported by Android focus event
}
```

`RenderState` keeps the renderer / EGL context / shaders (those are
GL-context-wide and shared across hosts). `TawcState` gains:

```rust
hosts: HashMap<ActivityId, OutputHost>,
desktop: DesktopRegistry,
```

The desktop registry owns surface -> host assignment and persistent per-host
`Space<Window>` projections. The render loop renders only the foreground host's
projection. Background hosts retain their window assignments but have no output
mapped into their space, receive no frame callbacks, and do not trigger hidden
texture imports.

### Rust: the policy lives in `XdgShellHandler::new_toplevel`

(As built, a reserved launch host is checked before this; see "Reserved
launch hosts" below.)

```text
on new_toplevel(t):
    if t has parent p and desktop.assigned_host(p) is alive:
        desktop.assign(t, desktop.assigned_host(p))    # ride on parent's host
    else:
        new_id = fresh ActivityId
        hosts.insert(new_id, OutputHost::pending(new_id))
        desktop.assign(t, new_id)
        spawn_activity(new_id)                          # reverse-JNI to Kotlin
    configure t with hosts[desktop.assigned_host(t)].logical_size
```

Single-Activity mode is the same function with a one-line change: always
return the first host's id. Keeping that escape hatch is a hard requirement
of this design.

### Kotlin: dumb mechanism only

Two reverse-JNI commands suffice from Rust → Kotlin:

- `spawnActivity(activityId: String)` — `startActivity(Intent(this,
  CompositorActivity::class).apply { data = Uri.parse("tawc://$activityId");
  flags = FLAG_ACTIVITY_NEW_DOCUMENT or FLAG_ACTIVITY_MULTIPLE_TASK })`.
- `finishActivity(activityId: String)` — look up the running Activity by id
  and call `finishAndRemoveTask`.

Optional polish reverse-JNI (later phases):

- `setTaskDescription(activityId, title, appId)` — for the recents card label.

Kotlin → Rust JNI tags every event with the `activityId`:

- `nativeRegisterActivitySurface(activityId, surface, w, h)` from
  `surfaceCreated`.
- `nativeOnActivitySurfaceChanged(activityId, w, h)`.
- `nativeOnActivitySurfaceDestroyed(activityId)`.
- `nativeOnActivityDestroyed(activityId)` from `onDestroy`.
- `nativeOnActivityFocusChanged(activityId, hasFocus)` from
  `onWindowFocusChanged`.
- Touch / IME events gain an `activityId` parameter.

A small Kotlin-side singleton (lives in `CompositorService`) tracks
`activityId → CompositorActivity` references (weak refs, so
backgrounded/destroyed Activities clean themselves up). That's the entire
Kotlin-side state.

### Concrete types

- `ActivityId` is a UUID string. Generated by Rust when the policy decides
  to spawn a new host (`uuid::Uuid::new_v4().to_string()`); passed to
  Kotlin in `spawnActivity` and stored in the Intent's `data` URI as
  `tawc://activity/<uuid>`. Kotlin reads it back in `onCreate` from
  `intent.data?.lastPathSegment`. JNI-friendly (`jstring`); resilient to
  Activity recreation across config changes.
- `OutputHost` keys: `HashMap<ActivityId, OutputHost>` in `TawcState`.
- `DesktopRegistry`: owns the shared surface -> host assignment map for
  Wayland and X11 root surfaces.
- Subsurfaces and popups are not assigned directly. The renderer derives
  their host from their root surface's assignment, exactly the way it
  derives their position from the root today.

### Threading

- Reverse-JNI calls from Rust (`spawnActivity`, `finishActivity`,
  `setTaskDescription`) cross from the compositor thread to Kotlin. They
  must run on the main thread (Activity APIs require it). Reuse the
  existing pattern in `NativeBridge.onShowKeyboard`:
  `mainHandler.post { … }`.
- Kotlin → Rust JNI calls (surface lifecycle, input, focus) come off
  Android's main thread. They should not block; use the existing calloop
  channel pattern (one channel per event kind, `OnceLock<Sender>` so JNI
  can post). `nativeOnActivityDestroyed` etc. become channel sends; the
  compositor processes them on its own thread.
- Reverse-JNI fire-and-forget for `spawnActivity` is fine — the host record
  is already in the table before Rust posts the spawn message; the
  Activity registering its surface later just transitions the host from
  `pending` to ready.

### `CompositorActivity` (per-window, was per-process)

What used to be the only Activity becomes one instance per task. Each instance:

- Holds a `SurfaceView` exactly like today.
- Reads its `activityId` from `intent.data` (a `tawc://<uuid>` URI set by
  `spawnActivity`).
- On `surfaceCreated/Changed/Destroyed`, forwards the event to native tagged
  with its `activityId`.
- Forwards touch / IME / focus events tagged with `activityId`.
- On `onDestroy`, calls `nativeOnActivityDestroyed(activityId)`.

`MainActivity` stays the launcher and is the only Activity in
`category.LAUNCHER`. It can offer a "show all windows" UI later.

### Race: toplevel created before its host's Surface is ready

`spawnActivity` returns immediately; the new Activity's
`SurfaceView.surfaceCreated` callback fires asynchronously some
milliseconds later. Meanwhile the Wayland client may already be sending
buffer commits for the new toplevel.

Handling: a host enters the table in a `pending` state with
`egl_surface = None`. The render loop simply skips hosts without a
surface. Buffer imports still happen (those are GL-global), so when the
Activity finally registers its `Surface`, the first frame can render
without re-importing.

The *initial configure* is deferred the same way: `new_toplevel` sends
it only if the assigned host already has a size, else
`reconfigure_all_toplevels` sends it on "Host registered" (~50–500 ms
later). This avoids configure(0,0) and size guesses, but breaks clients
that attach a buffer after exactly one roundtrip past the initial
commit — wl-clipboard's popup-surface fallback died with
`xdg_surface error 3: must ack the initial configure` this way (it now
uses data-control; see clipboard.md). If a second such client turns up:
send a provisional configure with the last registered host's logical
size (kept in `TawcState` across host teardown) when the assigned host
has none yet, and let the real registration correct it. Cost: one
extra resize in split-screen/freeform, where sizes differ.

## wl_output and toplevel sizing

A single `wl_output` global, created in `TawcState::new` and alive for the
whole compositor lifetime. Its mode starts as the Android panel size
(`Display.getRealMetrics`, read in `CompositorService` and passed through
`nativeStartCompositor`) and switches to the backing host's physical size
on the first Activity surface registration. `TawcState::set_output_mode`
is the only writer. Toplevels still see "the screen." This is a
deliberate simplification:

- It avoids touching xdg-output / per-output enter/leave plumbing now.
- Per-Activity sizing is handled at the xdg-toplevel.configure level: when
  a toplevel is assigned to a host, we send it that host's logical size.
- Multi-display proper, fractional scale, and freeform sizes remain
  natural extensions: they'd add per-host outputs without restructuring
  the assignment layer.

Why advertise before any host exists: zero outputs is a state very little
client code handles. `SDL_VideoInit` fails outright ("The video driver did
not add any displays"), taking down every SDL app started against a cold
compositor, and Xwayland has no root-window geometry. The provisional mode
overestimates the eventual Activity surface (panel 1080x2400 vs Activity
1080x2169 on the standing target); that costs one ordinary mode change,
which clients already handle for rotation and scale changes. Guarded by
`tests/integration/tests/cold_start.rs`.

Configure-event sequencing:

- A new toplevel gets **no** configure until its host has a real logical
  size (`configure_toplevel_for_host` returns `None` before that). The
  provisional output mode is deliberately not used for sizing — nothing
  is configured off a service-side guess.
- When the host's `surfaceChanged` arrives with a different physical size
  (e.g. user is in split-screen on a tablet), the toplevel gets a fresh
  configure with the new logical size.
- When a toplevel is reassigned to a different host (parent dies, dialog
  becomes its own window), it gets a fresh configure with the new host's
  size.

The single-output choice is reversible. Once we want freeform windowing or
external displays, we move to one wl_output per OutputHost.

## Input / focus / IME

- Touch from a SurfaceView is delivered already tagged with the
  `activityId`. Coordinates are in that SurfaceView's pixel space (which
  matches the host's physical size 1:1 thanks to immersive fullscreen).
  `event_loop.rs` divides by `output_scale` to get logical coords; this
  stays the same — `output_scale` is global. The compositor maps the touch
  to the first alive toplevel whose root surface is assigned to that
  `activityId` in `DesktopRegistry`.
- Keyboard focus follows whichever Activity Android currently shows. When an
  Activity gains focus (`onWindowFocusChanged(true)`), the compositor sets
  keyboard focus to that host's foreground toplevel and marks the host
  `foreground = true`; `onWindowFocusChanged(false)` clears it. The
  compositor only ever has one focused host at a time. This naturally
  fixes the "touch focus stuck on the single foreground window" gap that
  existed before the multi-Activity rework.
- IME show/hide (`onShowKeyboard` / `onHideKeyboard` reverse JNI) needs to
  target the focused Activity's view, not a global `inputViewRef`. The
  call signature gains an `activityId`; the Service-side singleton looks
  up the matching `CompositorActivity` (weak ref) and calls
  `showSoftInput`/`hideSoftInputFromWindow` on its `SurfaceView`. The Rust
  side tracks the current keyboard target as `Option<ActivityId>` so moving
  focus between Android tasks emits a retarget even if text input remains
  enabled. Kotlin keeps the late-view replay sticky per Activity.
- text-input-v3 state stays single-instance for now: only one toplevel can
  have IME focus at a time, and that's the foreground host's foreground
  toplevel. Per-host text-input state is a possible future refinement but
  isn't needed to make windowing work.

## Backgrounded hosts: tell the client to stop rendering

When a host is not in the foreground (Android `onWindowFocusChanged(false)`,
or `surfaceDestroyed` while Activity still alive but stopped, or
`pending`-but-not-yet-bound), the compositor should tell every toplevel
assigned to that host that it's been minimized so the client can stop
producing frames. Continuing to feed frame callbacks to a backgrounded
client wastes the chroot CPU/GPU and our import cycles for buffers we'll
never display.

The mechanism is a per-host `Foreground`/`Background` state that the
compositor mirrors into Wayland configure events:

- **Foreground host** (the one Android is currently showing): toplevels are
  configured with `Activated` set, `Suspended` cleared. Frame callbacks
  fire normally. Buffer imports run.
- **Background host** (Activity exists but Android isn't showing it):
  toplevels are configured with `Activated` cleared and `Suspended` set
  (xdg-shell v6 — already advertised; Smithay's `ToplevelSurface` sends
  `Suspended` only to clients with `version >= 6`, otherwise just clears
  `Activated`). Frame callbacks are **not** sent. Buffer imports are
  skipped for that host's toplevels (no point: we won't render them, and
  the client shouldn't be producing fresh buffers anyway). `wl_buffer`
  release on attach still happens through smithay's `merge_into` — we
  don't need to do anything special.
- **Pending host** (Activity spawned, surface not yet registered): same
  as Background. The client paints its first frame, we hold it; when the
  surface arrives we flip to Foreground and the held buffer renders
  immediately.

State transitions:

- `onWindowFocusChanged(activityId, true)` → host → Foreground; send
  `Activated` configure to its toplevels; resume frame callbacks.
- `onWindowFocusChanged(activityId, false)` → host → Background; send
  `Suspended`/un-`Activated` configure; stop sending frame callbacks.
- `nativeOnActivitySurfaceDestroyed(activityId)` while Activity still
  alive (e.g. user backgrounded long enough that the Surface was torn
  down) → host stays Background, `egl_surface = None`. Already-suspended
  configure is in flight; nothing further needed.
- `nativeOnActivityDestroyed(activityId)` → policy fires close on the
  toplevels (see Lifecycle); host record cleaned up by the cleanup pass.

Smithay note: `ToplevelSurface::send_configure()` always sends (it
exists for the initial configure where pending == current is the
expected state). For Activated/Suspended toggles the right call is
`send_pending_configure()`, which is a no-op when the pending state
already matches what was last configured — sending a redundant configure
between a Vulkan client's first and second commit confuses the WSI
swapchain enough that vkcube wedges after two frames. This applies to
`reconfigure_all_toplevels` too: Register and SurfaceChanged often arrive
back-to-back with the same size and the second configure is what trips
the WSI.

Efficiency consequences:

- A backgrounded host costs the compositor only the per-frame state walk
  (no `eglMakeCurrent`, no swap, no buffer import, no callback fire).
- A well-behaved client (anything Smithay-aware: GTK4, recent Firefox,
  Qt6) stops drawing entirely on `Suspended`. Even pre-v6 clients drop
  to "minimized-style" behaviour on cleared `Activated` (GTK3 is
  conservative here).
- An ill-behaved client that keeps drawing despite `Suspended` only
  costs us the buffer commit dispatch (which we can't avoid — the
  client owns the socket); we don't import or render its frames.

## Lifecycle

Picking reasonable defaults — none of these need to be perfect first try, but
they do need to leave room for tightening later.

- **User swipes the task away in recents** → `onDestroy`. Native sees
  `nativeOnActivityDestroyed`. The compositor sends `xdg_toplevel.close` to
  every toplevel assigned to that host. If a client honours close, its
  toplevel dies and the host record is dropped. If a client refuses (e.g.
  shows a "save?" dialog), the toplevel stays alive but currently has no
  host to render into. Initial behaviour: the toplevel goes invisible until
  it dies on its own. We can revisit (re-spawn an Activity? show a dummy
  "this window can't be closed" Activity?) once we see real cases.
- **User backgrounds via home button or switches tasks** →
  `onWindowFocusChanged(false)` first (host → Background, configure with
  `Suspended`/un-`Activated`, stop frame callbacks), then `onPause`/`onStop`
  and eventually `surfaceDestroyed` (host's `egl_surface` cleared). We pause
  rendering for that host but keep the toplevels assigned. When the user
  brings the task back, surface is recreated, focus returns, host →
  Foreground, configure with `Activated`, frame callbacks resume.
- **System reclaims the Activity under memory pressure** → same as
  destroyed-via-recents. The tradeoff is the same: tighter lifecycle
  rules can come later.
- **Client destroys a toplevel** while its Activity is still alive →
  `WindowManager.onToplevelClosed`. If that was the host's last toplevel
  → `finishAndRemoveTask` so the recents card disappears. If a parent
  toplevel dies before its dialog → reassign the dialog to a new
  Activity (dialog without parent becomes its own window).

## Render-loop changes

Original design notes (the loop is now vsync-driven; see
[rendering.md](rendering.md) "Frame clock"):

1. Frame tick fires.
2. Dispatch client messages (global, as today).
3. Import buffers — **only for surfaces whose host is `Foreground`**.
   Backgrounded clients shouldn't be producing frames; if they do, we let
   the buffers sit committed (the dispatch above already accepted them)
   but skip the texture import. When the host returns to Foreground we
   import on the next tick. Pending hosts whose first buffer is already
   in flight: import on Foreground transition.
4. For each `Foreground` `OutputHost` whose `egl_surface` is bound:
   - `eglMakeCurrent(egl_surface)`
   - Draw assigned toplevels (and their popups/subsurfaces).
   - `swap_buffers()`.
5. Send frame callbacks **only for `Foreground` hosts'** surfaces.
   Backgrounded hosts' clients are sitting on `Suspended` configures and
   should not be polling for frame callbacks; sending them would prompt
   wasted client work.
6. Cleanup as today, plus host pruning (see "Cleanup and orphaned hosts").

`needs_render` becomes per-host. `last_rendered_toplevels` is replaced by
per-host counters surfaced in `nativeQueryState`.

## MainActivity role

`MainActivity` is the home screen: one pane for the open distro
(terminal, app list, distro info or intro; notes/android.md "Home
screen"). It never starts the compositor; the first rootfs client's
connection does, and the first toplevel spawns a `CompositorActivity`.

- Stays the only Activity in `category.LAUNCHER`; `singleTask`, so one
  recents card for the app, plus one per window.
- It does NOT host a SurfaceView.

## Single-Activity mode toggle

Stored as a boolean in `SharedPreferences` (key
`tawc.window_mode.single_activity`). Read by `CompositorService` at
startup and pushed to Rust via `nativeSetSingleActivityMode(bool)`. The
Rust policy function checks the flag: if set, return the existing host's
id (creating one on demand for the first toplevel) instead of spawning a
new host. Toggleable from a future settings UI; for v1, it's a flag the
implementer can flip in code.

## Cleanup and orphaned hosts

The frame-timer cleanup pass already prunes dead toplevels. It gains:

- For each host: if its Activity weak-ref is gone AND it has no remaining
  toplevels assigned, drop the host record. Release the `EGLSurface` and
  `ANativeWindow`.
- For each desktop-registry assignment: if the root surface is dead, remove it.
  If the host no longer exists, log + drop the entry (should not happen in
  practice; defensive cleanup).
- Hosts that lost their Activity but still have alive toplevels stay in
  the table (the "client refused close" case). They render to nothing
  (`egl_surface = None`) until the toplevel finally dies.

## Tests

Add to `tests/integration/`:

- `test_two_toplevels_two_activities` — open two GTK4 demo windows from
  the chroot, assert two `CompositorActivity` instances exist (poll
  `dumpsys activity activities | grep CompositorActivity`) and two
  `OutputHost` entries via `nativeQueryState`.
- `test_dialog_uses_parent_host` — open `gtk4-demo` with the Assistant
  dialog (or a known parent+child case), assert one `CompositorActivity`
  and that the dialog renders into it.
- `test_swipe_away_closes_toplevel` — `am stack remove-task` (the closest
  ADB equivalent of swiping away), assert the corresponding toplevel
  receives `xdg_toplevel.close` and dies.
- `test_close_last_toplevel_finishes_activity` — close a toplevel from
  the client side, assert the matching Activity is finished
  (`finishAndRemoveTask` ran).
- `test_backgrounded_client_stops_rendering` — open a GTK4 toplevel,
  background the Activity (`input keyevent KEYCODE_HOME`), assert the
  client receives a configure with `Suspended`/un-`Activated` and that
  the per-toplevel frame counter stops advancing within ~5 frames.
  Bring the Activity back, assert the client resumes drawing.
- `test_single_activity_mode` — set the SharedPreference, restart, open
  two toplevels, assert one `CompositorActivity`.

`nativeQueryState` already logs a `COMPOSITOR_STATE` line; extend it with
`hosts=N pending_hosts=M` so the tests can grep for it.

## Open questions for the implementer

- **Activity launch flags.** Start with
  `Intent.FLAG_ACTIVITY_NEW_DOCUMENT | FLAG_ACTIVITY_MULTIPLE_TASK` plus
  `documentLaunchMode="intoExisting"` and a unique URI per id. If
  Samsung/OEM launchers merge cards or refuse to show separate entries,
  fall back to `FLAG_ACTIVITY_MULTIPLE_TASK` with
  `taskAffinity=""`. Test on the OnePlus 9 (real device) and the AVD.
- **`xdg_toplevel.close` handshake.** v1: send close on Activity destroy
  and don't wait. If we observe ugly UX (toplevels hanging visibly
  after the card is gone), revisit with a "close pending" timeout.
- **Recents card content.** `ActivityManager.TaskDescription` gets a label
  (toplevel `xdg_toplevel.set_title`) and ideally an icon
  (`xdg_toplevel.set_app_id` → look up Android icon from `.desktop` /
  pacman?). Icon lookup is its own subproject; v1 ships with an app
  generic icon and the title.

## Original implementation phases

Each phase is independently testable; nothing assumes a future phase exists.

0. **Move compositor into a foreground `CompositorService`.** The
   `nativeOnSurfaceCreated`/`Destroyed` JNI entry points get renamed to
   per-activity variants but still serve a single Activity; nothing else
   changes yet. Wayland socket lives in the Service. Verify all existing
   integration tests still pass.
1. **Refactor to `OutputHost` with one host.** Compositor still has one
   EGLSurface, one Activity. The point is to land the host abstraction on
   the native side so subsequent phases just add hosts.
   `output_size` and `RenderState.egl_surface` move into
   `TawcState.hosts: HashMap<ActivityId, OutputHost>` (length 1, with a
   hardcoded default `ActivityId` for the initial Activity).
2. **Desktop-registry assignment table.** Single host still, but the
   render loop reads host projections from `DesktopRegistry` instead of
   iterating `state.toplevels` directly. Verifies that the bookkeeping doesn't
   regress single-window behaviour.
3. **Policy + reverse-JNI to spawn Activities (still no-op).** Add the
   `XdgShellHandler::new_toplevel` policy function and
   `spawnActivity`/`finishActivity` reverse-JNI, but keep
   `single_activity_mode = true` so the policy returns the existing host.
   Exercises the round-trip without changing user-visible behaviour.
4. **`CompositorActivity` per-document.** Add `activityId`, switch
   `documentLaunchMode="intoExisting"`, give each Activity its own UUID
   in `intent.data`. Manifest changes, Intent plumbing. Still one window
   in practice.
5. **Flip the policy.** `single_activity_mode = false`. Spawn a new
   Activity per non-child toplevel. Verify: open two GTK apps → two
   recents cards. Open Firefox + a dialog → still one card.
6. **Per-host input/focus.** Touch goes to the host that owns the
   SurfaceView; keyboard focus follows Android focus.
7. **Lifecycle + suspend round-trip.** Swipe away → close. Activity
   destroyed → toplevel close. Activity backgrounded → host → Background,
   `Suspended` configure sent, frame callbacks stop, buffer imports
   skipped. Activity returns from background → host → Foreground,
   `Activated` configure, callbacks and rendering resume.
8. **Polish.** Task labels (`set_title` → `TaskDescription`), icons,
   refused-close handling, single-activity mode toggle in settings UI,
   settle on freeform vs fullscreen story.

Phases 0-3 are pure refactors and should keep all existing integration
tests green. Phase 4 onward needs new tests (multi-toplevel scenarios) per
the Tests section above.

## As built (2026-04-26)

Files of interest:
- `compositor/src/host.rs` — `OutputHost` + `SurfaceEvent` channel
  + `new_activity_id()`. The activity-id minter uses an epoch-nanos /
  counter combo instead of UUIDs to avoid pulling in the `uuid` crate;
  the values are guaranteed unique within and across runs.
- `compositor/src/lib.rs` — `nativeStartCompositor` /
  `nativeRegisterActivitySurface` / `nativeOnActivitySurfaceChanged` /
  `nativeOnActivitySurfaceDestroyed` / `nativeOnActivityDestroyed` /
  `nativeOnActivityFocusChanged` JNI plus `spawn_activity_from_native`
  / `finish_activity_from_native` reverse-JNI.
- `compositor/src/event_loop.rs` — surface-event source, focus
  set/clear via `set_host_foreground`, per-host render loop
  iteration, and `finishActivity` cleanup when a host loses its last
  toplevel.
- `compositor/src/compositor.rs` — `TawcState::hosts`,
  `single_activity_mode`, `assign_toplevel_to_host` policy returning `HostAssignment`,
  per-host configure sizing in `new_toplevel`, and host-scoped accessors over
  Smithay's `XdgShellState::toplevel_surfaces()`.
- `compositor/src/desktop.rs` — `DesktopRegistry` host assignment,
  foreground/visible projection, and persistent per-host `Space<Window>`
  updates.
- `app/src/main/java/me/phie/tawc/compositor/CompositorService.kt`
  — foreground-service shell, posts `specialUse` notification, holds
  the Activity registry.
- `app/src/main/java/me/phie/tawc/compositor/CompositorActivity.kt`
  — reads `activityId` from `intent.data?.lastPathSegment`, binds to
  Service in `onCreate`, forwards surface / touch / focus events
  tagged with the id, gates the test BroadcastReceiver on
  `hasWindowFocus()` so multi-Activity test runs don't fan out.
- `app/src/main/java/me/phie/tawc/compositor/NativeBridge.kt`
  — `attachService` captures application context for `spawnActivity`
  reverse-JNI; `finishActivity` looks up the matching Activity via
  the Service and calls `finishAndRemoveTask`.
- `app/src/main/AndroidManifest.xml` — foreground-service
  permissions (`SPECIAL_USE`), `documentLaunchMode="intoExisting"` +
  `taskAffinity=""` on `CompositorActivity`, `tawc://activity` intent
  filter.

### Channel-creation ordering (Phase 0/1 race)

`SurfaceHolder.surfaceCreated` fires within milliseconds of
`bindService`; if the compositor thread hadn't yet installed the
surface-event channel sender, that first registration would be
silently dropped and the Activity would render nothing.

Resolution: `nativeStartCompositor` creates ALL calloop channel pairs
(touch, text-input, surface-event, state-query) BEFORE spawning the
compositor thread, then moves the receivers into the closure. Channel
senders go into global `Mutex<Option<…>>` slots; events queue safely
in the channel buffer until the compositor thread plugs the receivers
into its event loop.

### Deferred EGL surface creation

The compositor thread sets up its EGL display + context up front via
`create_raw_egl_context()` (which leaves the context current with
`NO_SURFACE` — Adreno + emulator gfxstream both support
`EGL_KHR_surfaceless_context`). `RenderState::create_egl_surface_for_window`
makes a per-host `EGLSurface` later, when an Activity's
`nativeRegisterActivitySurface` reaches the event-loop handler.
`OutputHost::Drop` releases the `ANativeWindow` reference acquired by
`ANativeWindow_fromSurface`.

### Foreground / suspend semantics

`OutputHost.foreground: bool` is the single source of truth.
`set_host_foreground` walks toplevels assigned to the host and sets /
unsets `XdgState::Activated` + `XdgState::Suspended` in their pending
state, then `send_configure`. Smithay short-circuits no-op configures,
so toggling is cheap and idempotent.

Render loop draws only the foreground host's assigned windows to that host's
EGL surface. The foreground `Space<Window>` drives output enter/leave, frame
callbacks, and input hit-testing; when Android has not reported a foreground
host, there is no visible desktop projection. Frame callbacks are sent through
Smithay `Window::send_frame` for windows mapped in that foreground space.
Backgrounded clients stop producing frames as soon as Smithay-aware
clients (GTK4, recent Firefox, Qt6) honour `Suspended`; legacy clients
see only cleared `Activated` and behave as if minimised. Commits from
backgrounded surfaces can still mark the compositor dirty, but Smithay
only imports textures for windows when the foreground desktop render
path asks for render elements.

### Activity cleanup on last toplevel

The frame-timer cleanup pass collects each dead toplevel along with
its `(_, Option<ActivityId>)`; once the dead-toplevel removal is done
it walks the unique host IDs and, for any host with no remaining
assigned toplevels, calls `finish_activity_from_native(host_id)`.
Every `CompositorActivity` exists for exactly one Wayland window — when
the window goes away the recents card disappears with it.

### Reserved launch hosts (launch splash)

A launcher tap starts its `CompositorActivity` before any window
exists, under a `launch-<random>` id the compositor reserves
(`TawcState.pending_launches`, `launch.rs`). The policy checks it
first: a new root toplevel (or mapping X11 root window) whose process
is in the launch's session is assigned to that host with
`spawn_activity: false`; an xdg-activation `activate` with the launch's
token, or a late app_id match, moves an already-hosted window there and
finishes its old Activity. Reserved hosts pin the compositor against
idle stop and are never finished for having no toplevels; the splash
Activity releases them on destroy. Details: [launcher.md](launcher.md)
"Launch splash".

### Test action focused-activity gate

Test input arrives via dev exec broker `ic-*` actions
(`ic-commit-text`, `ic-set-composing-text`, `ic-send-key-event`, …),
each of which drives the active `TawcInputConnection` like the system
IMM would. The handlers in `me.phie.tawc.dev.InputActions` resolve a
single foreground `CompositorActivity` via
`CompositorService.focusedActivity()` (walking the
`activities: Map<String, WeakReference<…>>` with the `hasWindowFocus()`
predicate). One handler call ⇒ one IC method call ⇒ no per-activity
duplication regardless of how many windows the test left open.
`query-state` doesn't need a focused activity (it lives on the service)
so it works before any client window is up.

## What this design preserves

- **Reverting to single-window is one assignment-policy line.** The
  WindowManager always returning the same Activity ID degrades to today's
  behaviour with no native-side changes.
- **Output count is decoupled from Activity count.** One wl_output today,
  many wl_outputs later, both fit.
- **Toplevel-to-window relationship is N:1, not 1:1.** Dialogs (parent
  toplevels) ride on their parent's Activity. Future "tab groups" or
  application-level grouping can use the same mechanism.
- **No Wayland-visible coupling to Android tasks.** Clients see "a screen"
  and configure events; they never observe Activity lifecycle.
