//! Input dispatch tests. Drive the gtk4-debug-app through compositor input
//! paths (text-input-v3 commits, key events, touch taps) and assert the
//! debug app observes the expected client-side behaviour.
//!
//! These tests deliberately avoid asserting anything about buffer types
//! (AHB vs SHM). The point is to verify how the compositor *dispatches
//! input*, not how clients render. That makes them safe to run on the
//! emulator (where libhybris/AHB is unavailable) — for application/buffer
//! coverage see `tests/applications.rs`.

use tawc_integration::adb;
use tawc_integration::helpers::{assert_compositor_clean, start_text_input, TIMEOUT};

// Physical screen coordinates for tapping inside the text view.
// Compositor uses 2x scale, so logical = physical / 2.
// Text content starts at approximately physical (80, 234).
// Monospace 18pt ≈ 22 physical px per character.
const TAP_TEXT_MID_X: u32 = 200;
const TAP_TEXT_MID_Y: u32 = 250;

/// Env passed to gtk4-debug-app for input tests. Currently empty — i.e.
/// GTK4 uses its default GSK renderer (Vulkan/GL via libhybris on device).
/// We'd rather use `GSK_RENDERER=cairo` so this group is buffer-path
/// independent and runs on the emulator too, but the cairo path currently
/// dies right after the first frame with "the target surface has been
/// finished" — see issues/gtk4-cairo-renderer-broken.md.
const INPUT_ENV: &str = "";

#[test]
fn test_text_input_and_backspace() {
    let mut app = start_text_input(INPUT_ENV);

    // Type multi-word text (covers basic input + spaces)
    adb::input_text("hello world").expect("Failed to send text");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("Text should be 'hello world'");

    // Backspace should delete last character
    adb::input_keyevent(adb::KEYCODE_DEL).expect("Failed to send backspace");
    app.wait_for_text("hello worl", TIMEOUT)
        .expect("Text should be 'hello worl' after backspace");

    app.stop().expect("Debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_click_cursor_positioning() {
    let mut app = start_text_input(INPUT_ENV);

    // Type text
    adb::input_text("abcdef").expect("Failed to send text");
    app.wait_for_text("abcdef", TIMEOUT)
        .expect("Text should be 'abcdef'");

    // Click in the middle - should produce CURSOR_POS events and move cursor
    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("Failed to tap");

    let cursor_pos = app
        .wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("Click did not produce any CURSOR_POS events");
    assert!(
        cursor_pos > 0 && cursor_pos < 6,
        "Cursor should be in middle, got position {}",
        cursor_pos
    );

    // Clicking should not have changed the text
    let text = app.last_text().expect("No text after click");
    assert_eq!(text, "abcdef", "Click changed text content to '{}'", text);

    // Backspace should delete from cursor position, not from end
    let change_count = app.text_changed_count();
    adb::input_keyevent(adb::KEYCODE_DEL).expect("Failed to send backspace");
    let text = app
        .wait_for_text_change(change_count, TIMEOUT)
        .expect("No TEXT_CHANGED after backspace");
    assert_ne!(
        text, "abcde",
        "Backspace deleted from end instead of cursor position"
    );
    assert_eq!(
        text.len(),
        5,
        "Expected 5 characters after deleting one from 'abcdef', got '{}'",
        text
    );

    // Type at cursor position - should insert in middle, not at end
    let change_count = app.text_changed_count();
    adb::input_text("X").expect("Failed to send text");
    let text = app
        .wait_for_text_change(change_count, TIMEOUT)
        .expect("No TEXT_CHANGED after typing X");
    assert!(
        !text.ends_with("X") || text.len() != 6,
        "Typed 'X' was appended to end instead of inserted at cursor position, got '{}'",
        text
    );

    app.stop()
        .expect("Debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}
