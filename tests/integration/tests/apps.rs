//! Application-level smoke tests. Verify that real desktop programs
//! launch, map a toplevel, and (for some) do something simple in response
//! to input. Deliberately do **not** assert which buffer path the client
//! uses — that's the `graphics::` module's job. The same app may appear
//! here (launch smoke) and in `graphics::` (buffer-path coverage); see
//! `notes/testing.md` "Test layout" for the partition.
//!
//! Requires the compositor to be up and an in-app distro installed. Most
//! tests need a real GPU stack, so they fail on the emulator.

use std::time::Duration;

use tawc_integration::helpers::{
    assert_compositor_clean, launch_and_wait_for_toplevel, TIMEOUT,
};
use tawc_integration::{adb, compositor};

const FIREFOX_CMD: &str = "firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);
const LXTERMINAL_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const LXTERMINAL_EXIT_TIMEOUT: Duration = Duration::from_secs(10);

/// Firefox launches, maps a toplevel, and the compositor sees a client.
/// Buffer-path coverage lives in `graphics::test_firefox_uses_hardware_buffers`.
#[test]
fn test_firefox_launches() {
    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog
    // (killall doesn't give Firefox a clean shutdown, leaving these behind)
    let _ = adb::rootfs_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups",
    );

    let mut firefox = launch_and_wait_for_toplevel(FIREFOX_CMD, "Firefox", FIREFOX_LAUNCH_TIMEOUT);

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client while Firefox running, got {:?}",
        state
    );

    firefox.stop().expect("Firefox process group failed to stop cleanly");
    assert_compositor_clean();
}

/// supertuxkart launches and maps a toplevel; sticks around past first
/// paint (some GL init failures only manifest a moment after the window
/// appears). Buffer-path coverage lives in
/// `graphics::test_supertuxkart_uses_hardware_buffers`.
#[test]
fn test_supertuxkart_launches() {
    let mut stk = launch_and_wait_for_toplevel("supertuxkart", "supertuxkart", STK_LAUNCH_TIMEOUT);

    std::thread::sleep(Duration::from_secs(1));
    assert!(
        stk.is_running(),
        "supertuxkart exited shortly after mapping its window"
    );

    stk.stop()
        .expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}

/// lxterminal hosts a VTE terminal — the canonical surroundingless
/// text-input-v3 client (see `test_surroundingless_client_uses_keyboard_for_backspace`
/// in tests/input.rs for the protocol-level coverage). This test wires
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
/// fresh install already has it — no `scripts/install-test-deps.sh`
/// rerun needed.
#[test]
fn test_lxterminal_input_and_exit() {
    // Stub out the system IME so it doesn't amplify our `commitText` into
    // the lxterminal IC and produce extra characters.
    adb::enable_test_input().expect("enable-test-input action");

    let mut term = launch_and_wait_for_toplevel(
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
