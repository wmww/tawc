# Hardware Backspace can get stuck down

Physical/emulator Backspace can remain logically pressed in Wayland clients
(observed in lxterminal and Firefox), causing client-side repeat until the
client is restarted.

## Current state

- Soft-keyboard Backspace works through `TawcInputConnection`.
- tawc now has a real hardware-key path for mapped Android key down/up events:
  focused view key callbacks -> `nativeOnHardwareKeyEvent` -> `SurfaceEvent::HardwareKey`
  -> real `wl_keyboard` press/release.
- The compositor tracks accepted held hardware keys so repeat `ACTION_DOWN`s do
  not create duplicate Wayland presses. It does not synthesize/fake key releases.

## Findings

Temporary diagnostic logging was added and then removed. On the emulator host
keyboard path, pressing Backspace once produced only an app-visible down event:

```text
Activity.dispatchKeyEvent key=67 action=0 repeat=0 scan=14 source=0x301 device=0 flags=0x8
SurfaceView.dispatchKeyEvent key=67 action=0 repeat=0 scan=14 source=0x301 device=0 flags=0x8
SurfaceView.onKeyDown before key=67 action=0 repeat=0 scan=14 source=0x301 device=0 flags=0x8
```

No corresponding `ACTION_UP` reached the Activity, focused view, InputConnection,
JNI, or compositor. Moving the bridge from `dispatchKeyEvent` to
`SurfaceView.onKeyDown/onKeyUp` did not change that emulator-host-key behavior.
Temporarily letting `deviceId == 0` Backspace fall through to Android's default
text-editor path also did not produce `InputConnection.deleteSurroundingText` or
an up event.

`adb shell input keyevent 67` is not a reliable reproduction of the problem. It
can take a different Android dispatch path than the emulator host keyboard.

## Not yet known

- Whether a real physical phone keyboard produces paired `ACTION_DOWN` /
  `ACTION_UP` events for Backspace.
- Whether the emulator host keyboard path should be handled through a different
  Android API than normal focused-view key callbacks.

## Next steps

1. On the physical target, temporarily re-add narrow Backspace boundary logging
   and press a real physical Backspace once.
2. If the phone delivers paired events, keep the normal hardware-key path and
   investigate the emulator `source=0x301 device=0 flags=0x8` path separately.
3. If the phone also delivers only down, identify the Android input mechanism
   that exposes real key release state for that source. Do not solve this by
   synthesizing a release.
