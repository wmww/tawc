//! Shared test helpers used by the per-group submodules under `tests/`.
//! OnceLock state means one-time path checks run once per
//! `cargo test` invocation. The compositor itself is launched by
//! `run-integration-tests.sh` before the test binary starts; tests just
//! assert it's there.

use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use crate::debug_app::DebugApp;
use crate::rootfs_process::RootfsProcess;
use crate::{adb, compositor, rootfs, GraphicsBackend};

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

/// Return the toolkitless Wayland debug app path, verifying that
/// the integration runner copied it into the rootfs. Memoized per test binary.
pub fn ensure_wayland_debug_app() -> String {
    require_compositor();
    static BIN: OnceLock<String> = OnceLock::new();
    BIN.get_or_init(|| rootfs::ensure_wayland_debug_app().expect("wayland debug app build"))
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

/// Start the toolkitless Wayland debug app's text-input mode and wait
/// until text-input-v3 is enabled. This app uses plain libwayland + SHM and
/// owns its edit buffer, so it is faster to launch than GTK and gives tests
/// full control over text/cursor behaviour.
pub fn start_wayland_debug_text_input(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    adb::enable_test_input().expect("enable-test-input action");
    let app = DebugApp::start(backend, &binary, "text-input", env)
        .expect("Failed to start wayland debug app");
    app.wait_ready()
        .expect("Wayland debug app did not become ready");
    wait_for_keyboard_shown(TIMEOUT);
    adb::wait_for_active_input_connection(TIMEOUT)
        .expect("TawcInputConnection did not become active");
    app
}

/// Start wayland-debug-app's surrounding-less text-input mode.
pub fn start_wayland_debug_text_input_no_surrounding(
    backend: GraphicsBackend,
    env: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    adb::enable_test_input().expect("enable-test-input action");
    let app = DebugApp::start(backend, &binary, "text-input-no-surrounding", env)
        .expect("Failed to start surrounding-less wayland debug app");
    app.wait_ready()
        .expect("Surrounding-less wayland debug app did not become ready");
    wait_for_keyboard_shown(TIMEOUT);
    adb::wait_for_active_input_connection(TIMEOUT)
        .expect("TawcInputConnection did not become active");
    app
}

/// Start wayland-debug-app's text-input mode that reports the cursor before
/// trailing newlines while still reporting the full surrounding text.
pub fn start_wayland_debug_text_input_stale_newline(
    backend: GraphicsBackend,
    env: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    adb::enable_test_input().expect("enable-test-input action");
    let app = DebugApp::start(backend, &binary, "text-input-stale-newline", env)
        .expect("Failed to start stale-newline wayland debug app");
    app.wait_ready()
        .expect("Stale-newline wayland debug app did not become ready");
    wait_for_keyboard_shown(TIMEOUT);
    adb::wait_for_active_input_connection(TIMEOUT)
        .expect("TawcInputConnection did not become active");
    app
}

pub fn start_wayland_debug_clipboard_copy(
    backend: GraphicsBackend,
    env: &str,
    text: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let subcommand = format!("clipboard-copy '{}'", text.replace('\'', "'\\''"));
    let app = DebugApp::start(backend, &binary, &subcommand, env)
        .expect("Failed to start wayland clipboard-copy debug app");
    app.wait_ready()
        .expect("Wayland clipboard-copy debug app did not become ready");
    app.wait_for("CLIPBOARD_SET", TIMEOUT)
        .expect("Wayland clipboard-copy debug app did not set clipboard");
    app.wait_for("CLIPBOARD_SEND", TIMEOUT)
        .expect("Wayland clipboard-copy debug app did not receive clipboard send request");
    app
}

pub fn start_wayland_debug_clipboard_paste(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "clipboard-paste", env)
        .expect("Failed to start wayland clipboard-paste debug app");
    app.wait_ready()
        .expect("Wayland clipboard-paste debug app did not become ready");
    app
}

/// Start wayland-debug-app's fullscreen touch visualizer. It does not
/// enable text-input; tests drive touch through the focused SurfaceView and
/// assert on the client's wl_touch events.
pub fn start_wayland_debug_touch(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "touch", env)
        .expect("Failed to start wayland touch debug app");
    app.wait_ready()
        .expect("Wayland touch debug app did not become ready");
    app
}

