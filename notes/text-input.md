# Text Input Plan

## Overview

Text input bridges two very different worlds: Android's `InputConnection` API (where a soft keyboard sends composed text) and Wayland's text-input protocol (where apps expect IME composition events). The goal is a clean, correct bridge that lets the Android on-screen keyboard drive text input into Wayland clients, with the keyboard appearing and disappearing naturally in response to client focus.

## Wayland Protocol: zwp_text_input_v3

Text-input-v3 is the primary protocol. It's the right fit for our architecture because it maps naturally to Android's `InputConnection` — both are designed around the same concepts (composing text, committing text, deleting surrounding text, content type hints).

**Client → compositor (requests):**
- `enable` / `disable`: Client wants/doesn't want IME input on the focused surface.
- `set_surrounding_text(text, cursor, anchor)`: Context text around cursor (max 4000 bytes).
- `set_content_type(hint, purpose)`: What kind of input (normal, password, email, URL, number, etc.).
- `set_cursor_rectangle(x, y, w, h)`: Where the cursor is in surface coordinates (for positioning the keyboard).
- `commit`: Atomically applies all pending state. Increments a serial.

**Compositor → client (events):**
- `enter(surface)` / `leave(surface)`: Text input focus.
- `preedit_string(text, cursor_begin, cursor_end)`: Composing/preview text.
- `commit_string(text)`: Final committed text.
- `delete_surrounding_text(before_length, after_length)`: Delete around cursor.
- `done(serial)`: Batch-applies all preceding events. Serial must match client's commit count.

The `done` event is critical for atomicity. All preedit/commit/delete events are batched and applied together when `done` arrives. The client processes them in order: clear old preedit → delete surrounding → insert commit string → insert new preedit.

### What about wl_keyboard?

**Update**: wl_keyboard IS required. Testing showed Firefox won't send text-input-v3 `enable` unless the seat has keyboard capability and the surface has keyboard focus. Without keyboard focus, clicking a text field in Firefox opens the autocomplete dropdown but never enables text input. The seat must advertise keyboard capability (`seat.add_keyboard()`) and the compositor must send `wl_keyboard.enter` to the focused surface.

xkbcommon needs XKB data files to initialize. On Android, set `XKB_CONFIG_ROOT` to point at the chroot's xkeyboard-config data before calling `add_keyboard()`.

The gap without full key event support is non-text keys: arrow keys, escape, Ctrl+C/V/Z, and other keyboard shortcuts have no text-input-v3 equivalent. This is a real limitation but orthogonal to text input and can be added later.

For special keys that Android IMEs do send (backspace, enter), we can handle them through text-input-v3:
- **Backspace**: `delete_surrounding_text(1, 0)` + `done`
- **Enter**: `commit_string("\n")` + `done` (or `commit_string("\r\n")` depending on content type)

### Not needed

- **zwp_input_method_v1/v2**: For when a separate Wayland client acts as the IME. We use Android's built-in IME instead.
- **virtual-keyboard-unstable-v1**: For injecting keystrokes from a Wayland client. We inject from the compositor itself.

## Smithay's text-input-v3: Can't use it as-is

Smithay has a built-in `TextInputManagerState`, but its implementation is designed for the input-method architecture where a separate Wayland client acts as the IME. Specifically, in `text_input_handle.rs`:

```rust
// Discard requsets without any active input method instance.
if !data.input_method_handle.has_instance() {
    debug!("discarding text-input request without IME running");
    return;
}
```

All client requests (enable, disable, set_surrounding_text, set_content_type, commit) are forwarded to an `InputMethodHandle` — and if no input-method client is connected, they're silently discarded. The entire commit handler also routes through `input_method_handle.activate_input_method()` / `deactivate_input_method()`.

This is the wrong model for us. Our IME is Android's Gboard, communicating via `InputConnection`, not a Wayland client using `zwp_input_method_v2`. We need to either:

1. **Implement our own text-input-v3 handler** — register the `zwp_text_input_manager_v3` global ourselves, handle requests directly without the input-method intermediary.
2. **Patch the Smithay fork** — add a mode that routes requests to a callback instead of requiring an input-method instance.

Option 1 is cleaner. The protocol is simple enough (one manager global, one per-seat text-input object) that a custom implementation is straightforward and avoids fighting Smithay's assumptions. We can still use Smithay's generated protocol types (`wayland-protocols` crate) for the wire format.

## Android Side: InputConnection

Android's IME communicates via `InputConnection`, an interface your View provides:

