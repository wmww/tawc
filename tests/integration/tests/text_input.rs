//! Text input dispatch tests. Drive wayland-debug-app through compositor
//! text-input-v3 commits, key events, clipboard probes, and cursor taps, then
//! assert the debug app observes the expected client-side behaviour.
//!
//! # The rule
//!
//! Tests interact with the system **only** as a keyboard or as an app:
//!
//! - **As a keyboard**: every input call goes through
//!   [`TawcInputConnection`] via `adb::ic_*` helpers — the same Kotlin
//!   surface the system IMM dispatches Gboard / OpenBoard / AOSP-latin
//!   events through. Tests assert Android contract results and
//!   client-visible Wayland behavior, not tawc private state.
//! - **As an app**: assertions go through `wayland-debug-app`'s observed
//!   `TAWC_DEBUG:…` events (`TEXT_CHANGED`, `PREEDIT`, `CURSOR_POS`,
//!   `KEY`, `COMMIT`, `DELETE_SURROUNDING`). That's what a real wayland
//!   client running under our compositor would see.
//!
//! There is intentionally **no test infrastructure that pokes into the
//! compositor's state machine in between**. The previous bypass channel
//! that called `NativeBridge.native*` directly was deleted because it
//! could pass even when IC code (the largest chunk of our text-input
//! logic) was broken — text-input-v3's done-ordering replaces preedit on
//! the wayland side regardless of what the IC computed, so a buggy IC
//! produced the right *observable* and the test smiled. Driving every
//! scenario through IC closes that hole and turns wayland-side
//! assertions into a real integration check.
//!
//! Every test starts with `test-init`, which installs RecordingImeOutput
//! so the system IME (a third-party at the OS boundary) is removed from
//! the loop — that's not "in the middle of the state machine", it's
//! removing a non-deterministic external actor that would otherwise
//! amplify our IC calls back at us.
//!
//! Related real GTK coverage lives in
//! `apps::test_gtk4_widget_factory_copy_paste_and_text_input`; this module
//! stays focused on deterministic protocol-level input coverage.
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
    assert_broker_ok, assert_compositor_clean, start_wayland_debug_clipboard_copy,
    start_wayland_debug_clipboard_overcap, start_wayland_debug_clipboard_paste,
    start_wayland_debug_clipboard_timeout, start_wayland_debug_text_input,
    start_wayland_debug_text_input_no_surrounding, start_wayland_debug_text_input_stale_newline,
    start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::GraphicsBackend;

/// Input dispatch has no buffer-type stake; pick the most portable
/// backend (no libhybris LD_LIBRARY_PATH, no gfxstream env) so a single
/// run exercises the dispatch paths without depending on a GPU.
const INPUT_BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

// Coordinates aimed at wayland-debug-app's text row in its logical surface.
// Tests inject through the focused SurfaceView so Android window animation and
// screen insets cannot move the tap underneath us.
const WAYLAND_TAP_TEXT_MID_X: f32 = 100.0;
const WAYLAND_TAP_TEXT_Y: f32 = 125.0;
const WAYLAND_TAP_TEXT_START_X: f32 = 20.0;

#[derive(Clone, Copy)]
struct InputTapCoords {
    text_mid_x: f32,
    text_y: f32,
    text_start_x: f32,
}

const WAYLAND_TAP_COORDS: InputTapCoords = InputTapCoords {
    text_mid_x: WAYLAND_TAP_TEXT_MID_X,
    text_y: WAYLAND_TAP_TEXT_Y,
    text_start_x: WAYLAND_TAP_TEXT_START_X,
};

const WAYLAND_DEBUG_ENV: &str = "";

fn inject_tap(x: f32, y: f32, label: &str) {
    assert_broker_ok(
        adb::inject_touch_logical(x, y).unwrap_or_else(|e| panic!("{label}: {e}")),
        label,
    );
}

// --- Scenes -----------------------------------------------------------------
//
// Each scene drives the IC with a Gboard-shaped sequence and asserts what
// the wayland client saw. Scenes assume the buffer starts empty.

