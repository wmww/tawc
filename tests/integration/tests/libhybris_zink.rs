//! Tests for the LibhybrisZink path: distro Mesa + Zink (Gallium driver
//! translating GL/GLES to Vulkan), routed through libhybris's libvulkan
//! as the only Vulkan ICD. Every spawn pins
//! [`GraphicsBackend::LibhybrisZink`] so the user's persisted Settings
//! pick can't leak in.
//!
//! Representative subset — not a full mirror of `hybris::`. The TLS /
//! dlopen regression coverage lives there; here we just confirm the
//! stack is plumbed correctly: Vulkan still hits libhybris, and GL
//! hits Zink (not the silent llvmpipe fallback).
//!
//! The GL/Zink path is gated on the vendor Adreno Vulkan exposing
//! `VK_KHR_dynamic_rendering` (Vulkan 1.3-era extension). On test
//! devices to date — OnePlus 9 / Android 14 / Adreno 660v2 (Vulkan
//! 1.1.128) and Pixel 4a / Android 16 / Adreno 618 (Vulkan 1.1.128) —
//! the vendor driver is too old and Zink declines to init. The Mesa
//! patch (`deps/mesa-patches/mesa/06-tawc-zink-nokms.patch`) is the
//! plumbing; whether `test_eglinfo_reports_zink_renderer` passes
//! depends on the device. Failing this test on a Vulkan-1.1 device is
//! expected; passing it on a Vulkan-1.3 device is the tripwire we
//! want.
//!
//! libhybris is aarch64-only in tawc, so these fail on the emulator.

use std::time::Duration;

use tawc_integration::helpers::{
    assert_renders_via_ahb, require_compositor,
};
use tawc_integration::{adb, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::LibhybrisZink;

const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

/// `vulkaninfo --summary` must still report libhybris's Android Vulkan
/// driver — LibhybrisZink keeps libhybris as the only ICD via
/// `VK_ICD_FILENAMES`, exactly so Zink can dispatch into it. Same
/// physical device the `Hybris` backend sees.
#[test]
fn test_vulkaninfo_loads_android_driver() {
    require_compositor();

    let out = adb::rootfs_run_with(BACKEND, "vulkaninfo --summary")
        .expect("failed to run vulkaninfo in chroot");
    assert!(
        out.status.success(),
        "vulkaninfo exited non-zero: status={:?}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        stdout.contains("VK_KHR_wayland_surface"),
        "VK_KHR_wayland_surface not advertised — VK_ICD_FILENAMES didn't \
         point at libhybris's libvulkan, or vulkanplatform_wayland.so \
         didn't load?\nstdout:\n{stdout}"
    );
    assert!(
        stdout.contains("driverID") && stdout.contains("DRIVER_ID_"),
        "no Vulkan physical device reported\nstdout:\n{stdout}"
    );
}

/// `eglinfo -B` under LibhybrisZink must report Zink as the GLES
/// renderer. The silent-failure mode this guards against is Mesa
/// falling back to `llvmpipe` when Zink can't init — usually because
/// libhybris's Vulkan is missing a required feature (descriptor
/// indexing, sync2, dynamic_rendering, …). Catching it here is the
/// whole point.
///
/// Expected to fail on devices whose vendor Adreno Vulkan is below
/// 1.3 — see module doc. Runs unconditionally so a future device with
/// a current Adreno driver lights up the path.
#[test]
fn test_eglinfo_reports_zink_renderer() {
    require_compositor();

    let out = adb::rootfs_run_with(BACKEND, "eglinfo -B")
        .expect("failed to run eglinfo in chroot");
    let stdout = String::from_utf8_lossy(&out.stdout);

    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(
        !stdout.contains("llvmpipe"),
        "LibhybrisZink fell back to llvmpipe — Zink failed to init. \
         Likely a Vulkan feature gap on libhybris's side (descriptor \
         indexing, sync2, …). See notes/libhybris-zink.md \"Risks\".\n\
         stdout:\n{stdout}"
    );
    assert!(
        stdout.contains("Zink"),
        "GL_RENDERER does not mention Zink — MESA_LOADER_DRIVER_OVERRIDE \
         didn't reach the child, or libhybris's libEGL shadowed the \
         distro libglvnd via the 00_libhybris.json vendor entry \
         (__EGL_VENDOR_LIBRARY_FILENAMES should pin Mesa's 50_mesa.json).\n\
         stdout:\n{stdout}"
    );
    assert!(
        !stdout.contains("Android META-EGL"),
        "LibhybrisZink loaded libhybris's libEGL shim — \
         __EGL_VENDOR_LIBRARY_FILENAMES leak, or LD_LIBRARY_PATH put \
         libhybris ahead of distro Mesa.\nstdout:\n{stdout}"
    );
}

/// GTK4 widget-factory: Mesa picks Zink (per
/// `MESA_LOADER_DRIVER_OVERRIDE=zink`), Zink's draw commands turn into
/// Vulkan calls, libhybris's Vulkan dispatches present via the
/// `vulkanplatform_wayland` swapchain — which means GTK4's chrome
/// surface should land as AHB just like under the Hybris backend.
#[test]
fn test_gtk4_renders_via_ahb() {
    assert_renders_via_ahb(
        BACKEND,
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );
}