| InputConnection method | Description | Wayland equivalent |
|---|---|---|
| `commitText(text, newCursorPos)` | Insert finalized text | `commit_string(text)` + `done` |
| `setComposingText(text, newCursorPos)` | Set preedit/composing text | `preedit_string(text, ...)` + `done` |
| `finishComposingText()` | Commit current composing text | `preedit_string("")` + `commit_string(text)` + `done` |
| `deleteSurroundingText(before, after)` | Delete around cursor | `delete_surrounding_text(before, after)` + `done` |
| `sendKeyEvent(KeyEvent)` | Hardware-style key event | Map to appropriate text-input-v3 event |
| `getTextBeforeCursor(n, flags)` | Read context text | Return from client's `set_surrounding_text` |
| `getTextAfterCursor(n, flags)` | Read context text | Return from client's `set_surrounding_text` |

Gboard sends regular characters via `commitText`, and special keys (backspace, enter) via `sendKeyEvent`. Some keyboards differ — the implementation must handle both paths.

### sendKeyEvent mapping

When the IME sends key events (e.g. backspace, enter), map them to text-input-v3 operations:

| Android KeyEvent | text-input-v3 action |
|---|---|
| KEYCODE_DEL (backspace) | `delete_surrounding_text(1, 0)` + `done` |
| KEYCODE_FORWARD_DEL | `delete_surrounding_text(0, 1)` + `done` |
| KEYCODE_ENTER | `commit_string("\n")` + `done` |
| KEYCODE_DPAD_LEFT/RIGHT/UP/DOWN | (No text-input-v3 equivalent — deferred to wl_keyboard) |
| KEYCODE_TAB | `commit_string("\t")` + `done` |

### Showing/hiding the keyboard

This is where text-input-v3 integrates beautifully with Android:

- When a Wayland client sends text-input-v3 `enable` + `commit`: call `InputMethodManager.showSoftInput()`. The Android keyboard appears.
- When it sends `disable` + `commit`: call `InputMethodManager.hideSoftInputFromWindow()`. The keyboard disappears.
- Map text-input-v3 `set_content_type` to `EditorInfo.inputType` for the appropriate keyboard layout (password dots, email @, number pad, etc.).

This means the keyboard naturally appears when a user taps a text field in Firefox (Firefox sends `enable`) and disappears when they tap away (`disable`). No manual keyboard toggle needed.

## Architecture

### Data Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ Android                                                         │
│                                                                 │
│  ┌──────────┐    InputConnection     ┌──────────────────────┐   │
│  │  Gboard  │ ───────────────────────│  TawcInputView       │   │
│  │  (IME)   │  commitText()          │  (custom View)       │   │
│  │          │  setComposingText()    │                      │   │
│  │          │  sendKeyEvent()        │  Maintains:          │   │
│  │          │  deleteSurrounding()   │  - surrounding text  │   │
│  └──────────┘                        │  - composing state   │   │
│                                      │  - content type      │   │
│                                      └──────────┬───────────┘   │
│                                                 │ JNI           │
└─────────────────────────────────────────────────┼───────────────┘
                                                  │
