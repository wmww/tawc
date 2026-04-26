//! Shared test helpers used by the per-group submodules under `tests/`.
//! OnceLock state means one-time setup (compositor start, debug-app
//! build) runs once per `cargo test` invocation.

use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use crate::chroot_process::ChrootProcess;
use crate::debug_app::DebugApp;
use crate::{adb, chroot, compositor};

/// Default deadline for short waits (compositor state queries, app startup
/// after a window is mapped, etc).
pub const TIMEOUT: Duration = Duration::from_secs(5);

/// One-time compositor setup shared by all tests in the current binary.
/// Idempotent. Does NOT stop the compositor on exit — the next test binary
/// (or the next `cargo test` run) reuses it via `ensure_running`'s
/// already-running fast-path. `run-integration-tests.sh` does a final
/// force-stop after the whole suite completes.
pub fn ensure_compositor() {
    static INIT: OnceLock<()> = OnceLock::new();
    INIT.get_or_init(|| {
        compositor::ensure_running().expect("Failed to ensure compositor is running");
    });
}

/// Build/install the gtk4-debug-app inside the chroot if needed and return
/// the path to the binary. Memoized per test binary.
pub fn ensure_gtk4_debug_app() -> String {
    ensure_compositor();
    static BIN: OnceLock<String> = OnceLock::new();
    BIN.get_or_init(|| chroot::ensure_debug_app().expect("gtk4 debug app build"))
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

/// Spawn a long-running graphical app via ChrootProcess and wait until the
/// compositor sees an AHB buffer import (= the app has rendered its first
/// hardware-buffered frame). Panics if the app crashes or doesn't render
/// within `timeout`.
///
/// On return the process is still running; the caller is responsible for
/// stopping it (typically via `proc.stop()`).
pub fn launch_and_wait_for_ahb(cmd: &str, name: &str, timeout: Duration) -> ChrootProcess {
    ensure_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut proc = ChrootProcess::spawn(cmd).unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
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