/// Start wayland-debug-app's fractional-scale reporter.
pub fn start_wayland_debug_scale(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "scale", env)
        .expect("Failed to start wayland scale debug app");
    app.wait_ready()
        .expect("Wayland scale debug app did not become ready");
    app
}

/// Start wayland-debug-app's subsurface touch scene.
pub fn start_wayland_debug_subsurface(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "subsurface", env)
        .expect("Failed to start wayland subsurface debug app");
    app.wait_ready()
        .expect("Wayland subsurface debug app did not become ready");
    app
}

/// Start wayland-debug-app's subsurface scene with the child input region empty.
pub fn start_wayland_debug_subsurface_input_empty(
    backend: GraphicsBackend,
    env: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "subsurface-input-empty", env)
        .expect("Failed to start wayland input-empty subsurface debug app");
    app.wait_ready()
        .expect("Wayland input-empty subsurface debug app did not become ready");
    app
}

/// Start wayland-debug-app's xdg_popup touch scene.
pub fn start_wayland_debug_popup(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "popup", env)
        .expect("Failed to start wayland popup debug app");
    app.wait_ready()
        .expect("Wayland popup debug app did not become ready");
    app
}

/// Start wayland-debug-app's grabbed popup switching scene.
pub fn start_wayland_debug_popup_switch(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    adb::logcat_clear().expect("Failed to clear logcat");
    let app = DebugApp::start(backend, &binary, "popup-switch", env)
        .expect("Failed to start wayland popup switch debug app");
    app.wait_ready()
        .expect("Wayland popup switch debug app did not become ready");
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
    assert_eq!(
        state.hosts, 0,
        "Expected no Android hosts after cleanup, got {:?}",
        state
    );
    assert_eq!(
        state.bound_hosts, 0,
        "Expected no bound Android hosts after cleanup, got {:?}",
        state
    );
    // Verify the screen reflects the clean state — the last rendered frame
    // should show 0 toplevels, not a stale frame from the previous client.
    compositor::wait_for_rendered_toplevels(0, TIMEOUT)
        .expect("Screen still shows toplevels after cleanup");
}