┌─────────────────────────────────────────────────┼───────────────┐
│ Rust Compositor                                 │               │
│                                                 ▼               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  text_input.rs                                           │   │
│  │                                                          │   │
│  │  Receives JNI events via calloop channel                 │   │
│  │  Routes to text-input-v3:                                │   │
│  │                                                          │   │
│  │  commitText("hello")       → commit_string("hello")      │   │
│  │                              + done                      │   │
│  │  setComposingText("hel")   → preedit_string("hel", 0,3) │   │
│  │                              + done                      │   │
│  │  deleteSurrounding(1, 0)   → delete_surrounding_text(1,0)│   │
│  │                              + done                      │   │
│  │  sendKeyEvent(BACKSPACE)   → delete_surrounding_text(1,0)│   │
│  │                              + done                      │   │
│  │                                                          │   │
│  │  Receives client requests:                               │   │
│  │  enable/disable             → JNI → show/hide keyboard   │   │
│  │  set_content_type           → JNI → update EditorInfo    │   │
│  │  set_surrounding_text       → store for InputConnection  │   │
│  │  set_cursor_rectangle       → (future: position IME)     │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────┐                               │
│  │  Custom text-input-v3 impl   │  zwp_text_input_v3 protocol  │
│  │  (NOT Smithay's built-in)    │  preedit, commit, delete,     │
│  │                              │  done, enter, leave           │
│  └──────────────────────────────┘                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Text Input Focus

Without wl_keyboard, we manage text-input focus ourselves. The rule is simple: text-input focus follows the surface that has touch focus (i.e., the first alive toplevel — same as current touch focus logic). When focus changes:

1. Send `leave(old_surface)` on the old surface's text-input instances.
2. Update internal focus tracking.
3. Send `enter(new_surface)` on the new surface's text-input instances.

### Reverse Channel: Compositor → Android

Client requests that need to flow back to Android:

| Client request | Android action |
|---|---|
| `enable` + `commit` | `InputMethodManager.showSoftInput()` |
| `disable` + `commit` | `InputMethodManager.hideSoftInputFromWindow()` |
| `set_content_type(hint, purpose)` | Update `EditorInfo.inputType`, restart input |
| `set_surrounding_text(text, cursor, anchor)` | Store for `InputConnection.getTextBeforeCursor()` etc. |
| `set_cursor_rectangle(x, y, w, h)` | (Future: `InputMethodManager.updateCursorAnchorInfo()`) |

This requires a reverse JNI channel: Rust compositor calls back into Kotlin. Use JNI `CallVoidMethod` on the Activity (cached global ref) from the compositor thread.

## Code Organization

### New files

**Rust (server/compositor/src/):**
- `text_input.rs` — All text input logic:
  - Custom `zwp_text_input_manager_v3` / `zwp_text_input_v3` handler (bypassing Smithay's input-method-coupled implementation)
  - JNI event types (enum for commitText, setComposingText, sendKeyEvent, etc.)
  - Calloop channel sender/receiver (same pattern as touch in `input.rs`)
  - Event handler that routes Android IME events to text-input-v3 protocol events
  - Surrounding text state (received from Wayland clients, served to Android via reverse JNI)
  - Text input focus management

**Kotlin (server/app/src/main/java/me/phie/tawc/):**
- `TawcInputConnection.kt` — Custom `InputConnection` implementation:
  - `commitText()`, `setComposingText()`, `sendKeyEvent()`, `deleteSurroundingText()` → JNI
  - `getTextBeforeCursor()`, `getTextAfterCursor()` → read from state synced from compositor
  - Thread-safe state for surrounding text (updated from compositor thread via reverse JNI)

### Modified files

**Rust:**
- `compositor.rs` — Register text-input-v3 global, handle protocol dispatch
- `event_loop.rs` — Add text input calloop channel source, handle reverse channel
- `lib.rs` — New JNI entry points for text input events + reverse callbacks

**Kotlin:**
- `MainActivity.kt` — Make SurfaceView focusable, override `onCreateInputConnection()`, handle show/hide keyboard callbacks from native
- `NativeBridge.kt` — New JNI declarations for text input

### Module boundaries

`text_input.rs` owns all text-input-related state and logic. `compositor.rs` registers the protocol global and delegates. `event_loop.rs` dispatches channel events to `text_input.rs` handler functions. This mirrors how touch works: `input.rs` owns touch state, `event_loop.rs` dispatches.

## Implementation Plan

See [plan.md](../plan.md) phases 6-7 for the implementation steps.

## Testing Results

Text input was tested end-to-end on a real device (OnePlus 9 Pro, LineageOS):

- **Working**: Full pipeline: Gboard → InputConnection → JNI → calloop channel → zwp_text_input_v3 → Firefox. Typed "claude.ai" and searched successfully.
- **Composing text**: Gboard sends `setComposingText` for each keystroke, then `commitText` when a word is finalized (e.g. after period/space). This maps correctly to preedit_string/commit_string.
- **Keyboard show/hide**: Keyboard appears when Firefox URL bar is focused (Firefox sends text_input enable), disappears when unfocused.
- **Backspace**: Works via `sendKeyEvent(KEYCODE_DEL)` → `deleteSurroundingText(1,0)`.
- **Auto-capitalization**: Gboard auto-capitalizes (e.g. "claude" → "Claude"). This is Gboard behavior, not compositor behavior.

### Known Issues

- **OnceLock stale channels**: The original OnceLock-based channel pattern caused stale senders after compositor restart. Fixed by switching to Mutex<Option<Sender>>.
- **Coordinate estimation**: Simulated touch via `adb shell input tap` uses physical pixel coordinates. The compositor divides by scale (2x) to get logical coords sent to Wayland clients. When estimating keyboard key positions from screenshots, remember the image is at physical resolution but Firefox layouts at logical resolution.

## Open Questions

1. ~~**Smithay fork or fully custom?**~~ Resolved: custom implementation bypassing Smithay's input-method-coupled TextInputManagerState.

2. **Surrounding text synchronization**: `InputConnection.getTextBeforeCursor()` is called by the IME on its own thread. Currently not implemented — Gboard works without it but some IMEs may need it for accurate predictions.

3. **Key repeat**: Android IME handles its own repeat (holding backspace sends repeated `sendKeyEvent`). Each repeated event from Android gets translated individually — this works.

4. **Content type hints**: `set_content_type` from Firefox is received but not yet forwarded to Android's EditorInfo. Would improve keyboard layout (URL keyboard for URL bars, etc.).