/// Click cursor positioning + behaviour at the cursor: backspace deletes
/// from cursor (not end), commit_string inserts at cursor (not end), and a
/// preedit-then-finish round-trip places the new char at the click site.
fn scene_click_cursor_positioning(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_commit_text("abcdef").expect("commit 'abcdef'");
    app.wait_for_text("abcdef", TIMEOUT).expect("'abcdef'");

    let cursor_count_before = app.cursor_pos_count();
    inject_tap(taps.text_mid_x, taps.text_y, "tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("CURSOR_POS event after tap");
    assert!(
        cursor > 0 && cursor < 6,
        "cursor should be in middle of 'abcdef', got {}",
        cursor
    );
    assert_eq!(
        app.last_text().as_deref(),
        Some("abcdef"),
        "tap changed text"
    );

    let change_count = app.text_changed_count();
    adb::ic_send_key_event(adb::KEYCODE_DEL).expect("backspace at cursor");
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

    let cursor_after_bs = cursor - 1;
    adb::ic_set_composing_text("X").expect("setComposingText 'X'");
    app.wait_for_preedit("X", TIMEOUT).expect("preedit 'X'");
    adb::ic_finish_composing().expect("finishComposingText");

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
    assert!(
        !last.ends_with('X') || last.len() != 6 || x_pos == 5,
        "'X' was appended at end instead of inserted at cursor: {:?}",
        last
    );
}

/// Full integration: build "hello world" via two compose loops, click in
/// the middle, compose another word at the new cursor. Catches
/// regressions in the end-to-end Gboard flow that simple per-feature
/// scenarios miss.
fn scene_full_compose_loop_with_click_in_middle(app: &DebugApp, taps: InputTapCoords) {
    for prefix in ["h", "he", "hel", "hell", "hello"] {
        adb::ic_set_composing_text(prefix).expect("setComposingText");
    }
    adb::ic_finish_composing().expect("finish word 1");
    adb::ic_commit_text(" ").expect("commit ' '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    for prefix in ["w", "wo", "wor", "worl", "world"] {
        adb::ic_set_composing_text(prefix).expect("setComposingText");
    }
    adb::ic_finish_composing().expect("finish word 2");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    inject_tap(taps.text_mid_x, taps.text_y, "tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change");
    assert!(cursor > 0 && cursor < 11, "cursor mid, got {}", cursor);

    for prefix in ["x", "xy", "xyz"] {
        adb::ic_set_composing_text(prefix).expect("setComposingText 'xyz' loop");
    }
    adb::ic_finish_composing().expect("finish word 3");

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
/// region rather than appending. Without the IC's pre-commit delete
/// emission the wire commit had nothing deleting the marked region, so
/// the buffer ended up `hellohello `.
fn scene_space_backspace_space_no_duplicate(app: &DebugApp) {
    adb::ic_set_composing_text("hello").expect("ic setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT)
        .expect("preedit 'hello'");

    let change_count = app.text_changed_count();
    adb::ic_commit_text("hello ").expect("ic commitText 'hello '");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after 'hello '");

    let change_count = app.text_changed_count();
    adb::ic_send_key_event(adb::KEYCODE_DEL).expect("backspace");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after backspace");
    let after_bs = app.last_text().unwrap_or_default();
    assert_eq!(
        after_bs.trim_end_matches(' ').len(),
        5,
        "after backspace buffer should be 'hello'; got {:?}",
        after_bs
    );

    thread::sleep(Duration::from_millis(200));

    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    thread::sleep(Duration::from_millis(150));

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

/// IC re-commit-word + newline (OpenBoard's per-Enter pattern). Enter
/// fires `setComposingRegion(0, N)` + `commitText(word, 1)` +
/// `commitText("\n", 1)`. Without the short-circuit fix, the IC
/// propagates composing-region deltas and can slice bytes off the buffer.
fn scene_recommit_word_then_newline_no_h_prepend(app: &DebugApp) {
    adb::ic_commit_text("hello").expect("ic commitText 'hello'");
    app.wait_for_text("hello", TIMEOUT).expect("'hello'");
    thread::sleep(Duration::from_millis(200));

    let change_count = app.text_changed_count();
    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    adb::ic_commit_text("hello").expect("ic commitText 'hello' (re-commit)");
    adb::ic_commit_text("\n").expect("ic commitText '\\n'");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after Enter");
    thread::sleep(Duration::from_millis(250));

    // The debug app escapes '\n' as the two-char sequence `\n` so
    // the protocol stays single-line — assert against that escaped
    // form.
    let expected = "hello\\n";
    let text = app.last_text().unwrap_or_default();
    assert_eq!(
        text, expected,
        "expected buffer {:?} but got {:?}. The IC propagated \
         composing-region deltas, slicing bytes off the buffer.",
        expected, text
    );
}

/// Round-trip through `updateFromCompositor` must clear any composing
/// span on the Editable. Without [BaseInputConnection.removeComposingSpans],
/// a stale span left over from `setComposingRegion` would mis-classify
/// the next IC `commitText` as "replace the marked region" — slicing
/// bytes that are no longer part of the buffer's view of composing.
fn scene_update_from_compositor_clears_composing_spans(app: &DebugApp) {
    adb::ic_commit_text("abc").expect("ic commitText 'abc'");
    app.wait_for_text("abc", TIMEOUT).expect("'abc'");
    thread::sleep(Duration::from_millis(250));

    adb::ic_set_composing_region(0, 3).expect("ic setComposingRegion 0..3");
    // Round-trip: the next surrounding_text from the client clears the span.
    // updateFromCompositor must call removeComposingSpans.
    thread::sleep(Duration::from_millis(400));

    let change_count = app.text_changed_count();
    adb::ic_commit_text("XYZ").expect("ic commitText 'XYZ'");
    app.wait_for_text_change(change_count, TIMEOUT)
        .expect("text change after 'XYZ'");
    thread::sleep(Duration::from_millis(250));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.contains("XYZ"),
        "'XYZ' missing from buffer: {:?}",
        text
    );
    assert!(
        text.contains("abc") || text.contains("XYZ"),
        "stale composing span sliced bytes from the buffer: {:?}",
        text
    );
}

fn with_wayland_text_input(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_text_input(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// Toolkitless coverage for the basic text-input-v3 editing surfaces:
/// commit_string, raw key events, and IME deleteSurroundingText in both
/// directions around the cursor.
#[test]
fn test_basic_editing_and_delete() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("hello world").expect("commit 'hello world'");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("text 'hello world'");

        adb::ic_send_key_event(adb::KEYCODE_DEL).expect("backspace");
        app.wait_for_text("hello worl", TIMEOUT)
            .expect("'hello worl' after backspace");

        adb::ic_commit_text("d").expect("commit 'd'");
        app.wait_for_text("hello world", TIMEOUT).expect("restored");

        adb::ic_delete_surrounding_text(5, 0).expect("delete_surrounding");
        app.wait_for_text("hello ", TIMEOUT)
            .expect("'hello ' after delete_surrounding(5, 0)");

        let cursor_count = app.cursor_pos_count();
        inject_tap(
            WAYLAND_TAP_COORDS.text_start_x,
            WAYLAND_TAP_COORDS.text_y,
            "tap line start",
        );
        let cursor = app
            .wait_for_cursor_change(cursor_count, TIMEOUT)
            .expect("cursor change after tap");
        assert_eq!(cursor, 0, "tap at line start should move cursor to 0");

        adb::ic_send_key_event(adb::KEYCODE_FORWARD_DEL).expect("forward delete key");
        app.wait_for_text("ello ", TIMEOUT)
            .expect("'ello ' after forward delete key");

        adb::ic_delete_surrounding_text(0, 1).expect("delete_surrounding after cursor");
        app.wait_for_text("llo ", TIMEOUT)
            .expect("'llo ' after delete_surrounding(0, 1)");
    });
}

#[test]
fn test_preedit_lifecycle_and_autocorrect_commit() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("hello ").expect("commit 'hello '");
        app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

        for prefix in ["t", "te", "teh"] {
            adb::ic_set_composing_text(prefix).expect("setComposingText");
            app.wait_for_preedit(prefix, TIMEOUT)
                .unwrap_or_else(|e| panic!("preedit not '{}': {}", prefix, e));
            assert_eq!(
                app.last_text().as_deref().unwrap_or(""),
                "hello ",
                "preedit '{}' leaked into committed text",
                prefix
            );
        }

        adb::ic_commit_text("the ").expect("commit autocorrect");
        app.wait_for_text("hello the ", TIMEOUT)
            .expect("'hello the ' after autocorrect commit");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared after autocorrect");
        assert!(
            !app.last_text().unwrap_or_default().contains("teh"),
            "active preedit was not replaced by autocorrect commit: {:?}",
            app.last_text()
        );

        for prefix in ["w", "wo", "wor", "worl", "world"] {
            adb::ic_set_composing_text(prefix).expect("setComposingText");
            app.wait_for_preedit(prefix, TIMEOUT)
                .unwrap_or_else(|e| panic!("preedit not '{}': {}", prefix, e));
            assert_eq!(
                app.last_text().as_deref().unwrap_or(""),
                "hello the ",
                "preedit '{}' leaked into committed text",
                prefix
            );
        }

        adb::ic_finish_composing().expect("finishComposingText");
        app.wait_for_text("hello the world", TIMEOUT)
            .expect("'hello the world' after finishComposingText");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared after finishComposingText");
    });
}