pub fn assert_broker_ok(output: std::process::Output, action: &str) {
    assert!(
        output.status.success(),
        "{action} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

/// Spawn a long-running graphical app via RootfsProcess and wait until the
/// compositor reports at least one mapped toplevel **with a buffer
/// attached** (i.e. the client has committed its first frame, whatever
/// buffer type it picked). Doesn't care which path — the `apps::` tests
/// use this to verify a program launches and gets to first paint; buffer-
/// type assertions belong in `cpu_graphics::` / `libhybris::` / `gfxstream::`
/// (or `xwayland::`).
///
/// Waiting for first-frame-attached (not just "toplevel mapped") matters
/// for tests that drive the client immediately after launch — e.g.
/// `lxterminal` needs the shell + IM ready before injected text reaches
/// the PTY, and that only happens once the first frame has gone through.
///
/// `backend` pins the in-rootfs [GraphicsBackend] for this spawn. Suite
/// tests pass an explicit value (typically `Cpu` for `apps::`,
/// `settings::`, `text_input::`, and `touch_input::` since they don't
/// care about buffer type and CPU is the most portable path); without an
/// override the user's Settings pick would leak in,
/// making tests non-hermetic.
///
/// Panics if the app crashes or doesn't reach first paint within `timeout`.
pub fn launch_and_wait_for_toplevel(
    backend: GraphicsBackend,
    cmd: &str,
    name: &str,
    timeout: Duration,
) -> RootfsProcess {
    require_compositor();

    let mut proc = RootfsProcess::spawn_with(backend, cmd)
        .unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
    proc.ensure_pgid();

    let deadline = Instant::now() + timeout;
    let mut painted = false;
    while Instant::now() < deadline {
        if !proc.is_running() {
            proc.stop().ok();
            panic!("{name} crashed/exited before first paint");
        }
        if let Ok(state) = compositor::query_state(TIMEOUT) {
            if state.toplevels >= 1 && (state.surfaces_wlegl + state.surfaces_shm) >= 1 {
                painted = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(200));
    }

    // Let the app finish opening before the caller acts. Same grace
    // period as `launch_and_wait_for_ahb` so tests that drive input
    // immediately after launch (e.g. `lxterminal`) get the IM-enable +
    // shell-readiness window they used to get from the AHB-import wait.
    thread::sleep(Duration::from_millis(1000));

    assert!(
        painted,
        "{name} did not reach first paint within {:?}",
        timeout
    );
    proc
}

/// Spawn a long-running graphical app via RootfsProcess and wait until the
/// compositor sees an AHB buffer import (= the app has rendered its first
/// hardware-buffered frame). Panics if the app crashes or doesn't render
/// within `timeout`.
///
/// `backend` pins the in-rootfs [GraphicsBackend] — pass `Libhybris` or
/// `Gfxstream` (the two paths that can actually produce AHBs). CPU has
/// no AHB path, so callers should use [assert_renders_via_shm] instead.
///
/// On return the process is still running; the caller is responsible for
/// stopping it (typically via `proc.stop()`).
pub fn launch_and_wait_for_ahb(
    backend: GraphicsBackend,
    cmd: &str,
    name: &str,
    timeout: Duration,
) -> RootfsProcess {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut proc = RootfsProcess::spawn_with(backend, cmd)
        .unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
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

/// Spawn `cmd`, wait until the compositor imports an SHM buffer for it,
/// assert no AHB import ever fires during the run, assert ≥1 mapped
/// toplevel, then stop cleanly and assert the compositor returns to a
/// clean state. The single-call smoke for the
/// `gtk*-with-software-renderer` / `weston-simple-shm` / cpu-only family.
///
/// `backend` pins the in-rootfs [GraphicsBackend]. Same SHM-only client
/// against libhybris (GDK_GL=disabled, GSK_RENDERER=cairo) versus
/// against CPU (no GPU available → distro Mesa falls through to
/// llvmpipe, the few GL apps that work still pick SHM) tests slightly
/// different invariants — the helper handles both.
pub fn assert_renders_via_shm(backend: GraphicsBackend, cmd: &str, name: &str, timeout: Duration) {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = RootfsProcess::spawn_with(backend, cmd)
        .unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
    app.ensure_pgid();

    let deadline = Instant::now() + timeout;
    let mut saw = false;
    while Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("{name} crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_shm_import(&logs) {
            saw = true;
            break;
        }
        thread::sleep(Duration::from_millis(200));
    }
    // Grace period so a regression that only manifests on the second
    // SHM commit (e.g. GTK4's cairo double-release bug) still trips
    // the no-AHB / still-running checks below.
    thread::sleep(Duration::from_millis(500));

    assert!(
        saw,
        "{name} did not produce SHM buffer imports within {timeout:?}"
    );

    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(
        !saw_ahb_import(&logs),
        "Unexpected AHB import — {name} should not use hardware buffers \
         under backend={:?}",
        backend,
    );
    assert!(
        app.is_running(),
        "{name} exited shortly after first SHM commit (regression of an \
         SHM cleanup bug under backend={:?}?)",
        backend,
    );

    let state =
        compositor::query_state(TIMEOUT).expect("query compositor state while client running");
    assert!(
        state.toplevels >= 1,
        "compositor sees no toplevel for {name}: {state:?}"
    );

    app.stop()
        .unwrap_or_else(|e| panic!("{name} failed to stop cleanly: {e}"));
    assert_compositor_clean();
}

/// Spawn `cmd`, wait for an AHB import (the hardware-buffer fast path),
/// assert ≥1 mapped toplevel, stop cleanly, assert clean. The single-
/// call smoke for the `gtk*-default-renderer` / `weston-simple-egl` /
/// `vkcube` / real-toolkit family.
///
/// `backend` pins the in-rootfs [GraphicsBackend] — meaningful values
/// are `Libhybris` and `Gfxstream`. (Under CPU, the AHB path doesn't
/// exist; the same client would commit SHM and this helper would
/// timeout.)
///
/// Bespoke tests that need a steady-state surface-count or
/// frames-counter check (e.g. Firefox's WebRender → AHB assertion) keep
/// using [launch_and_wait_for_ahb] directly.
pub fn assert_renders_via_ahb(backend: GraphicsBackend, cmd: &str, name: &str, timeout: Duration) {
    let mut app = launch_and_wait_for_ahb(backend, cmd, name, timeout);
    let state =
        compositor::query_state(TIMEOUT).expect("query compositor state while client running");
    assert!(
        state.toplevels >= 1,
        "compositor sees no toplevel for {name}: {state:?}"
    );
    app.stop()
        .unwrap_or_else(|e| panic!("{name} failed to stop cleanly: {e}"));
    assert_compositor_clean();
}
