//! Graphics-stack tests. **Every test in this module asserts which buffer
//! path a client uses** — `wl_shm`, libhybris-backed AHB through
//! `android_wlegl`, or Vulkan via `vulkanplatform_wayland.so`. Tests that
//! only need to verify "app launches without crashing" live in `apps::`;
//! tests for the bionic-built Xwayland (including buffer-type assertions
//! for X11 clients) live in `xwayland::`; libhybris-specific regressions
//! that don't drive a real client live in `hybris::`.
//!
//! Coverage matrix:
//!   - Minimal upstream demos (`weston-simple-shm`, `weston-simple-egl`,
//!     `vkcube`) for the three buffer paths in isolation.
//!   - Real toolkits: GTK3, GTK4, Firefox, supertuxkart — each pinned to
//!     a specific path so a renderer-selection regression (e.g. WebRender
//!     disabled, GSK falling back to cairo) shows up.
//!   - Static `vulkaninfo` / `eglinfo` sanity that the right driver
//!     loaded in the first place.
//!
//! Requires libhybris and an Android GPU driver, so these tests fail on
//! the emulator.
//!
//! `weston-simple-vulkan` would be the natural counterpart to the SHM /
//! EGL demos but only landed post-14.0; Arch ships weston 14.0.2, so we
//! use `vkcube` (vulkan-tools) as the stand-in until that catches up.

use std::time::Duration;

use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::helpers::{
    assert_client_animating, assert_compositor_clean, launch_and_wait_for_ahb, require_compositor,
    saw_ahb_import, saw_shm_import, TIMEOUT,
};
use tawc_integration::{adb, compositor};

const WESTON_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const VKCUBE_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);

const FIREFOX_CMD: &str = "firefox --no-remote";

