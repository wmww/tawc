# Input tests bypass `TawcInputConnection` and race the device IME

The integration tests in `tests/integration/tests/input.rs` purport to
cover input dispatch end-to-end, but in practice they sit at the
wayland-protocol layer and the entire Android-side adapter
(`TawcInputConnection`) is uncovered. The same architecture also makes
test outcomes depend on whichever system IME the device happens to have
installed, producing different failure modes on different hardware
(documented separately in `input-test-flaky-on-emulator.md`).

This issue subsumes that older one and proposes a plan that fixes both
problems together.

## Problem 1: tests skip the Android-side state machine

The "bypass" broadcasts the test suite uses
(`me.phie.tawc.TEXT_INPUT`, `SET_COMPOSING_TEXT`, `FINISH_COMPOSING_TEXT`,
`DELETE_SURROUNDING_TEXT`, `KEY_EVENT`) hit
`CompositorActivity.testInputReceiver`, which calls one of:

```kotlin
NativeBridge.nativeCommitText(text, before, after)         // 0,0 by default
NativeBridge.nativeSetComposingText(text, before, after)   // 0,0 by default
NativeBridge.nativeFinishComposingText()
NativeBridge.nativeSendKeyEvent(keycode)
```

Each of those native entries (in `compositor/src/lib.rs`) is a one-line
trampoline that puts a `TextInputEvent` on a channel; the text-input
event loop then calls `ti.commit_string()` / `ti.preedit_string()` /
`ti.delete_surrounding_text()` directly on the `ZwpTextInputV3`
wayland-server resource. End-to-end, a test broadcast becomes a
wayland-protocol event sent to the client.

`TawcInputConnection` is bypassed entirely. That means **none** of the
following is covered by the input test suite:

- `commitText`'s `composingRegionIsPreedit` shortcut for OpenBoard's
  per-Enter "re-commit composing region" pattern (notes/text-input.md
  "the bug: every Enter prepends `h` to the previous word").
- `computeReplaceDeltas`: turning Gboard's `commitText("the", 1)`
  issued while `Editable` has composing span `0..3` into the wire
  `(delete_before=3, delete_after=0)` paired with the commit string.
  This is the IC's main job. A regression here doesn't fail any input
  test.
- `setComposingText` / `setComposingRegion` Editable-mirror updates
  and the Android-composing-bytes vs wayland-cursor-relative-overlay
  bridge that the file's header comment goes to length to explain.
- `finishComposingText` flag-clear vs native-call semantics.
- `deleteSurroundingText` → key-event translation.
- `updateFromCompositor` (the inverse direction): Editable replace,
  `composingRegionIsPreedit = false`, `removeComposingSpans`,
  `lastSyncedCursor` tracking, `imm.updateSelection`.
- `sendKeyEvent` Android-keycode → evdev-keycode translation.

The reason the bypass tests appear to pass for things like the
autocorrect scene — `setComposing("teh") + commit("the ")` →
`"the "` — is that text-input-v3's done-ordering does the
preedit-replacement *implicitly* on the wayland side:
`preedit_string(None) + commit_string("the ") + done()` is enough to
make the GTK client replace "teh" with "the ". The test never has to
exercise IC's delta computation; the wayland protocol alone produces
the right observable on the GTK side.

Real Gboard input takes a completely different code path through IC
that has its own state machine, and our test suite happens to look
right because the wayland fallback hides the missing coverage.

## Problem 2: tests race whatever IME the device has installed

