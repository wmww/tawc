//! Xwayland tests. Anything that exercises the bionic-built Xwayland the
//! compositor spawns at startup — pure-X11 clients, the `-tawc-test-pattern`
//! debug hook, the TAWC-DRI AHB-shipping pipe, and EGL-on-X11 via the
//! libhybris X11 platform plugin. Buffer-type assertions inside these tests
//! stay here (rather than moving to `graphics::`) because what's under test
//! is the Xwayland integration, not the buffer plumbing in isolation.
//!
//! Requires libhybris + a real Android GPU driver, so these fail on the
//! emulator and on the gfxstream backend (skipped explicitly).

use std::time::Duration;

use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::helpers::{assert_compositor_clean, require_compositor};
use tawc_integration::{adb, compositor, rootfs};

const XWAYLAND_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);

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
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    // `-update 1` forces a redraw every second so the client keeps
    // pushing buffers — without it xclock draws once and goes silent,
    // and the test would race the very first SHM import. DISPLAY=:0 is
    // already exported by RootfsEnv on rootfs entry, but be explicit
    // so the test doesn't depend on env order.
    let mut app =
        RootfsProcess::spawn("DISPLAY=:0 xclock -update 1").expect("spawn xclock");
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + XWAYLAND_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("xclock crashed/exited before rendering — Xwayland startup failed?");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if tawc_integration::helpers::saw_shm_import(&logs) {
            saw_buffer = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    assert!(
        saw_buffer,
        "xclock did not produce SHM buffer imports within {:?} — \
         Xwayland connection or X11 surface association broken?",
        XWAYLAND_LAUNCH_TIMEOUT
    );

    // A `compositor::xwayland: associated X11 surface ... to host`
    // line should also be present, confirming our XwmHandler hooked
    // the X11 window into a render host. Surface-without-host would
    // import a buffer but never draw.
    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(
        logs.contains("xwayland: associated X11 surface"),
        "xclock surface never associated to a render host — \
         X11Wm + xwayland_shell wiring broken?\nlogs:\n{logs}"
    );

    app.stop().expect("xclock failed to stop cleanly");
    assert_compositor_clean();
}

