use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use tawc_integration::{adb, chroot, chroot_process::ChrootProcess, compositor, debug_app::DebugApp};

extern "C" fn stop_compositor() {
    compositor::stop_if_started();
}

/// One-time compositor setup shared by all tests. Idempotent.
fn ensure_compositor() {
    static INIT: OnceLock<()> = OnceLock::new();
    INIT.get_or_init(|| {
        // Register atexit handler to stop compositor when tests finish
        unsafe { libc::atexit(stop_compositor) };
        compositor::ensure_running().expect("Failed to ensure compositor is running");
    });
}

/// One-time setup for a specific debug-app variant. Returns the binary path
/// inside the chroot. Each variant has its own cache so switching between
/// GTK3 and GTK4 tests doesn't re-run the build-check dance.
fn setup_app(spec: &chroot::DebugAppSpec) -> String {
    ensure_compositor();
    match spec.name {
        "gtk3-debug-app" => {
            static GTK3_BIN: OnceLock<String> = OnceLock::new();
            GTK3_BIN
                .get_or_init(|| chroot::ensure_debug_app(&chroot::GTK3).expect("gtk3 debug app"))
                .clone()
        }
        "gtk4-debug-app" => {
            static GTK4_BIN: OnceLock<String> = OnceLock::new();
            GTK4_BIN
                .get_or_init(|| chroot::ensure_debug_app(&chroot::GTK4).expect("gtk4 debug app"))
                .clone()
        }
        other => panic!("unknown debug app spec: {}", other),
    }
}

const TIMEOUT: Duration = Duration::from_secs(5);

/// Start the text-input subcommand of a debug app and wait for READY.
/// `env` is prepended to the launch command (e.g. "GDK_GL=disabled" or
/// "LD_BIND_NOW=1"); pass "" for no extra env.
fn start_text_input(spec: &chroot::DebugAppSpec, env: &str) -> DebugApp {
    let binary = setup_app(spec);
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(&binary, "text-input", env).expect("Failed to start debug app");
    app.wait_ready().expect("Debug app did not become ready");

    // Verify compositor sees the toplevel
    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state after app start");
    assert!(state.toplevels >= 1,
        "Compositor should see at least 1 toplevel after app start, got {:?}", state);
    assert!(state.clients >= 1,
        "Compositor should see at least 1 client after app start, got {:?}", state);

    app
}

/// True if compositor logcat shows a libhybris android_wlegl AHB import.
fn saw_ahb_import(logs: &str) -> bool {
    logs.contains("wlegl: imported ANativeWindowBuffer as texture")
}

/// Assert the compositor has no connected clients or toplevels, and that
/// the screen actually shows an empty frame (not a stale frame from the
/// previous client).
fn assert_compositor_clean() {
    let state = compositor::wait_for_state(0, 0, TIMEOUT)
        .expect("Compositor did not return to clean state");
    assert_eq!(state.surfaces_wlegl, 0,
        "Expected no wlegl surfaces after cleanup, got {:?}", state);
    assert_eq!(state.surfaces_shm, 0,
        "Expected no SHM surfaces after cleanup, got {:?}", state);
    // Verify the screen reflects the clean state — the last rendered frame
    // should show 0 toplevels, not a stale frame from the previous client.
    compositor::wait_for_rendered_toplevels(0, TIMEOUT)
        .expect("Screen still shows toplevels after cleanup");
}

// Physical screen coordinates for tapping inside the text view.
// Compositor uses 2x scale, so logical = physical / 2.
// Text content starts at approximately physical (80, 234).
// Monospace 18pt ≈ 22 physical px per character.
const TAP_TEXT_MID_X: u32 = 200;
const TAP_TEXT_MID_Y: u32 = 250;

