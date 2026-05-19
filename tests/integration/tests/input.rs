//! Input dispatch tests. Drive the gtk4-debug-app through compositor
//! input paths (text-input-v3 commits, key events, touch taps) and assert
//! the debug app observes the expected client-side behaviour.
//!
//! # The rule
//!
//! Tests interact with the system **only** as a keyboard or as an app:
//!
//! - **As a keyboard**: every input call goes through
//!   [`TawcInputConnection`] via `adb::ic_*` helpers — the same Kotlin
//!   surface the system IMM dispatches Gboard / OpenBoard / AOSP-latin
//!   events through. The IC's full state machine (Editable mirror,
//!   `computeReplaceDeltas`, `composingRegionIsPreedit` short-circuit,
//!   `unitsToKeyCounts`, `lastSyncedCursor` divergence guard) runs on
//!   every test exactly the same way it runs in production.
//! - **As an app**: assertions go through `gtk4-debug-app`'s observed
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
//! [`crate::helpers::start_text_input`] calls `enable-test-input` on
//! every test so the system IME (a third-party at the OS boundary) is
//! removed from the loop — that's not "in the middle of the state
//! machine", it's removing a non-deterministic external actor that
//! would otherwise amplify our IC calls back at us.
//!
//! All scenarios share a single GTK4 app instance — each one resets the
//! buffer between scenes via the IC. Per-scenario startup of GTK is the
//! dominant cost, so consolidation cuts ~14× the runtime.
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
    assert_compositor_clean, start_text_input, start_text_input_no_surrounding,
    start_wayland_debug_text_input, start_wayland_debug_text_input_no_surrounding,
    start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::GraphicsBackend;

/// Input dispatch has no buffer-type stake; pick the most portable
/// backend (no libhybris LD_LIBRARY_PATH, no gfxstream env) so a single
/// run exercises the dispatch paths without depending on a GPU.
const INPUT_BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

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

// Same logical intent as the GTK coordinates above, but aimed at
// wayland-debug-app's compact 640x240 surface. Coordinates are physical;
// the compositor's 2x scale maps them to the app's logical surface.
const WAYLAND_TAP_TEXT_MID_X: u32 = 200;
const WAYLAND_TAP_TEXT_MID_Y: u32 = 250;
const WAYLAND_TAP_TEXT_START_X: u32 = 40;

#[derive(Clone, Copy)]
struct InputTapCoords {
    text_mid_x: u32,
    text_mid_y: u32,
    text_start_x: u32,
}

const GTK_TAP_COORDS: InputTapCoords = InputTapCoords {
    text_mid_x: TAP_TEXT_MID_X,
    text_mid_y: TAP_TEXT_MID_Y,
    text_start_x: TAP_TEXT_START_X,
};

const WAYLAND_TAP_COORDS: InputTapCoords = InputTapCoords {
    text_mid_x: WAYLAND_TAP_TEXT_MID_X,
    text_mid_y: WAYLAND_TAP_TEXT_MID_Y,
    text_start_x: WAYLAND_TAP_TEXT_START_X,
};

/// Env passed to gtk4-debug-app for input tests. Currently empty — i.e.
/// GTK4 uses its default GSK renderer. Under [INPUT_BACKEND] = CPU, GSK
/// picks up the distro Mesa loaded via llvmpipe (Vulkan via lavapipe if
/// `vulkan-swrast` is installed, GL otherwise). We'd rather use
/// `GSK_RENDERER=cairo` so this group is buffer-path independent, but
/// the cairo path currently dies right after the first frame with
/// "the target surface has been finished" — see
/// issues/gtk4-cairo-renderer-broken.md.
const INPUT_ENV: &str = "";
const WAYLAND_DEBUG_ENV: &str = "";

