# Input

## Touch Input

Touch events flow: Android `onTouchEvent` -> JNI `nativeOnTouchEvent` -> `calloop::channel`
-> Smithay `TouchHandle` -> `wl_touch` protocol events to client.

**Architecture:**
- `compositor/CompositorActivity.kt` sets an `OnTouchListener` on the SurfaceView. It
  dispatches DOWN, MOVE, UP, POINTER_DOWN, POINTER_UP, and CANCEL events per-pointer via JNI.
- `input.rs` holds a global `Mutex<Option<channel::Sender<TouchEvent>>>` so the JNI
  thread can send events to the compositor thread without shared mutable state.
  Uses Mutex instead of OnceLock so the channel can be replaced on compositor restart.
- `event_loop.rs` has a calloop channel source that converts `TouchEvent` into Smithay
  `DownEvent`/`MotionEvent`/`UpEvent` and calls `touch.down()`/`.motion()`/`.up()` +
  `.frame()`. Events are flushed immediately to minimize latency.
- Coordinates arrive in physical pixels from Android and are divided by the current
  output scale to get logical Wayland coordinates.
- Touch-down hit-tests the host's windows top-down through each window's
  placement (`surface_at`; see [rendering.md](rendering.md) "Window
  Management"): the screen point is mapped into the window's frame, and
  smithay gets that frame's location and surface origin. The scrim under a
  dialog swallows a point that misses the dialog; nothing below it is hit.
  Each touch slot remembers the window it went down on (`touch_frames`), and
  its motion is mapped through *that* window's placement, so a drag that
  leaves a scaled or centered window keeps consistent coordinates.
  Within a window, toplevel, subsurface, and popup trees are hit in draw
  order. A surface with an explicit `wl_surface.set_input_region` only receives
  the touch if the local point is inside that region; `NULL` input region keeps
  the Wayland default of the whole surface. This matters for Firefox/WebRender:
  visible render-only child surfaces may use an empty input region, so the touch
  must fall through to the browser toplevel.
- Multi-touch is supported: each Android pointer ID maps to a Smithay `TouchSlot`.
- The seat advertises only real input capabilities. Keyboard capability is
  required for Firefox to enable text input (see text-input.md).
  `wl_pointer` has [its own section](#pointer-input) below. Do not turn touch
  into pointer events: touchscreen input stays on `wl_touch`.
- Touch-down moves both keyboard focus AND text-input-v3 focus to the target's
  keyboard-focusable surface via `TawcState::set_input_focus` — they are
  conceptually one focus and splitting them invites drift. `wl_subsurface`
  targets focus their main surface because the core protocol forbids keyboard
  focus on subsurfaces; non-grabbed `xdg_popup` touches leave keyboard focus
  unchanged. Touch does not commit or finish text input preedit. If the client
  moves its cursor, its next
  `set_surrounding_text(cause=other)` drives reactive preedit cleanup.

**GTK3 touch handling note:** GTK3 handles `wl_touch` events natively — GtkGestureMultiPress
processes `GDK_TOUCH_BEGIN` directly, and GDK's Wayland backend sets `emulating_pointer=TRUE`
on the primary touch which synthesizes crossing events for child widget routing. The GTK3
menubar workaround primes that cold crossing state with one synthetic pointer
enter/leave per new toplevel; it does not convert touches into pointer clicks. When debugging
touch, check coordinates carefully — the GTK widget tree only routes events to children whose
GdkWindow allocation contains the hit point.

## Pointer Input

Real mouse input flows: Android mouse-source `MotionEvent` ->
`nativeOnPointerEvent` -> `calloop::channel` -> Smithay `PointerHandle` ->
`wl_pointer`. Touch and pointer are separate paths end to end.

**The source split.** Android launders mouse buttons through the touch path:
a click arrives at `OnTouchListener` as `ACTION_DOWN`/`MOVE`/`UP`. So
`CompositorActivity.dispatchTouchToCompositor` splits on the event source
before doing anything else:

- `SOURCE_TOUCHSCREEN` and `SOURCE_STYLUS` -> `wl_touch`. Stylus stays on
  touch; a real `zwp_tablet_v2` path is out of scope.
- `SOURCE_MOUSE` (which is also what Android reports for a touchpad driving a
  cursor) -> `wl_pointer`.
- Anything else (rotary encoders, gamepads) is ignored.

Without the split a single click delivers both a `wl_touch.down` and a
`wl_pointer.button` and clients double-handle it.

Wheel (`ACTION_SCROLL`) and hover (`ACTION_HOVER_*`) never reach the touch
listener at all; `TawcSurfaceView` overrides `onGenericMotionEvent` and
`onHoverEvent` for them. The hover hook is explicit rather than relying on
Android's fall-through to generic motion.

**Buttons.** Android reports a button *bitmask*, not per-button actions, so
presses and releases come from diffing `MotionEvent.buttonState` against the
previous value; `ACTION_BUTTON_PRESS`/`RELEASE` are ignored because the diff
already covers them. `BUTTON_PRIMARY`/`SECONDARY`/`TERTIARY`/`BACK`/`FORWARD`
map to evdev `BTN_LEFT` 0x110, `BTN_RIGHT` 0x111, `BTN_MIDDLE` 0x112,
`BTN_SIDE` 0x113, `BTN_EXTRA` 0x114. A mouse side button also raises
`KEYCODE_BACK`/`KEYCODE_FORWARD`; `TawcSurfaceView` swallows those so the
click does not *also* run the Android Back policy below.

**Scroll units and direction.** Android `AXIS_VSCROLL` is positive scrolling
away from the user, Wayland's vertical axis is positive downward, so vertical
is negated; `AXIS_HSCROLL` is already positive-right and is not. One detent is
`AXIS_VSCROLL == 1.0` -> `axis_value120` 120, and 15 logical pixels in the
legacy `axis` value (libinput's wheel-click angle, which is what desktop
clients are tuned against — deliberately *not*
`ViewConfiguration.getScaledVerticalScrollFactor()`, which is density-scaled
physical pixels and would make scroll distance depend on the phone). Smithay
sends `axis_value120` to `wl_pointer` v8+ clients and accumulates
`axis_discrete` for older ones itself. No "natural scrolling" inversion is
applied — that is the client's business. A touchpad (the `InputDevice` also
reports `SOURCE_TOUCHPAD`) uses `axis_source = finger` and gets an `axis_stop`
frame after a short idle gap, which is what makes GTK's kinetic scrolling
settle.

**Focus.** Pointer motion and touch share one hit test (`surface_at` in
`event_loop.rs`), including the visible-host guard and the
`set_input_region`-honouring `WindowSurfaceType::ALL` lookup. Motion does
**not** move keyboard focus — hover is not activation. A button *press* takes
the same path as touch-down: it dismisses a menu it lands outside of and moves
keyboard/text-input focus. Smithay's default grab keeps pointer focus while a
button is held, and `PopupPointerGrab` is installed on `xdg_popup` grab.
Pointer motion is mapped into `pointer_frame`'s placement: the hovered window,
pinned while smithay reports a grab; a hit on another window during a grab is
delivered as no focus. `pointer_location` is in that window frame;
`pointer_screen_location` is the screen position, which `query-state` reports
as `pointer_x`/`pointer_y`. Scroll is not scaled.

Keyboard focus after a window change goes to the topmost focusable window on
the visible host (`first_toplevel_for_host`), so a dialog takes focus over its
parent and focus returns to the parent when it closes. Back sends Escape to the
focused dialog, which closes most.

**Crossing.** Android synthesizes `ACTION_HOVER_EXIT` before every mouse
`ACTION_DOWN` and `ACTION_HOVER_ENTER` after the matching `ACTION_UP`. Those
are *not* mapped to `wl_pointer.leave`/`enter` — doing so wraps every click in
a crossing pair, which closes GTK menus and breaks drags. Real leaves are sent
by the compositor on Activity focus loss, surface destroy, and host switch
(`clear_pointer_focus`).

**Seat capability.** `wl_pointer` has exactly one owner,
`TawcState::sync_pointer_capability`, with two independent reasons: attached
mouse hardware and the [GTK3 broken menus
workaround](gtk3-broken-menus-workaround.md). Neither may touch the seat
directly — smithay's `Seat::add_pointer` on a seat that already has a pointer
*replaces* the `PointerHandle`, dropping focus and any live grab. The owner
acts only on the 0<->1 transition. Mouse presence comes from Android:
`MouseWatcher` enumerates `InputDevice`s, filters to `SOURCE_MOUSE`, and
follows `InputManager.InputDeviceListener`, sending the aggregate over the
surface-event channel so it is ordered with focus changes like hardware keys.

With the workaround at its default the capability is on regardless, so the
owner only changes observable behaviour for users who disabled it. Capability
*removal* on unplug is legal but rare in the wild; if real clients turn out to
mishandle it, the fallback is to make the capability sticky for the session
once a mouse has ever been seen.

## Cursor

Android draws the pointer sprite itself, above the app's surfaces, so TAWC
does not render a second cursor into the Wayland scene. `SeatHandler::
cursor_image` maps the client's request onto the Activity SurfaceView's
`PointerIcon` (`compositor/src/cursor.rs` -> `NativeBridge.setPointerIcon` /
`setPointerIconBitmap`, both posted to the UI thread because
`View.setPointerIcon` must run there).

- `wp_cursor_shape_v1` is advertised, so the common case is a named shape.
  Rust sends the CSS/cursor-shape name; Kotlin owns the name ->
  `PointerIcon.TYPE_*` table. Android has no sprite for a few shapes
  (`dnd-*`, single-direction resizes), so those collapse onto the nearest one.
- Hidden -> `TYPE_NULL`.
- Legacy `wl_pointer.set_cursor` with a surface (Xwayland is the main user) is
  read out of its SHM buffer and becomes a `PointerIcon.create` bitmap, with
  the hotspot smithay recorded. Re-read when that surface commits again, so
  animated cursors follow. Non-SHM cursor buffers (wlegl/AHB) are not worth
  importing — they fall back to the arrow.

The broker `query-state` action reports the live pointer and cursor —
`pointer_present`, `pointer_x`, `pointer_y`, `pointer_focus`, `cursor_shape`
— because motion, axis and cursor changes are all far too high-volume to log.
`cursor_shape` is a shape name, `hidden`, `bitmap`, or `none`.

Note a client's `set_cursor` is only honoured with the serial of a live
`wl_pointer.enter` (smithay's `allow_setting_cursor`). The GTK3 workaround's
prime enters and immediately restores, so a shape requested off that prime
alone is dropped; a real mouse holds the enter and it sticks.

## Pointer Work Not Done

Deliberately out of scope so far, in rough order of usefulness:

- `zwp_relative_pointer_v1` + `zwp_pointer_constraints_v1` for games and 3D
  apps. Android's `View.requestPointerCapture()` is the matching side;
  locked-pointer without capture would let the system cursor drift out of the
  window.
- Pointer-initiated `wl_data_device` drag-and-drop. `clipboard.rs` handles
  selections only.
- `pointer_gestures` (pinch/swipe) from Android touchpad gestures.
- Xwayland move/resize grabs, still stubbed with an explicit "we don't have an
  X11-aware seat path yet" (`compositor/src/xwayland.rs`). X11 clients get
  pointer events for free through Xwayland's own seat binding; only the
  WM-side grabs need work.
- A real `zwp_tablet_v2` path for stylus input, which stays on `wl_touch`.

Open question carried over from the design: whether removing the seat's
pointer capability on unplug is safe in the wild, or whether it should become
sticky-for-the-session once a mouse has ever been seen. Decide with a real
Bluetooth mouse — the emulator has no mouse `InputDevice` at all, so it cannot
answer it. (Our own test client, `wayland-debug-app`, treats capability
removal as fatal.)

## Simulating Touch via adb

`adb shell input tap X Y` injects touch events through the Android input framework.
Integration tests that need drag or multi-touch use the debug broker
`inject-touch` action instead. It creates normalized `MotionEvent`s against
the focused `SurfaceView`, so tests avoid hard-coded screen coordinates while
still exercising `CompositorActivity`'s MotionEvent-to-JNI dispatch.
Coordinates are in screen pixels (same space as `screencap`). The app uses immersive
fullscreen, so screen coordinates map 1:1 to SurfaceView coordinates. The compositor
divides by the current output scale to get Wayland logical coordinates.

**Debug loop:**
1. Screenshot: `adb shell screencap -p /data/local/tmp/tawc-dev/screenshot.png && adb pull /data/local/tmp/tawc-dev/screenshot.png /tmp/screenshot.png`
2. Identify target element's pixel coordinates in the screenshot
3. Tap: `adb shell input tap X Y`
4. Screenshot again to see result
5. Clean up: `adb shell rm /data/local/tmp/tawc-dev/screenshot.png && rm /tmp/screenshot.png`

Be precise -- after output scaling, UI elements are small in physical pixels. The Firefox tab
close "X" and toolbar hamburger are only ~50-60px apart.

**Keyboard key coordinates:** derive these from the current screenshot. When
the Android OSK is visible, identify the row centers directly from the captured
image.
Each QWERTY key is roughly `screen_width / 10` wide. ASDFGH keys are wider
and offset from QWERTY; ZXCVBN keys start after a wider shift key.

**Firefox UI element positions:** derive these from the current screenshot
and output scale. Do not treat these as stable across devices or scale
settings:
- Tab bar: logical y ≈ 0-20
- URL/address bar: logical y ≈ 40-70
- Content area starts at logical y ≈ 80

Convert logical coordinates to physical tap coordinates with
`physical = logical * current_output_scale`, then verify against the screenshot.

## Hardware Keyboard

Physical USB/Bluetooth/emulator keys enter Android through
focused `SurfaceView.onKeyDown` / `onKeyUp`, not through `TawcInputConnection`.
The view forwards mapped `ACTION_DOWN`/`ACTION_UP` events to Rust via
`nativeOnHardwareKeyEvent(activityId, keycode, pressed, repeatCount)`.
The JNI layer translates Android `KEYCODE_*` values to Linux evdev keycodes
with `compositor/src/keymap.rs` and sends host-scoped `SurfaceEvent::HardwareKey`
events on the surface event channel. Using the surface channel keeps hardware
keys ordered with Android `FocusChanged` events and lets the compositor ignore
stale keys from background/destroyed Activities.

The compositor emits real `wl_keyboard` press/release events to the current
keyboard focus. Printable keys stay key events; clients/toolkits use normal
XKB handling to turn them into text. Accepted held keys are tracked by Activity
id so their real Android `ACTION_UP` is forwarded even if Activity foreground
bookkeeping changes after key-down. Android repeat `ACTION_DOWN`s do not create
new Wayland presses while the key is already held; Wayland clients repeat from
`wl_keyboard.repeat_info` until the release arrives. Backspace/Delete/Enter/Tab/
Escape, arrows, modifiers, letters, digits, punctuation, function keys, and
numpad keys share the same Android-to-evdev table used by IME-originated key
events. Unmapped Android/system keys return `false` from JNI so Android can
keep its normal handling.

Known gap: emulator host Backspace can arrive as an app-visible down event
without a matching up event; see [hardware-backspace-stuck-down](../issues/hardware-backspace-stuck-down.md).

## Android Back Button

`CompositorActivity` consumes Android Back for active Wayland windows and
forwards it to Rust via `nativeOnBackPressed(activityId)`. The compositor
decides from Wayland state, in this order:

1. If the host has an active grabbing `xdg_popup`, dismiss only the topmost
   grabbed popup (`PopupUngrabStrategy::Topmost`). Do not dismiss the whole
   popup stack; nested menus should peel back one layer at a time.
2. Else if the host is fullscreen / immersive, restore it to maximized. This
   clears the xdg fullscreen state, sends a maximized configure, and asks
   Android to show system bars again.
3. Else inject one Escape key press/release into the focused Wayland keyboard
   target.

Back is host-scoped. The `activityId` must still be the foreground host; stale
events for destroyed/backgrounded Activities are ignored. A popup must belong
to the Activity that received Back before it is dismissed; otherwise the policy
falls through to that host's fullscreen/Escape behavior. `CompositorActivity`
uses a default-priority `OnBackInvokedCallback` on API 33+ so transient Android
UI such as the IME can consume Back first, and the legacy `onBackPressed`
override on older supported Android versions.