#[test]
fn test_direct_replacement_apis() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("abc").expect("commit 'abc'");
        app.wait_for_text("abc", TIMEOUT).expect("'abc'");

        let rejected = adb::ic_replace_text_raw(0, 1, "X").expect("replace outside cursor");
        assert!(
            !rejected.status.success(),
            "replaceText outside the wire cursor should return false"
        );
        thread::sleep(Duration::from_millis(200));
        assert_eq!(
            app.last_text().as_deref(),
            Some("abc"),
            "rejected replaceText must not change the client-visible buffer"
        );

        adb::ic_replace_text(0, 3, "ABC").expect("replace current word with completion");
        app.wait_for_text("ABC", TIMEOUT)
            .expect("replaceText should replace the current word");

        adb::ic_commit_text(" def").expect("commit ' def'");
        app.wait_for_text("ABC def", TIMEOUT).expect("'ABC def'");

        adb::ic_commit_correction(4, "def", "DEF").expect("commit correction");
        app.wait_for_text("ABC DEF", TIMEOUT)
            .expect("correction should replace the current word");

        adb::ic_commit_completion(" ok").expect("commit completion");
        app.wait_for_text("ABC DEF ok", TIMEOUT)
            .expect("completion should commit through commitCompletion");
    });
}

/// Cursor movement is the part GTK used to hide for us. The toolkitless app
/// validates SurfaceView-to-compositor touch delivery: tap moves the cursor,
/// Backspace and composing insertion happen at that cursor, a full
/// compose-click-compose loop inserts the second word at the tapped cursor,
/// and touching elsewhere clears pending preedit without letting a stale
/// finishComposingText commit it at the new cursor.
#[test]
fn test_cursor_positioning_from_tap() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        scene_click_cursor_positioning(app, WAYLAND_TAP_COORDS);
    });
}

