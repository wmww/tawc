//! Xwayland tests. Anything that exercises the bionic-built Xwayland the
//! compositor socket-activates for X11 clients — pure-X11 clients, the TAWC-DRI
//! AHB-shipping pipe, and EGL-on-X11 via the libhybris X11 platform
//! plugin. Buffer-type assertions inside these tests stay here (rather
//! than moving to `cpu_graphics::` / `libhybris::` / `gfxstream::`) because
//! what's under test is the Xwayland integration, not the buffer
//! plumbing in isolation.
//!
//! The pure-X11 SHM smoke pins [`GraphicsBackend::Cpu`] so it can run
//! on the x86_64 emulator. TAWC-DRI / `xwl_tawc` and the X11 EGL
//! platform plugin are libhybris-native and still pin
//! [`GraphicsBackend::Libhybris`]; those tests require libhybris + a
//! real Android GPU driver and are ignored on x86 devices.

use std::time::{Duration, Instant};

use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    app_fd_targets, assert_broker_ok, assert_compositor_clean, fd_growth, has_shm_surface,
    TIMEOUT,
};
use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::{adb, compositor, rootfs, GraphicsBackend};

const SHM_BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const BACKEND: GraphicsBackend = GraphicsBackend::Libhybris;

const XWAYLAND_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const XWAYLAND_IDLE_STOP_TIMEOUT: Duration = Duration::from_secs(20);

fn xwayland_pids() -> Vec<u32> {
    compositor::query_state(TIMEOUT)
        .expect("query-state for xwayland pids")
        .xwayland_pids
}

