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

/// Reset app-side mutable test state. Call this at the start of every
/// integration test so persisted device settings and previous test input
/// state cannot leak in.
pub fn test_init() {
    require_compositor();
    let closed = adb::test_init().expect("test-init action");
    if closed > 0 {
        assert_compositor_clean();
    }
}

/// Return the toolkitless Wayland debug app path, verifying that
/// the integration runner copied it into the rootfs. Memoized per test binary.
pub fn ensure_wayland_debug_app() -> String {
    require_compositor();
    static BIN: OnceLock<String> = OnceLock::new();
    BIN.get_or_init(|| rootfs::ensure_wayland_debug_app().expect("wayland debug app build"))
        .clone()
}

/// Wait until test-mode input has an active `TawcInputConnection`.
pub fn wait_for_keyboard_shown(timeout: Duration) {
    adb::wait_for_active_input_connection(timeout)
        .expect("TawcInputConnection did not become active");
}

/// Start the toolkitless Wayland debug app's text-input mode and wait
/// until text-input-v3 is enabled. This app uses plain libwayland + SHM and
/// owns its edit buffer, so it is faster to launch than GTK and gives tests
/// full control over text/cursor behaviour.
pub fn start_wayland_debug_text_input(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
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
    let app = DebugApp::start(backend, &binary, "text-input-no-surrounding", env)
        .expect("Failed to start surrounding-less wayland debug app");
    app.wait_ready()
        .expect("Surrounding-less wayland debug app did not become ready");
    wait_for_keyboard_shown(TIMEOUT);
    adb::wait_for_active_input_connection(TIMEOUT)
        .expect("TawcInputConnection did not become active");
    app
}

/// Start wayland-debug-app's Qt/KTextEditor-style text-input mode: the
/// active preedit is included in `set_surrounding_text` reports (cursor
/// after it), and preedit-only changes also push a report. Kate under Qt
/// behaves this way; the compositor must tolerate the echo without
/// telling the IME its composition ended.
pub fn start_wayland_debug_text_input_echo_preedit(
    backend: GraphicsBackend,
    env: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, "text-input-echo-preedit", env)
        .expect("Failed to start echo-preedit wayland debug app");
    app.wait_ready()
        .expect("Echo-preedit wayland debug app did not become ready");
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
    start_wayland_debug_clipboard_text_source(backend, env, "clipboard-copy", text)
}

/// GTK3-style copy: the debug app sets the clipboard selection twice
/// back-to-back, the second time re-announcing SAVE_TARGETS.
pub fn start_wayland_debug_clipboard_copy_double(
    backend: GraphicsBackend,
    env: &str,
    text: &str,
) -> DebugApp {
    start_wayland_debug_clipboard_text_source(backend, env, "clipboard-copy-double", text)
}

fn start_wayland_debug_clipboard_text_source(
    backend: GraphicsBackend,
    env: &str,
    command: &str,
    text: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let subcommand = format!("{} '{}'", command, text.replace('\'', "'\\''"));
    let app = DebugApp::start(backend, &binary, &subcommand, env)
        .unwrap_or_else(|e| panic!("Failed to start wayland {command} debug app: {e}"));
    app.wait_ready()
        .unwrap_or_else(|e| panic!("Wayland {command} debug app did not become ready: {e}"));
    app.wait_for("CLIPBOARD_SET", TIMEOUT)
        .unwrap_or_else(|e| panic!("Wayland {command} debug app did not set clipboard: {e}"));
    app.wait_for("CLIPBOARD_SEND", TIMEOUT)
        .unwrap_or_else(|e| panic!("Wayland {command} debug app did not receive send request: {e}"));
    app
}

pub fn start_wayland_debug_clipboard_overcap(backend: GraphicsBackend, env: &str) -> DebugApp {
    start_wayland_debug_clipboard_source(backend, env, "clipboard-copy-overcap", "overcap")
}

pub fn start_wayland_debug_clipboard_timeout(backend: GraphicsBackend, env: &str) -> DebugApp {
    start_wayland_debug_clipboard_source(backend, env, "clipboard-copy-timeout", "timeout")
}

fn start_wayland_debug_clipboard_source(
    backend: GraphicsBackend,
    env: &str,
    subcommand: &str,
    label: &str,
) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, subcommand, env)
        .unwrap_or_else(|e| panic!("Failed to start wayland clipboard-{label} debug app: {e}"));
    app.wait_ready()
        .unwrap_or_else(|e| panic!("Wayland clipboard-{label} debug app did not become ready: {e}"));
    app.wait_for("CLIPBOARD_SET", TIMEOUT)
        .unwrap_or_else(|e| panic!("Wayland clipboard-{label} debug app did not set clipboard: {e}"));
    app.wait_for("CLIPBOARD_SEND", TIMEOUT)
        .unwrap_or_else(|e| panic!("Wayland clipboard-{label} debug app did not receive send request: {e}"));
    app
}

