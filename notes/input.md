# Input

## Touch Input

Touch events flow: Android `onTouchEvent` -> JNI `nativeOnTouchEvent` -> `calloop::channel`
-> Smithay `TouchHandle` -> `wl_touch` protocol events to client.

**Architecture:**
- `MainActivity.kt` sets an `OnTouchListener` on the SurfaceView. It dispatches DOWN,
  MOVE, UP, POINTER_DOWN, POINTER_UP, and CANCEL events per-pointer via JNI.
- `input.rs` holds a global `Mutex<Option<channel::Sender<TouchEvent>>>` so the JNI
  thread can send events to the compositor thread without shared mutable state.
  Uses Mutex instead of OnceLock so the channel can be replaced on compositor restart.
- `event_loop.rs` has a calloop channel source that converts `TouchEvent` into Smithay
  `DownEvent`/`MotionEvent`/`UpEvent` and calls `touch.down()`/`.motion()`/`.up()` +
  `.frame()`. Events are flushed immediately to minimize latency.
- Coordinates arrive in physical pixels from Android and are divided by the scale factor
  (2) to get logical Wayland coordinates.
- Multi-touch is supported: each Android pointer ID maps to a Smithay `TouchSlot`.
- The seat advertises pointer, keyboard, and touch capabilities. Keyboard capability
  is required for Firefox to enable text input (see text-input.md).
- Keyboard focus is set on touch-down and when new toplevels are created.

**Known issues:**
- Focus is always the first alive toplevel (no multi-window focus management yet).

## Simulating Touch via adb

`adb shell input tap X Y` injects touch events through the Android input framework.
Coordinates are in screen pixels (same space as `screencap`). The app uses immersive
fullscreen, so screen coordinates map 1:1 to SurfaceView coordinates. The compositor
divides by the scale factor (2) to get Wayland logical coordinates.

**Debug loop:**
1. Screenshot: `adb shell su -c "screencap -p /sdcard/screenshot.png" && adb pull /sdcard/screenshot.png /tmp/screenshot.png`
2. Identify target element's pixel coordinates in the 1080x2400 image
3. Tap: `adb shell input tap X Y`
4. Screenshot again to see result
5. Clean up: `adb shell rm /sdcard/screenshot.png && rm /tmp/screenshot.png`

Be precise -- at 2x scale, UI elements are small in physical pixels. The Firefox tab
close "X" and toolbar hamburger are only ~50-60px apart.

**Keyboard key coordinates** (Gboard QWERTY layout on 1080x2400 screen):
When the Android OSK is visible, the keyboard rows are approximately at:
- QWERTY row: y ≈ 1775
- ASDFGH row: y ≈ 1875  
- ZXCVBN row: y ≈ 1975
- Bottom row (?123, space, period, enter): y ≈ 2200
Each QWERTY key is ~108px wide (1080/10). ASDFGH keys are ~120px wide (offset from QWERTY).
ZXCVBN keys start after a wider shift key (~160px).

**Firefox UI element positions** (2x scale, 540x1200 logical):
- Tab bar: logical y ≈ 0-20
- URL/address bar: logical y ≈ 40-70 (physical y ≈ 80-140)
- Content area starts at logical y ≈ 80
