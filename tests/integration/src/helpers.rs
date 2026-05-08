//! Shared test helpers used by the per-group submodules under `tests/`.
//! OnceLock state means one-time setup (debug-app build) runs once per
//! `cargo test` invocation. The compositor itself is launched by
//! `run-integration-tests.sh` before the test binary starts; tests just
//! assert it's there.

use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use crate::rootfs_process::RootfsProcess;
use crate::debug_app::DebugApp;
use crate::{adb, rootfs, compositor};

/// Default deadline for short waits (compositor state queries, app startup
/// after a window is mapped, etc).
pub const TIMEOUT: Duration = Duration::from_secs(5);

/// Fail the current test with a clear message if the compositor isn't
/// running on the device. Tests must call this (directly or via another
/// helper that does) before touching the compositor — the launch lives
/// in `run-integration-tests.sh`, not in the test harness.
pub fn require_compositor() {
    compositor::assert_running();
}

/// Build/install the gtk4-debug-app inside the chroot if needed and return
/// the path to the binary. Memoized per test binary.
pub fn ensure_gtk4_debug_app() -> String {
    require_compositor();
    static BIN: OnceLock<String> = OnceLock::new();
    BIN.get_or_init(|| rootfs::ensure_debug_app().expect("gtk4 debug app build"))
        .clone()
}

/// Wait until the compositor reports that the Android keyboard has been shown,
/// meaning at least one client has enabled text input. Polls the `tawc` logcat
/// tag for the `onShowKeyboard` message emitted by NativeBridge.
pub fn wait_for_keyboard_shown(timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let logs = adb::logcat_dump("tawc").expect("Failed to dump logcat");
        if logs.contains("onShowKeyboard") {
            return;
        }
        if Instant::now() > deadline {
            panic!("Timeout waiting for onShowKeyboard in logcat");
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Start the text-input subcommand of the gtk4 debug app and wait for READY.
/// `env` is prepended to the launch command (e.g. "GSK_RENDERER=cairo");
/// pass "" for no extra env. Verifies the compositor sees the toplevel
/// before returning.
pub fn start_text_input(env: &str) -> DebugApp {
    let binary = ensure_gtk4_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(&binary, "text-input", env).expect("Failed to start debug app");
    app.wait_ready().expect("Debug app did not become ready");

    // Wait for the client's IM context to enable text input. Without this,
    // text injected via broadcast is dropped because the compositor gates
    // commit_string on the text-input-v3 enabled flag.
    wait_for_keyboard_shown(TIMEOUT);

    let state =
        compositor::query_state(TIMEOUT).expect("Failed to query compositor state after app start");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel after app start, got {:?}",
        state
    );
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client after app start, got {:?}",
        state
    );

    app
}

/// True if compositor logcat shows a libhybris android_wlegl AHB import.
pub fn saw_ahb_import(logs: &str) -> bool {
    logs.contains("wlegl: imported ANativeWindowBuffer as texture")
}

/// Assert that an animating client is actually committing new frames, not
/// stuck on its swapchain. The compositor's `frames` counter only advances
/// when a client commits — either via a new buffer import or a re-attach
/// of an existing wl_buffer (`buffer_commit_pending`). A client blocked in
/// `vkAcquireNextImageKHR` or otherwise wedged keeps the counter flat.
///
/// Samples `frames` twice over `window`. Fails if the delta is below
/// `min_frames`, which should be set well below the steady-state rate
/// (~60 fps × window) but high enough that "0 frames" is unambiguous.
pub fn assert_client_animating(name: &str, window: Duration, min_frames: u64) {
    let before = compositor::query_state(TIMEOUT)
        .unwrap_or_else(|e| panic!("query compositor state before {name} animation check: {e}"));
    thread::sleep(window);
    let after = compositor::query_state(TIMEOUT)
        .unwrap_or_else(|e| panic!("query compositor state after {name} animation check: {e}"));

    let delta = after.frames.saturating_sub(before.frames);
    assert!(
        delta >= min_frames,
        "{name} appears stuck — compositor rendered {delta} frames in {window:?} \
         (expected >= {min_frames}). before={before:?} after={after:?}. \
         Likely the client is blocked on its swapchain (no buffer release / \
         frame callback from the compositor)."
    );
}

/// True if compositor logcat shows an SHM buffer import.
pub fn saw_shm_import(logs: &str) -> bool {
    logs.contains("SHM buffer imported")
}

/// Assert the compositor has no connected clients or toplevels, and that
/// the screen actually shows an empty frame (not a stale frame from the
/// previous client).
pub fn assert_compositor_clean() {
    let state = compositor::wait_for_state(0, 0, TIMEOUT)
        .expect("Compositor did not return to clean state");
    assert_eq!(
        state.surfaces_wlegl, 0,
        "Expected no wlegl surfaces after cleanup, got {:?}",
        state
    );
    assert_eq!(
        state.surfaces_shm, 0,
        "Expected no SHM surfaces after cleanup, got {:?}",
        state
    );
    // Verify the screen reflects the clean state — the last rendered frame
    // should show 0 toplevels, not a stale frame from the previous client.
    compositor::wait_for_rendered_toplevels(0, TIMEOUT)
        .expect("Screen still shows toplevels after cleanup");
}

/// Spawn a long-running graphical app via RootfsProcess and wait until the
/// compositor sees an AHB buffer import (= the app has rendered its first
/// hardware-buffered frame). Panics if the app crashes or doesn't render
/// within `timeout`.
///
/// On return the process is still running; the caller is responsible for
/// stopping it (typically via `proc.stop()`).
pub fn launch_and_wait_for_ahb(cmd: &str, name: &str, timeout: Duration) -> RootfsProcess {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut proc = RootfsProcess::spawn(cmd).unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
    proc.ensure_pgid();

    let deadline = Instant::now() + timeout;
    let mut saw_ahb = false;
    while Instant::now() < deadline {
        if !proc.is_running() {
            proc.stop().ok();
            panic!("{name} crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_ahb_import(&logs) {
            saw_ahb = true;
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    // Let the app finish opening its window before the caller kills it
    thread::sleep(Duration::from_millis(1000));

    assert!(
        saw_ahb,
        "{name} did not produce hardware (AHB) buffer imports within {:?}",
        timeout
    );
    proc
}