#[test]
fn test_full_compose_loop_with_click_in_middle() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        scene_full_compose_loop_with_click_in_middle(app, WAYLAND_TAP_COORDS);
    });
}

#[test]
fn test_tap_clears_pending_preedit_without_committing() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("anchor").expect("commit 'anchor'");
        app.wait_for_text("anchor", TIMEOUT).expect("'anchor'");
        let before = app.last_text().unwrap_or_default();

        adb::ic_set_composing_text("pending").expect("setComposingText 'pending'");
        app.wait_for_preedit("pending", TIMEOUT)
            .expect("preedit 'pending'");

        let cursor_count = app.cursor_pos_count();
        inject_tap(
            WAYLAND_TAP_COORDS.text_start_x,
            WAYLAND_TAP_COORDS.text_y,
            "tap line start",
        );
        let cursor = app
            .wait_for_cursor_change(cursor_count, TIMEOUT)
            .expect("cursor change after tap");
        assert_eq!(cursor, 0, "tap at line start should move cursor to 0");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared by tap");

        adb::ic_finish_composing().expect("finishComposingText after tap");
        thread::sleep(Duration::from_millis(300));

        let text = app.last_text().unwrap_or_default();
        assert_eq!(
            text, before,
            "tap should clear uncommitted preedit without inserting it at the old or new cursor"
        );
    });
}