fn wait_for_xwayland_running(expected: bool, timeout: Duration) -> Vec<u32> {
    let deadline = Instant::now() + timeout;
    loop {
        let pids = xwayland_pids();
        if pids.is_empty() != expected {
            return pids;
        }
        assert!(
            Instant::now() < deadline,
            "Xwayland running={} did not become {expected}; pids={pids:?}",
            !pids.is_empty()
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn wait_for_xwayland_restarted(old_pids: &[u32], timeout: Duration) -> Vec<u32> {
    let deadline = Instant::now() + timeout;
    loop {
        let pids = xwayland_pids();
        if !pids.is_empty() && pids.iter().all(|pid| !old_pids.contains(pid)) {
            return pids;
        }
        assert!(
            Instant::now() < deadline,
            "Xwayland did not restart; old_pids={old_pids:?} current_pids={pids:?}",
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn wait_for_first_xclock_render(timeout: Duration) {
    let deadline = Instant::now() + timeout;
    while !has_shm_surface() {
        assert!(
            Instant::now() < deadline,
            "xclock never produced an SHM surface"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn x11_socket_exists() -> bool {
    adb::rootfs_run_with(SHM_BACKEND, "test -S /tmp/.X11-unix/X0")
        .map(|output| output.status.success())
        .unwrap_or(false)
}

fn wait_for_x11_socket(expected: bool, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let exists = x11_socket_exists();
        if exists == expected {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "X11 socket exists={exists} did not become {expected}"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn inject_touch_logical_when_focused(x: f32, y: f32, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let last_error = match adb::inject_touch_logical(x, y) {
            Ok(_) => return,
            Err(e) => e.to_string(),
        };
        assert!(
            Instant::now() < deadline,
            "touch injection never found a focused host; last error={}",
            last_error
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn shell_quote_single(text: &str) -> String {
    format!("'{}'", text.replace('\'', "'\\''"))
}

fn wait_for_android_clipboard(expected: &str, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        if got == expected {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "Android clipboard did not become {:?}; last={:?}",
            expected,
            got
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

#[test]
fn test_xwayland_setting_starts_and_stops_process_live() {
    tawc_integration::helpers::test_init();

    assert_broker_ok(adb::set_xwayland(false).expect("disable xwayland"), "set-xwayland");
    wait_for_xwayland_running(false, XWAYLAND_LAUNCH_TIMEOUT);

    assert_broker_ok(adb::set_xwayland(true).expect("enable xwayland"), "set-xwayland");
    assert!(adb::get_xwayland().expect("get xwayland"));
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);
    std::thread::sleep(Duration::from_millis(500));
    assert!(
        xwayland_pids().is_empty(),
        "enabling Xwayland should expose the X11 socket without starting the process"
    );

    let mut first = RootfsProcess::spawn_with(SHM_BACKEND, "DISPLAY=:0 xclock -update 1")
        .expect("spawn first lazy-start xclock");
    first.ensure_pgid();
    let initial_pids = wait_for_xwayland_running(true, XWAYLAND_LAUNCH_TIMEOUT);
    // Wait for xclock's first rendered SHM buffer before the kill: this
    // test verifies the full start-render-stop cycle. The early-kill
    // (pre-render) window has its own regression test below.
    wait_for_first_xclock_render(XWAYLAND_LAUNCH_TIMEOUT);
    first.stop().expect("first xclock failed to stop cleanly");
    wait_for_xwayland_running(false, XWAYLAND_IDLE_STOP_TIMEOUT);
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);

    assert_broker_ok(adb::set_xwayland(false).expect("disable xwayland"), "set-xwayland");
    assert!(!adb::get_xwayland().expect("get xwayland disabled"));
    wait_for_xwayland_running(false, XWAYLAND_LAUNCH_TIMEOUT);
    wait_for_x11_socket(false, XWAYLAND_LAUNCH_TIMEOUT);

    assert_broker_ok(adb::set_xwayland(true).expect("re-enable xwayland"), "set-xwayland");
    assert!(adb::get_xwayland().expect("get xwayland re-enabled"));
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);
    std::thread::sleep(Duration::from_millis(500));
    assert!(
        xwayland_pids().is_empty(),
        "re-enabling Xwayland should wait for the first X11 client"
    );

    let mut app = RootfsProcess::spawn_with(SHM_BACKEND, "DISPLAY=:0 xclock -update 1")
        .expect("spawn xclock after re-enabling Xwayland");
    app.ensure_pgid();
    wait_for_xwayland_restarted(&initial_pids, XWAYLAND_LAUNCH_TIMEOUT);
    let deadline = Instant::now() + XWAYLAND_LAUNCH_TIMEOUT;
    while Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("xclock exited before rendering after Xwayland re-enable");
        }
        if has_shm_surface() {
            assert_broker_ok(
                adb::set_xwayland(false).expect("disable xwayland with active X11 client"),
                "set-xwayland",
            );
            wait_for_xwayland_running(false, XWAYLAND_LAUNCH_TIMEOUT);
            app.stop().ok();
            assert_broker_ok(
                adb::set_xwayland(true).expect("restore xwayland after active-client stop"),
                "set-xwayland",
            );
            wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);
            assert!(
                xwayland_pids().is_empty(),
                "restoring Xwayland after a forced stop should only recreate the socket"
            );
            assert_compositor_clean();
            return;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    app.stop().ok();
    panic!("xclock did not render after Xwayland re-enable");
}

/// An X client SIGKILLed while Xwayland is still starting — before the
/// compositor's WM connection has marked itself disconnectable — must not
/// leave Xwayland lingering past its `-terminate` delay. The xserver only
/// re-evaluated the terminate condition on client close, so the activating
/// client's early EOF was processed while the WM still looked like a real
/// client and the idle timer never armed; fixed by
/// `deps/xwayland-patches/xwayland/03-terminate-rearm-on-disconnect-mode.patch`.
#[test]
fn test_xwayland_idle_stops_after_early_client_kill() {
    tawc_integration::helpers::test_init();

    assert_broker_ok(adb::set_xwayland(true).expect("enable xwayland"), "set-xwayland");
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);

    let mut app = RootfsProcess::spawn_with(SHM_BACKEND, "DISPLAY=:0 xclock -update 1")
        .expect("spawn xclock for early kill");
    wait_for_xwayland_running(true, XWAYLAND_LAUNCH_TIMEOUT);
    // Kill as soon as the Xwayland process exists: this reliably lands
    // before xclock's X11 handshake completes, the window that used to
    // wedge the idle-stop.
    app.stop().expect("early-killed xclock failed to stop cleanly");
    wait_for_xwayland_running(false, XWAYLAND_IDLE_STOP_TIMEOUT);
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);
    assert_compositor_clean();
}

#[test]
fn test_android_clipboard_text_to_x11() {
    tawc_integration::helpers::test_init();
    let android_text = "android clipboard to x11";
    adb::clipboard_set_text(android_text).expect("set Android clipboard");
    wait_for_android_clipboard(android_text, XWAYLAND_LAUNCH_TIMEOUT);

    let bin = rootfs::ensure_x11_debug_app().expect("build x11-debug-app");
    let app = DebugApp::start(SHM_BACKEND, &bin, "paste", "DISPLAY=:0")
        .expect("start x11-debug-app paste");
    app.wait_ready()
        .expect("x11-debug-app paste did not map its X11 window");
    inject_touch_logical_when_focused(40.0, 40.0, Duration::from_secs(2));
    app.wait_for_tag_value("CLIPBOARD_PASTE", android_text, XWAYLAND_LAUNCH_TIMEOUT)
        .expect("X11 client did not receive Android clipboard text");

    assert_compositor_clean();
}

/// A backgrounded X11 client must not be able to pull the Android
/// clipboard. `XwmHandler::allow_selection_access` refuses unless an X11
/// window holds focus, so a Wayland app taking focus cuts X11 off;
/// refusal is a `SelectionNotify` with no property, which the client
/// reports as `denied`.
///
/// Deliberately not covered: one X11 client reading while *another* X11
/// window is focused. Telling them apart needs a smithay change to
/// forward the requesting window, and one Xwayland is shared by every
/// distro — see the handler's comment.
#[test]
fn test_x11_clipboard_denied_behind_wayland_window() {
    tawc_integration::helpers::test_init();
    let android_text = "android clipboard behind wayland";
    adb::clipboard_set_text(android_text).expect("set Android clipboard");
    wait_for_android_clipboard(android_text, XWAYLAND_LAUNCH_TIMEOUT);

    let bin = rootfs::ensure_x11_debug_app().expect("build x11-debug-app");
    let mut x11 = DebugApp::start(SHM_BACKEND, &bin, "paste-loop", "DISPLAY=:0")
        .expect("start x11-debug-app paste-loop");
    x11.wait_ready()
        .expect("x11-debug-app did not map its X11 window");
    let expected = format!("ok={android_text}");
    wait_for_x11_clipboard_try(&x11, &expected, "focused");

    // A Wayland app takes focus; the X11 client is now in the background.
    let mut wayland = tawc_integration::helpers::start_wayland_debug_touch(SHM_BACKEND, "");
    wait_for_x11_clipboard_try(&x11, "denied", "backgrounded");
    let cutoff = x11.count_with_tag("CLIPBOARD_TRY") + 1;
    x11.wait_for_tag_count("CLIPBOARD_TRY", cutoff + 3, XWAYLAND_LAUNCH_TIMEOUT)
        .expect("X11 client stopped attempting pastes");
    let attempts = x11.payloads_with_tag("CLIPBOARD_TRY");
    let new = &attempts[cutoff..];
    assert!(
        new.iter().all(|attempt| attempt == "denied"),
        "backgrounded X11 client read the Android clipboard; attempts={new:?}"
    );

    wayland
        .stop()
        .expect("wayland touch app crashed or failed to stop cleanly");
    x11.stop()
        .expect("x11-debug-app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// Wait for a `paste-loop` attempt made after this call — the app's whole
/// history is retained, so waiting on the tag alone would match an
/// attempt from an earlier focus state.
fn wait_for_x11_clipboard_try(app: &DebugApp, expected: &str, label: &str) {
    let already = app.count_with_tag("CLIPBOARD_TRY");
    let deadline = Instant::now() + XWAYLAND_LAUNCH_TIMEOUT;
    loop {
        let attempts = app.payloads_with_tag("CLIPBOARD_TRY");
        let new = &attempts[already.min(attempts.len())..];
        if new.iter().any(|attempt| attempt == expected) {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "{label} X11 client never reported CLIPBOARD_TRY:{expected}; attempts={new:?}"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

#[test]
fn test_x11_clipboard_text_to_android() {
    tawc_integration::helpers::test_init();
    let x11_text = "x11 clipboard to android";
    let sentinel = "android clipboard before x11";
    adb::clipboard_set_text(sentinel).expect("set Android clipboard sentinel");
    wait_for_android_clipboard(sentinel, XWAYLAND_LAUNCH_TIMEOUT);

    let bin = rootfs::ensure_x11_debug_app().expect("build x11-debug-app");
    let subcommand = format!("copy {}", shell_quote_single(x11_text));
    let mut app = DebugApp::start(SHM_BACKEND, &bin, &subcommand, "DISPLAY=:0")
        .expect("start x11-debug-app copy");
    app.wait_ready()
        .expect("x11-debug-app copy did not map its X11 window");
    app.wait_for("CLIPBOARD_SET", XWAYLAND_LAUNCH_TIMEOUT)
        .expect("X11 client did not set clipboard");
    app.wait_for("CLIPBOARD_SEND", XWAYLAND_LAUNCH_TIMEOUT)
        .expect("XWayland did not request X11 clipboard contents");
    wait_for_android_clipboard(x11_text, XWAYLAND_LAUNCH_TIMEOUT);
    app.stop()
        .expect("x11-debug-app copy crashed or failed to stop cleanly");

    assert_compositor_clean();
}

/// XWayland: launch a pure-X11 client (`xclock`) against the bionic-built
/// Xwayland the compositor spawns at startup, and verify it actually
/// reaches our SHM path. Covers everything from `XWayland::spawn` ➜
/// `X11Wm` ➜ X11 surface ↔ wl_surface association ➜ SHM buffer commit ➜
/// renderer pickup.
///
/// X11 clients pixman-render to RGBA SHM buffers; AHB is unavailable to
/// them since GLAMOR is disabled in our Xwayland build (see
/// `notes/xwayland.md`).
#[test]
fn test_xwayland_xclock_renders_via_shm() {
    tawc_integration::helpers::test_init();

    // `-update 1` forces a redraw every second so the client keeps
    // pushing buffers — without it xclock draws once and goes silent,
    // and the test would race the very first SHM import. DISPLAY=:0 is
    // already exported by RootfsEnv on rootfs entry, but be explicit
    // so the test doesn't depend on env order.
    let mut app = RootfsProcess::spawn_with(SHM_BACKEND, "DISPLAY=:0 xclock -update 1")
        .expect("spawn xclock");
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + XWAYLAND_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("xclock crashed/exited before rendering — Xwayland startup failed?");
        }
        if has_shm_surface() {
            saw_buffer = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    assert!(
        saw_buffer,
        "xclock did not produce a SHM surface within {:?} — \
         Xwayland connection or X11 surface association broken?",
        XWAYLAND_LAUNCH_TIMEOUT
    );

    let state = compositor::query_state(TIMEOUT).expect("query xclock compositor state");
    assert!(
        state.x11_surfaces_with_host >= 1,
        "xclock surface never associated to a render host — \
         X11Wm + xwayland_shell wiring broken? state={state:?}"
    );

    app.stop().expect("xclock failed to stop cleanly");
    assert_compositor_clean();
}

/// TAWC-DRI Phase 2 step 3: a chroot-side client allocates a real
/// AHardwareBuffer via libhybris+libnativewindow, CPU-fills it with
/// a green→yellow gradient, and sends it through TAWCDRIPresentBuffer
/// (with FD passing). The X server rebuilds the AHB via
/// AHardwareBuffer_createFromHandle, ships it through android_wlegl,
/// and attaches the resulting wl_buffer to the X11 window's wl_surface.
/// The compositor's wlegl import path then turns it into a GL texture.
///
/// End-to-end proof of the AHB-shipping pipe between an X11 client and
/// the compositor: client AHB → TAWC-DRI → server AHB → android_wlegl
/// → compositor → GL texture. No SHM fallback (which would tint the
/// buffer magenta), no GLAMOR.
///
/// See notes/xwayland.md for the broader Phase 2 plan.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "xwayland skipped on x86 device"
)]
fn test_tawc_dri_ahb_present_round_trip() {
    tawc_integration::helpers::test_init();

    let bin = rootfs::ensure_tawc_dri_test().expect("build tawc-dri-test");
    // HOLD_SECS=1 is enough — the test client commits the AHB once, the
    // X server attaches it to the wl_surface, and the compositor state
    // counters record the wlegl create_buffer / texture import. We don't
    // need to keep the window mapped for screencap inspection in an
    // integration test that only verifies compositor state.
    let before = compositor::query_state(TIMEOUT).expect("query compositor state before TAWC-DRI");
    let cmd = format!("DISPLAY=:0 TAWC_DRI_HOLD_SECS=1 {}", bin);
    let output = adb::rootfs_run_with(BACKEND, &cmd).expect("run tawc-dri-test");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "tawc-dri-test exited non-zero ({:?}). \
         AHB allocation, native_handle shipping, or X server dispatch is broken.\n\
         stdout: {stdout}\nstderr: {stderr}",
        output.status.code()
    );
    assert!(
        stderr.contains("tawc-dri-test: OK"),
        "tawc-dri-test exited 0 but didn't reach the OK line — \
         did the binary, DISPLAY, or hold loop change?\nstderr: {stderr}"
    );
    // Confirm the AHB pixel format and dimensions reached the
    // compositor in AHB form (not the SHM fallback). Test client
    // allocates 320x240 R8G8B8A8 (= AHB format 1).
    let after = compositor::query_state(TIMEOUT).expect("query compositor state after TAWC-DRI");
    assert!(
        after.wlegl_create_buffer_total > before.wlegl_create_buffer_total
            && after.last_wlegl_width == 320
            && after.last_wlegl_height == 240
            && after.last_wlegl_format == 1,
        "compositor never reported wlegl_create_buffer_total for 320x240 fmt=1, \
         meaning the TAWC-DRI AHB never reached the compositor's android_wlegl \
         import path. The client may have fallen back to SHM (which would \
         tint the buffer magenta), or the X server's TAWC-DRI dispatch may \
         be silently dropping the request. before={before:?} after={after:?}",
    );
    // Also verify the AHB completed the trip into a GL texture on the
    // compositor side. A pure SHM fallback path won't bump this counter.
    // Catches a regression where the AHB import succeeds but the GL bind
    // fails (e.g. format/usage mismatch).
    assert!(
        after.wlegl_import_texture_total > before.wlegl_import_texture_total,
        "compositor imported the AHB metadata but never bound it as a GL \
         texture. The AHB→GL path (compositor/src/render.rs) likely got an \
         EGL error. before={before:?} after={after:?}",
    );
    assert_compositor_clean();
}

/// TAWC-DRI Phase 2 step 3 stress: the same chroot-side client runs an
/// animated double-buffered loop at 60 fps for 2 seconds (120 frames),
/// alternating between two AHBs and repainting each frame. This catches
/// regressions the one-shot test can't:
///   - Per-frame allocation leaks in the X server's
///     xwl_tawc_present_native_handle (each present creates a fresh AHB
///     + wl_buffer; if those leak in a way that scales the X server
///     stalls or hits gralloc OOM)
///   - Rendering staleness: if the compositor caches the first frame's
///     texture forever instead of re-importing, the on-screen image
///     freezes — but with the loop, "frozen" still means animation
///     sweep is missing. We assert the wlegl create-buffer counter
///     advances by >=120 (one per frame, no dropped imports) AND that
///     the test client's reported
///     fps stays at the configured 60fps target (so the X server isn't
///     blocking it).
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "xwayland skipped on x86 device"
)]
fn test_tawc_dri_ahb_present_animated_loop() {
    tawc_integration::helpers::test_init();

    let bin = rootfs::ensure_tawc_dri_test().expect("build tawc-dri-test");
    // 120 frames at 60fps = 2 seconds. Long enough to surface a leak
    // (~120 per-frame AHB+wl_buffer pairs accumulated server-side)
    // without bloating the test runtime.
    const FRAMES: u32 = 120;
    let cmd = format!(
        "DISPLAY=:0 TAWC_DRI_LOOP_FRAMES={} {}",
        FRAMES, bin
    );
    let before = compositor::query_state(TIMEOUT).expect("query compositor state before TAWC-DRI loop");

    // Sample the wlegl destroy counter WHILE the client runs: buffer
    // releases must be served frame by frame. Sampling only after the
    // run could false-pass — when the client exits its window is torn
    // down, and a fully-stalled release path would drop every leaked
    // wl_buffer at teardown, advancing the counter just the same. A
    // sample counts only while the create counter shows the client
    // still mid-run (strictly before the last present, hence before
    // any teardown drops).
    let sampler_done = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let destroys_mid_run = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
    let sampler = {
        let done = sampler_done.clone();
        let seen = destroys_mid_run.clone();
        let create_base = before.wlegl_create_buffer_total;
        let destroy_base = before.wlegl_buffer_destroy_total;
        std::thread::spawn(move || {
            while !done.load(std::sync::atomic::Ordering::Relaxed) {
                if let Ok(state) = compositor::query_state_once() {
                    let presents =
                        state.wlegl_create_buffer_total.saturating_sub(create_base);
                    if presents < FRAMES as u64 {
                        let delta = state
                            .wlegl_buffer_destroy_total
                            .saturating_sub(destroy_base);
                        seen.fetch_max(delta, std::sync::atomic::Ordering::Relaxed);
                    }
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        })
    };

    let output = adb::rootfs_run_with(BACKEND, &cmd).expect("run tawc-dri-test loop");
    sampler_done.store(true, std::sync::atomic::Ordering::Relaxed);
    sampler.join().expect("destroy-counter sampler thread panicked");
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success(),
        "tawc-dri-test loop mode exited non-zero ({:?}). \
         Likely a per-frame buffer-handling regression server-side.\n\
         stderr: {stderr}\nstdout: {stdout}",
        output.status.code()
    );
    // The client logs a summary line on success — parse the fps and
    // assert it didn't fall noticeably below the 60fps target. Below
    // 50fps would mean the X server (or compositor) is back-pressuring
    // the client.
    let fps_line = stderr
        .lines()
        .find(|l| l.contains("loop ran") && l.contains("fps"))
        .unwrap_or_else(|| panic!("test client never reported its loop fps:\n{stderr}"));
    let fps: f32 = fps_line
        .split_whitespace()
        .filter_map(|w| w.strip_suffix("fps").and_then(|n| n.parse::<f32>().ok())
                          .or_else(|| {
                              let tok = w.trim_end_matches(')');
                              tok.parse::<f32>().ok().filter(|&v| v > 1.0 && v < 1000.0)
                          }))
        .next()
        .unwrap_or_else(|| {
            panic!("could not parse fps out of '{fps_line}'\nstderr:\n{stderr}")
        });
    assert!(
        fps >= 50.0,
        "TAWC-DRI loop client only sustained {fps:.1} fps (expected >=50 \
         at the 60fps target). Server-side per-frame work is backpressuring \
         the client — likely an O(N) leak or a slow path.\nstderr:\n{stderr}"
    );

    let after = compositor::query_state(TIMEOUT).expect("query compositor state after TAWC-DRI loop");
    let create_count = after
        .wlegl_create_buffer_total
        .saturating_sub(before.wlegl_create_buffer_total);
    // The compositor should import every frame's AHB.
    assert!(
        create_count >= FRAMES as u64,
        "compositor imported only {create_count} AHBs for {FRAMES} client \
         frames. Some imports were dropped or never reached the compositor — \
         most likely the X server is silently failing AHardwareBuffer_createFromHandle \
         under sustained load. before={before:?} after={after:?}",
    );
    // Buffer releases must be served back through the pipe: the compositor
    // releases each replaced wl_buffer, Xwayland's release listener destroys
    // the per-present trio (and forwards TAWCDRIBufferRelease to v0.3
    // clients), and the destroy shows up here. A stalled release path means
    // the X server leaks one AHB per frame and v0.3 clients starve in
    // dequeueBuffer backpressure. Asserted on the mid-run samples (see the
    // sampler above) so exit-time teardown drops can't fake a pass.
    let destroy_count = destroys_mid_run.load(std::sync::atomic::Ordering::Relaxed);
    assert!(
        destroy_count >= (FRAMES / 2) as u64,
        "only {destroy_count} wlegl buffer destroys observed mid-run for \
         {FRAMES} presented frames — the compositor is not releasing (or \
         Xwayland is not destroying) TAWC-DRI wl_buffers frame-by-frame. \
         before={before:?} after={after:?}",
    );
    assert_compositor_clean();
}

/// TAWC-DRI Phase 2 step 4: full EGL-on-X11 stack via libhybris's
/// `eglplatform_x11.so`. The chroot client opens a normal Xlib display,
/// asks libhybris for `EGL_PLATFORM_X11_KHR`, creates a context, and
/// runs `glClear` + `eglSwapBuffers` for a few frames. Each swap should
/// allocate an AHB via the AHB gralloc backend, ship it through
/// TAWC-DRI to the X server, and end up imported by the compositor.
///
/// Asserts:
///   - The client exits 0 and reaches its `OK` line (no EGL errors).
///   - The compositor's wlegl create-buffer counter advances with
///     `last_wlegl_format=1` (AHB import) AND the texture-import counter
///     advances (GL bind).
///
/// If this fails with no AHB counter movement, the libhybris X11 plugin is
/// broken — the client never made it onto the TAWC-DRI wire (probably
/// `eglGetPlatformDisplay` fell back to NULL or surface creation
/// errored). If the AHB lands but no GL texture import, the compositor
/// rejected the AHB format/usage emitted by the plugin's gralloc
/// allocate (it should match what `tawc-dri-test` already emits cleanly).
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "xwayland skipped on x86 device"
)]
fn test_eglx11_renders_via_ahb() {
    tawc_integration::helpers::test_init();

    let bin = rootfs::ensure_eglx11_test().expect("build eglx11-test");
    // The swap loop can finish before the compositor's next frame tick.
    // Keep the window mapped briefly so Smithay's lazy render-element import
    // has time to bind at least one AHB as a texture.
    let cmd = format!(
        "DISPLAY=:0 HYBRIS_EGLPLATFORM=x11 TAWC_EGLX11_FRAMES=30 \
         TAWC_EGLX11_HOLD_SECS=1 {}",
        bin
    );
    let before = compositor::query_state(TIMEOUT).expect("query compositor state before eglx11");
    let output = adb::rootfs_run_with(BACKEND, &cmd).expect("run eglx11-test");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "eglx11-test exited non-zero ({:?}). The libhybris X11 EGL platform \
         plugin failed to initialize, create a surface, or swap a buffer.\n\
         stdout: {stdout}\nstderr: {stderr}",
        output.status.code()
    );
    assert!(
        stderr.contains("eglx11-test: OK"),
        "eglx11-test exited 0 but didn't reach the OK line.\nstderr: {stderr}"
    );

    let after = compositor::query_state(TIMEOUT).expect("query compositor state after eglx11");
    assert!(
        after.wlegl_create_buffer_total > before.wlegl_create_buffer_total
            && after.last_wlegl_format == 1,
        "compositor never reported wlegl_create_buffer_total for fmt=1. \
         The libhybris EGL plugin's swap chain didn't ship an AHB through \
         TAWC-DRI to the compositor. Either eglGetPlatformDisplay didn't \
         dispatch to our x11 plugin, or queueBuffer's TAWCDRIPresentBuffer \
         was silently dropped server-side.\n\
         eglx11-test stderr:\n{stderr}\nbefore={before:?} after={after:?}",
    );
    assert!(
        after.wlegl_import_texture_total > before.wlegl_import_texture_total,
        "compositor imported the AHB but never bound it as a GL texture. \
         Format/usage mismatch between the libhybris plugin's gralloc \
         allocate and the compositor's wlegl import path. before={before:?} after={after:?}",
    );
    assert_compositor_clean();
}

/// Phase 2 step 5: real-app shakedown. Runs Arch's stock `es2gears_x11`
/// (from `mesa-demos`) for a few seconds against our libhybris X11 EGL
/// plugin and asserts the full pipe stays healthy under sustained-frame
/// load:
///   - Hundreds of wlegl create-buffer counter increments (one per swap
///     → AHB import) — proves the plugin isn't falling back to SHM under
///     any vendor-specific config the GLES driver picks.
///   - Compositor and X server still alive after the run.
///
/// This is the regression net for the X server's wl_buffer.release
/// listener and the libhybris plugin's queueBuffer wire shape — both
/// load-bearing for any non-trivial GL workload.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "xwayland skipped on x86 device"
)]
fn test_es2gears_x11_renders_via_ahb() {
    tawc_integration::helpers::test_init();

    let before = compositor::query_state(TIMEOUT).expect("query compositor state before es2gears_x11");
    // Run es2gears until the assertions are satisfied rather than for a
    // fixed wall-clock window: a cold start (Xwayland spawn + GLES
    // driver init + host binding) can eat several seconds before the
    // first full-size buffer ships, so a fixed-duration run races it.
    let mut app = RootfsProcess::spawn_with(
        BACKEND,
        "DISPLAY=:0 HYBRIS_EGLPLATFORM=x11 es2gears_x11 > /dev/null 2>&1",
    )
    .expect("spawn es2gears_x11");

    // Two conditions prove the pipe:
    //   - >=100 AHB imports: the plugin isn't falling back to SHM and
    //     the X server isn't dropping presents.
    //   - last AHB dims == host logical size: es2gears creates a
    //     300x300 window and the compositor resizes it after mapping;
    //     TAWC-DRI v0.3 ConfigureNotify (SelectInput + XGE special
    //     events) must walk that resize down into libhybris's buffer
    //     pool. Pixel-blind, but catches exactly the regression class
    //     where every X11 GL window composited solid black for ~2
    //     months (buffers stuck at creation size).
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let state = compositor::query_state(TIMEOUT).expect("query compositor state");
        let create_count = state
            .wlegl_create_buffer_total
            .saturating_sub(before.wlegl_create_buffer_total);
        if create_count >= 100
            && state.last_wlegl_width == state.output_logical_w as u32
            && state.last_wlegl_height == state.output_logical_h as u32
        {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "es2gears_x11 pipe never got healthy: {create_count} AHB \
             imports (want >=100; low means SHM fallback or dropped \
             presents), last AHB {}x{} vs host logical {}x{} (mismatch \
             means the TAWC-DRI ConfigureNotify event channel is \
             broken). before={before:?} state={state:?}",
            state.last_wlegl_width,
            state.last_wlegl_height,
            state.output_logical_w,
            state.output_logical_h,
        );
        std::thread::sleep(Duration::from_millis(200));
    }

    // Frame pacing: with the pipe healthy, presents must tick at display
    // cadence, not the client's free-run rate. Xwayland paces TAWC-DRI
    // commits with wl_surface.frame callbacks (~60Hz frame timer);
    // before that es2gears free-ran at ~2500 presents/sec. Allow wide
    // margins (high-refresh panels, timer jitter) — the regression this
    // guards is a ~40x blowout, not a few percent.
    let pace_before = compositor::query_state(TIMEOUT)
        .expect("query compositor state for pacing sample");
    std::thread::sleep(Duration::from_secs(3));
    let pace_after = compositor::query_state(TIMEOUT)
        .expect("query compositor state after pacing sample");
    let rate = pace_after
        .wlegl_create_buffer_total
        .saturating_sub(pace_before.wlegl_create_buffer_total) as f64
        / 3.0;
    assert!(
        rate <= 200.0,
        "es2gears_x11 presented {rate:.0} buffers/sec — frame pacing is \
         broken (Xwayland is committing every present immediately instead \
         of one per wl_surface.frame callback), the client is free-running."
    );
    assert!(
        rate >= 20.0,
        "es2gears_x11 presented only {rate:.0} buffers/sec after the pipe \
         was healthy — presents (or BufferRelease backpressure) stalled."
    );

    app.stop()
        .expect("es2gears_x11 crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// Pixel-level check that an X11 GL client's rendering actually reaches
/// the screen. The counter-based tests above stayed green for ~2 months
/// while every X11 GL window composited solid black (see the TAWC-DRI
/// v0.3 section of notes/xwayland.md) — they prove AHBs flow, not
/// that the content is visible. Here eglx11-test draws a pure-blue
/// triangle covering the window centre; after the compositor resizes
/// the window to the host's logical size, the centre of the screen
/// must be blue. The compositor's lime AHB edge tint lives at the
/// window borders and never reaches the centre, so it can't fake a
/// pass.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "xwayland skipped on x86 device"
)]
fn test_eglx11_triangle_pixels_on_screen() {
    tawc_integration::helpers::test_init();

    let bin = rootfs::ensure_eglx11_test().expect("build eglx11-test");
    // Keep swapping (paced to ~60fps) for the whole test rather than a
    // short burst + hold: the compositor's post-map resize can land
    // after an unpaced burst finishes, and a client that has stopped
    // dequeuing can never pick it up. The frame budget outlives any
    // plausible deadline; the test stops the client explicitly.
    let cmd = format!(
        "DISPLAY=:0 HYBRIS_EGLPLATFORM=x11 TAWC_EGLX11_TRIANGLE=1 \
         TAWC_EGLX11_FRAMES=100000 TAWC_EGLX11_SLEEP_MS=16 {}",
        bin
    );
    let mut app = RootfsProcess::spawn_with(BACKEND, &cmd).expect("spawn eglx11-test");

    // Wait until the buffer pool has followed the compositor's resize —
    // only then is the on-screen window full-size with the triangle
    // centred on the output.
    // rendered_toplevels only counts Wayland toplevels, so gate on the
    // X11 surface being host-bound plus the buffer dims having followed
    // the resize.
    let deadline = Instant::now() + Duration::from_secs(15);
    let state = loop {
        let state = compositor::query_state(TIMEOUT).expect("query compositor state");
        if state.last_wlegl_width == state.output_logical_w as u32
            && state.last_wlegl_height == state.output_logical_h as u32
            && state.x11_surfaces_with_host > 0
        {
            break state;
        }
        assert!(
            Instant::now() < deadline,
            "eglx11-test buffers never reached the host logical size \
             (TAWC-DRI ConfigureNotify chain broken?): {state:?}"
        );
        std::thread::sleep(Duration::from_millis(200));
    };
    // One more beat so the full-size frame is actually composited.
    std::thread::sleep(Duration::from_millis(500));

    let shot = adb::screencap_raw().expect("raw screencap");
    let x = ((state.output_physical_w / 2).max(0)) as u32;
    let y = ((state.output_physical_h / 2).max(0)) as u32;
    let [r, g, b, _a] = shot
        .pixel_rgba(x, y)
        .unwrap_or_else(|| panic!("screencap missing centre pixel at {x},{y}"));
    assert!(
        b > 170 && r < 100 && g < 120,
        "screen centre should show eglx11-test's pure-blue triangle, got \
         R{r} G{g} B{b}. Black means the client rendered outside its \
         buffers (the resize-follow bug); magenta means SHM fallback; \
         lime means only the compositor's AHB edge tint is visible."
    );

    app.stop().expect("eglx11-test crashed or failed to stop cleanly");
    assert_compositor_clean();
}

/// Xwayland start/idle-stop cycles inside one compositor must not leak
/// app-process fds.
#[test]
fn test_xwayland_cycles_do_not_leak() {
    tawc_integration::helpers::test_init();
    assert_broker_ok(adb::set_xwayland(true).expect("enable xwayland"), "set-xwayland");
    wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);

    let cycle = || {
        let mut app = RootfsProcess::spawn_with(SHM_BACKEND, "DISPLAY=:0 xclock -update 1")
            .expect("spawn xclock");
        wait_for_xwayland_running(true, XWAYLAND_LAUNCH_TIMEOUT);
        wait_for_first_xclock_render(XWAYLAND_LAUNCH_TIMEOUT);
        app.stop().expect("xclock failed to stop cleanly");
        wait_for_xwayland_running(false, XWAYLAND_IDLE_STOP_TIMEOUT);
        wait_for_x11_socket(true, XWAYLAND_LAUNCH_TIMEOUT);
    };
    // Warm-up: one-time lazy initialisation is not a leak.
    cycle();
    let before = app_fd_targets();
    for _ in 0..4 {
        cycle();
    }
    let after = app_fd_targets();
    assert!(
        after.len() <= before.len() + 2,
        "fds grew over 4 Xwayland cycles: {} -> {} ({})",
        before.len(),
        after.len(),
        fd_growth(&before, &after)
    );
}
