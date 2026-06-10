//! Application-level smoke tests. Verify that real desktop programs
//! launch, map a toplevel, and (for some) do something simple in response
//! to input. Deliberately do **not** assert which buffer path the client
//! uses — that's the `cpu_graphics::` / `libhybris::` / `gfxstream::`
//! modules' job. The same app may appear here (launch smoke) and in one
//! of those (buffer-path coverage); see `notes/testing.md` "Test layout"
//! for the partition.
//!
//! Every test here runs under [`GraphicsBackend::Cpu`]: the launch path
//! is the most portable backend and these tests don't care which buffer
//! type ends up on the wire. Backend-specific launches (Firefox/STK
//! under libhybris / gfxstream) live in the per-backend modules.
//!
//! Requires the compositor to be up and an in-app distro installed.

use std::time::Duration;

use tawc_integration::helpers::{
    assert_broker_ok, assert_compositor_clean, firefox_profile_cleanup,
    launch_and_wait_for_toplevel, wait_for_keyboard_shown, TIMEOUT,
};
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

const FIREFOX_CMD: &str = "firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);
const LXTERMINAL_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const LXTERMINAL_EXIT_TIMEOUT: Duration = Duration::from_secs(10);
const GTK_WIDGET_FACTORY_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

// Wayland logical coordinates inside a GtkEntry on gtk4-widget-factory's first page.
// The stock app opens on page 1, whose first column contains editable entries
// near the upper left. Tapping this point focuses one of those GtkEntry widgets
// and makes GTK enable text-input-v3.
const GTK_WIDGET_FACTORY_ENTRY_X: f32 = 85.0;
const GTK_WIDGET_FACTORY_ENTRY_Y: f32 = 110.0;

fn ctrl_key(keycode: u32) {
    assert_broker_ok(
        adb::ic_send_modified_key_event(keycode, true, false, false)
            .unwrap_or_else(|e| panic!("Ctrl key action failed: {e}")),
        "ic-send-modified-key-event",
    );
    std::thread::sleep(Duration::from_millis(150));
}

