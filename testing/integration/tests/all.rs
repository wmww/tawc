use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use tawc_integration::{adb, chroot, chroot_process::ChrootProcess, compositor, debug_app::DebugApp};

extern "C" fn stop_compositor() {
    compositor::stop_if_started();
}

/// One-time setup: ensure compositor running, deps installed, debug app built.
/// Returns the path to the debug app binary inside the chroot.
fn setup() -> String {
    static BINARY_PATH: OnceLock<String> = OnceLock::new();
    BINARY_PATH
        .get_or_init(|| {
            // Register atexit handler to stop compositor when tests finish
            unsafe { libc::atexit(stop_compositor) };
            compositor::ensure_running().expect("Failed to ensure compositor is running");
            chroot::ensure_debug_app().expect("Failed to ensure debug app")
        })
        .clone()
}

const TIMEOUT: Duration = Duration::from_secs(5);

/// Start the text-input debug app and wait for it to be ready.
/// `gdk_gl` controls buffer type: "disabled" for SHM, "gles:always" for hardware (AHB).
fn start_text_input(gdk_gl: &str) -> DebugApp {
    let binary = setup();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(&binary, "text-input", gdk_gl).expect("Failed to start debug app");
    app.wait_ready().expect("Debug app did not become ready");
    app
}

// Physical screen coordinates for tapping inside the text view.
// Compositor uses 2x scale, so logical = physical / 2.
// Text content starts at approximately physical (80, 234).
// Monospace 18pt ≈ 22 physical px per character.
const TAP_TEXT_MID_X: u32 = 200;
const TAP_TEXT_MID_Y: u32 = 250;

#[test]
fn test_text_input_and_backspace() {
    let mut app = start_text_input("disabled");

    // Type multi-word text (covers basic input + spaces)
    adb::input_text("hello world").expect("Failed to send text");
    app.wait_for_text("hello world", TIMEOUT)
        .expect("Text should be 'hello world'");

    // Backspace should delete last character
    adb::input_keyevent(adb::KEYCODE_DEL).expect("Failed to send backspace");
    app.wait_for_text("hello worl", TIMEOUT)
        .expect("Text should be 'hello worl' after backspace");

    // Verify SHM buffers were used (GDK_GL=disabled → software rendering → SHM)
    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(logs.contains("SHM buffer imported"),
        "Expected SHM buffer import in compositor logs (GDK_GL=disabled should use SHM)");
    assert!(!logs.contains("AHB imported as texture"),
        "Unexpected AHB buffer import (GDK_GL=disabled should not use hardware buffers)");

    app.stop().expect("Debug app crashed or failed to stop cleanly");
}

#[test]
fn test_click_cursor_positioning() {
    let mut app = start_text_input("gles:always");

    // Type text
    adb::input_text("abcdef").expect("Failed to send text");
    app.wait_for_text("abcdef", TIMEOUT)
        .expect("Text should be 'abcdef'");

    // Click in the middle - should produce CURSOR_POS events and move cursor
    let cursor_count_before = app.cursor_pos_count();
    adb::input_tap(TAP_TEXT_MID_X, TAP_TEXT_MID_Y).expect("Failed to tap");

    let cursor_pos = app.wait_for_cursor_change(cursor_count_before, TIMEOUT)
        .expect("Click did not produce any CURSOR_POS events");
    assert!(cursor_pos > 0 && cursor_pos < 6,
        "Cursor should be in middle, got position {}", cursor_pos);

    // Clicking should not have changed the text
    let text = app.last_text().expect("No text after click");
    assert_eq!(text, "abcdef", "Click changed text content to '{}'", text);

    // Backspace should delete from cursor position, not from end
    let change_count = app.text_changed_count();
    adb::input_keyevent(adb::KEYCODE_DEL).expect("Failed to send backspace");
    let text = app.wait_for_text_change(change_count, TIMEOUT)
        .expect("No TEXT_CHANGED after backspace");
    assert_ne!(text, "abcde",
        "Backspace deleted from end instead of cursor position");
    assert_eq!(text.len(), 5,
        "Expected 5 characters after deleting one from 'abcdef', got '{}'", text);

    // Type at cursor position - should insert in middle, not at end
    let change_count = app.text_changed_count();
    adb::input_text("X").expect("Failed to send text");
    let text = app.wait_for_text_change(change_count, TIMEOUT)
        .expect("No TEXT_CHANGED after typing X");
    assert!(!text.ends_with("X") || text.len() != 6,
        "Typed 'X' was appended to end instead of inserted at cursor position, got '{}'", text);

    // Verify hardware (AHB) buffers were used (GDK_GL=gles:always → GL rendering → AHB)
    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(logs.contains("AHB imported as texture"),
        "Expected AHB buffer import in compositor logs (GDK_GL=gles:always should use hardware buffers)");

    app.stop().expect("Debug app crashed or failed to stop cleanly");
}

const FIREFOX_CMD: &str = "GDK_GL=disabled MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 \
    MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 \
    MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
    DISPLAY= firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);

#[test]
fn test_firefox_launches_with_hardware_buffers() {
    setup();
    adb::logcat_clear().expect("Failed to clear logcat");

    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog
    // (killall doesn't give Firefox a clean shutdown, leaving these behind)
    let _ = adb::chroot_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock"
    );

    let mut firefox = ChrootProcess::spawn(FIREFOX_CMD)
        .expect("Failed to spawn Firefox");
    firefox.ensure_pgid();

    // Poll logcat until we see an AHB import, meaning Firefox rendered a frame.
    // Also check if Firefox crashed (chroot process exits early).
    let deadline = Instant::now() + FIREFOX_LAUNCH_TIMEOUT;
    let mut saw_ahb = false;
    while Instant::now() < deadline {
        if !firefox.is_running() {
            firefox.stop().ok();
            panic!("Firefox crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if logs.contains("AHB imported as texture") {
            saw_ahb = true;
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }
    // Let Firefox finish opening its window before killing it
    thread::sleep(Duration::from_millis(1000));

    firefox.stop().expect("Firefox process group failed to stop cleanly");

    assert!(saw_ahb,
        "Firefox did not produce hardware (AHB) buffer imports within {:?}",
        FIREFOX_LAUNCH_TIMEOUT);
}
