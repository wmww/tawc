//! Tests for the gfxstream-bridge backend: every "X renders via
//! hardware buffers" smoke run under [`GraphicsBackend::Gfxstream`].
//! The kumquat server runs in the compositor process (always on), and
//! the chroot-side `libvulkan_gfxstream.so` + ICD JSON ride in the APK
//! — `BridgeInstallProvider` lays them into each rootfs at install
//! time, so there is no extra setup beyond pinning the backend on each
//! spawn (which selects `VK_ICD_FILENAMES` + `VIRTGPU_KUMQUAT` /
//! `LIBGL_*` env, not libhybris).
//!
//! Companion module: `libhybris::` runs the same smokes against the
//! libhybris path. A regression in one backend shouldn't accidentally
//! hide behind the other.
//!
//! Most tests here are `#[ignore]`d because phase 4 (AHB handoff
//! after `vkQueuePresentKHR`) and phase 5 (`VK_KHR_wayland_surface`)
//! of the bridge aren't wired yet — see `notes/gfxstream-bridge.md`
//! "Remaining work". They're kept here so they're ready to run the
//! moment the path works (run via `cargo test -- --ignored` or drop
//! the attribute). `test_vkcube_renders_via_ahb` is the one that
//! works today and runs unconditionally.
//!
//! `scripts/run-integration-tests.sh` marks the active tests in this
//! module ignored on emulator targets because the bridge currently does
//! not work there.

use std::time::Duration;

use tawc_integration::helpers::{
    assert_client_animating, assert_compositor_clean, assert_renders_via_ahb,
    firefox_profile_cleanup, launch_and_wait_for_ahb, require_compositor, TIMEOUT,
};
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Gfxstream;

const WESTON_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const VKCUBE_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);

const FIREFOX_CMD: &str = "firefox --no-remote";

/// `eglinfo -B` picks up the distro Mesa libEGL (no libhybris shim on
/// LD_LIBRARY_PATH); under a fully-wired bridge GL goes Mesa → Zink →
/// gfxstream-vk → kumquat → Adreno (see notes/gfxstream-bridge.md
/// "GL/GLES path: Zink, not native gfxstream-GL"). What we guard
/// against is software fallback (`llvmpipe` / `softpipe` / `swrast`),
/// which means Zink didn't pick up gfxstream-vk — today's blocker is
/// `egl: failed to create dri2 screen` (no DRI device for the GBM/DRI2
/// path).
#[test]
#[ignore = "gfxstream not yet working"]
fn test_eglinfo_loads_mesa_via_gfxstream() {
    tawc_integration::helpers::test_init();
    require_compositor();

    let out = adb::rootfs_run_with(BACKEND, "eglinfo -B")
        .expect("failed to run eglinfo in chroot");
    // Under distro Mesa the GBM platform probe fails (no /dev/dri/cardN
    // inside the chroot), making the binary exit 1 even when Wayland
    // EGL works fine; inspect stdout instead.
    let stdout = String::from_utf8_lossy(&out.stdout);

    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(
        !stdout.contains("llvmpipe")
            && !stdout.contains("softpipe")
            && !stdout.contains("swrast"),
        "Mesa fell back to a software rasteriser — Zink didn't pick up \
         gfxstream-vk. Today's blocker is `egl: failed to create dri2 \
         screen` (no /dev/dri/cardN for the GBM/DRI2 path). See \
         notes/gfxstream-bridge.md \"GL/GLES path: Zink, not native \
         gfxstream-GL\" for the fix.\nstdout:\n{stdout}"
    );
    // Confirm we're routed through the bridge: vulkaninfo proves the
    // bridge is up; the GL renderer string should mention the same
    // Adreno via gfxstream-vk.
    assert!(
        stdout.contains("Adreno") || stdout.contains("gfxstream"),
        "GLES renderer doesn't mention Adreno or gfxstream — Zink may \
         be routing to a different Vulkan ICD than the one we staged.\n\
         stdout:\n{stdout}"
    );
}