/// Sanity-check that libhybris can load the Android Vulkan driver via
/// `android_dlopen` and that our `vulkanplatform_wayland.so` advertises
/// `VK_KHR_wayland_surface` by remapping `VK_KHR_android_surface`. Runs
/// `vulkaninfo --summary` in the chroot; `/usr/local/lib` is first on
/// `LD_LIBRARY_PATH` so libhybris's `libvulkan.so.1` shadows the standard
/// `vulkan-icd-loader` copy. Doesn't exercise swapchain/present — the
/// `test_vulkan_client_uses_hardware_buffers` test below does.
#[test]
fn test_vulkaninfo_loads_android_driver() {
    require_compositor();

    let out = adb::rootfs_run("vulkaninfo --summary").expect("failed to run vulkaninfo in chroot");
    assert!(
        out.status.success(),
        "vulkaninfo exited non-zero: status={:?}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    let stdout = String::from_utf8_lossy(&out.stdout);
    let _stderr = String::from_utf8_lossy(&out.stderr);

    // The WSI platform layer rewrites VK_KHR_android_surface to
    // VK_KHR_wayland_surface in the instance extension list — a
    // signature only our libhybris+vulkanplatform_wayland stack
    // produces. The distro vulkan-icd-loader by itself only
    // advertises VK_KHR_xcb_surface / VK_KHR_xlib_surface (or
    // nothing on a headless box), never VK_KHR_wayland_surface
    // pointing at an Android Vulkan driver.
    assert!(
        stdout.contains("VK_KHR_wayland_surface"),
        "VK_KHR_wayland_surface not advertised — distro vulkan-icd-loader shadowed our libvulkan.so, or vulkanplatform_wayland.so didn't load?\nstdout:\n{stdout}"
    );

    // Some Android Vulkan driver got loaded. The exact vendor depends on the
    // phone (Adreno / Mali / ...) — just check we have a driver at all.
    assert!(
        stdout.contains("driverID") && stdout.contains("DRIVER_ID_"),
        "no Vulkan physical device reported\nstdout:\n{stdout}"
    );
}

/// EGL counterpart to `test_vulkaninfo_loads_android_driver`. `eglinfo -B`
/// in the chroot must report a real GPU driver, not a software fallback,
/// regardless of which backend wired it up.
///
/// - **Libhybris backend:** `eglinfo` picks up libhybris's `libEGL.so`
///   shim (`android_dlopen` → bionic libEGL → vendor blob), reports
///   `EGL_VENDOR=Android META-EGL` and the device's GPU as the GLES
///   renderer (`Adreno`, `Mali`, …). The `Android META-EGL` signature is
///   the smoking gun that the libhybris shim was loaded.
/// - **Gfxstream backend:** `eglinfo` picks up the distro Mesa libEGL,
///   which under a fully-wired bridge would route GL through Zink ⇒
///   gfxstream-vk ⇒ kumquat ⇒ Adreno (notes/gfxstream-bridge.md
///   "GL/GLES path: Zink, not native gfxstream-GL"). Reports
///   `EGL_VENDOR=Mesa Project` and a real GPU renderer. The thing we
///   guard against is `llvmpipe` / `softpipe` / `swrast` — those mean
///   Mesa fell back to software because Zink didn't pick up
///   gfxstream-vk (today's failure mode: `egl: failed to create dri2
///   screen`, no DRI device for the GBM/DRI2 path).
#[test]
fn test_eglinfo_loads_android_driver() {
    require_compositor();

    let out = adb::rootfs_run("eglinfo -B").expect("failed to run eglinfo in chroot");

    // eglinfo's exit code tracks whether *every* platform initialized
    // — under libhybris it picks the single "Default display platform"
    // and exits 0; under distro Mesa it probes Wayland/X11/GBM/Device
    // independently, and the GBM platform fails because there's no
    // /dev/dri/cardN inside the chroot, which makes the binary exit 1
    // even when Wayland EGL (the path the compositor's clients use)
    // works fine. Inspect stdout instead.
    let stdout = String::from_utf8_lossy(&out.stdout);

    // GLES profile vendor/renderer should reflect a real GPU. Both
    // backends must report this — eglinfo without a working driver
    // truncates the GLES section entirely.
    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );

    match tawc_integration::graphics_backend().as_str() {
        "libhybris" => {
            // Distinctive Android EGL identifier; Mesa would say "Mesa
            // Project". This is also what tells us our libEGL.so shim was
            // loaded and that it dlopen'd the bionic libEGL behind it —
            // distro Mesa libEGL alone could never produce this string.
            assert!(
                stdout.contains("Android META-EGL"),
                "EGL vendor string is not Android — distro Mesa libEGL \
                 shadowed our shim or libhybris failed to load the Android \
                 driver?\nstdout:\n{stdout}"
            );
        }
        "gfxstream" => {
            // Distro Mesa libEGL is the loader; what we're checking is
            // that GL didn't fall back to software (= Zink-on-gfxstream-vk
            // is wired up).
            assert!(
                !stdout.contains("llvmpipe")
                    && !stdout.contains("softpipe")
                    && !stdout.contains("swrast"),
                "Mesa fell back to a software rasteriser (llvmpipe / \
                 softpipe / swrast) — Zink didn't pick up gfxstream-vk. \
                 Today's blocker is `egl: failed to create dri2 screen` \
                 (no /dev/dri/cardN for the GBM/DRI2 path). See \
                 notes/gfxstream-bridge.md \"GL/GLES path: Zink, not \
                 native gfxstream-GL\" for the fix.\nstdout:\n{stdout}"
            );
            // And confirm we're routed through the bridge: vulkaninfo
            // already proves the bridge is up; the GL renderer string
            // should mention the same Adreno via gfxstream-vk.
            assert!(
                stdout.contains("Adreno") || stdout.contains("gfxstream"),
                "GLES renderer doesn't mention Adreno or gfxstream — Zink \
                 may be routing to a different Vulkan ICD than the one we \
                 staged.\nstdout:\n{stdout}"
            );
        }
        other => panic!("unknown graphics backend: {other}"),
    }
}