/// TAWC-DRI Phase 2 step 2: opt the compositor into Xwayland's
/// `-tawc-test-pattern` path via `debug.tawc.xwl_test_pattern`,
/// restart, and verify a server-allocated AHB ships through
/// `android_wlegl` to the compositor. This proves the buffer-shipping
/// plumbing (server-side libnativewindow allocate → AHB native_handle
/// → android_wlegl create_buffer → compositor gralloc1 import) end-to-
/// end without any X clients existing. Visibility (attaching the AHB
/// to a wl_surface) is deferred to step 3 alongside TAWC-DRI buffer
/// presentation. See notes/xwayland.md.
///
/// The test takes ownership of the compositor lifecycle (force-stop +
/// start) twice: once to enable the test pattern, once to clean up.
/// `assert_compositor_clean` at the end leaves the suite in the same
/// shape every other test expects.
#[test]
fn test_xwayland_test_pattern_ahb_round_trip() {
    if tawc_integration::skip_if_gfxstream(
        "Xwayland's `-tawc-test-pattern` allocates the AHB via libhybris+\
         libnativewindow; the test asserts the compositor sees that \
         specific allocation path. Bridge-side AHB allocation goes via \
         gfxstream's renderer instead — different code path, no analogue",
    ) {
        return;
    }
    require_compositor();

    // Enable opt-in flag.
    adb::shell("setprop debug.tawc.xwl_test_pattern 1")
        .expect("setprop debug.tawc.xwl_test_pattern");

    // Defer-style cleanup that runs even if the test panics, so the
    // prop doesn't leak into subsequent tests in the same suite run.
    struct PropGuard;
    impl Drop for PropGuard {
        fn drop(&mut self) {
            // `setprop` clears via empty quoted string.
            let _ = adb::shell(r#"setprop debug.tawc.xwl_test_pattern """#);
            let _ = adb::shell("am force-stop me.phie.tawc");
            std::thread::sleep(Duration::from_millis(300));
            let _ = adb::shell("am start -n me.phie.tawc/.MainActivity");
            // Wait for compositor to come back so the next test sees it ready.
            let deadline = std::time::Instant::now() + Duration::from_secs(15);
            while std::time::Instant::now() < deadline {
                if let Ok(true) = compositor::is_running() {
                    return;
                }
                std::thread::sleep(Duration::from_millis(150));
            }
        }
    }
    let _guard = PropGuard;

    // Fresh logcat so we can read the bring-up cleanly.
    adb::logcat_clear().expect("logcat clear");

    adb::shell("am force-stop me.phie.tawc").expect("force-stop");
    std::thread::sleep(Duration::from_millis(400));
    adb::shell("am start -n me.phie.tawc/.MainActivity").expect("am start");

    // Wait for the compositor to come back AND for Xwayland to spawn,
    // since the test-pattern path runs during Xwayland's screen init.
    let deadline = std::time::Instant::now() + Duration::from_secs(20);
    let mut compositor_up = false;
    while std::time::Instant::now() < deadline {
        if let Ok(true) = compositor::is_running() {
            compositor_up = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(150));
    }
    assert!(
        compositor_up,
        "compositor did not come back up after restart; \
         debug.tawc.xwl_test_pattern flow likely broken Xwayland startup"
    );

    // Compositor logs `wlegl: create_buffer ...` on a successful AHB
    // import via android_wlegl. The test-pattern allocates one
    // 512x512 R8G8B8A8 (format=1) AHB at startup; that should be the
    // first such log line.
    let log_deadline = std::time::Instant::now() + Duration::from_secs(15);
    let mut saw_log = false;
    let mut last_logs = String::new();
    while std::time::Instant::now() < log_deadline {
        let logs = adb::logcat_dump_tawc().expect("logcat dump");
        if logs.contains("wlegl: create_buffer 512x512 stride=") {
            saw_log = true;
            last_logs = logs;
            break;
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    assert!(
        saw_log,
        "compositor never logged `wlegl: create_buffer 512x512` after \
         enabling debug.tawc.xwl_test_pattern + restarting. Likely the \
         android_wlegl bind is missing on Xwayland's side, or libnativewindow \
         dlopen failed (check /data/data/me.phie.tawc/share/xtmp/xwayland.log).\n\
         last logs:\n{}",
         &last_logs[last_logs.len().saturating_sub(4096)..],
    );

    // Sanity-check: format=1 (R8G8B8A8_UNORM) and a non-magenta route.
    // The compositor doesn't log "magenta" but the SHM-import path uses
    // a different code path entirely, so a hit on `wlegl: create_buffer`
    // implies the AHB path fired (not the SHM fallback).
    assert!(
        last_logs.contains("fmt=1 usage=0x"),
        "wlegl: create_buffer matched but format/usage payload differs from \
         what the test pattern allocates (R8G8B8A8 = fmt 1). Filling code \
         in xwayland-tawc.c may have drifted.\nlogs:\n{}",
         &last_logs[last_logs.len().saturating_sub(4096)..],
    );
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
fn test_tawc_dri_ahb_present_round_trip() {
    if tawc_integration::skip_if_gfxstream(
        "tawc-dri-test allocates an AHB through libhybris+libnativewindow \
         and ships it through TAWC-DRI to the libhybris-built Xwayland. \
         Tests the libhybris AHB-shipping path end-to-end; no libhybris \
         analogue under the bridge",
    ) {
        return;
    }
    require_compositor();
    adb::logcat_clear().expect("logcat clear");

    let bin = rootfs::ensure_tawc_dri_test().expect("build tawc-dri-test");
    // HOLD_SECS=1 is enough — the test client commits the AHB once, the
    // X server attaches it to the wl_surface, and the compositor logs
    // the wlegl create_buffer / texture import. We don't need to keep
    // the window mapped for screencap inspection in an integration test
    // that only verifies the compositor logs.
    let cmd = format!("DISPLAY=:0 TAWC_DRI_HOLD_SECS=1 {}", bin);
    let output = adb::rootfs_run(&cmd).expect("run tawc-dri-test");
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
    let logs = adb::logcat_dump_tawc().expect("logcat dump");
    assert!(
        logs.contains("wlegl: create_buffer 320x240")
            && logs.contains("fmt=1"),
        "compositor never logged `wlegl: create_buffer 320x240 ... fmt=1`, \
         meaning the TAWC-DRI AHB never reached the compositor's android_wlegl \
         import path. The client may have fallen back to SHM (which would \
         tint the buffer magenta), or the X server's TAWC-DRI dispatch may \
         be silently dropping the request.\nlast 4k of compositor logs:\n{}",
         &logs[logs.len().saturating_sub(4096)..],
    );
    // Also verify the AHB completed the trip into a GL texture on the
    // compositor side — this is the line `import_shm_buffers`-style
    // path won't produce. Catches a regression where the AHB import
    // succeeds but the GL bind fails (e.g. format/usage mismatch).
    assert!(
        logs.contains("wlegl: imported ANativeWindowBuffer as texture 320x240"),
        "compositor imported the AHB metadata but never bound it as a GL \
         texture. The AHB→GL path (compositor/src/render.rs) likely got an \
         EGL error.\nlogs:\n{}",
         &logs[logs.len().saturating_sub(4096)..],
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
///     sweep is missing, which is invisible from logs alone. We instead
///     assert that >=120 wlegl: create_buffer lines appear (one per
///     frame, no dropped imports) AND that the test client's reported
///     fps stays at the configured 60fps target (so the X server isn't
///     blocking it).
#[test]
fn test_tawc_dri_ahb_present_animated_loop() {
    if tawc_integration::skip_if_gfxstream(
        "Animated variant of test_tawc_dri_ahb_present_round_trip — same \
         libhybris+libnativewindow path",
    ) {
        return;
    }
    require_compositor();
    adb::logcat_clear().expect("logcat clear");

    let bin = rootfs::ensure_tawc_dri_test().expect("build tawc-dri-test");
    // 120 frames at 60fps = 2 seconds. Long enough to surface a leak
    // (~120 per-frame AHB+wl_buffer pairs accumulated server-side)
    // without bloating the test runtime.
    const FRAMES: u32 = 120;
    let cmd = format!(
        "DISPLAY=:0 TAWC_DRI_LOOP_FRAMES={} {}",
        FRAMES, bin
    );
    let output = adb::rootfs_run(&cmd).expect("run tawc-dri-test loop");
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

    let logs = adb::logcat_dump_tawc().expect("logcat dump");
    let create_count = logs.matches("wlegl: create_buffer 320x240").count();
    // The compositor should import every frame's AHB. Allow a small
    // slack for log-cat truncation / startup noise (we cleared logs
    // before the run, but a few stray lines from prior messages may
    // sneak in via the buffered Android log).
    assert!(
        create_count >= FRAMES as usize,
        "compositor imported only {create_count} AHBs for {FRAMES} client \
         frames. Some imports were dropped or never reached the compositor — \
         most likely the X server is silently failing AHardwareBuffer_createFromHandle \
         under sustained load. \nlast 4k of logs:\n{}",
         &logs[logs.len().saturating_sub(4096)..],
    );
    // Compositor side mustn't have logged any tawc-error lines during
    // the run — those signal AHB import failures or X11 dispatch errors
    // that didn't kill the X server but did drop the buffer.
    assert!(
        !logs.contains("xwl_tawc:") || !logs.contains("failed"),
        "compositor logged xwl_tawc errors during the loop:\n{}",
         &logs[logs.len().saturating_sub(4096)..],
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
///   - The compositor logs at least one `wlegl: create_buffer 320x240
///     ... fmt=1` (AHB import) AND at least one
///     `wlegl: imported ANativeWindowBuffer as texture 320x240` (GL bind).
///
/// If this fails *with no AHB log lines*, the libhybris X11 plugin is
/// broken — the client never made it onto the TAWC-DRI wire (probably
/// `eglGetPlatformDisplay` fell back to NULL or surface creation
/// errored). If the AHB lands but no GL texture import, the compositor
/// rejected the AHB format/usage emitted by the plugin's gralloc
/// allocate (it should match what `tawc-dri-test` already emits cleanly).
#[test]
fn test_eglx11_renders_via_ahb() {
    if tawc_integration::skip_if_gfxstream(
        "Tests libhybris's X11 EGL platform plugin (eglplatform_x11.so) \
         end-to-end. The eglx11-test binary has RUNPATH=/usr/lib/hybris \
         baked in so it loads libhybris's libEGL even with empty \
         LD_LIBRARY_PATH — the test still passes under gfxstream, but \
         what's being tested is the libhybris path, not the bridge",
    ) {
        return;
    }
    require_compositor();
    adb::logcat_clear().expect("logcat clear");

    let bin = rootfs::ensure_eglx11_test().expect("build eglx11-test");
    // 30 frames is plenty to surface init failures and to give the
    // compositor's per-X11 SurfaceView time to attach (same race the
    // `tawc-dri-test` HOLD_SECS dance handles).
    let cmd = format!(
        "DISPLAY=:0 HYBRIS_EGLPLATFORM=x11 TAWC_EGLX11_FRAMES=30 {}",
        bin
    );
    let output = adb::rootfs_run(&cmd).expect("run eglx11-test");
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

    let logs = adb::logcat_dump_tawc().expect("logcat dump");
    assert!(
        logs.contains("wlegl: create_buffer 320x240") && logs.contains("fmt=1"),
        "compositor never logged `wlegl: create_buffer 320x240 ... fmt=1`. \
         The libhybris EGL plugin's swap chain didn't ship an AHB through \
         TAWC-DRI to the compositor. Either eglGetPlatformDisplay didn't \
         dispatch to our x11 plugin, or queueBuffer's TAWCDRIPresentBuffer \
         was silently dropped server-side.\n\
         eglx11-test stderr:\n{stderr}\n\
         last 4k of compositor logs:\n{}",
        &logs[logs.len().saturating_sub(4096)..],
    );
    assert!(
        logs.contains("wlegl: imported ANativeWindowBuffer as texture 320x240"),
        "compositor imported the AHB but never bound it as a GL texture. \
         Format/usage mismatch between the libhybris plugin's gralloc \
         allocate and the compositor's wlegl import path.\nlogs:\n{}",
        &logs[logs.len().saturating_sub(4096)..],
    );
    assert_compositor_clean();
}

/// Phase 2 step 5: real-app shakedown. Runs Arch's stock `es2gears_x11`
/// (from `mesa-demos`) for a few seconds against our libhybris X11 EGL
/// plugin and asserts the full pipe stays healthy under sustained-frame
/// load:
///   - Hundreds of `wlegl: create_buffer` lines (one per swap → AHB
///     import) — proves the plugin isn't falling back to SHM under any
///     vendor-specific config the GLES driver picks.
///   - Zero `AHardwareBuffer_createFromHandle failed` lines on the
///     compositor side — guards against the FD-leak / handle-shape
///     regressions that bit Phase 2 step 4 (compositor RLIMIT_NOFILE
///     trip from per-present buffer leaks).
///   - Compositor and X server still alive after the run (no
///     `Xwayland disconnected: Protocol error` from a botched
///     create_buffer).
///
/// This is the regression net for the X server's wl_buffer.release
/// listener and the libhybris plugin's queueBuffer wire shape — both
/// load-bearing for any non-trivial GL workload.
#[test]
fn test_es2gears_x11_renders_via_ahb() {
    if tawc_integration::skip_if_gfxstream(
        "es2gears_x11 is a stock distro binary linked against /usr/lib/\
         libEGL.so.1 (libglvnd → distro Mesa). It exercises the libhybris \
         X11 EGL plugin only when libhybris's libEGL shadows that path on \
         LD_LIBRARY_PATH. Under gfxstream the chroot's LD_LIBRARY_PATH is \
         empty (Mesa loads stock), so HYBRIS_EGLPLATFORM=x11 has no effect \
         and the plugin path under test never runs. No analogue under bridge",
    ) {
        return;
    }
    require_compositor();
    adb::logcat_clear().expect("logcat clear");

    // 4s gives es2gears time to hit hundreds of swap cycles even on a
    // slow device. The release-listener fix bounds the live AHB
    // working set to the compositor's queue depth (~3) regardless of
    // total frame count, so longer runs would only dilute signal.
    let cmd = "DISPLAY=:0 HYBRIS_EGLPLATFORM=x11 timeout 4 es2gears_x11 \
               > /dev/null 2>&1; true";
    let output = adb::rootfs_run(cmd).expect("run es2gears_x11");
    assert!(
        output.status.success(),
        "es2gears_x11 wrapper exited non-zero ({:?}). The wrapper ends \
         with `; true` so timeout's 124 doesn't fail the assertion — \
         non-zero here means the chroot couldn't even spawn the binary \
         (missing mesa-demos? run `bash scripts/install-test-deps.sh`).",
        output.status.code()
    );

    let logs = adb::logcat_dump_tawc().expect("logcat dump");
    let create_count = logs.matches("wlegl: create_buffer").count();
    assert!(
        create_count >= 100,
        "compositor only imported {create_count} AHBs from es2gears_x11. \
         Expected >=100 — either the libhybris EGL plugin fell back to a \
         non-AHB path for the GLES driver's chosen config, or the X server \
         is dropping presents.\nlast 4k of logs:\n{}",
        &logs[logs.len().saturating_sub(4096)..],
    );
    assert!(
        !logs.contains("AHardwareBuffer_createFromHandle failed"),
        "compositor logged AHardwareBuffer_createFromHandle failure during \
         the run — most likely an FD leak (the X server's wl_buffer.release \
         listener got disconnected) or a handle-shape regression in the \
         libhybris plugin.\nlast 4k of logs:\n{}",
        &logs[logs.len().saturating_sub(4096)..],
    );
    assert!(
        !logs.contains("Xwayland disconnected: Protocol error"),
        "Xwayland died during the run with a protocol error. \
         Last 4k of logs:\n{}",
        &logs[logs.len().saturating_sub(4096)..],
    );
    assert_compositor_clean();
}
