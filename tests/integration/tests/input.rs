//! Input dispatch tests. Drive the gtk4-debug-app through compositor input
//! paths (text-input-v3 commits, key events, touch taps) and assert the
//! debug app observes the expected client-side behaviour.
//!
//! All scenarios share a single GTK4 app instance — each one resets the
//! buffer via the compositor's own `delete_surrounding_text` path, then
//! exercises a distinct dispatch behaviour. Per-scenario startup of GTK is
//! the dominant cost in this suite, so consolidation cuts ~14× the runtime.
//!
//! Deliberately avoid asserting anything about buffer types (AHB vs SHM):
//! the point is how the compositor *dispatches input*, not how clients
//! render. That keeps these tests safe to run on the emulator (where
//! libhybris/AHB is unavailable).

use std::thread;
use std::time::{Duration, Instant};

use tawc_integration::adb;
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    assert_compositor_clean, start_text_input, start_text_input_no_surrounding, TIMEOUT,
};

// Physical screen coordinates for tapping inside the text view.
// Compositor uses 2x scale, so logical = physical / 2.
// Text content starts at approximately physical (80, 234).
// The actual char width depends on font size set in gtk4-debug-app.c —
// each scenario only asserts cursor lies somewhere in the middle of the
// typed string, so the tap can land within a wide range of columns.
const TAP_TEXT_MID_X: u32 = 200;
const TAP_TEXT_MID_Y: u32 = 250;
// Just inside the left edge of the text content (text starts at ~x=80).
// Tapping here should land the cursor at position 0 — matches the user's
// "click at the very start of the line" repro.
const TAP_TEXT_START_X: u32 = 85;

/// Env passed to gtk4-debug-app for input tests. Currently empty — i.e.
/// GTK4 uses its default GSK renderer (Vulkan/GL via libhybris on device).
/// We'd rather use `GSK_RENDERER=cairo` so this group is buffer-path
/// independent and runs on the emulator too, but the cairo path currently
/// dies right after the first frame with "the target surface has been
/// finished" — see issues/gtk4-cairo-renderer-broken.md.
const INPUT_ENV: &str = "";

/// Reset the GTK4 buffer + preedit between scenarios so we don't have to
/// pay GTK startup per scenario. Goes through the compositor-bypass path
/// (NativeBridge.nativeFinishComposingText / nativeDeleteSurroundingText)
/// so it doesn't perturb TawcInputConnection state — the inbound
/// `set_surrounding_text` round-trip resets `composingRegionIsPreedit` and
/// `lastSyncedCursor` cleanly.
fn reset_buffer(app: &DebugApp) {
    // Step 1: clear any active preedit. If the preedit was non-empty the
    // compositor commits it first (per text-input-v3 done ordering);
    // we'll delete the resulting bytes in step 2.
    adb::input_finish_composing().expect("reset: finish_composing");
    // Wait long enough for any commit_string -> client commit ->
    // surrounding_text round-trip to land back at the debug app.
    thread::sleep(Duration::from_millis(250));

    // Step 2: delete buffer contents. `input_delete_surrounding` translates
    // to that-many Backspace + Forward-Delete key events on wl_keyboard, so
    // bound the counts to the actual buffer length — flooding GTK with
    // thousands of stray no-op keypresses kills the IM connection. The
    // buffer length is a safe upper bound on how many key events we need
    // regardless of where the cursor sits.
    let current = app.last_text().unwrap_or_default();
    if !current.is_empty() {
        let len = current.encode_utf16().count() as u32;
        adb::input_delete_surrounding(len, len).expect("reset: delete_surrounding");
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            if app.last_text().unwrap_or_default().is_empty() {
                break;
            }
            thread::sleep(Duration::from_millis(50));
        }
        let after = app.last_text().unwrap_or_default();
        assert!(
            after.is_empty(),
            "reset_buffer left buffer = {:?}",
            after
        );
    }

    // Step 3: sanity check — preedit overlay is also clear before next
    // scenario starts. Stale preedit would corrupt subsequent assertions.
    let preedit = app.last_preedit().unwrap_or_default();
    assert!(
        preedit.is_empty(),
        "reset_buffer left preedit = {:?}",
        preedit
    );
}

