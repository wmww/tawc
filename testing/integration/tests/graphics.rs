//! Graphics-stack tests. Drive the three Wayland buffer paths the
//! compositor supports — `wl_shm`, EGL/GLES via libhybris+`android_wlegl`,
//! and Vulkan via `vulkanplatform_wayland.so` — using minimal upstream
//! demos (`weston-simple-shm`, `weston-simple-egl`, `vkcube`) instead of
//! full toolkits, so a regression here points at the buffer plumbing
//! itself rather than at GTK/Firefox/STK glue. Also covers the static
//! `vulkaninfo` / `eglinfo` checks that just verify libhybris loads the
//! right Android driver.
//!
//! Requires libhybris and an Android GPU driver, so these tests fail on
//! the emulator.
//!
//! `weston-simple-vulkan` would be the natural counterpart to the SHM /
//! EGL demos but only landed post-14.0; Arch ships weston 14.0.2, so we
//! use `vkcube` (vulkan-tools) as the stand-in until that catches up.

use std::time::Duration;

use tawc_integration::chroot_process::ChrootProcess;
use tawc_integration::helpers::{
    assert_client_animating, assert_compositor_clean, launch_and_wait_for_ahb, require_compositor,
    saw_ahb_import, saw_shm_import, TIMEOUT,
};
use tawc_integration::{adb, compositor};

const WESTON_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const VKCUBE_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

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

    let out = adb::chroot_run("vulkaninfo --summary").expect("failed to run vulkaninfo in chroot");
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
/// in the chroot picks up libhybris's `libEGL.so` shim (it goes through
/// the same `android_dlopen` path, so the CFI patch message fires) and
/// reports the Android driver's EGL/GLES strings — `Android META-EGL`
/// for the EGL implementation and the GPU vendor/renderer (e.g. `Adreno`,
/// `Mali`) for GLES.
#[test]
fn test_eglinfo_loads_android_driver() {
    require_compositor();

    let out = adb::chroot_run("eglinfo -B").expect("failed to run eglinfo in chroot");
    assert!(
        out.status.success(),
        "eglinfo exited non-zero: status={:?}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    let stdout = String::from_utf8_lossy(&out.stdout);
    let _stderr = String::from_utf8_lossy(&out.stderr);

    // Distinctive Android EGL identifier; Mesa would say "Mesa
    // Project". This is also what tells us our libEGL.so shim was
    // loaded and that it dlopen'd the bionic libEGL behind it —
    // distro Mesa libEGL alone could never produce this string.
    assert!(
        stdout.contains("Android META-EGL"),
        "EGL vendor string is not Android — distro Mesa libEGL shadowed our shim or libhybris failed to load the Android driver?\nstdout:\n{stdout}"
    );

    // GLES profile vendor/renderer should reflect the actual Android GPU.
    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}"
    );
}

/// `weston-simple-shm` is the canonical minimal `wl_shm` client: a few
/// hundred lines that allocate a pool, attach a buffer, draw a moving
/// pattern. If this fails the SHM plumbing in the compositor is broken
/// regardless of what GTK/Firefox do.
#[test]
fn test_weston_simple_shm_uses_shm_buffers() {
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = ChrootProcess::spawn("weston-simple-shm").expect("spawn weston-simple-shm");
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