pub fn start_wayland_debug_clipboard_paste(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
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
    let app = DebugApp::start(backend, &binary, "touch", env)
        .expect("Failed to start wayland touch debug app");
    app.wait_ready()
        .expect("Wayland touch debug app did not become ready");
    app
}

/// Start wayland-debug-app's fractional-scale reporter.
pub fn start_wayland_debug_scale(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, "scale", env)
        .expect("Failed to start wayland scale debug app");
    app.wait_ready()
        .expect("Wayland scale debug app did not become ready");
    app
}

/// Start wayland-debug-app's deterministic fullscreen SHM render pattern.
pub fn start_wayland_debug_render_pattern(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, "render-pattern", env)
        .expect("Failed to start wayland render-pattern debug app");
    app.wait_ready()
        .expect("Wayland render-pattern debug app did not become ready");
    app
}

/// Start wayland-debug-app's subsurface touch scene.
pub fn start_wayland_debug_subsurface(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
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
    let app = DebugApp::start(backend, &binary, "subsurface-input-empty", env)
        .expect("Failed to start wayland input-empty subsurface debug app");
    app.wait_ready()
        .expect("Wayland input-empty subsurface debug app did not become ready");
    app
}

/// Start wayland-debug-app's xdg_popup touch scene.
pub fn start_wayland_debug_popup(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, "popup", env)
        .expect("Failed to start wayland popup debug app");
    app.wait_ready()
        .expect("Wayland popup debug app did not become ready");
    app
}

/// Start wayland-debug-app's grabbed popup switching scene.
pub fn start_wayland_debug_popup_switch(backend: GraphicsBackend, env: &str) -> DebugApp {
    let binary = ensure_wayland_debug_app();
    let app = DebugApp::start(backend, &binary, "popup-switch", env)
        .expect("Failed to start wayland popup switch debug app");
    app.wait_ready()
        .expect("Wayland popup switch debug app did not become ready");
    app
}

/// True if the compositor currently has at least one android_wlegl/AHB-backed
/// surface.
pub fn has_ahb_surface() -> bool {
    compositor::query_state(Duration::from_secs(2))
        .map(|state| state.surfaces_wlegl >= 1)
        .unwrap_or(false)
}

/// Assert that an animating client is actually committing new frames, not
/// stuck on its swapchain. The compositor's `frames` counter only advances
/// when a client commits — either via a new buffer import or a re-attach
/// of an existing wl_buffer (`buffer_commit_pending`). A client blocked in
/// `vkAcquireNextImageKHR` or otherwise wedged keeps the counter flat.
///
/// Polls `frames` until it has advanced by `min_frames`, failing if
/// that doesn't happen within `window`. `min_frames` should be set
/// well below the steady-state rate (~60 fps × window) but high
/// enough that "0 frames" is unambiguous. A healthy client passes as
/// soon as the frames land, so the window is a deadline, not a fixed
/// measurement cost.
pub fn assert_client_animating(name: &str, window: Duration, min_frames: u64) {
    let before = compositor::query_state(TIMEOUT)
        .unwrap_or_else(|e| panic!("query compositor state before {name} animation check: {e}"));
    let delta = compositor::wait_for_frames_advance(before.frames, min_frames, window);
    assert!(
        delta >= min_frames,
        "{name} appears stuck — compositor rendered {delta} frames in {window:?} \
         (expected >= {min_frames}). before={before:?}. \
         Likely the client is blocked on its swapchain (no buffer release / \
         frame callback from the compositor)."
    );
}

/// True if the compositor currently has at least one SHM-backed surface.
///
/// This is the test oracle for clients expected to use SHM.
pub fn has_shm_surface() -> bool {
    compositor::query_state(Duration::from_secs(2))
        .map(|state| state.surfaces_shm >= 1)
        .unwrap_or(false)
}

/// Assert the compositor has no connected clients or toplevels, and that
/// the screen actually shows an empty frame (not a stale frame from the
/// previous client).
pub fn assert_compositor_clean() {
    let _ = adb::cleanup_rootfs();
    // All counts (clients, toplevels, surfaces, hosts) are polled in
    // one condition: host teardown goes through an async Android
    // Activity finish() round trip, so the host counts can trail the
    // surface counts by a moment and must not be asserted point-in-time.
    compositor::wait_for_clean_state(TIMEOUT)
        .expect("Compositor did not return to clean state");
    // Verify the screen reflects the clean state — the last rendered frame
    // should show 0 toplevels, not a stale frame from the previous client.
    compositor::wait_for_rendered_toplevels(0, TIMEOUT)
        .expect("Screen still shows toplevels after cleanup");
}

