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
- Touch-down hit-tests the host's toplevel, subsurface, and popup trees in draw
  order. A surface with an explicit `wl_surface.set_input_region` only receives
  the touch if the local point is inside that region; `NULL` input region keeps
  the Wayland default of the whole surface. This matters for Firefox/WebRender:
  visible render-only child surfaces may use an empty input region, so the touch
  must fall through to the browser toplevel.
- Multi-touch is supported: each Android pointer ID maps to a Smithay `TouchSlot`.
- The seat normally advertises only real input capabilities. Keyboard
  capability is required for Firefox to enable text input (see text-input.md).
  The one explicit exception is the optional
  [GTK3 broken menus workaround](gtk3-broken-menus-workaround.md), which
  exposes a `wl_pointer` and sends a brief center enter/leave for new
  toplevels. Do not expand that into a general touch-to-pointer path; if real
  pointer hardware is added, wire it as real pointer input.
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
uses an overlay-priority `OnBackInvokedCallback` on API 33+ so the Wayland
policy wins over transient Android UI such as the IME, and the legacy
`onBackPressed` override on older supported Android versions.
