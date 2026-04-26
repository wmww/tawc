//! Application tests. Verify that real desktop programs (Firefox,
//! supertuxkart, GTK demos, vulkaninfo) launch without crashing, render
//! through the expected buffer path (AHB hardware buffers / SHM software
//! buffers), and look correct to the compositor.
//!
//! Requires libhybris and an Android GPU driver, so these tests fail on
//! the emulator.

use std::time::Duration;

use tawc_integration::chroot_process::ChrootProcess;
use tawc_integration::helpers::{
    assert_compositor_clean, ensure_compositor, launch_and_wait_for_ahb, saw_ahb_import,
    saw_shm_import, TIMEOUT,
};
use tawc_integration::{adb, chroot, compositor};

const FIREFOX_CMD: &str = "GDK_GL=gles:always MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 \
    MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 \
    MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
    DISPLAY= firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);
const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

#[test]
fn test_firefox_launches_with_hardware_buffers() {
    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog
    // (killall doesn't give Firefox a clean shutdown, leaving these behind)
    let _ = adb::chroot_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups",
    );

    let mut firefox = launch_and_wait_for_ahb(FIREFOX_CMD, "Firefox", FIREFOX_LAUNCH_TIMEOUT);

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel while Firefox running, got {:?}",
        state
    );
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client while Firefox running, got {:?}",
        state
    );

    firefox.stop().expect("Firefox process group failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_supertuxkart_launches_with_hardware_buffers() {
    let mut stk = launch_and_wait_for_ahb("supertuxkart", "supertuxkart", STK_LAUNCH_TIMEOUT);

    assert!(
        stk.is_running(),
        "supertuxkart exited after reaching first render"
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while STK running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel while STK running, got {:?}",
        state
    );
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client while STK running, got {:?}",
        state
    );

    stk.stop()
        .expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}

/// Sanity-check that libhybris can load the Android Vulkan driver via
/// `android_dlopen` and that our `vulkanplatform_wayland.so` advertises
/// `VK_KHR_wayland_surface` by remapping `VK_KHR_android_surface`. Runs
/// `vulkaninfo --summary` in the chroot; `/usr/local/lib` is first on
/// `LD_LIBRARY_PATH` so libhybris's `libvulkan.so.1` shadows the standard
/// `vulkan-icd-loader` copy. Doesn't exercise swapchain/present — see
/// issues/vulkan-vkcube-not-visible.md for that.
#[test]
fn test_vulkaninfo_loads_android_driver() {
    ensure_compositor();

    let out = adb::chroot_run("vulkaninfo --summary").expect("failed to run vulkaninfo in chroot");
    assert!(
        out.status.success(),
        "vulkaninfo exited non-zero: status={:?}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);

    // libhybris's CFI patch message goes to stderr on every load — confirms
    // our libvulkan.so (not the distro vulkan-icd-loader) was picked up.
    assert!(
        stderr.contains("libhybris: patched __cfi_slowpath"),
        "libhybris not active — distro vulkan-icd-loader shadowed our libvulkan.so?\nstderr:\n{stderr}"
    );

    // The WSI platform layer rewrites VK_KHR_android_surface to
    // VK_KHR_wayland_surface in the instance extension list.
    assert!(
        stdout.contains("VK_KHR_wayland_surface"),
        "VK_KHR_wayland_surface not advertised — vulkanplatform_wayland.so not loaded?\nstdout:\n{stdout}"
    );

    // Some Android Vulkan driver got loaded. The exact vendor depends on the
    // phone (Adreno / Mali / ...) — just check we have a driver at all.
    assert!(
        stdout.contains("driverID") && stdout.contains("DRIVER_ID_"),
        "no Vulkan physical device reported\nstdout:\n{stdout}"
    );
}

/// GTK3 with `GDK_GL=disabled` falls back to its software renderer and
/// presents via `wl_shm`. Use the stock `gtk3-demo-application` (just opens
/// a window with a menu bar) so this exercises the SHM client path with a
/// real, non-debug program.
#[test]
fn test_gtk3_app_uses_shm_buffers() {
    ensure_compositor();
    chroot::ensure_pkgs(&["gtk3"]).expect("install gtk3");
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = ChrootProcess::spawn("GDK_GL=disabled gtk3-demo-application")
        .expect("spawn gtk3-demo-application");
    app.ensure_pgid();

    // Wait for it to render at least one frame.
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

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while gtk3-demo-application running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    app.stop().expect("gtk3-demo-application failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK3 with `GDK_GL=gles:always` renders through android_wlegl and presents
/// via AHB hardware buffers.
#[test]
fn test_gtk3_app_uses_hardware_buffers() {
    chroot::ensure_pkgs(&["gtk3"]).expect("install gtk3");
    let mut app = launch_and_wait_for_ahb(
        "GDK_GL=gles:always gtk3-demo-application",
        "gtk3-demo-application",
        GTK_LAUNCH_TIMEOUT,
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while gtk3-demo-application running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    app.stop().expect("gtk3-demo-application failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK4's default GSK renderer goes through android_wlegl on libhybris, so
/// launching `gtk4-widget-factory` must produce AHB imports. Catches
/// GTK4-specific regressions in the GL path.
///
/// (We use `gtk4-widget-factory` rather than `gtk4-demo-application` because
/// the latter pulls in a session-bus / GApplication setup that errors out
/// in this rootfs and never actually maps a window.)
#[test]
fn test_gtk4_app_uses_hardware_buffers() {
    chroot::ensure_pkgs(&["gtk4"]).expect("install gtk4");
    let mut app = launch_and_wait_for_ahb(
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while gtk4-widget-factory running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
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
    ensure_compositor();
    chroot::ensure_pkgs(&["gtk4"]).expect("install gtk4");
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = ChrootProcess::spawn("GSK_RENDERER=cairo gtk4-widget-factory")
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

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while gtk4-widget-factory running");
    assert!(
        state.toplevels >= 1,
        "Compositor should see at least 1 toplevel, got {:?}",
        state
    );

    app.stop().expect("gtk4-widget-factory failed to stop cleanly");
    assert_compositor_clean();
}