/// `weston-simple-shm` is the canonical minimal `wl_shm` client: a few
/// hundred lines that allocate a pool, attach a buffer, draw a moving
/// pattern. If this fails the SHM plumbing in the compositor is broken
/// regardless of what GTK/Firefox do.
#[test]
fn test_weston_simple_shm_uses_shm_buffers() {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = RootfsProcess::spawn("weston-simple-shm").expect("spawn weston-simple-shm");
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + WESTON_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("weston-simple-shm crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_shm_import(&logs) {
            saw_buffer = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    std::thread::sleep(Duration::from_millis(500));

    assert!(
        saw_buffer,
        "weston-simple-shm did not produce SHM buffer imports within {:?}",
        WESTON_LAUNCH_TIMEOUT
    );

    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(
        !saw_ahb_import(&logs),
        "Unexpected AHB import — weston-simple-shm should never use hardware buffers"
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while weston-simple-shm running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    assert_client_animating("weston-simple-shm", Duration::from_millis(1500), 10);

    app.stop()
        .expect("weston-simple-shm failed to stop cleanly");
    assert_compositor_clean();
}

/// `weston-simple-egl` is the canonical minimal EGL/GLES client: an
/// animated triangle on `wl_egl_window`. On libhybris this binds
/// `android_wlegl`, allocates `ANativeWindowBuffer`s, and presents via
/// AHB — exactly the path the compositor's `wlegl` module imports.
///
/// Under the gfxstream backend the path becomes Mesa's distro
/// `libEGL.so.1` ➜ Zink ➜ `gfxstream-vk` ➜ kumquat → Android Vulkan; the
/// AHB still arrives at the compositor through `android_wlegl` once
/// Phase 4-5 is wired (notes/gfxstream-bridge.md "Remaining work"). The
/// log line shape is identical to libhybris, so no per-backend asserts
/// needed.
#[test]
fn test_weston_simple_egl_uses_hardware_buffers() {
    let mut app = launch_and_wait_for_ahb(
        "weston-simple-egl",
        "weston-simple-egl",
        WESTON_LAUNCH_TIMEOUT,
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while weston-simple-egl running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    assert_client_animating("weston-simple-egl", Duration::from_millis(1500), 10);

    app.stop()
        .expect("weston-simple-egl failed to stop cleanly");
    assert_compositor_clean();
}

/// Vulkan client end-to-end: `vkcube` opens a Wayland surface via
/// `vulkanplatform_wayland.so`, the swapchain hands out
/// `ANativeWindowBuffer`s, and the compositor imports those as AHB
/// textures. Complements `test_vulkaninfo_loads_android_driver` (which
/// only inspects extension strings) by actually exercising present.
///
/// Under the gfxstream backend this is the canonical end-to-end Vulkan
/// test for the bridge: the chroot's `gfxstream-vk` ICD encodes the
/// command stream over kumquat, the in-process kumquat thread
/// translates it via `libgfxstream_backend.so`, and the resulting AHB
/// reaches the compositor through `android_wlegl`. Currently fails
/// because Phase 4 (AHB handoff after `vkQueuePresentKHR`) and Phase 5
/// (`VK_KHR_wayland_surface` plumbing) aren't wired yet — see
/// notes/gfxstream-bridge.md "Remaining work to a fully-integrated
/// bridge backend".
#[test]
fn test_vulkan_client_uses_hardware_buffers() {
    // `--c` caps the frame count; we still kill via stop() so the value
    // mostly just guards against the test runner hanging if stop() fails.
    // Cap is set well above what the animation check below needs so vkcube
    // doesn't naturally exit mid-test and falsely flag as stuck.
    let mut app = launch_and_wait_for_ahb(
        "vkcube --wsi wayland --c 3000",
        "vkcube",
        VKCUBE_LAUNCH_TIMEOUT,
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while vkcube running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    assert_client_animating("vkcube", Duration::from_millis(1500), 10);

    app.stop().expect("vkcube failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK3 with `GDK_GL=disabled` falls back to its software renderer and
/// presents via `wl_shm`. Uses the stock `gtk3-demo-application` (just
/// opens a window with a menu bar) so this exercises the SHM client path
/// with a real, non-debug program.
#[test]
fn test_gtk3_app_uses_shm_buffers() {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = RootfsProcess::spawn("GDK_GL=disabled gtk3-demo-application")
        .expect("spawn gtk3-demo-application");
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + GTK_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("gtk3-demo-application crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_shm_import(&logs) {
            saw_buffer = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    std::thread::sleep(Duration::from_millis(500));

    assert!(
        saw_buffer,
        "gtk3-demo-application did not produce SHM buffer imports within {:?} (GDK_GL=disabled should use SHM)",
        GTK_LAUNCH_TIMEOUT
    );

    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(
        !saw_ahb_import(&logs),
        "Unexpected AHB import — gtk3-demo-application with GDK_GL=disabled should not use hardware buffers"
    );

    app.stop().expect("gtk3-demo-application failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK3 with the chroot's default `GDK_GL=gles:always` (set by the shared
/// `env -i` wrapper — see `RootfsEnv.kt`) renders through `android_wlegl`
/// and presents via AHB hardware buffers.
#[test]
fn test_gtk3_app_uses_hardware_buffers() {
    let mut app = launch_and_wait_for_ahb(
        "gtk3-demo-application",
        "gtk3-demo-application",
        GTK_LAUNCH_TIMEOUT,
    );

    app.stop().expect("gtk3-demo-application failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK4's default GSK renderer goes through `android_wlegl` on libhybris,
/// so launching `gtk4-widget-factory` must produce AHB imports. Catches
/// GTK4-specific regressions in the GL path.
///
/// (We use `gtk4-widget-factory` rather than `gtk4-demo-application`
/// because the latter pulls in a session-bus / GApplication setup that
/// errors out in this rootfs and never actually maps a window.)
#[test]
fn test_gtk4_app_uses_hardware_buffers() {
    let mut app = launch_and_wait_for_ahb(
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );

    app.stop().expect("gtk4-widget-factory failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK4 with `GSK_RENDERER=cairo` uses the software cairo renderer and
/// presents via `wl_shm`. Exercises the GTK4 SHM path, which is distinct
/// from GTK3's (GTK4 ping-pongs fresh wl_buffers per frame and caches the
/// first released cairo surface — a regression that double-releases SHM
/// buffers will crash GTK4 here while leaving GTK3 working).
#[test]
fn test_gtk4_app_uses_shm_buffers() {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = RootfsProcess::spawn("GSK_RENDERER=cairo gtk4-widget-factory")
        .expect("spawn gtk4-widget-factory");
    app.ensure_pgid();

    // Wait for at least one SHM buffer import. The double-release bug would
    // typically only manifest on the *second* SHM commit, so also keep the
    // process alive for a moment afterwards and re-check the compositor sees
    // it as a healthy client.
    let deadline = std::time::Instant::now() + GTK_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("gtk4-widget-factory crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_shm_import(&logs) {
            saw_buffer = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    std::thread::sleep(Duration::from_millis(1000));

    assert!(
        saw_buffer,
        "gtk4-widget-factory did not produce SHM buffer imports within {:?} (GSK_RENDERER=cairo should use SHM)",
        GTK_LAUNCH_TIMEOUT
    );
    assert!(
        app.is_running(),
        "gtk4-widget-factory exited shortly after first SHM commit (regression of the cairo double-release bug?)"
    );

    let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
    assert!(
        !saw_ahb_import(&logs),
        "Unexpected AHB import — gtk4-widget-factory with GSK_RENDERER=cairo should not use hardware buffers"
    );

    app.stop().expect("gtk4-widget-factory failed to stop cleanly");
    assert_compositor_clean();
}

/// Firefox steady-state asserts the chrome surface presents through the
/// libhybris/AHB path, not falling back to SHM. Pairs with
/// `apps::test_firefox_launches` (which only checks that the process
/// comes up).
///
/// Earlier revisions counted "wlegl: imported ANativeWindowBuffer as
/// texture" log lines in a 2-second window. That looked sensible — the
/// AHB is hot, the import path runs every frame — but actually the
/// compositor only logs an *import* the first time it sees a given AHB;
/// subsequent commits of the same buffer come through the cached
/// EGLImage and emit nothing. Firefox's WebRender-on-AHB path settles
/// into a small ring (2-6 buffers) within the first few hundred ms, so
/// post-launch the import-line count is zero even though every chrome
/// frame is rendering as AHB at 60 fps.
///
/// Use the compositor state snapshot instead. It exposes the *currently
/// attached* buffer count per type (`surfaces_wlegl`, `surfaces_shm`) and
/// a monotonic `frames` counter that ticks on every commit. The two
/// together catch:
///   - "Firefox attached AHB once, then switched to SHM mid-run"
///     → surfaces_wlegl drops to 0, surfaces_shm rises.
///   - "WebRender disabled, every chrome frame is cairo/SHM"
///     → surfaces_wlegl == 0 from the start.
///   - "Firefox is wedged / not committing"
///     → frames doesn't advance.
#[test]
fn test_firefox_uses_hardware_buffers() {
    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog.
    let _ = adb::rootfs_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups",
    );

    let mut firefox = launch_and_wait_for_ahb(FIREFOX_CMD, "Firefox", FIREFOX_LAUNCH_TIMEOUT);

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    let frames_before = state.frames;
    std::thread::sleep(Duration::from_secs(2));
    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor steady-state");
    assert!(
        state.surfaces_wlegl >= 1,
        "Firefox has no AHB-attached surfaces during steady state — \
         fell back to SHM? state={state:?}"
    );
    assert!(
        state.surfaces_shm == 0,
        "Firefox attached SHM buffers during steady-state rendering — \
         WebRender disabled, chrome falling back to cairo? Likely a \
         libhybris EGL probe regression (Firefox auto-detects WebRender \
         and dmabuf via libhybris stubs — see notes/firefox.md). \
         state={state:?}"
    );
    assert!(
        state.frames > frames_before,
        "Compositor frames counter did not advance over 2 s — Firefox \
         appears wedged / not committing. before={frames_before} after={state:?}"
    );

    firefox.stop().expect("Firefox process group failed to stop cleanly");
    assert_compositor_clean();
}

/// supertuxkart presents through the libhybris/AHB GL path. Pairs with
/// `apps::test_supertuxkart_launches`.
#[test]
fn test_supertuxkart_uses_hardware_buffers() {
    let mut stk = launch_and_wait_for_ahb("supertuxkart", "supertuxkart", STK_LAUNCH_TIMEOUT);

    assert!(
        stk.is_running(),
        "supertuxkart exited after reaching first render"
    );

    stk.stop()
        .expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}