`updateFromCompositor` (`TawcInputConnection.kt:330`) calls
`imm.updateSelection(view, sel, sel, -1, -1)` whenever the wayland
client reports `set_surrounding_text` back to the compositor. IMM
forwards that to the currently-bound IME (Gboard on stock Android,
AOSP latin on the emulator, OpenBoard on someone's daily driver, …).
Most IMEs read `composingStart=-1, composingEnd=-1` as "the editor
cleared its composing region" and react by calling
`IC.finishComposingText` defensively, which calls
`nativeFinishComposingText`, which on the wayland side commits any
preedit currently set by the bypass path.

So a typical scenario from `scene_compose_lifecycle`:

1. `TEXT_INPUT "hello "` broadcast → bypass → wayland `commit_string`
2. GTK commits, sends `set_surrounding_text("hello ", 6, 6)` back
3. compositor → JNI → `mainHandler.post { ic.updateFromCompositor(...) }`
4. `SET_COMPOSING_TEXT "w"` broadcast → bypass → wayland
   `preedit_string("w")` — the **expected** end state is buffer="hello "
   with preedit="w"
5. (Async) Android main thread: `updateFromCompositor` runs →
   `imm.updateSelection(..., -1, -1)`
6. Gboard / AOSP IME / whatever reacts: `IC.finishComposingText`
7. `IC.finishComposingText` → `nativeFinishComposingText`
8. Compositor commits the just-set preedit "w" → buffer="hello w"

The order of steps 4 and 5–8 is unstable because step 5 was queued by
step 3 and the broadcast in step 4 races it. Whether you lose the race
depends on:

- the IME's "react to updateSelection(-1,-1)" timing
- the device's CPU / scheduler
- prior state accumulated across scenes (which is why the issue
  manifests in `scene_compose_lifecycle` even though a freshly-launched
  app + the same broadcast sequence works in isolation — see the
  diagnostic notes in `input-test-flaky-on-emulator.md`).

That's why this test passes consistently on the OnePlus 9, fails ~20%
on the x86_64 emulator, and fails ~100% on a Pixel 4a — three
different IMEs, three different timing profiles, same code under test.

## What the test suite misses (concrete examples)

Each of these would be a real regression that the current test suite
does not catch:

- Reverting any of the three text-input bug fixes from `7ad5869` (Fix
  three text-input bugs: vanishing preedit, duplicated word, prepended
  h) — all live in IC, not in the wayland path.
- Breaking `computeReplaceDeltas` so `delete_before`/`delete_after`
  always come back zero: Gboard would still commit the right text
  visually but inserts duplicates because the previous composing
  region wouldn't be deleted.
- Breaking `composingRegionIsPreedit` flag tracking: OpenBoard's
  per-Enter re-commit would re-introduce the "prepended h" behaviour.
- `updateFromCompositor` not calling `removeComposingSpans`: stale
  composing spans in the Editable would mis-classify subsequent
  `commitText` calls.

## Tentative plan

Two pieces, both required, in roughly this order:

### Step 1 — neutralise the system IME for the test runner

Ship a noop `InputMethodService` stub (debug build only) inside the
tawc app — `me.phie.tawc.test.NoopInputMethodService` or similar — that
declares itself as an IME, accepts the binding, and does nothing on
any IC method. Then the integration test runner's bring-up steps add:

```bash
adb shell ime enable me.phie.tawc/.test.NoopInputMethodService
adb shell ime set    me.phie.tawc/.test.NoopInputMethodService
```

…before launching the compositor, and reverts on teardown.

This removes the third-party reactor entirely and makes the bypass
tests deterministic on every device, regardless of which IME the user
has installed for daily use. Cross-device flakiness from the
"updateSelection → IME's defensive finishComposingText" race
disappears because there is no IME to react.

`input-test-flaky-on-emulator.md` (which describes the race on the
AOSP latin IME) is closed by this step.

### Step 2 — actually exercise `TawcInputConnection`

The `IC_*` broadcasts (`IC_COMMIT_TEXT`, `IC_SET_COMPOSING_TEXT`,
`IC_SET_COMPOSING_REGION`, `IC_FINISH_COMPOSING`, `IC_SET_SELECTION`)
already exist in `CompositorActivity.kt` and call the active
`TawcInputConnection`'s methods directly. They were marked "may be
racy with the system IME's reactions" — Step 1 makes them safe.

Convert the bulk of the input test suite to use the `IC_*` broadcasts.
Asserting both the wayland-side observable (GTK's `TEXT_CHANGED` /
`PREEDIT` events) AND the IC-side state (`Editable` contents,
composing span via `BaseInputConnection.getComposingSpanStart/End`,
selection cursor) so a regression in either layer fails the test.
Expose the IC state to the test via a debug broadcast that returns
`Editable.toString()` + composing range + selection range, or via a
logcat readout the test parses.

Concrete scenes to convert / add:

- `scene_compose_lifecycle` → drive via `IC_SET_COMPOSING_TEXT`; assert
  Editable's composing region matches the preedit at each step.
- New scene: Gboard-style commit-replace — `IC_SET_COMPOSING_TEXT
  "teh"` then `IC_COMMIT_TEXT "the "` — assert the wire pairs
  `delete_before=3` with the commit (currently uncovered).
- New scene: OpenBoard's per-Enter pattern —
  `IC_COMMIT_TEXT "hello"`, `IC_SET_COMPOSING_REGION 0..5`,
  `IC_COMMIT_TEXT "hello"`, `IC_COMMIT_TEXT "\n"` — assert no extra
  characters get prepended (regression test for `7ad5869`).
- New scene: setComposingRegion-then-commit replaces committed bytes,
  not preedit overlay.

### Step 3 — keep a small "wayland-protocol-only" test set

Keep the bypass broadcasts (`TEXT_INPUT` etc.) and use them
deliberately for a small set of tests that genuinely want to assert
"the compositor's text-input-v3 done-ordering is correct given these
exact protocol events", independently of IC. Label the file / module
clearly so the distinction stays visible (e.g. move them to
`tests/wayland_text_input.rs` and rename `input.rs` to
`tests/input_method.rs` once it actually tests the input method
layer).

## Out of scope for this issue

- Real-IME-reactivity coverage. The point of Step 1 is to *remove* the
  system IME from the test loop because it's a non-deterministic
  third-party. A separate, smaller test suite that explicitly drives
  Gboard-via-instrumentation could come later to cover "the compositor
  behaves correctly when a real IME pings IMM at the wrong moment",
  but it should be a focused regression suite, not the workhorse.
- Multi-window / multi-activity input dispatch. Already a separate
  problem.

## References

- `notes/text-input.md` — the IC ↔ wayland-text-input-v3 bridge design.
- `app/src/main/java/me/phie/tawc/compositor/TawcInputConnection.kt`
  — header comment goes through the Android↔wayland model mismatch
  in detail.
- `app/src/main/java/me/phie/tawc/compositor/CompositorActivity.kt:93`
  — `testInputReceiver` (both bypass and `IC_*` paths).
- `compositor/src/text_input.rs:518` — bypass-side
  `TextInputEvent::CommitString` handler.
- `tests/integration/tests/input.rs` — the test suite this issue is
  about.
- `input-test-flaky-on-emulator.md` — predecessor that this issue
  subsumes; documents the failure mode without identifying the
  underlying architecture problem.