/// Pre-launch profile hygiene for Firefox tests. The harness kills
/// Firefox without a clean shutdown, which leaves two kinds of poison
/// in the profile:
///
///   - lock/session files that trigger the session-restore or
///     "Firefox is already running" paths;
///   - an ever-growing `toolkit.startup.recent_crashes` counter
///     (`toolkit.startup.last_success` never updates because no run
///     shuts down cleanly). Once it passes max_resumed_crashes (3),
///     every launch silently relaunches into safe mode: no window
///     within the toplevel wait, and safe mode forces software
///     rendering, which breaks the AHB assertions even when the
///     relaunch works.
///
/// Delete the locks and pin `toolkit.startup.max_resumed_crashes=-1`
/// (detector off) via user.js, which Firefox re-applies every startup.
/// The prefs.js guard keeps user.js out of non-profile dirs the glob
/// also matches ("Profile Groups").
pub fn firefox_profile_cleanup(backend: GraphicsBackend) {
    let _ = adb::rootfs_run_with(
        backend,
        r#"rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json; \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups; \
         for d in ~/.config/mozilla/firefox/*/; do \
            [ -f "$d/prefs.js" ] || continue; \
            echo 'user_pref("toolkit.startup.max_resumed_crashes", -1);' > "$d/user.js"; \
         done"#,
    );
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

    assert!(
        painted,
        "{name} did not reach first paint within {:?}",
        timeout
    );

    // Wait until the window actually shows up in a rendered frame
    // (not just in client state) before the caller acts. Replaces a
    // flat 1s grace sleep; tests that drive input immediately after
    // launch (e.g. `lxterminal`) additionally gate on
    // `wait_for_keyboard_shown`, which is the real IM-readiness signal.
    compositor::wait_for_rendered_toplevels_at_least(1, TIMEOUT)
        .unwrap_or_else(|e| panic!("{name} window never reached the screen: {e}"));
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
        if has_ahb_surface() {
            saw_ahb = true;
            break;
        }
        thread::sleep(Duration::from_millis(200));
    }

    assert!(
        saw_ahb,
        "{name} did not produce hardware (AHB) buffer imports within {:?}",
        timeout
    );

    // Wait for the window to land in a rendered frame so the caller
    // doesn't act on (or kill) an app that is still opening. Replaces
    // a flat 1s grace sleep.
    compositor::wait_for_rendered_toplevels_at_least(1, TIMEOUT)
        .unwrap_or_else(|e| panic!("{name} window never reached the screen: {e}"));
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

    let mut app = RootfsProcess::spawn_with(backend, cmd)
        .unwrap_or_else(|e| panic!("Failed to spawn {name}: {e}"));
    app.ensure_pgid();

    let deadline = Instant::now() + timeout;
    let mut saw = None;
    while Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("{name} crashed/exited before rendering");
        }
        if let Ok(state) = compositor::query_state(Duration::from_secs(2)) {
            if state.surfaces_shm >= 1 {
                saw = Some(state);
                break;
            }
        }
        thread::sleep(Duration::from_millis(200));
    }
    let Some(first_seen) = saw else {
        panic!(
            "{name} did not produce SHM buffer imports or an attached SHM surface within {timeout:?}"
        );
    };
    // Give the client a chance to make a second commit so a regression
    // that only manifests then (e.g. GTK4's cairo double-release bug)
    // still trips the no-AHB / still-running checks below. Waits on the
    // compositor's per-commit `frames` counter instead of a flat 500ms;
    // a client that stays static after one commit is fine, so a
    // shortfall at the deadline is not an error.
    compositor::wait_for_frames_advance(first_seen.frames, 1, Duration::from_millis(500));

    let state =
        compositor::query_state(TIMEOUT).expect("query compositor state while client running");
    assert!(
        state.surfaces_wlegl == 0,
        "Unexpected AHB import — {name} should not use hardware buffers \
         under backend={:?}: {state:?}",
        backend,
    );
    assert!(
        app.is_running(),
        "{name} exited shortly after first SHM commit (regression of an \
         SHM cleanup bug under backend={:?}?)",
        backend,
    );

    assert!(
        state.toplevels >= 1,
        "compositor sees no toplevel for {name}: {state:?}"
    );
    assert!(
        state.surfaces_shm >= 1,
        "compositor sees no SHM-backed surface for {name}: {state:?}"
    );
    assert!(
        state.surfaces_wlegl == 0,
        "{name} attached an AHB surface under backend={:?}: {state:?}",
        backend,
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
    assert!(
        state.surfaces_wlegl >= 1,
        "compositor sees no AHB-backed surface for {name}: {state:?}"
    );
    app.stop()
        .unwrap_or_else(|e| panic!("{name} failed to stop cleanly: {e}"));
    assert_compositor_clean();
}