// --- Scenario helpers -------------------------------------------------------
// Each helper exercises one dispatch behaviour. They each assume the buffer
// starts empty (call `reset_buffer` before invoking). Panics propagate up to
// the single `#[test]` so the failing scenario is identified by the function
// name in the backtrace.

/// Compositor-bypass commit + key event path: typing, backspace, then
/// `deleteSurroundingText` for IME-driven trimming.
fn scene_basic_input_and_delete(app: &DebugApp) {
    // Type multi-word text (covers basic input + spaces).
    adb::input_text("hello world").expect("commit 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("text 'hello world'");

    // Backspace via wl_keyboard.
    adb::input_keyevent(adb::KEYCODE_DEL).expect("backspace");
    app.wait_for_text("hello worl", TIMEOUT)
        .expect("'hello worl' after backspace");

    // Restore so the delete_surrounding part below has a known buffer.
    adb::input_text("d").expect("commit 'd'");
    app.wait_for_text("hello world", TIMEOUT).expect("restored");

    // deleteSurroundingText path (Gboard's word-trim / suggestion-delete).
    adb::input_delete_surrounding(5, 0).expect("delete_surrounding");
    app.wait_for_text("hello ", TIMEOUT)
        .expect("'hello ' after delete_surrounding(5, 0)");
}

/// Compose-loop preedit progresses letter-by-letter, isn't visible in the
/// buffer until finishComposingText commits it. Also covers
/// "compose-after-committed-text" — the preedit doesn't disturb the
/// already-committed prefix.
fn scene_compose_lifecycle(app: &DebugApp) {
    // Build a committed prefix so we can verify the preedit doesn't disturb
    // it (covers the old test_compose_after_committed_text).
    adb::input_text("hello ").expect("commit 'hello '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    for prefix in ["w", "wo", "wor", "worl", "world"] {
        adb::input_set_composing(prefix).expect("setComposingText");
        app.wait_for_preedit(prefix, TIMEOUT)
            .unwrap_or_else(|e| panic!("preedit not '{}': {}", prefix, e));
        // The committed buffer must remain "hello " throughout — preedit is
        // cursor-relative overlay, never document content until finish.
        assert_eq!(
            app.last_text().as_deref().unwrap_or(""),
            "hello ",
            "preedit '{}' leaked into the buffer",
            prefix
        );
    }

    adb::input_finish_composing().expect("finishComposingText");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world' after finishComposingText");
    app.wait_for_preedit("", TIMEOUT)
        .expect("preedit cleared after finishComposingText");
}

/// Autocorrect / replacement-on-commit: the commit string replaces the
/// active preedit (no `tehthe`).
fn scene_autocorrect_replaces_preedit(app: &DebugApp) {
    adb::input_set_composing("teh").expect("setComposingText 'teh'");
    app.wait_for_preedit("teh", TIMEOUT).expect("preedit 'teh'");

    // Per text-input-v3 done ordering, commit_string replaces the existing
    // preedit at the cursor — the user must see "the ", not "tehthe ".
    adb::input_text("the ").expect("commit 'the '");
    app.wait_for_text("the ", TIMEOUT)
        .expect("'the ' (autocorrect replaced 'teh')");
    app.wait_for_preedit("", TIMEOUT)
        .expect("preedit cleared after the commit");
}

/// Click cursor positioning + behaviour at the cursor: backspace deletes
/// from cursor (not end), commit_string inserts at cursor (not end), and a
/// preedit-then-finish round-trip places the new char at the click site.
fn scene_click_cursor_positioning(app: &DebugApp) {
    adb::input_text("abcdef").expect("commit 'abcdef'");
    app.wait_for_text("abcdef", TIMEOUT).expect("'abcdef'");

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("CURSOR_POS event after tap");
    assert!(
        cursor > 0 && cursor < 6,
        "cursor should be in middle of 'abcdef', got {}",
        cursor
    );
    // Tap must not change the text contents.
    assert_eq!(
        app.last_text().as_deref(),
        Some("abcdef"),
        "tap changed text"
    );

    // Backspace at cursor — deletes char at index `cursor-1`, not the last.
    let change_count = app.text_changed_count();
    adb::input_keyevent(adb::KEYCODE_DEL).expect("backspace at cursor");
    let after_bs = app
        .wait_for_text_change(change_count, TIMEOUT)
        .expect("TEXT_CHANGED after backspace");
    assert_eq!(
        after_bs.len(),
        5,
        "expected 5 chars after one backspace from 'abcdef', got {:?}",
        after_bs
    );
    assert_ne!(
        after_bs, "abcde",
        "backspace deleted from end, not cursor: {:?}",
        after_bs
    );

    // Commit at cursor via the compose+finish path. This also verifies
    // single-char preedit + finish (the old test_compose_after_cursor_move).
    let cursor_after_bs = cursor - 1; // backspace shifted cursor left by one
    adb::input_set_composing("X").expect("setComposingText 'X'");
    app.wait_for_preedit("X", TIMEOUT).expect("preedit 'X'");
    adb::input_finish_composing().expect("finishComposingText");

    let deadline = Instant::now() + TIMEOUT;
    let mut last = String::new();
    while Instant::now() < deadline {
        last = app.last_text().unwrap_or_default();
        if last.len() == 6 && last.contains('X') {
            break;
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(
        last.len(),
        6,
        "expected len 6 after inserting 'X' into 5-char buffer, got {:?}",
        last
    );
    let x_pos = last.find('X').expect("'X' must appear in buffer");
    assert_eq!(
        x_pos, cursor_after_bs as usize,
        "'X' should sit at cursor position {}, got pos {} in {:?}",
        cursor_after_bs, x_pos, last
    );
    // Sanity: not appended at end (the old test_click_cursor_positioning's
    // final assertion).
    assert!(
        !last.ends_with('X') || last.len() != 6 || x_pos == 5,
        "'X' was appended at end instead of inserted at cursor: {:?}",
        last
    );
}

/// `setComposingText` with delete-before/after deltas — Gboard's
/// `setComposingRegion(s,e)` + `setComposingText("...")` "tap to retype"
/// flow. The marked committed bytes get deleted before the new preedit
/// is set; without the fix, both the original word and the preedit would
/// coexist (the "hellohello" duplicate-text bug).
fn scene_compose_region_replaces_committed_text(app: &DebugApp) {
    adb::input_text("hello world").expect("commit 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change after tap");
    assert!(
        cursor > 0 && cursor < 11,
        "cursor mid 'hello world', got {}",
        cursor
    );

    // Replace `cursor` chars before the cursor with preedit "HELLO".
    adb::input_set_composing_with_delete("HELLO", cursor, 0)
        .expect("setComposingText with delete");
    app.wait_for_preedit("HELLO", TIMEOUT)
        .expect("preedit 'HELLO'");
    let committed_now = app.last_text().expect("text after replace");
    assert!(
        !committed_now.starts_with("hello"),
        "replacement should have deleted committed bytes; got {:?}",
        committed_now
    );

    adb::input_finish_composing().expect("finishComposingText");
    let deadline = Instant::now() + TIMEOUT;
    let mut last = String::new();
    while Instant::now() < deadline {
        last = app.last_text().unwrap_or_default();
        if last.contains("HELLO") {
            break;
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert!(
        last.contains("HELLO"),
        "replacement HELLO missing from buffer: {:?}",
        last
    );
    assert_eq!(
        last.matches("hello").count(),
        0,
        "original 'hello' should have been replaced; got {:?}",
        last
    );
}

/// Same as the preedit version but committing directly — Gboard's
/// autocorrect-replace flow. `commitText` over a composing region must
/// also delete the original bytes.
fn scene_commit_text_replaces_composing_region(app: &DebugApp) {
    adb::input_text("hello world").expect("commit 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change after tap");
    assert!(
        cursor > 0 && cursor < 11,
        "cursor mid, got {}",
        cursor
    );

    adb::input_text_with_delete("FOO", cursor, 0).expect("commitText replacement");
    let deadline = Instant::now() + TIMEOUT;
    let mut last = String::new();
    while Instant::now() < deadline {
        last = app.last_text().unwrap_or_default();
        if last.contains("FOO") && !last.contains("hello") {
            break;
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert!(
        last.contains("FOO"),
        "replacement FOO missing from buffer: {:?}",
        last
    );
    assert_eq!(
        last.matches("hello").count(),
        0,
        "original 'hello' should have been replaced; got {:?}",
        last
    );
}

/// Regression: stale preedit must not be re-committed when the user clicks
/// elsewhere. Without the fix, `current_preedit` was still set after the
/// `cause=other` round-trip, so a later `finishComposingText` would commit
/// the preedit again at the new cursor — `hellohello` in the buffer.
fn scene_finish_composing_after_click_no_duplicate(app: &DebugApp) {
    adb::input_set_composing("hello").expect("setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT).expect("preedit 'hello'");

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    app.wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("cursor change");

    // Compositor's touch handler clears `current_preedit` so finish is a
    // no-op on the wire. Without the fix, "hello" gets re-committed.
    adb::input_finish_composing().expect("finishComposingText");
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.matches("hello").count() <= 1,
        "finishComposingText after click duplicated the preedit; got {:?}",
        text
    );
}

/// Regression: clicking with a pending preedit must commit the typed text
/// (so it's not silently lost) AND clear the preedit overlay (so it doesn't
/// follow the cursor visually). Tap lands in the middle of committed text.
fn scene_click_during_preedit_commits_pending_text(app: &DebugApp) {
    adb::input_text("world").expect("commit 'world'");
    app.wait_for_text("world", TIMEOUT).expect("'world'");

    adb::input_set_composing("hello").expect("setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT).expect("preedit 'hello'");
    assert_eq!(
        app.last_text().as_deref().unwrap_or(""),
        "world",
        "preedit must not appear in the buffer before any commit"
    );

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    app.wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("cursor change after tap");

    // Compositor finalises any active preedit on Touch::Down before the
    // touch reaches the client.
    app.wait_for_preedit("", TIMEOUT)
        .expect("preedit should be cleared by the click");
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.contains("world"),
        "originally-committed 'world' was lost; got {:?}",
        text
    );
    assert!(
        text.contains("hello"),
        "pending preedit 'hello' was dropped on cursor move; got {:?}",
        text
    );
}

/// Regression: same Touch::Down preedit-finalise, but the tap lands at the
/// start of the line. Bug repro — the in-progress word vanished entirely
/// when the cursor jumped to position 0.
fn scene_click_at_start_during_preedit_preserves_pending_text(app: &DebugApp) {
    adb::input_text("hello ").expect("commit 'hello '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    adb::input_set_composing("world").expect("setComposingText 'world'");
    app.wait_for_preedit("world", TIMEOUT).expect("preedit 'world'");
    assert_eq!(
        app.last_text().as_deref().unwrap_or(""),
        "hello ",
        "preedit must not appear in the buffer before any commit"
    );

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_START_X, TAP_TEXT_MID_Y).expect("tap line start");
    let cursor = app
        .wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("cursor change after tap");
    assert_eq!(
        cursor, 0,
        "tap at line start should land cursor at 0, got {}",
        cursor
    );
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.contains("world"),
        "pending preedit 'world' was dropped on cursor move; got {:?}",
        text
    );
}

/// Full integration: build "hello world" via two compose loops, click in the
/// middle, compose another word at the new cursor. Catches regressions in
/// the end-to-end Gboard flow that simple per-feature scenarios miss.
fn scene_full_compose_loop_with_click_in_middle(app: &DebugApp) {
    // Word 1.
    for prefix in ["h", "he", "hel", "hell", "hello"] {
        adb::input_set_composing(prefix).expect("setComposingText");
    }
    adb::input_finish_composing().expect("finish word 1");
    adb::input_text(" ").expect("commit ' '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    // Word 2.
    for prefix in ["w", "wo", "wor", "worl", "world"] {
        adb::input_set_composing(prefix).expect("setComposingText");
    }
    adb::input_finish_composing().expect("finish word 2");
    app.wait_for_text("hello world", TIMEOUT).expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change");
    assert!(
        cursor > 0 && cursor < 11,
        "cursor mid, got {}",
        cursor
    );

    for prefix in ["x", "xy", "xyz"] {
        adb::input_set_composing(prefix).expect("setComposingText 'xyz' loop");
    }
    adb::input_finish_composing().expect("finish word 3");

    let deadline = Instant::now() + TIMEOUT;
    let mut last = String::new();
    while Instant::now() < deadline {
        last = app.last_text().unwrap_or_default();
        if last.len() == 14 && last.contains("xyz") {
            break;
        }
        thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(
        last.len(),
        14,
        "expected len 14 (11 + 'xyz'), got {:?} (len {})",
        last,
        last.len()
    );
    let xyz_pos = last.find("xyz").expect("'xyz' in text");
    assert_eq!(
        xyz_pos, cursor as usize,
        "'xyz' should land at cursor pos {} but went to {} in {:?}",
        cursor, xyz_pos, last
    );
    // The original words may legitimately split when the cursor is mid-word
    // — assert only that nothing got duplicated.
    assert!(
        last.matches("hello").count() <= 1,
        "'hello' duplicated in {:?}",
        last
    );
    assert!(
        last.matches("world").count() <= 1,
        "'world' duplicated in {:?}",
        last
    );
}

/// IC delta-computation: `<word><space><backspace>` then
/// `setComposingRegion + commitText("<word> ")` must replace the marked
/// region rather than appending. Without the IC's pre-commit Backspace
/// emission the wire commit had nothing deleting the marked region, so
/// the buffer ended up `hellohello `.
fn scene_ic_space_backspace_space_no_duplicate(app: &DebugApp) {
    // 1. Compose "hello" via the IC.
    adb::ic_set_composing_text("hello").expect("ic setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT).expect("preedit 'hello'");

    // 2. Space: commitText("hello ", 1) replaces preedit + adds space.
    let change_count = app.text_changed_count();
    adb::ic_commit_text("hello ").expect("ic commitText 'hello '");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after 'hello '");

    // 3. Backspace: deletes the trailing space.
    let change_count = app.text_changed_count();
    adb::input_keyevent(adb::KEYCODE_DEL).expect("backspace");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after backspace");
    let after_bs = app.last_text().unwrap_or_default();
    assert_eq!(
        after_bs.trim_end_matches(' ').len(),
        5,
        "after backspace buffer should be 'hello'; got {:?}",
        after_bs
    );

    // Let the round-trip sync the Editable so step 4 sees up-to-date state.
    thread::sleep(Duration::from_millis(200));

    // 4. setComposingRegion(0, 5): mark "hello" composing on the Editable.
    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    thread::sleep(Duration::from_millis(150));

    // 5. Second space: commitText("hello ", 1). With the fix, the IC
    //    computes (5, 0) deltas (region was set by setComposingRegion =
    //    committed text on the Wayland side), wire is
    //    Backspace×5 (wl_keyboard) + commit_string("hello ") (text-input).
    let change_count = app.text_changed_count();
    adb::ic_commit_text("hello ").expect("ic commitText 'hello ' over region");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after second space");
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert_eq!(
        text.matches("hello").count(),
        1,
        "'hello' duplicated — got {:?} (len {}). The IC didn't delete the \
         composing region before commitText, so the new 'hello ' was \
         appended instead of replacing the marked one.",
        text,
        text.len()
    );
}

/// IC commit with a diverged Editable cursor: setSelection moves the
/// Editable's cursor under us without any Wayland-side equivalent. The IC
/// must detect the divergence and refuse to propagate composing-region
/// deltas; otherwise the wire `delete_surrounding_text(N)` slices N bytes
/// from the wrong position.
fn scene_ic_commit_after_diverged_cursor_no_byte_slicing(app: &DebugApp) {
    // Build "hello world" via the IC so lastSyncedCursor settles to 11.
    adb::ic_commit_text("hello world").expect("ic commitText 'hello world'");
    app.wait_for_text("hello world", TIMEOUT).expect("'hello world'");
    thread::sleep(Duration::from_millis(200));

    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    adb::ic_set_selection(5, 5)
        .expect("ic setSelection 5..5 (diverges Editable from Wayland)");

    let change_count = app.text_changed_count();
    adb::ic_commit_text("X").expect("ic commitText 'X' (diverged)");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after the IME's commit");
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.contains("world"),
        "'world' was sliced by a diverged-cursor wire delete — got {:?}. \
         The IC propagated composing-region deltas while the Editable's \
         cursor was out of sync with the Wayland buffer's cursor.",
        text
    );
}

/// IC re-commit-word + newline (OpenBoard's per-Enter pattern). Each Enter
/// fires `setComposingRegion(0, N)` + `commitText(word, 1)` +
/// `commitText("\n", 1)`. Without the short-circuit fix, the second iteration
/// onward sliced bytes off the buffer (visible as a stray "h" prepended on
/// each Enter).
fn scene_ic_recommit_word_then_newline_no_h_prepend(app: &DebugApp) {
    adb::ic_commit_text("hello").expect("ic commitText 'hello'");
    app.wait_for_text("hello", TIMEOUT).expect("'hello'");
    thread::sleep(Duration::from_millis(200));

    for i in 1..=3 {
        let change_count = app.text_changed_count();
        adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
        adb::ic_commit_text("hello").expect("ic commitText 'hello' (re-commit)");
        adb::ic_commit_text("\n").expect("ic commitText '\\n'");
        app.wait_for_text_change(change_count, TIMEOUT)
            .expect("text change after Enter");
        thread::sleep(Duration::from_millis(250));

        // The debug app escapes '\n' as the two-char sequence `\n` so the
        // protocol stays single-line — assert against that escaped form.
        let expected = format!("hello{}", "\\n".repeat(i));
        let text = app.last_text().unwrap_or_default();
        assert_eq!(
            text, expected,
            "Enter #{i}: expected buffer {:?} but got {:?}. Without the fix \
             the IC propagates composing-region deltas, slicing bytes off \
             the buffer (visible as a stray 'h' prepended on each Enter).",
            expected, text
        );
    }
}

// --- The single test --------------------------------------------------------

/// One #[test] that drives the full input-dispatch suite. Opening
/// gtk4-debug-app dominates the per-test cost (>1s of GTK startup +
/// libhybris GL setup), so consolidating amortises it across all
/// scenarios. Each scenario is a separate function so a panic identifies
/// exactly where coverage broke.
#[test]
fn test_input_dispatch() {
    let mut app = start_text_input(INPUT_ENV);

    // 1. Bypass-IC compositor dispatch: type/backspace/delete_surrounding.
    scene_basic_input_and_delete(&app);
    reset_buffer(&app);

    // 2. Compose lifecycle: preedit-not-in-buffer + finish-commits.
    scene_compose_lifecycle(&app);
    reset_buffer(&app);

    // 3. Autocorrect: commit replaces preedit (text-input-v3 done ordering).
    scene_autocorrect_replaces_preedit(&app);
    reset_buffer(&app);

    // 4. Click cursor positioning: backspace/insert at cursor, not end.
    scene_click_cursor_positioning(&app);
    reset_buffer(&app);

    // 5. setComposingRegion + setComposingText replaces committed bytes.
    scene_compose_region_replaces_committed_text(&app);
    reset_buffer(&app);

    // 6. setComposingRegion + commitText replaces committed bytes.
    scene_commit_text_replaces_composing_region(&app);
    reset_buffer(&app);

    // 7. Click clears stale preedit so finishComposing doesn't duplicate.
    scene_finish_composing_after_click_no_duplicate(&app);
    reset_buffer(&app);

    // 8. Click during preedit (mid) commits pending text and clears overlay.
    scene_click_during_preedit_commits_pending_text(&app);
    reset_buffer(&app);

    // 9. Click during preedit (line start) doesn't lose the typed word.
    scene_click_at_start_during_preedit_preserves_pending_text(&app);
    reset_buffer(&app);

    // 10. End-to-end Gboard flow: compose, click mid, compose, finish.
    scene_full_compose_loop_with_click_in_middle(&app);
    reset_buffer(&app);

    // 11. IC delta computation: setComposingRegion + commitText replaces.
    scene_ic_space_backspace_space_no_duplicate(&app);
    reset_buffer(&app);

    // 12. IC divergence guard: setSelection without protocol equivalent.
    scene_ic_commit_after_diverged_cursor_no_byte_slicing(&app);
    reset_buffer(&app);

    // 13. IC short-circuit: re-commit equal to marked region is a no-op.
    scene_ic_recommit_word_then_newline_no_h_prepend(&app);

    app.stop().expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// A surrounding-less text-input client (the shape of VTE-under-Wayland
/// and any terminal-like client that enables text-input-v3 for the soft
/// keyboard but holds no editable buffer behind the surface) must:
///   1. Receive `commit_string` for IME-committed text — text-input-v3
///      `enable` + `commit_string` works without `set_surrounding_text`.
///   2. Receive Backspace as a real `wl_keyboard` key event, NOT as a
///      `delete_surrounding_text` protocol event. The protocol's "current
///      cursor index" is undefined for surrounding-less clients; sending
///      `delete_surrounding_text` to one (lxterminal/VTE was the original
///      reproducer) closes the connection.
///
/// This test is the regression guard for the "lxterminal disappears on
/// the first backspace" bug. The harness watches the bare GtkIMContext
/// signals — `commit` for the space, `delete-surrounding` (which must
/// never fire), and the `wl_keyboard` Backspace via GtkEventControllerKey.
#[test]
fn test_surroundingless_client_uses_keyboard_for_backspace() {
    let mut app = start_text_input_no_surrounding(INPUT_ENV);

    // Sanity: the IM context never asked us for surrounding text on its
    // own and we never volunteered it — confirms we're exercising the
    // surrounding-less path.
    assert_eq!(
        app.count_with_tag("DELETE_SURROUNDING"),
        0,
        "DELETE_SURROUNDING fired before we even sent input"
    );

    // 1. Space arrives as a commit_string. The compositor's commit_string
    // path is independent of surrounding text, so this should just work.
    adb::input_text(" ").expect("commit ' '");
    app.wait_for_tag_value("COMMIT", " ", TIMEOUT)
        .expect("commit_string ' ' did not reach the IM context");

    // 2. Gboard-style backspace: deleteSurroundingText(1, 0). The IC
    // translates this to a Backspace key event before crossing JNI; the
    // compositor must therefore deliver Backspace via wl_keyboard, NOT
    // a delete_surrounding_text protocol event.
    adb::input_delete_surrounding(1, 0).expect("delete_surrounding(1, 0)");
    app.wait_for_tag_value("KEY", "BackSpace", TIMEOUT)
        .expect("Backspace key did not reach the client");

    // 3. The whole point: no delete_surrounding_text on the wire ever.
    // If this fires, the compositor is regressing back to the lxterminal
    // crash path.
    assert_eq!(
        app.count_with_tag("DELETE_SURROUNDING"),
        0,
        "compositor sent delete_surrounding_text to a surrounding-less \
         client — this would close real-world clients (lxterminal/VTE) \
         on receipt"
    );

    app.stop().expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}