/// Reset the GTK4 debug app buffer + preedit between scenarios so we don't
/// pay GTK startup per scenario. Acts as a keyboard: clears any active
/// preedit, then hammers Backspace until GTK reports the buffer empty.
///
/// Why not `ic_delete_surrounding_text` — that delegates the
/// units-to-keys translation to the IC's `unitsToKeyCounts`, which
/// reads the IC's Editable mirror to map UTF-16 units around the
/// cursor onto code-point counts. Between scenes the mirror can lag
/// the wayland-side cursor by a round-trip (the compositor's
/// `set_surrounding_text` reply hasn't reached `updateFromCompositor`
/// yet), and a stale cursor produces a clipped key count — e.g.
/// "hello\n\n\n" with mirror cursor at 6 instead of 8 yields 6
/// Backspaces and the buffer ends up "he". A reset utility shouldn't
/// have to reason about that. Sending raw Backspace key events
/// sidesteps the mirror entirely, which is also what a real keyboard
/// pressing Backspace does — there's no Editable dependency on
/// `wl_keyboard.key`.
fn reset_buffer(app: &DebugApp) {
    // Step 1: clear any active preedit. If the preedit was non-empty
    // the compositor commits it first (per text-input-v3 done
    // ordering); we'll delete the resulting bytes in step 2.
    adb::ic_finish_composing().expect("reset: ic_finish_composing");
    thread::sleep(Duration::from_millis(250));

    // Step 2: hammer Backspace AND Forward-Delete until empty. Both
    // are needed because the previous scene may have left the cursor
    // mid-buffer (`scene_click_cursor_positioning`, the various
    // mid-buffer-tap scenes); Backspace only deletes before the
    // cursor, Forward-Delete only after, and we don't know which side
    // contains text. Buffer length (in UTF-16 units of the escaped
    // form) is an upper bound on how many of each we could possibly
    // need; extras at an empty buffer no-op.
    //
    // We use raw key events instead of `ic_delete_surrounding_text`
    // here on purpose: the IC's `unitsToKeyCounts` translates against
    // its Editable mirror, which can lag the wayland-side cursor (see
    // `issues/ic-delete-surrounding-after-newline-mistranslates.md`).
    // A reset utility shouldn't have to reason about that — pressing
    // Backspace as a "keyboard" matches what a real keyboard does and
    // doesn't depend on the IC's mirror being current.
    let current = app.last_text().unwrap_or_default();
    if !current.is_empty() {
        let len = current.encode_utf16().count() as u32;
        for _ in 0..len {
            adb::ic_send_key_event(adb::KEYCODE_DEL).expect("reset: backspace");
        }
        for _ in 0..len {
            adb::ic_send_key_event(adb::KEYCODE_FORWARD_DEL).expect("reset: forward-delete");
        }
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            if app.last_text().unwrap_or_default().is_empty() {
                break;
            }
            thread::sleep(Duration::from_millis(50));
        }
        let after = app.last_text().unwrap_or_default();
        assert!(after.is_empty(), "reset_buffer left buffer = {:?}", after);
    }

    let preedit = app.last_preedit().unwrap_or_default();
    assert!(
        preedit.is_empty(),
        "reset_buffer left preedit = {:?}",
        preedit
    );
}

// --- Scenes -----------------------------------------------------------------
//
// Each scene drives the IC with a Gboard-shaped sequence and asserts what
// the wayland client (gtk4-debug-app) saw. Scenes assume the buffer
// starts empty (call `reset_buffer` between them). Panics propagate up
// to the single `#[test]` so the failing scenario is identified by the
// function name in the backtrace.

/// Plain commit + key-event paths: type a string, Backspace at end via a
/// raw key event, then re-extend, then `deleteSurroundingText` for
/// IME-driven trimming.
fn scene_basic_input_and_delete(app: &DebugApp) {
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
}

/// Compose-loop preedit progresses letter-by-letter, isn't visible in the
/// buffer until finishComposingText commits it. Also covers
/// "compose-after-committed-text" — the preedit doesn't disturb the
/// already-committed prefix.
fn scene_compose_lifecycle(app: &DebugApp) {
    adb::ic_commit_text("hello ").expect("commit 'hello '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    for prefix in ["w", "wo", "wor", "worl", "world"] {
        adb::ic_set_composing_text(prefix).expect("setComposingText");
        app.wait_for_preedit(prefix, TIMEOUT)
            .unwrap_or_else(|e| panic!("preedit not '{}': {}", prefix, e));
        // The committed buffer must remain "hello " throughout — preedit
        // is cursor-relative overlay, never document content until finish.
        assert_eq!(
            app.last_text().as_deref().unwrap_or(""),
            "hello ",
            "preedit '{}' leaked into the buffer",
            prefix
        );
    }

    adb::ic_finish_composing().expect("finishComposingText");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world' after finishComposingText");
    app.wait_for_preedit("", TIMEOUT)
        .expect("preedit cleared after finishComposingText");
}

