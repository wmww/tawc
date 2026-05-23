# Text Input

## Overview

Text input bridges Android's `InputConnection` API and Wayland's `zwp_text_input_v3` protocol. Both are designed around the same concepts (composing text, committing text, deleting surrounding text, content type hints), but differ in details that the compositor must translate correctly.

## Protocol: zwp_text_input_v3

Text-input-v3 is a double-buffered, serial-synchronized protocol.

**Client → compositor (requests), applied atomically on `commit`:**
- `enable` / `disable`: Whether client wants IME input. Enable resets ALL associated state.
- `set_surrounding_text(text, cursor, anchor)`: UTF-8 text around cursor, byte offsets.
- `set_text_change_cause(cause)`: Why text changed — `input_method` (IME did it) or `other` (user touch, arrow keys, etc.).
- `set_content_type(hint, purpose)`: What kind of text field.
- `set_cursor_rectangle(x, y, w, h)`: Cursor position in surface coords.
- `commit`: Atomically applies all pending requests. Compositor counts these.

**Compositor → client (events), applied atomically on `done`:**
- `enter(surface)` / `leave(surface)`: Text input focus.
- `preedit_string(text, cursor_begin, cursor_end)`: Composing text preview. cursor_begin/cursor_end are byte offsets — when equal, shown as a caret; when different, shown as highlight.
- `commit_string(text)`: Final text to insert.
- `delete_surrounding_text(before_length, after_length)`: Bytes to delete. Relative to cursor position from client's last `set_surrounding_text`. Emitted ONLY in the same `done()` cycle as a `commit_string` / `preedit_string` (Gboard's `setComposingRegion` replace pattern) — see "Why standalone deletion is a key event" below.
- `done(serial)`: Apply all pending events. Serial = number of client commits seen.

**Done event application order (critical):**
1. Replace existing preedit string with the cursor.
2. Delete requested surrounding text.
3. Insert commit string with the cursor at its end.
4. Calculate surrounding text to send.
5. Insert new preedit text in cursor position.
6. Place cursor inside preedit text.

**Serial synchronization:**
- Compositor counts client `commit` requests per instance.
- `done(serial)` carries this count.
- Matching serial: client updates its state normally.
- Mismatched serial: client still applies text changes, but defers state updates until a matching serial arrives.

**State lifecycle:**
- `enable` resets all state from the previous cycle (surrounding text, change cause, content type, cursor rectangle, AND compositor-side preedit/commit/delete state).
- After `enter` event or committed `disable`: all state is invalidated.

### wl_keyboard requirement

Firefox won't send text-input-v3 `enable` unless the seat has keyboard capability and the surface has keyboard focus. The seat must advertise keyboard capability (`seat.add_keyboard()`) and send `wl_keyboard.enter` to the focused surface.

### Why standalone deletion is a key event

Wayland's `delete_surrounding_text` is defined relative to "the current cursor index from the client's last `set_surrounding_text`." Clients that enable text-input but don't push surrounding text — terminals are the canonical case (VTE under Wayland brings the soft keyboard up but holds no editable buffer behind the prompt) — close the connection if you send it to them, since the cursor index it references is undefined for them. The integration test `text_input::test_surroundingless_client_uses_keyboard_for_backspace` exercises this shape against `wayland-debug-app text-input-no-surrounding`.

The IC translates standalone `deleteSurroundingText(N, M)` into N×Backspace + M×Forward-Delete `wl_keyboard` events at the source, before the JNI call — `TawcInputConnection.deleteSurroundingText`. UTF-16-unit counts become user-perceived-character counts via `unitsToKeyPlan` (uses `Character.codePointCount` on the Editable mirror around `wireCursor`), so an emoji surrogate pair → one Backspace, not two. When the Editable is empty (terminal-like clients, no mirror to consult) the unit counts pass straight through — exact for ASCII, which is the only case where an empty mirror coexists with a non-zero unit count.