fn wait_for_android_clipboard(expected: &str, timeout: Duration) {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        if got == expected {
            return;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "Android clipboard did not become {:?}; last={:?}",
            expected,
            got
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// Firefox launches, maps a toplevel, and the compositor sees a client.
/// Buffer-path coverage lives in `libhybris::test_firefox_renders_via_ahb` and
/// `gfxstream::test_firefox_renders_via_ahb`.
#[test]
fn test_firefox_launches() {
    tawc_integration::helpers::test_init();
    firefox_profile_cleanup(BACKEND);

    let mut firefox = launch_and_wait_for_toplevel(BACKEND, FIREFOX_CMD, "Firefox", FIREFOX_LAUNCH_TIMEOUT);

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client while Firefox running, got {:?}",
        state
    );

    firefox.stop().expect("Firefox session failed to stop cleanly");
    assert_compositor_clean();
}

/// supertuxkart launches and maps a toplevel; sticks around past first
/// paint (some GL init failures only manifest a moment after the window
/// appears). Buffer-path coverage lives in
/// `libhybris::test_supertuxkart_renders_via_ahb` and
/// `gfxstream::test_supertuxkart_renders_via_ahb`.
#[test]
fn test_supertuxkart_launches() {
    tawc_integration::helpers::test_init();
    let mut stk = launch_and_wait_for_toplevel(BACKEND, "supertuxkart", "supertuxkart", STK_LAUNCH_TIMEOUT);

    std::thread::sleep(Duration::from_secs(1));
    assert!(
        stk.is_running(),
        "supertuxkart exited shortly after mapping its window"
    );

    stk.stop()
        .expect("supertuxkart session failed to stop cleanly");
    assert_compositor_clean();
}

/// lxterminal hosts a VTE terminal — the canonical surroundingless
/// text-input-v3 client (see
/// `text_input::test_surroundingless_client_uses_keyboard_for_backspace`
/// for the protocol-level coverage). This test wires
/// up the full real-world stack: launch lxterminal, verify it stays up
/// past first paint, type `exit` followed by Enter, and assert the
/// process actually exits. That implicitly proves:
///   - `commit_string` (text-input-v3) reaches VTE → PTY → shell as
///     keystrokes; without it the shell never sees `exit`.
///   - `wl_keyboard` Enter reaches the shell as a newline; without it
///     the shell never executes the typed command.
///   - Compositor cleans up on a normal client exit (no leftover
///     toplevel/client).
///
/// `lxterminal` ships in the distro's [DEFAULT_BASE_PACKAGES], so a
/// fresh install already has it — no extra test package install
/// rerun needed.
#[test]
fn test_lxterminal_input_and_exit() {
    tawc_integration::helpers::test_init();

    let mut term = launch_and_wait_for_toplevel(
        BACKEND,
        "lxterminal",
        "lxterminal",
        LXTERMINAL_LAUNCH_TIMEOUT,
    );

    // launch_and_wait_for_toplevel already panics if the process exits
    // before its first window, but a terminal that opens then closes
    // shortly after (e.g. shell crash on startup) wouldn't be caught by
    // the in-loop check. Re-verify after a grace period.
    std::thread::sleep(Duration::from_secs(1));
    assert!(
        term.is_running(),
        "lxterminal exited shortly after first frame — shell crash on startup?"
    );
    wait_for_keyboard_shown(TIMEOUT);

    // Drive the shell: "exit" via the IC's commitText, newline via
    // wl_keyboard Enter. Same path a soft-keyboard user typing into
    // the terminal would take — the IC translates `commitText` into
    // text-input-v3 `commit_string` and `sendKeyEvent(KEYCODE_ENTER)`
    // into a `wl_keyboard` Enter.
    adb::ic_commit_text("exit").expect("commit 'exit'");
    adb::ic_send_key_event(adb::KEYCODE_ENTER).expect("Enter");

    let deadline = std::time::Instant::now() + LXTERMINAL_EXIT_TIMEOUT;
    while std::time::Instant::now() < deadline {
        if !term.is_running() {
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(
        !term.is_running(),
        "lxterminal still running {:?} after `exit`+Enter — input didn't \
         reach the shell. commit_string lost between text-input-v3 and \
         VTE's PTY, or KEYCODE_ENTER never delivered as a wl_keyboard key.",
        LXTERMINAL_EXIT_TIMEOUT
    );

    // term already exited; stop() returns Err for already-gone
    // processes — discard it. Drop tears down the lingering local adb
    // shell wrapper.
    let _ = term.stop();
    assert_compositor_clean();
}

/// The GTK debug app gives us protocol-level assertions; this is the
/// stock GTK4 app smoke for the same user-facing path. It drives a real
/// `gtk4-widget-factory` GtkEntry:
///   - Android clipboard -> Wayland selection -> GTK paste (`Ctrl+V`)
///   - Android IC text input and editing inside GTK
///   - GTK copy (`Ctrl+C`) -> Wayland selection -> Android clipboard
#[test]
fn test_gtk4_widget_factory_copy_paste_and_text_input() {
    tawc_integration::helpers::test_init();
    let paste_text = "gtk4 widget factory paste";
    let expected = "gtk4 widget factory paste edited";

    adb::clipboard_set_text(paste_text).expect("set Android clipboard");

    let mut factory = launch_and_wait_for_toplevel(
        BACKEND,
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_WIDGET_FACTORY_LAUNCH_TIMEOUT,
    );

    assert_broker_ok(
        adb::inject_touch_logical(GTK_WIDGET_FACTORY_ENTRY_X, GTK_WIDGET_FACTORY_ENTRY_Y)
            .expect("tap gtk4-widget-factory entry"),
        "tap gtk4-widget-factory entry",
    );
    wait_for_keyboard_shown(TIMEOUT);

    ctrl_key(adb::KEYCODE_A);
    ctrl_key(adb::KEYCODE_V);

    adb::ic_commit_text(" input").expect("commit GTK input text");
    std::thread::sleep(Duration::from_millis(250));
    adb::ic_delete_surrounding_text(5, 0).expect("delete GTK input text");
    std::thread::sleep(Duration::from_millis(250));
    adb::ic_commit_text("edited").expect("commit GTK edited text");
    std::thread::sleep(Duration::from_millis(250));

    ctrl_key(adb::KEYCODE_A);
    ctrl_key(adb::KEYCODE_C);
    wait_for_android_clipboard(expected, TIMEOUT);

    factory
        .stop()
        .expect("gtk4-widget-factory failed to stop cleanly");
    assert_compositor_clean();
}