#[test]
fn test_text_input_and_backspace() {
    let mut app = start_text_input(&chroot::GTK3, "GDK_GL=disabled");

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
    assert!(!saw_ahb_import(&logs),
        "Unexpected AHB buffer import (GDK_GL=disabled should not use hardware buffers)");

    app.stop().expect("Debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_click_cursor_positioning() {
    let mut app = start_text_input(&chroot::GTK3, "GDK_GL=gles:always");

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
    assert!(saw_ahb_import(&logs),
        "Expected AHB buffer import in compositor logs (GDK_GL=gles:always should use hardware buffers)");

    app.stop().expect("Debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

const FIREFOX_CMD: &str = "GDK_GL=gles:always MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 \
    MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 \
    MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
    DISPLAY= firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);

#[test]
fn test_firefox_launches_with_hardware_buffers() {
    ensure_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog
    // (killall doesn't give Firefox a clean shutdown, leaving these behind)
    let _ = adb::chroot_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups"
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
        if saw_ahb_import(&logs) {
            saw_ahb = true;
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    // Let Firefox finish opening its window before killing it
    thread::sleep(Duration::from_millis(1000));

    assert!(saw_ahb,
        "Firefox did not produce hardware (AHB) buffer imports within {:?}",
        FIREFOX_LAUNCH_TIMEOUT);

    // Verify compositor sees Firefox's toplevel
    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    assert!(state.toplevels >= 1,
        "Compositor should see at least 1 toplevel while Firefox running, got {:?}", state);
    assert!(state.clients >= 1,
        "Compositor should see at least 1 client while Firefox running, got {:?}", state);

    firefox.stop().expect("Firefox process group failed to stop cleanly");
    assert_compositor_clean();
}

const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);

#[test]
fn test_supertuxkart_launches_with_hardware_buffers() {
    ensure_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut stk = ChrootProcess::spawn("supertuxkart")
        .expect("Failed to spawn supertuxkart");
    stk.ensure_pgid();

    // Poll logcat until we see an AHB import, meaning STK rendered a frame.
    // STK compiles many shaders during startup so allow a long timeout.
    let deadline = Instant::now() + STK_LAUNCH_TIMEOUT;
    let mut saw_ahb = false;
    while Instant::now() < deadline {
        if !stk.is_running() {
            stk.stop().ok();
            panic!("supertuxkart crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_ahb_import(&logs) {
            saw_ahb = true;
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    // Let STK finish opening its window before killing it
    thread::sleep(Duration::from_millis(1000));

    assert!(saw_ahb,
        "supertuxkart did not produce hardware (AHB) buffer imports within {:?}",
        STK_LAUNCH_TIMEOUT);

    assert!(stk.is_running(),
        "supertuxkart exited after reaching first render");

    // Verify compositor sees STK's toplevel
    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while STK running");
    assert!(state.toplevels >= 1,
        "Compositor should see at least 1 toplevel while STK running, got {:?}", state);
    assert!(state.clients >= 1,
        "Compositor should see at least 1 client while STK running, got {:?}", state);

    stk.stop().expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK4 on libhybris always renders through android_wlegl (no software path:
/// the SHM fallback is GTK3-specific). This test drives the GTK4 debug app
/// through the same text-input flow as the GTK3 cursor test and asserts the
/// compositor saw AHB buffer imports.
#[test]
fn test_gtk4_text_input_hardware_buffers() {
    let mut app = start_text_input(&chroot::GTK4, "");

    adb::input_text("gtk4 works").expect("Failed to send text");
    app.wait_for_text("gtk4 works", TIMEOUT)
        .expect("Text should be 'gtk4 works'");

    adb::input_keyevent(adb::KEYCODE_DEL).expect("Failed to send backspace");
    app.wait_for_text("gtk4 work", TIMEOUT)
        .expect("Text should be 'gtk4 work' after backspace");

    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(saw_ahb_import(&logs),
        "Expected AHB buffer import in compositor logs (GTK4 on libhybris is always hardware-buffered)");

    app.stop().expect("gtk4-debug-app crashed or failed to stop cleanly");
    assert_compositor_clean();
}