The combined commit-or-preedit-WITH-delete path (Gboard's `setComposingRegion` + `commitText`/`setComposingText` "tap to retype") still uses the protocol's `delete_surrounding_text` event in the SAME `done()` as the `commit_string` / `preedit_string`. Splitting that into BS keys + commit_string would let the client fire `set_surrounding_text(cause=other)` between the two — which the compositor's preedit-clearing logic interprets as "the user moved the cursor, drop the IME's preedit," undoing the commit. The protocol path keeps it atomic.

Open trade-off: the combined path still vulnerable to the same crash as standalone delete if a surrounding-less client (e.g. terminal) is the one receiving the `setComposingRegion` + replace flow. Gboard does that on tap-to-retype of typed words; in a terminal the user would have to tap-select a typed word to trigger it, which is rare. Tracked as a follow-up — proper fix needs the compositor to suppress the preedit-clear when the cause=other event is the echo of its own freshly-emitted Backspaces.

### Keys also sent as wl_keyboard events

- **Enter** (evdev KEY_ENTER=28): real key event required for single-line fields (URL bars) which ignore `commit_string("\n")`. Gboard sends this both via `sendKeyEvent(KEYCODE_ENTER)` and via `commitText("\n")` — the JNI layer reroutes the latter to the same key path.

### Not needed

- **zwp_input_method_v1/v2**: For when a separate Wayland client acts as the IME.
- **virtual-keyboard-unstable-v1**: For injecting keystrokes from a Wayland client.

## Android Side: InputConnection

### Key methods

| InputConnection method | Action | Wayland equivalent |
|---|---|---|
| `commitText(text, pos)` | Insert finalized text | (atomic delete for setComposingRegion replace) `delete_surrounding_text(before, after)` + `commit_string(text)` + `done` |
| `setComposingText(text, pos)` | Set composing text | (atomic delete) `delete_surrounding_text(before, after)` + `preedit_string(text, len, len)` + `done` |
| `finishComposingText()` | Finalize composing text | If a preedit is tracked: `commit_string(tracked_preedit)` + `preedit_string(None)` + `done`; otherwise no-op |
| `deleteSurroundingText(before, after)` | Delete around cursor (char counts) | Backspace×before + Forward-Delete×after on `wl_keyboard` (see "Why standalone deletion is a key event") |
| `sendKeyEvent(event)` | Hardware key event | `wl_keyboard` press+release via `keymap::android_to_evdev` |
| `getTextBeforeCursor(n)` | IME queries editor text | Served from BaseInputConnection's Editable |
| `getTextAfterCursor(n)` | IME queries editor text | Served from BaseInputConnection's Editable |

### sendKeyEvent mapping

Every Android `KeyEvent` we recognise becomes a real `wl_keyboard` press+release pair (text-input-v3 has no key-event channel). Translation lives in `compositor/src/keymap.rs::android_to_evdev` and covers:

- editing: Backspace, Delete, Enter, Tab, Escape, Space
- navigation: arrows, Home/End, PageUp/Down, Insert
- modifiers/locks: Shift/Ctrl/Alt/Meta (L+R), CapsLock, NumLock, ScrollLock, Pause, SysRq
- F1–F12, A–Z, 0–9, common punctuation, full numpad

`commitText("\n")` is intercepted in `nativeCommitText` and routed through the same `KEY_ENTER` path — Gboard sends Enter that way. Anything outside the table (media keys, gamepad buttons, Android-specific BACK/HOME/MENU) is logged and dropped.

### BaseInputConnection and Editable

`TawcInputConnection` extends `BaseInputConnection(view, true)` (fullEditor=true), which maintains an internal `Editable` buffer. The IME (Gboard, OpenBoard, etc.) queries this buffer via `getTextBeforeCursor`, `getTextAfterCursor`, and `getExtractedText` to drive predictions, autocorrect, and word-boundary detection. **The Editable must mirror the Wayland client's actual text** for those features to work — otherwise the IME's internal model drifts from reality and autocorrect/composing/delete operate on the wrong positions.

We keep the Editable in sync via two channels:

1. **Outbound (Android → Wayland):** every overridden IME method calls `super` first to update the Editable, then JNI to forward to the compositor. This ensures the Editable predicts what the Wayland client will have right after our event takes effect.

2. **Inbound (Wayland → Android):** when a client commits a `set_surrounding_text`, the compositor calls reverse-JNI `onUpdateEditableText(text, selStart, selEnd)`. That replaces the active `TawcInputConnection`'s Editable contents and selection so the IME sees the editor's truth, not just our predictions. This catches:
   - Cursor moves caused by user touch / arrow keys (`change_cause=other`)
   - Editor-side text changes (autocomplete in Firefox URL bar, paste, undo, mid-stream insertions)
   - Drift between what we sent and what the client actually applied

Without (2), the IME's text model silently desyncs after the first non-IME cursor move and every later operation lands at the wrong position. The single `TawcInputConnection` is cached on `NativeBridge.activeInputConnection` so reverse-JNI updates always hit the live IME session (and broadcast tests share state across multi-step flows).

With `fullEditor=true`, `mFallbackMode=false`, so `sendCurrentText()` is a no-op — calling `super` does NOT cause duplicate input via key events.

### Showing/hiding the keyboard

- When any text input instance is enabled: `InputMethodManager.showSoftInput()`.
- When no instances are enabled: `InputMethodManager.hideSoftInputFromWindow()`.
- Keyboard visibility is tracked to avoid redundant calls and handle rapid disable+enable cycles (e.g., cursor movement within a text field may trigger disable+enable in quick succession).
- `NativeBridge.pendingKeyboardShown` is sticky: it records the compositor's most recent show/hide request and is replayed when `inputView` registers. Required because clients that map + enable text-input faster than Android brings the Activity up (lxterminal/VTE was the canonical case) would otherwise see `onShowKeyboard` no-op against a null `inputView` and the IMM would never call `onCreateInputConnection`, leaving no bound `TawcInputConnection`.

### Cursor change notification

`updateFromCompositor` (the inbound channel above) **always** calls `InputMethodManager.updateSelection(view, sel, sel, -1, -1)` immediately after replacing the Editable. The IMM call carries `composingStart=composingEnd=-1`, which tells the bound IME there's no preedit annotation on the editor side — exactly what's true post-replace. This single call covers both jobs:
- For `change_cause=input_method` it just keeps Gboard's IMM-cached selection in step with our predictions (rare desync, but cheap to keep clean).
- For `change_cause=other` it makes Gboard drop its in-flight composing region and re-query the editor — without this, autocorrect, predictions, and word boundaries silently land at the wrong position after a tap.

There is no separate compositor → Android `onUpdateSelection` reverse-JNI call: `updateFromCompositor` is the only path, and it's always the right one because the IMM update only makes sense when the Editable was just replaced anyway.

## Architecture

### State Model

Per text-input instance, the compositor tracks:

1. **Serial counter** (`commit_count`): Number of client commits processed.
2. **Pending state** (applied on next commit): enable/disable, surrounding text, change cause.
3. **Current (committed) state**: enabled flag, surrounding text.
4. **Compositor-side tracking**: current preedit text sent to client (needed for `finishComposingText`).

State is properly double-buffered: pending fields are stored when requests arrive and applied atomically on `commit`, per the protocol spec. The `enabled` flag is per-instance, not global.

### Data Flow

```
Android Gboard (IME)
     ↓
InputConnection callbacks (TawcInputConnection.kt)
  - Calls super (updates BaseInputConnection Editable)
  - deleteSurroundingText: unitsToKeyPlan → emitKeys → nativeSendKeyEvent
    (and STOP — no nativeDeleteSurroundingText, see "Why standalone deletion is a key event")
  - commitText/setComposingText with computeReplaceDeltas() != 0:
    nativeCommitText(text, before, after) / nativeSetComposingText(text, before, after)
    — the deletes ride atomically with the commit/preedit on the wire
  - Plain commitText/setComposingText/finishComposingText: corresponding native call
     ↓
TextInputEvent enum + calloop channel (KeyPress / CommitString / SetPreeditString / FinishComposingText)
     ↓
Event loop source (event_loop.rs)
     ↓
If KeyPress: send wl_keyboard key event (press + release)
Else: text_input_state.handle_android_event()
     ↓
Protocol events to client:
  - preedit_string(text, cursor_begin=len, cursor_end=len)
  - commit_string(text)
  - delete_surrounding_text(byte_before, byte_after) — only with a same-cycle commit/preedit
  - done(serial)
     ↓
Wayland client (Firefox) applies per done ordering
     ↓
Client sends back: set_surrounding_text + set_text_change_cause + commit
     ↓
Compositor processes commit:
  - Stores surrounding text (double-buffered)
  - Pushes Editable + IMM update via updateFromCompositor (single round-trip)
  - If change_cause=other AND we had a tracked preedit: emit preedit_string(None) + done
  - Updates keyboard visibility
```

### Reverse Channel: Compositor → Android

| Trigger | Android action |
|---|---|
| Any instance enabled (via sync_keyboard_visibility) | `InputMethodManager.showSoftInput()` |
| No instances enabled | `InputMethodManager.hideSoftInputFromWindow()` |
| Client commits a `set_surrounding_text` (any cause) | `TawcInputConnection.updateFromCompositor(text, selStart, selEnd)` — replaces Editable contents and selection AND pokes `IMM.updateSelection` so the IME drops any composing region |
| Resolved EditorInfo for the focused instance changes | `onContentTypeChanged(inputType, imeFlags)` → `restartInput`, so the next `onCreateInputConnection` carries the new fields |

### Input Focus

Keyboard focus and text-input-v3 focus are conceptually one thing — "the surface receiving input from the user". Both move together through the single `TawcState::set_input_focus(target)` helper. Every place that previously updated only one of them was a latent drift bug; the helper is now the only entry point.

Call sites: `FocusChanged` (Android Activity gains/loses focus), the frame-timer cleanup (focused toplevel died), `new_toplevel` (a fresh toplevel landed on the foreground host), and `TouchEvent::Down` (user tapped — moves focus to the target surface before delivering the touch).

## Implementation Notes

### Cursor movement during preedit

Touch events choose a Wayland input target and are delivered as touch events. They do not commit or finish text input preedit. A touch can scroll, hit a button, start a gesture, or be ignored; committing preedit before the client reports an editor-state change guesses at user intent and can desync the Android IME from the Wayland client.

When a client actually moves the cursor or changes text outside IME control, it reports the new state with `set_surrounding_text` plus `set_text_change_cause(cause=other)`. At that point the compositor clears its tracked `current_preedit` and, if needed, sends `preedit_string(None) + done` so the client stops rendering stale preedit at the new cursor. The pending preedit is not committed: by the time `cause=other` reaches us, text-input-v3 has no way to insert text at the old cursor.

### finishComposingText

Android's `finishComposingText()` means "commit the current composing text as-is." We track the last preedit text sent to each instance (`current_preedit`). On finishComposingText with a tracked preedit, we send `commit_string(tracked_preedit)` + `preedit_string(None)` + `done`. Without this, the composing text would just vanish. If no preedit is tracked, the event is stale and no-ops.

**`current_preedit` must be cleared when the Wayland client commits a `set_surrounding_text` with `cause=other`.** A non-IME cursor move (user touch, arrow keys) means the client's view has changed independently. If we keep our `current_preedit` tracking, a subsequent `finishComposingText` from the IME (which it issues defensively after cursor moves) re-commits the preedit at the new cursor — and "words randomly reappear" in the editor. Cause=other is the explicit signal from the protocol that stale preedit must be abandoned.

### setComposingRegion + setComposingText (Gboard's "tap to retype")

Android's IME can mark already-committed text as a composing region via `setComposingRegion(start, end)`. The bytes stay in the editor's buffer; only the IME's annotation changes. The next `setComposingText("...", 1)` *replaces* that region with the new preedit.

Wayland's text-input-v3 has no equivalent — preedit is overlay, not a span over committed text. To bridge, `TawcInputConnection` tracks a `composingRegionIsPreedit` flag:

- `setComposingText(text)` → flag = `true` (the new region IS the new Wayland preedit).
- `setComposingRegion(start, end)` → flag = `false` (the marked region is committed text on Wayland).
- `commitText` / `finishComposingText` / `updateFromCompositor` → flag = `false` (no region after).

When the next `setComposingText` or `commitText` runs and the flag is `false`, the IC computes (before, after) UTF-16 unit deltas around the cursor that span the existing composing region. These deltas travel as extra parameters on `nativeSetComposingText` / `nativeCommitText`. The compositor emits `delete_surrounding_text(before_bytes, after_bytes)` first, then the new preedit/commit string, all in one `done()` cycle. Without this delete the original word stays in committed text and the replacement becomes a duplicate.

This only works when the cursor is *inside* the composing region — Wayland's `delete_surrounding_text` deletes around the cursor. IMEs typically pick a region that contains the cursor (the word at the click location), so the constraint is rarely violated. When the cursor sits outside the region, the IC falls back to plain preedit_string and accepts the divergence — `set_surrounding_text` from the client will reconcile the Editable.

Standalone `deleteSurroundingText` (Gboard's plain Backspace) does NOT come through this path — see "Why standalone deletion is a key event" above.

### Cursor synchronisation gate on delta propagation

The wire-side `delete_surrounding_text` is relative to the *Wayland client's cursor*, not the Editable's. Our IC computes deltas against the Editable cursor; the two have to agree for the deltas to land on the right bytes.

They desync in two ways:

**1. The IME moves the Editable cursor under us via `setSelection`.** Wayland text-input-v3 has no "move the cursor" request, so we can't push a cursor change to the client; the Editable says cursor=N while the client still has cursor=M. The IC tracks `lastSyncedCursor`, set on every `updateFromCompositor` from the Wayland client. `computeReplaceDeltas` returns `(0, 0)` whenever the Editable's current cursor differs from either `lastSyncedCursor` or `wireCursor`. Trade-off: a tap-to-retype flow that moves the cursor (rare; IMEs usually mark the region without moving the cursor) won't get its delete propagated. The next round-trip recovers state, possibly leaving a transient duplicate; this is strictly better than slicing arbitrary bytes off the buffer.

**2. The Wayland buffer's cursor moves past the client's reported context.** GTK's `set_surrounding_text` is allowed to be a "context window" — it can report the full text with a cursor before trailing newlines when the real cursor is on a fresh empty line. After we send `commit_string("\n")`, the wire cursor advances by one but GTK's next `set_surrounding_text` may report the old line cursor. The IC tracks `wireCursor` separately from `lastSyncedCursor`: it is reset from client reports, except for the immediate echo of a trailing-newline commit where the reported cursor is behind only by newline characters, then advanced by outbound commits, committed preedits, and standalone deletes. `deleteSurroundingText` translates key counts around `wireCursor`, so broad deletes after stale newline context land at the client's actual cursor while still counting surrogate pairs as one key.

OpenBoard's per-Enter handler trips this shape: after the user types `<word><space><backspace><enter>`, on each subsequent Enter it fires `setComposingRegion(0, len(word))` + `commitText(word, 1)` + `commitText("\n", 1)`. The first `commitText` would normally translate to `delete_surrounding_text(len(word))` + `commit_string(word)` — a "replace the marked region with itself" no-op. `commitText` short-circuits that no-op. The following newline advances `wireCursor` even if the next surrounding-text report hides the trailing newline, so a later standalone `deleteSurroundingText` does not compute Backspaces from the stale Editable selection.

`commitText` short-circuits when the new text equals the marked composing region's content: skip the wire `delete_surrounding_text` + `commit_string` entirely. The bytes are already on the buffer; nothing needs to travel. Real tap-to-retype with different text falls through to the normal delta-propagating path, gated by `lastSyncedCursor` for the divergence case.

### UTF-8 bytes vs UTF-16 code units vs Unicode chars

Three units are in play and they don't all agree:

- **Wayland protocol:** UTF-8 byte offsets (`set_surrounding_text` cursor/anchor, `delete_surrounding_text` before/after).
- **Android `InputConnection`:** UTF-16 code units (Java `char`). `deleteSurroundingText(2, 0)` for an emoji means 2 UTF-16 units, which is one non-BMP scalar = 4 UTF-8 bytes.
- **Rust `char`:** Unicode scalar values. One emoji = 1 Rust char = 2 UTF-16 units = 4 UTF-8 bytes.

For surrounding-text round-trips, `byte_offset_to_utf16_count` converts the client's UTF-8 byte cursor/anchor into the UTF-16 code-unit counts Android wants. For the combined commit-with-delete path, `utf16_units_to_bytes` walks the stored surrounding text counting UTF-16 units (`char::len_utf16`) and accumulates UTF-8 bytes (`char::len_utf8`), falling back to 1:1 mapping when no surrounding text is available. For standalone deletion, `unitsToKeyPlan` (Kotlin) walks the Editable mirror at `wireCursor` to convert UTF-16 unit counts into Backspace key counts — one keypress per codepoint, so a surrogate-pair emoji becomes one Backspace, not two.

### Keyboard visibility deferred

`sync_keyboard_visibility()` checks the final enabled state of all instances and only shows/hides the keyboard when the state actually changes. This prevents rapid disable+enable cycles (common during cursor movement within a text field) from causing keyboard flicker or failed re-show.

### Content type → EditorInfo

`set_content_type(hint, purpose)` is double-buffered like the rest of the per-cycle state: pending values are stored in `InstanceState` and applied on `commit`. After commit, if the committing instance is on the focused client and the resolved Android EditorInfo differs from what we last pushed, the compositor calls reverse-JNI `onContentTypeChanged(inputType, imeFlags)`. `NativeBridge` caches the values and triggers `InputMethodManager.restartInput`, which makes `TawcSurfaceView.onCreateInputConnection` rebuild its `EditorInfo` with the new fields. The same push runs on `enter` so a focus move to a client that doesn't re-enable still gets the right keyboard.

Translation lives in `compositor/src/text_input.rs::wayland_content_to_android_input_type`. Highlights:

| `content_purpose` | `EditorInfo.inputType` |
|---|---|
| normal/alpha | `TYPE_CLASS_TEXT` |
| digits | `TYPE_CLASS_NUMBER` |
| number | `TYPE_CLASS_NUMBER \| FLAG_SIGNED \| FLAG_DECIMAL` |
| pin | `TYPE_CLASS_NUMBER \| NUMBER_VARIATION_PASSWORD` |
| phone | `TYPE_CLASS_PHONE` |
| url | `TYPE_CLASS_TEXT \| TEXT_VARIATION_URI` |
| email | `TYPE_CLASS_TEXT \| TEXT_VARIATION_EMAIL_ADDRESS` |
| name | `TYPE_CLASS_TEXT \| TEXT_VARIATION_PERSON_NAME` |
| password | `TYPE_CLASS_TEXT \| TEXT_VARIATION_(VISIBLE_)PASSWORD` (chosen by `hidden_text`) |
| date / time / datetime | `TYPE_CLASS_DATETIME` (+ DATE / TIME variation) |
| terminal | `TYPE_CLASS_TEXT \| FLAG_NO_SUGGESTIONS` |

Hints add flags on TEXT-class fields: `auto_capitalization` → `FLAG_CAP_SENTENCES`, `uppercase` → `FLAG_CAP_CHARACTERS`, `titlecase` → `FLAG_CAP_WORDS`, `spellcheck` → `FLAG_AUTO_CORRECT`, `completion` → `FLAG_AUTO_COMPLETE`, `multiline` → `FLAG_MULTI_LINE`, `hidden_text` (without password purpose) → `FLAG_NO_SUGGESTIONS`. `sensitive_data` adds `IME_FLAG_NO_PERSONALIZED_LEARNING` so the IME doesn't store typed text in its dictionary.

## Open Questions

1. **Batch editing**: Android groups IME operations between `beginBatchEdit()` / `endBatchEdit()`. Currently each operation gets its own `done` event. Batching into a single `done` would be more correct but functionally the current approach works for simple cases.

2. **Composing region replacement when cursor outside region**: `pendingComposingRegionReplacement` only emits a delete when the cursor sits inside the composing region. Outside that case the new preedit lands at the cursor without removing the old region; the next `set_surrounding_text` from the client reconciles the Editable but Wayland transiently shows the original word AND the new preedit. Real IMEs almost always pick regions containing the cursor, so this is acceptable in practice.

## Test infrastructure note

**The rule: tests interact with the system as a keyboard or as an app, never inside the compositor.**

- **As a keyboard**: every normal test driver call goes through the focused activity's active `TawcInputConnection` via the broker `ic-*` actions (`ic-commit-text`, `ic-replace-text`, `ic-set-composing-text`, `ic-set-composing-region`, `ic-finish-composing`, `ic-set-selection`, `ic-delete-surrounding-text`, `ic-send-key-event`) — the same Kotlin entry points the system IMM dispatches Gboard / OpenBoard / AOSP-latin events through. The IC's full state machine runs on every test step: Editable mirror, `computeReplaceDeltas`, `composingRegionIsPreedit` short-circuit, `wireCursor` tracking, `unitsToKeyPlan` key-translation, `lastSyncedCursor` divergence guard. Broker actions fail loudly if there is no matching active IC or if the IC method returns `false`.
- **As an app**: assertions go through `wayland-debug-app`'s observed `TAWC_DEBUG:…` events — `TEXT_CHANGED`, `PREEDIT`, `CURSOR_POS`, `KEY`, `COMMIT`, `DELETE_SURROUNDING`. That's what a real wayland client running under our compositor sees.

There is intentionally **no test infra that pokes `NativeBridge.native*` directly**. Earlier versions had a "bypass" channel that did — broadcasts, then later broker actions like `inject-text` — but it was pulled. The reason: `nativeCommitText` / `nativeSetComposingText` / `nativeFinishComposingText` / `nativeSendKeyEvent` are JNI primitives the IC calls into the Rust compositor with after running its state machine. Calling them directly skips the IC entirely. Wayland-side text-input-v3 done-ordering produces the right *observable* (preedit replaces on the next preedit/commit) regardless of what the IC computed, so a buggy IC can produce correct-looking GTK output and a bypass test smiles. Driving every scenario through IC closes that hole — wayland-side assertions become a real integration check rather than a redundant proof of text-input-v3.

The sanctioned middle-of-the-stack test toggle is `test-init`, which every integration test calls before doing work. It swaps `NativeBridge.imeOutput` to a fresh `RecordingImeOutput`, resets `Settings` to an in-memory factory-default store, clears the active IC, and asks attached Wayland/XWayland client windows to close. If it had to close anything, the Rust harness waits for the compositor to report zero clients/toplevels before the test continues. That's not bypassing anything in our state machine — it removes the *third-party* system IME (Gboard / OpenBoard / AOSP-latin) at the boundary so it doesn't react to our `updateSelection` calls and amplify them back into the IC. The recorder still creates and owns a test `TawcInputConnection` when the compositor requests keyboard show/restart, and tests wait for `input-ready` before driving `ic-*` actions. The special `ic-finish-hidden-composing` action exists only to model stale IME callbacks after keyboard hide/focus leave; normal input actions never target hidden ICs. Process death reverts the swap and discards in-memory settings, so test crashes can't persist test state across app launches.

See `notes/exec-broker.md` "Registered actions" for the full action surface and `app/src/main/java/me/phie/tawc/dev/InputActions.kt` for handler implementations.