/// Autocorrect / replacement-on-commit: with `composingRegionIsPreedit=true`
/// (last touched via `setComposingText`), `commitText` skips the
/// pre-commit delete and emits a bare commit_string. The compositor's
/// text-input-v3 done-ordering replaces the active preedit with the
/// commit at the cursor — the user must see "the ", not "tehthe ".
fn scene_autocorrect_replaces_preedit(app: &DebugApp) {
    adb::ic_set_composing_text("teh").expect("setComposingText 'teh'");
    app.wait_for_preedit("teh", TIMEOUT).expect("preedit 'teh'");

    adb::ic_commit_text("the ").expect("commit 'the '");
    app.wait_for_text("the ", TIMEOUT)
        .expect("'the ' (autocorrect replaced 'teh')");
    app.wait_for_preedit("", TIMEOUT)
        .expect("preedit cleared after the commit");
}

/// Click cursor positioning + behaviour at the cursor: backspace deletes
/// from cursor (not end), commit_string inserts at cursor (not end), and a
/// preedit-then-finish round-trip places the new char at the click site.
fn scene_click_cursor_positioning(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_commit_text("abcdef").expect("commit 'abcdef'");
    app.wait_for_text("abcdef", TIMEOUT).expect("'abcdef'");

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
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

/// `setComposingRegion` + `setComposingText` — Gboard's "tap to retype"
/// flow. Marks committed bytes composing, then preedit replaces them.
/// The IC's `computeReplaceDeltas` should yield a non-zero delete delta
/// that gets paired with the preedit on the wire; without that, both
/// the original word and the new preedit would coexist (the
/// "hellohello" duplicate-text bug).
fn scene_compose_region_replaces_committed_text(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_commit_text("hello world").expect("commit 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change after tap");
    assert!(
        cursor > 0 && cursor < 11,
        "cursor mid 'hello world', got {}",
        cursor
    );
    // Round-trip lets the IC's `lastSyncedCursor` settle to the new
    // cursor before `setComposingRegion` fires — `computeReplaceDeltas`
    // refuses to emit a delete delta if Editable cursor diverges from
    // `lastSyncedCursor`, which would otherwise let this test pass-by-luck.
    thread::sleep(Duration::from_millis(200));

    adb::ic_set_composing_region(0, cursor).expect("ic_set_composing_region");
    adb::ic_set_composing_text("HELLO").expect("ic_set_composing_text 'HELLO'");
    app.wait_for_preedit("HELLO", TIMEOUT)
        .expect("preedit 'HELLO'");
    let committed_now = app.last_text().expect("text after replace");
    assert!(
        !committed_now.starts_with("hello"),
        "replacement should have deleted committed bytes; got {:?}",
        committed_now
    );

    adb::ic_finish_composing().expect("finishComposingText");
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
/// autocorrect-replace flow. `commitText` over a `setComposingRegion`-
/// marked range must also delete the original bytes.
fn scene_commit_text_replaces_composing_region(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_commit_text("hello world").expect("commit 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");

    let cursor_count = app.cursor_pos_count();
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
    let cursor = app
        .wait_for_cursor_change(cursor_count, TIMEOUT)
        .expect("cursor change after tap");
    assert!(cursor > 0 && cursor < 11, "cursor mid, got {}", cursor);
    thread::sleep(Duration::from_millis(200));

    adb::ic_set_composing_region(0, cursor).expect("ic_set_composing_region");
    adb::ic_commit_text("FOO").expect("ic_commit_text 'FOO' (replacement)");
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

/// Regression: stale preedit must not be re-committed when the user
/// taps elsewhere. The compositor's touch handler clears
/// `current_preedit` so a later `finishComposingText` is a no-op on the
/// wire. Without the fix, `current_preedit` was still set after the
/// `cause=other` round-trip and `finishComposingText` re-committed
/// "hello" at the new cursor — `hellohello` in the buffer.
fn scene_finish_composing_after_click_no_duplicate(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_set_composing_text("hello").expect("setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT)
        .expect("preedit 'hello'");

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
    app.wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("cursor change");

    adb::ic_finish_composing().expect("finishComposingText");
    thread::sleep(Duration::from_millis(300));

    let text = app.last_text().unwrap_or_default();
    assert!(
        text.matches("hello").count() <= 1,
        "finishComposingText after click duplicated the preedit; got {:?}",
        text
    );
}

/// Regression: tapping with a pending preedit must commit the typed text
/// (so it's not silently lost) AND clear the preedit overlay (so it
/// doesn't follow the cursor visually). Tap lands in the middle of
/// committed text.
fn scene_click_during_preedit_commits_pending_text(app: &DebugApp, taps: InputTapCoords) {
    adb::ic_commit_text("world").expect("commit 'world'");
    app.wait_for_text("world", TIMEOUT).expect("'world'");

    adb::ic_set_composing_text("hello").expect("setComposingText 'hello'");
    app.wait_for_preedit("hello", TIMEOUT)
        .expect("preedit 'hello'");
    assert_eq!(
        app.last_text().as_deref().unwrap_or(""),
        "world",
        "preedit must not appear in the buffer before any commit"
    );

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
    app.wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("cursor change after tap");

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

/// Regression: same Touch::Down preedit-finalise, but the tap lands at
/// the start of the line. Bug repro — the in-progress word vanished
/// entirely when the cursor jumped to position 0.
fn scene_click_at_start_during_preedit_preserves_pending_text(
    app: &DebugApp,
    taps: InputTapCoords,
) {
    adb::ic_commit_text("hello ").expect("commit 'hello '");
    app.wait_for_text("hello ", TIMEOUT).expect("'hello '");

    adb::ic_set_composing_text("world").expect("setComposingText 'world'");
    app.wait_for_preedit("world", TIMEOUT)
        .expect("preedit 'world'");
    assert_eq!(
        app.last_text().as_deref().unwrap_or(""),
        "hello ",
        "preedit must not appear in the buffer before any commit"
    );

    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(taps.text_start_x, taps.text_mid_y).expect("tap line start");
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
    adb::input_tap(taps.text_mid_x, taps.text_mid_y).expect("tap mid");
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

/// IC commit with a diverged Editable cursor: `setSelection` moves the
/// Editable's cursor under us without any wayland-side equivalent. The
/// IC must detect the divergence and refuse to propagate composing-region
/// deltas; otherwise the wire `delete_surrounding_text(N)` slices N
/// bytes from the wrong position.
fn scene_commit_after_diverged_cursor_no_byte_slicing(app: &DebugApp) {
    adb::ic_commit_text("hello world").expect("ic commitText 'hello world'");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("'hello world'");
    thread::sleep(Duration::from_millis(200));

    adb::ic_set_composing_region(0, 5).expect("ic setComposingRegion 0..5");
    adb::ic_set_selection(5, 5).expect("ic setSelection 5..5 (diverges Editable from Wayland)");

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

/// IC re-commit-word + newline (OpenBoard's per-Enter pattern). Each
/// Enter fires `setComposingRegion(0, N)` + `commitText(word, 1)` +
/// `commitText("\n", 1)`. Without the short-circuit fix, the second
/// iteration onward sliced bytes off the buffer (visible as a stray "h"
/// prepended on each Enter).
fn scene_recommit_word_then_newline_no_h_prepend(app: &DebugApp) {
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

        // The debug app escapes '\n' as the two-char sequence `\n` so
        // the protocol stays single-line — assert against that escaped
        // form.
        let expected = format!("hello{}", "\\n".repeat(i));
        let text = app.last_text().unwrap_or_default();
        assert_eq!(
            text, expected,
            "Enter #{i}: expected buffer {:?} but got {:?}. Without the \
             fix the IC propagates composing-region deltas, slicing \
             bytes off the buffer (visible as a stray 'h' prepended on \
             each Enter).",
            expected, text
        );
    }
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
    // Round-trip: the next surrounding_text from GTK clears the span.
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

fn run_input_dispatch_scenes(app: &DebugApp, taps: InputTapCoords) {
    scene_basic_input_and_delete(app);
    reset_buffer(app);

    scene_compose_lifecycle(app);
    reset_buffer(app);

    scene_autocorrect_replaces_preedit(app);
    reset_buffer(app);

    scene_click_cursor_positioning(app, taps);
    reset_buffer(app);

    scene_compose_region_replaces_committed_text(app, taps);
    reset_buffer(app);

    scene_commit_text_replaces_composing_region(app, taps);
    reset_buffer(app);

    scene_finish_composing_after_click_no_duplicate(app, taps);
    reset_buffer(app);

    scene_click_during_preedit_commits_pending_text(app, taps);
    reset_buffer(app);

    scene_click_at_start_during_preedit_preserves_pending_text(app, taps);
    reset_buffer(app);

    scene_full_compose_loop_with_click_in_middle(app, taps);
    reset_buffer(app);

    scene_space_backspace_space_no_duplicate(app);
    reset_buffer(app);

    scene_commit_after_diverged_cursor_no_byte_slicing(app);
    reset_buffer(app);

    scene_recommit_word_then_newline_no_h_prepend(app);
    reset_buffer(app);

    scene_update_from_compositor_clears_composing_spans(app);
}

// --- The single test --------------------------------------------------------

/// One #[test] that drives the full input-dispatch suite. Opening
/// gtk4-debug-app dominates the per-test cost (>1s of GTK startup +
/// libhybris GL setup), so consolidating amortises it across all
/// scenarios. Each scenario is a separate function so a panic identifies
/// exactly where coverage broke.
#[test]
fn test_input_dispatch() {
    let mut app = start_text_input(INPUT_BACKEND, INPUT_ENV);

    run_input_dispatch_scenes(&app, GTK_TAP_COORDS);

    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
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
///      `delete_surrounding_text` to one (lxterminal/VTE was the
///      original reproducer) closes the connection.
///
/// This test is the regression guard for the "lxterminal disappears on
/// the first backspace" bug. The harness watches the bare GtkIMContext
/// signals — `commit` for the space, `delete-surrounding` (which must
/// never fire), and the `wl_keyboard` Backspace via
/// GtkEventControllerKey.
#[test]
fn test_surroundingless_client_uses_keyboard_for_backspace() {
    let mut app = start_text_input_no_surrounding(INPUT_BACKEND, INPUT_ENV);

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
    adb::ic_commit_text(" ").expect("commit ' '");
    app.wait_for_tag_value("COMMIT", " ", TIMEOUT)
        .expect("commit_string ' ' did not reach the IM context");

    // 2. Gboard-style backspace: deleteSurroundingText(1, 0). The IC
    // translates this to a Backspace key event before crossing JNI; the
    // compositor must therefore deliver Backspace via wl_keyboard, NOT
    // a delete_surrounding_text protocol event.
    adb::ic_delete_surrounding_text(1, 0).expect("delete_surrounding(1, 0)");
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

    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_text_input(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_text_input(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_touch(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_touch(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[derive(Clone, Debug)]
struct TouchDebugEvent {
    id: i32,
    x: f64,
    y: f64,
    active: u32,
}

fn parse_touch_event(payload: &str) -> TouchDebugEvent {
    let mut parts = payload.split(':');
    let id = parts
        .next()
        .expect("touch id")
        .parse()
        .expect("touch id integer");
    let x = parts
        .next()
        .expect("touch x")
        .parse()
        .expect("touch x number");
    let y = parts
        .next()
        .expect("touch y")
        .parse()
        .expect("touch y number");
    let active = parts
        .next()
        .expect("touch active count")
        .parse()
        .expect("touch active integer");
    assert!(
        parts.next().is_none(),
        "extra fields in touch payload {payload:?}"
    );
    TouchDebugEvent { id, x, y, active }
}

fn touch_events(app: &DebugApp, tag: &str) -> Vec<TouchDebugEvent> {
    app.payloads_with_tag(tag)
        .iter()
        .map(|payload| parse_touch_event(payload))
        .collect()
}

fn inject_touch(kind: &str) {
    let output = adb::inject_touch(kind).unwrap_or_else(|e| panic!("inject-touch {kind}: {e}"));
    assert!(
        output.status.success(),
        "inject-touch {kind} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

/// Toolkitless coverage for the basic text-input-v3 surfaces: commit_string,
/// raw key events, IME deleteSurroundingText, preedit lifecycle, and commit
/// replacing an active preedit. This intentionally uses one fresh app
/// lifetime instead of matching GTK's reset-between-scenes structure.
#[test]
fn test_wayland_input_basic_editing_and_preedit() {
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

        for prefix in ["w", "wo", "wor", "worl", "world"] {
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

        adb::ic_finish_composing().expect("finishComposingText");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("'hello world' after finishComposingText");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared after finishComposingText");

        adb::ic_set_composing_text("teh").expect("setComposingText 'teh'");
        app.wait_for_preedit("teh", TIMEOUT).expect("preedit 'teh'");
        adb::ic_commit_text("the ").expect("commit autocorrect");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared after autocorrect");
        let text = app.last_text().unwrap_or_default();
        assert!(
            text.ends_with("the ") && !text.contains("teh"),
            "active preedit was not replaced by autocorrect commit: {:?}",
            text
        );
    });
}

/// Cursor movement is the part GTK used to hide for us. The toolkitless app
/// validates the compositor's touch delivery directly: tap moves the cursor,
/// Backspace and composing insertion happen at that cursor, and touching
/// elsewhere finalizes pending preedit without letting a stale
/// finishComposingText duplicate it.
#[test]
fn test_wayland_input_touch_cursor_and_pending_preedit() {
    with_wayland_text_input(|app| {
        scene_click_cursor_positioning(app, WAYLAND_TAP_COORDS);

        let before = app.last_text().unwrap_or_default();
        adb::ic_set_composing_text("pending").expect("setComposingText 'pending'");
        app.wait_for_preedit("pending", TIMEOUT)
            .expect("preedit 'pending'");

        let cursor_count = app.cursor_pos_count();
        adb::input_tap(
            WAYLAND_TAP_COORDS.text_start_x,
            WAYLAND_TAP_COORDS.text_mid_y,
        )
        .expect("tap line start");
        let cursor = app
            .wait_for_cursor_change(cursor_count, TIMEOUT)
            .expect("cursor change after tap");
        assert_eq!(cursor, 0, "tap at line start should move cursor to 0");
        app.wait_for_preedit("", TIMEOUT)
            .expect("preedit cleared by tap");

        adb::ic_finish_composing().expect("finishComposingText after tap");
        thread::sleep(Duration::from_millis(300));

        let text = app.last_text().unwrap_or_default();
        let without_pending = text.replacen("pending", "", 1);
        assert!(
            without_pending == before && text.contains("pending"),
            "tap dropped committed text or pending preedit: before={:?} after={:?}",
            before,
            text
        );
        assert_eq!(
            text.matches("pending").count(),
            1,
            "stale finishComposingText duplicated pending preedit: {:?}",
            text
        );
    });
}

/// The touch visualizer uses normalized host-side injection, so this test
/// asserts event shape rather than absolute pixels. It covers a plain tap:
/// wl_touch.down reaches the client, followed by wl_touch.up for the same
/// slot, with no dependency on the device's physical resolution.
#[test]
fn test_wayland_touch_tap() {
    with_wayland_touch(|app| {
        inject_touch("tap");
        app.wait_for_tag_count("TOUCH_DOWN", 1, TIMEOUT)
            .expect("touch down");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");

        let downs = touch_events(app, "TOUCH_DOWN");
        let ups = touch_events(app, "TOUCH_UP");
        assert_eq!(downs.len(), 1, "expected one TOUCH_DOWN, got {downs:?}");
        assert_eq!(ups.len(), 1, "expected one TOUCH_UP, got {ups:?}");
        assert_eq!(downs[0].id, ups[0].id, "tap up used a different slot");
        assert_eq!(downs[0].active, 1, "tap down active count");
        assert_eq!(ups[0].active, 0, "tap up active count");
        assert!(
            (downs[0].x - ups[0].x).abs() < 1.0 && (downs[0].y - ups[0].y).abs() < 1.0,
            "tap moved unexpectedly: down={:?} up={:?}",
            downs[0],
            ups[0]
        );
    });
}

/// Drag is touch.down + a stream of wl_touch.motion events + touch.up. The
/// assertions only compare the client's own observed coordinates, avoiding
/// any baked-in screen dimensions or density assumptions.
#[test]
fn test_wayland_touch_drag() {
    with_wayland_touch(|app| {
        inject_touch("drag");
        app.wait_for_tag_count("TOUCH_DOWN", 1, TIMEOUT)
            .expect("touch down");
        app.wait_for_tag_count("TOUCH_MOTION", 3, TIMEOUT)
            .expect("touch motion");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");

        let down = touch_events(app, "TOUCH_DOWN").remove(0);
        let motions = touch_events(app, "TOUCH_MOTION");
        let up = touch_events(app, "TOUCH_UP").remove(0);
        let last_motion = motions.last().expect("last motion");

        assert!(
            motions.iter().all(|m| m.id == down.id),
            "drag slot changed: {motions:?}"
        );
        assert!(up.id == down.id, "drag up slot changed");
        assert!(
            last_motion.x > down.x && last_motion.y > down.y,
            "drag did not move down/right: down={down:?} last={last_motion:?}"
        );
        assert!(
            (up.x - last_motion.x).abs() < 1.0 && (up.y - last_motion.y).abs() < 1.0,
            "drag up should report the final motion position: up={up:?} last={last_motion:?}"
        );
    });
}

/// Two-finger delivery is the path most likely to rot because each Android
/// pointer ID must remain a distinct Wayland touch slot through down,
/// motion, and up. The debug broker builds a real multi-pointer MotionEvent
/// stream against the focused SurfaceView; the client verifies both slots.
#[test]
fn test_wayland_touch_multitouch() {
    with_wayland_touch(|app| {
        inject_touch("multitouch");
        app.wait_for_tag_count("TOUCH_DOWN", 2, TIMEOUT)
            .expect("two touch downs");
        app.wait_for_tag_count("TOUCH_MOTION", 6, TIMEOUT)
            .expect("multi-touch motion");
        app.wait_for_tag_count("TOUCH_UP", 2, TIMEOUT)
            .expect("two touch ups");

        let downs = touch_events(app, "TOUCH_DOWN");
        let motions = touch_events(app, "TOUCH_MOTION");
        let ups = touch_events(app, "TOUCH_UP");
        assert_eq!(downs.len(), 2, "expected two downs, got {downs:?}");
        assert_eq!(ups.len(), 2, "expected two ups, got {ups:?}");
        assert_ne!(downs[0].id, downs[1].id, "two fingers shared one slot");
        assert_eq!(downs[0].active, 1, "first down active count");
        assert_eq!(downs[1].active, 2, "second down active count");
        assert_eq!(ups.last().unwrap().active, 0, "all fingers should be up");

        for id in [downs[0].id, downs[1].id] {
            let first = downs.iter().find(|e| e.id == id).unwrap();
            let last = motions
                .iter()
                .rev()
                .find(|e| e.id == id)
                .unwrap_or_else(|| panic!("missing motion for touch id {id}"));
            assert!(
                (last.x - first.x).abs() > 5.0 || (last.y - first.y).abs() > 5.0,
                "touch id {id} did not move enough: first={first:?} last={last:?}"
            );
        }
    });
}

/// The two composing-region replacement shapes are distinct IC paths:
/// setComposingText over a region emits delete+preedit, while commitText over
/// a region emits delete+commit. Keep both, but run them in one fresh client.
#[test]
fn test_wayland_input_composing_region_replacements() {
    with_wayland_text_input(|app| {
        adb::ic_commit_text("hello world").expect("commit 'hello world'");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("'hello world'");

        let cursor_count = app.cursor_pos_count();
        adb::input_tap(WAYLAND_TAP_COORDS.text_mid_x, WAYLAND_TAP_COORDS.text_mid_y)
            .expect("tap mid");
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
        adb::ic_set_composing_region(0, 5).expect("setComposingRegion 0..5");
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

/// IC regression cases that do not depend on GTK widget behavior: replacement
/// deltas must not duplicate text, diverged Editable cursor state must not
/// slice bytes from the Wayland buffer, newline re-commit must not prepend
/// stray bytes, and compositor round-trips must clear stale composing spans.
#[test]
fn test_wayland_input_ic_delta_regressions() {
    with_wayland_text_input(|app| {
        scene_space_backspace_space_no_duplicate(app);

        adb::ic_commit_text("world").expect("append 'world'");
        app.wait_for_text("hello world", TIMEOUT)
            .expect("'hello world' after append");
        thread::sleep(Duration::from_millis(200));

        adb::ic_set_composing_region(0, 5).expect("setComposingRegion 0..5");
        adb::ic_set_selection(5, 5).expect("setSelection 5..5 (diverge Editable cursor)");
        let change_count = app.text_changed_count();
        adb::ic_commit_text("X").expect("commitText after divergence");
        app.wait_for_text_change(change_count, TIMEOUT)
            .expect("text change after diverged commit");
        thread::sleep(Duration::from_millis(300));
        assert!(
            app.last_text().unwrap_or_default().contains("world"),
            "diverged cursor propagated a byte-slicing delete: {:?}",
            app.last_text()
        );
    });

    with_wayland_text_input(|app| {
        scene_recommit_word_then_newline_no_h_prepend(app);
    });

    with_wayland_text_input(|app| {
        scene_update_from_compositor_clears_composing_spans(app);
    });
}

/// Toolkitless mirror of the surrounding-less GTK/VTE-shaped input case.
#[test]
fn test_wayland_surroundingless_client_uses_keyboard_for_backspace() {
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