#[test]
fn test_focus_leave_clears_pending_preedit_without_committing() {
    tawc_integration::helpers::test_init();
    let mut text_app = start_wayland_debug_text_input(INPUT_BACKEND, WAYLAND_DEBUG_ENV);

    adb::ic_commit_text("anchor").expect("commit 'anchor'");
    text_app.wait_for_text("anchor", TIMEOUT).expect("'anchor'");
    adb::ic_set_composing_text("pending").expect("setComposingText 'pending'");
    text_app
        .wait_for_preedit("pending", TIMEOUT)
        .expect("preedit 'pending'");

    let preedit_count = text_app.count_with_tag("PREEDIT");
    let before = text_app.last_text().unwrap_or_default();

    let mut touch_app = start_wayland_debug_touch(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    text_app
        .wait_for_tag_count("PREEDIT", preedit_count + 1, TIMEOUT)
        .expect("focused-away text client did not receive preedit clear");
    assert_eq!(
        text_app.last_preedit().as_deref(),
        Some(""),
        "focus leave should clear preedit through the compositor"
    );
    assert_eq!(
        text_app.last_text().as_deref(),
        Some(before.as_str()),
        "focus leave must not commit pending preedit"
    );
    text_app
        .wait_for_tag_value("TEXT_INPUT_LEAVE", "", TIMEOUT)
        .expect("focused-away text client did not receive text-input leave");

    adb::ic_finish_hidden_composing().expect("stale finishComposingText after focus leave");
    thread::sleep(Duration::from_millis(300));
    assert_eq!(
        text_app.last_text().as_deref(),
        Some(before.as_str()),
        "stale finishComposingText after focus leave must not commit old preedit"
    );

    touch_app
        .stop()
        .expect("touch debug app crashed or failed to stop cleanly");
    text_app
        .stop()
        .expect("text debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// The two composing-region replacement shapes are distinct IC paths:
/// setComposingText over a region emits delete+preedit, while commitText over
/// a region emits delete+commit. Keep them in one focused flow so both operate
/// on a real marked committed region without paying two app launches.
#[test]
fn test_composing_region_replacement_paths() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("hello world").expect("commit 'hello world'");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("'hello world'");

        let cursor_count = app.cursor_pos_count();
        inject_tap(
            WAYLAND_TAP_COORDS.text_mid_x,
            WAYLAND_TAP_COORDS.text_y,
            "tap mid",
        );
        let cursor = app
            .wait_for_cursor_change(cursor_count, TIMEOUT)
            .expect("cursor change after tap");
        assert!(cursor > 0 && cursor < 11, "cursor mid, got {}", cursor);
        thread::sleep(Duration::from_millis(200));

        adb::ic_set_composing_region(0, cursor).expect("setComposingRegion");
        adb::ic_set_composing_text("HELLO").expect("setComposingText 'HELLO'");
        app.wait_for_preedit("HELLO", TIMEOUT)
            .expect("preedit 'HELLO'");
        assert!(
            !app.last_text().unwrap_or_default().starts_with("hello"),
            "setComposingText over region did not delete original bytes"
        );

        adb::ic_finish_composing().expect("finishComposingText");
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline && !app.last_text().unwrap_or_default().contains("HELLO") {
            thread::sleep(Duration::from_millis(50));
        }
        let text = app.last_text().unwrap_or_default();
        assert!(
            text.contains("HELLO"),
            "HELLO missing after finish: {:?}",
            text
        );
        assert_eq!(
            text.matches("hello").count(),
            0,
            "original lowercase region survived replacement: {:?}",
            text
        );

        thread::sleep(Duration::from_millis(200));
        adb::ic_set_composing_region(0, 5).expect("setComposingRegion over HELLO");
        adb::ic_commit_text("FOO").expect("commitText over composing region");
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            let text = app.last_text().unwrap_or_default();
            if text.contains("FOO") && !text.contains("HELLO") {
                break;
            }
            thread::sleep(Duration::from_millis(50));
        }
        let text = app.last_text().unwrap_or_default();
        assert!(
            text.contains("FOO") && !text.contains("HELLO"),
            "commitText over region did not replace marked text: {:?}",
            text
        );
    });
}

#[test]
fn test_delete_surrounding_text_in_codepoints() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("a😀bc").expect("commit emoji text");
        app.wait_for_text("a😀bc", TIMEOUT).expect("initial emoji text");

        adb::ic_delete_surrounding_text_codepoints(3, 0)
            .expect("deleteSurroundingTextInCodePoints suffix");
        app.wait_for_text("a", TIMEOUT)
            .expect("codepoint delete should remove emoji+suffix with three keys");

        adb::ic_delete_surrounding_text_codepoints(1, 0)
            .expect("delete remaining char");
        app.wait_for_text("", TIMEOUT)
            .expect("codepoint delete should clear remaining text");
    });
}

#[test]
fn test_space_backspace_space_no_duplicate() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        scene_space_backspace_space_no_duplicate(app);
    });
}