/// Under gfxstream, `weston-simple-egl` goes through Mesa's distro
/// `libEGL.so.1` ➜ Zink ➜ `gfxstream-vk` ➜ kumquat → Android Vulkan;
/// the AHB still arrives at the compositor through `android_wlegl`
/// once phase 4-5 is wired (notes/gfxstream-bridge.md "Remaining
/// work"). The log line shape is identical to libhybris, so the helper
/// reuse is intentional.
#[test]
#[ignore = "gfxstream not yet working"]
fn test_weston_simple_egl_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    let mut app = launch_and_wait_for_ahb(
        BACKEND,
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

    app.stop().expect("weston-simple-egl failed to stop cleanly");
    assert_compositor_clean();
}

/// Canonical end-to-end Vulkan test for the bridge: the chroot's
/// `gfxstream-vk` ICD encodes the command stream over kumquat, the
/// in-process kumquat thread translates it via `libgfxstream_backend.so`,
/// and the resulting AHB reaches the compositor through `android_wlegl`.
/// Currently fails because phases 4 / 5 aren't wired — see
/// notes/gfxstream-bridge.md "Remaining work to a fully-integrated
/// bridge backend".
#[test]
#[cfg_attr(
    tawc_skip_gfxstream_on_target,
    ignore = "gfxstream skipped on emulator target"
)]
fn test_vkcube_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    let mut app = launch_and_wait_for_ahb(
        BACKEND,
        "vkcube --wsi wayland",
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

/// GTK3 with default GSK / GDK_GL settings under gfxstream. Should
/// render via AHB once Zink-on-gfxstream-vk is fully wired up.
#[test]
#[ignore = "gfxstream not yet working"]
fn test_gtk3_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    assert_renders_via_ahb(
        BACKEND,
        "gtk3-demo-application",
        "gtk3-demo-application",
        GTK_LAUNCH_TIMEOUT,
    );
}

/// GTK4's default GSK renderer under the bridge. Same caveat as the
/// other AHB-via-gfxstream tests — depends on phase 4-5 landing.
#[test]
#[ignore = "gfxstream not yet working"]
fn test_gtk4_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    assert_renders_via_ahb(
        BACKEND,
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );
}

/// Firefox under gfxstream — mirror of the libhybris steady-state
/// surface-count check in `libhybris::test_firefox_renders_via_ahb`. See
/// that test's doc for why we sample the compositor state snapshot
/// instead of counting log lines.
#[test]
#[ignore = "gfxstream not yet working"]
fn test_firefox_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    firefox_profile_cleanup(BACKEND);

    let mut firefox = launch_and_wait_for_ahb(
        BACKEND,
        FIREFOX_CMD,
        "Firefox",
        FIREFOX_LAUNCH_TIMEOUT,
    );

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    let frames_before = state.frames;
    std::thread::sleep(Duration::from_secs(2));
    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor steady-state");
    assert!(
        state.surfaces_wlegl >= 1,
        "Firefox has no AHB-attached surfaces during steady state — \
         fell back to SHM under gfxstream? state={state:?}"
    );
    assert!(
        state.surfaces_shm == 0,
        "Firefox attached SHM buffers during steady-state rendering — \
         WebRender / dmabuf detection broken on the bridge path? \
         state={state:?}"
    );
    assert!(
        state.frames > frames_before,
        "Compositor frames counter did not advance over 2 s under \
         gfxstream — Firefox wedged. before={frames_before} after={state:?}"
    );

    firefox.stop().expect("Firefox session failed to stop cleanly");
    assert_compositor_clean();
}

/// supertuxkart through the bridge GL path.
#[test]
#[ignore = "gfxstream not yet working"]
fn test_supertuxkart_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    let mut stk = launch_and_wait_for_ahb(
        BACKEND,
        "supertuxkart",
        "supertuxkart",
        STK_LAUNCH_TIMEOUT,
    );

    assert!(
        stk.is_running(),
        "supertuxkart exited after reaching first render"
    );

    stk.stop()
        .expect("supertuxkart session failed to stop cleanly");
    assert_compositor_clean();
}