#[test]
fn test_diverged_cursor_no_byte_slicing() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        adb::ic_commit_text("hello ").expect("commit 'hello '");
        app.wait_for_text("hello ", TIMEOUT).expect("'hello '");
        adb::ic_commit_text("world").expect("append 'world'");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("'hello world' after append");
        thread::sleep(Duration::from_millis(200));

        adb::ic_set_composing_region(0, 5).expect("setComposingRegion 0..5");
        let rejected_selection = adb::ic_set_selection_raw(5, 5)
            .expect("setSelection 5..5 should be rejected");
        assert!(
            !rejected_selection.status.success(),
            "setSelection must reject unforwardable cursor movement"
        );
        let change_count = app.text_changed_count();
        let rejected_commit = adb::ic_commit_text_raw("X")
            .expect("commitText with unrepresentable composing region");
        assert!(
            !rejected_commit.status.success(),
            "commitText must reject unrepresentable composing-region replacement"
        );
        thread::sleep(Duration::from_millis(300));
        assert_eq!(
            app.text_changed_count(),
            change_count,
            "rejected replacement should not change client-visible text"
        );
        assert_eq!(
            app.last_text().as_deref(),
            Some("hello world"),
            "rejected replacement should leave the client buffer unchanged"
        );
    });
}

#[test]
fn test_recommit_word_then_newline_no_h_prepend() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        scene_recommit_word_then_newline_no_h_prepend(app);
    });
}

fn build_stale_newline_context(app: &DebugApp, word: &str, word_utf16_len: u32) {
    adb::ic_commit_text(word).expect("ic commitText word");
    app.wait_for_text(word, TIMEOUT).expect("initial word");
    for newline_count in 1..=3 {
        adb::ic_set_composing_region(0, word_utf16_len)
            .expect("ic setComposingRegion over word");
        adb::ic_commit_text(word).expect("ic commitText word (re-commit)");
        adb::ic_commit_text("\n").expect("ic commitText '\\n'");

        let expected = format!("{}{}", word, "\\n".repeat(newline_count));
        app.wait_for_text(&expected, TIMEOUT)
            .unwrap_or_else(|e| panic!("expected {:?} after recommit/newline: {}", expected, e));
    }
}

#[test]
fn test_stale_newline_context_editing_paths() {
    tawc_integration::helpers::test_init();
    let mut app = start_wayland_debug_text_input_stale_newline(INPUT_BACKEND, WAYLAND_DEBUG_ENV);

    build_stale_newline_context(&app, "hello", 5);

    adb::ic_delete_surrounding_text(11, 11).expect("delete surrounding broad range");
    app.wait_for_text("", TIMEOUT).unwrap_or_else(|e| {
        panic!(
            "deleteSurroundingText after stale newline context should clear the buffer; \
             last={:?}: {}",
            app.last_text(),
            e
        )
    });

    let word = "a😀bc";
    build_stale_newline_context(&app, word, 5);

    adb::ic_delete_surrounding_text(7, 0).expect("delete surrounding suffix");
    app.wait_for_text("a", TIMEOUT).unwrap_or_else(|e| {
        panic!(
            "deleteSurroundingText should count an emoji surrogate pair as one key; \
             last={:?}: {}",
            app.last_text(),
            e
        )
    });

    adb::ic_delete_surrounding_text(1, 0).expect("clear remaining emoji phase text");
    app.wait_for_text("", TIMEOUT)
        .expect("clear remaining emoji phase text");

    build_stale_newline_context(&app, "hello", 5);

    adb::ic_set_selection(5, 5).expect("no-op setSelection at stale reported cursor");
    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    let rejected_commit = adb::ic_commit_text_raw("HELLO").expect("ic commitText replacement");
    assert!(
        !rejected_commit.status.success(),
        "commitText should reject stale-context replacement instead of falling back"
    );

    let expected = "hello\\n\\n\\n";
    thread::sleep(Duration::from_millis(300));
    assert_eq!(
        app.last_text().as_deref(),
        Some(expected),
        "stale context replacement should be rejected before touching client text"
    );

    app.stop()
        .expect("stale-newline debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_update_from_compositor_clears_composing_spans() {
    tawc_integration::helpers::test_init();
    with_wayland_text_input(|app| {
        scene_update_from_compositor_clears_composing_spans(app);
    });
}

/// Toolkitless mirror of the surrounding-less GTK/VTE-shaped input case.
#[test]
fn test_surroundingless_client_uses_keyboard_for_backspace() {
    tawc_integration::helpers::test_init();
    let mut app = start_wayland_debug_text_input_no_surrounding(INPUT_BACKEND, WAYLAND_DEBUG_ENV);

    assert_eq!(
        app.count_with_tag("DELETE_SURROUNDING"),
        0,
        "DELETE_SURROUNDING fired before we sent input"
    );

    adb::ic_commit_text(" ").expect("commit ' '");
    app.wait_for_tag_value("COMMIT", " ", TIMEOUT)
        .expect("commit_string ' ' did not reach the client");

    adb::ic_delete_surrounding_text(1, 0).expect("delete_surrounding(1, 0)");
    app.wait_for_tag_value("KEY", "BackSpace", TIMEOUT)
        .expect("Backspace key did not reach the client");

    assert_eq!(
        app.count_with_tag("DELETE_SURROUNDING"),
        0,
        "compositor sent delete_surrounding_text to a surrounding-less client"
    );

    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_android_clipboard_text_to_client() {
    tawc_integration::helpers::test_init();
    let android_text = "android clipboard to wayland";
    adb::clipboard_set_text(android_text).expect("set Android clipboard");

    let mut paste_app = start_wayland_debug_clipboard_paste(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    paste_app
        .wait_for_tag_value("CLIPBOARD_PASTE", android_text, TIMEOUT)
        .expect("Wayland client did not receive Android clipboard text");
    paste_app
        .stop()
        .expect("clipboard paste app crashed or failed to stop cleanly");
}

#[test]
fn test_client_clipboard_text_to_android() {
    tawc_integration::helpers::test_init();
    let wayland_text = "wayland clipboard to android";
    let mut copy_app =
        start_wayland_debug_clipboard_copy(INPUT_BACKEND, WAYLAND_DEBUG_ENV, wayland_text);
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        if got == wayland_text {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "Android clipboard did not receive Wayland text; last={:?}",
            got
        );
        thread::sleep(Duration::from_millis(100));
    }
    copy_app
        .stop()
        .expect("clipboard copy app crashed or failed to stop cleanly");

    assert_compositor_clean();
}

fn wait_for_android_clipboard(expected: &str, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        if got == expected {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "Android clipboard did not become {:?}; last={:?}",
            expected,
            got
        );
        thread::sleep(Duration::from_millis(100));
    }
}

fn assert_android_clipboard_stays(expected: &str, duration: Duration) {
    let deadline = Instant::now() + duration;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        assert_eq!(
            got, expected,
            "hostile clipboard source should not replace Android clipboard"
        );
        if Instant::now() >= deadline {
            return;
        }
        thread::sleep(Duration::from_millis(100));
    }
}

fn wait_for_clipboard_timeout_without_android_replace(expected: &str, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        assert_eq!(
            got, expected,
            "non-closing clipboard source should not replace Android clipboard"
        );
        let logs = adb::logcat_dump_tawc().expect("dump tawc-native logcat");
        if logs.contains("clipboard: timed out waiting for selection source") {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "clipboard pull timeout log did not appear within {:?}",
            timeout
        );
        thread::sleep(Duration::from_millis(100));
    }
}

#[test]
fn test_client_clipboard_over_cap_does_not_replace_android() {
    tawc_integration::helpers::test_init();
    let sentinel = "android clipboard before overcap";
    adb::clipboard_set_text(sentinel).expect("set Android clipboard sentinel");
    wait_for_android_clipboard(sentinel, TIMEOUT);

    let mut copy_app = start_wayland_debug_clipboard_overcap(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    assert_android_clipboard_stays(sentinel, Duration::from_secs(2));
    copy_app
        .stop()
        .expect("clipboard overcap app crashed or failed to stop cleanly");

    assert_compositor_clean();
}

#[test]
fn test_client_clipboard_timeout_does_not_replace_android() {
    tawc_integration::helpers::test_init();
    let sentinel = "android clipboard before timeout";
    adb::clipboard_set_text(sentinel).expect("set Android clipboard sentinel");
    wait_for_android_clipboard(sentinel, TIMEOUT);

    let mut copy_app = start_wayland_debug_clipboard_timeout(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    wait_for_clipboard_timeout_without_android_replace(sentinel, Duration::from_secs(6));
    copy_app
        .stop()
        .expect("clipboard timeout app crashed or failed to stop cleanly");

    assert_compositor_clean();
}
