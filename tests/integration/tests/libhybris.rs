//! Tests for the libhybris path: bionic-linker regressions (TLS, dlopen)
//! plus every "X renders via hardware buffers through libhybris" smoke.
//! Each spawn pins [`GraphicsBackend::Libhybris`] explicitly so the
//! user's persisted Settings pick can't leak in — under libhybris the
//! chroot's `LD_LIBRARY_PATH` puts libhybris's `libEGL.so` /
//! `libvulkan.so.1` ahead of the distro Mesa loader, which is precisely
//! what these tests are about.
//!
//! Companion module: `gfxstream::` runs the same hardware-buffer
//! smokes against the bridge backend, so a regression in one path
//! doesn't accidentally hide behind the other. The SHM-only smokes
//! (`weston-simple-shm`, GTK with software renderers) live in
//! `cpu_graphics::` since they're backend-agnostic by construction.
//!
//! libhybris is aarch64-only in tawc. `scripts/run-integration-tests.sh`
//! marks this module ignored on x86 devices.

use std::time::Duration;

use tawc_integration::helpers::{
    assert_client_animating, assert_compositor_clean, assert_renders_via_ahb,
    launch_and_wait_for_ahb, require_compositor, TIMEOUT,
};
use tawc_integration::{adb, compositor, rootfs, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Libhybris;

const WESTON_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const VKCUBE_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);

const FIREFOX_CMD: &str = "firefox --no-remote";

/// Regression test for libhybris's TLS handling. Three historical
/// regressions get covered in one round-trip; see repro.c for the
/// per-failure-mode breakdown. Quickly:
///
/// 1. unregister_tls_module CHECK abort on dlclose (Pixel Fold lxterminal).
///    Pre-promote-fix every TLS-using dlopen reserved a static slot
///    that unregister refused to free.
/// 2. TLSDESC dynamic resolver routed to glibc's __tls_get_addr
///    (Pixel 4a GTK/firefox; OnePlus 9 silent garbage). Post-promote-fix
///    register_soinfo_tls always registered dynamic, but TLSDESC's
///    handler kept the dynamic-resolver branch — its slow path calls
///    __tls_get_addr against host glibc with a bionic module_id, so
///    glibc's _dl_update_slotinfo asserts. Fixed by also promoting on
///    R_GENERIC_TLSDESC, mirroring the IE path.
/// 3. Per-.so __thread initial values silently zeroed (every device).
///    The patcher redirected bionic TPIDR reads to a fixed shared
///    tls_area.slots[] with no .tdata init, so a `__thread int g = 42`
///    in a dlopened bionic .so came back as 0. Fixed by reserving
///    bionic_tcb at the head of static_tls_layout, setting tls_tp_base
///    to offset_thread_pointer() (=16 on aarch64), and calling back
///    from promote_tls_module_to_static into hooks.c to memcpy .tdata
///    into the calling thread's tls_static_tls.
/// 4. Per-thread isolation and post-dlclose replay. A child thread must
///    see fresh initial TLS values, writes must not leak between threads,
///    and a new thread must be able to replay promoted-TLS initializers
///    after the original .so has been dlclose'd.
///
/// Repro binary at tests/apps/libhybris-tls-repro/. The glibc-side
/// executable and bionic-side tls_lib.so (a tiny .so with `__thread int
/// g_tls_var = 42;`) are host-cross-built by scripts/run-integration-tests.sh.
/// Drives a full hybris_dlopen + hybris_dlsym +
/// hybris_dlclose round-trip plus thread isolation, post-dlclose replay,
/// and an assert that get_tls() returns the declared initialiser.
///
/// Failure modes:
///   - SIGABRT (exit 134) inside hybris_dlclose — regression of #1
///   - exit 1 with `get_tls() = 0 (expected 42)` and a libhybris
///     pointer to the broken code path — regression of #2 or #3 (a
///     TLSDESC dynamic-resolver regression hands back garbage instead
///     of the .tdata initialiser, same symptom as a missing .tdata copy)
///   - exit 1 or SIGSEGV around `post-dlclose replay check` — the
///     promoted-TLS registry kept a pointer into an unloaded .so instead
///     of owning the initializer bytes
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_libhybris_tls_dlclose_does_not_abort() {
    tawc_integration::helpers::test_init();
    let bin = rootfs::ensure_libhybris_tls_repro().expect("libhybris-tls-repro build");

    // Run from inside the companion library dir so the repro's relative
    // weak_lib.so guard remains self-contained.
    let cmd = format!("cd /usr/local/lib && {} ./tls_lib.so", bin);
    let output = adb::rootfs_run_with(BACKEND, &cmd).expect("run libhybris-tls-repro");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    // Exit code maps to the failure mode (see fn doc + repro.c):
    //   134 = SIGABRT, the unregister CHECK abort;
    //     1 = repro's own assert (e.g. get_tls() != 42).
    // Any non-zero is a libhybris regression -- surface enough hints in
    // the message that someone seeing this in CI knows where to look.
    assert!(
        output.status.success(),
        "libhybris-tls-repro exited non-zero ({:?}).\n\
         - exit 134 + `unregister_tls_module CHECK 'mod.static_offset == SIZE_MAX' failed`\n\
           => the IE/TLSDESC lazy-promote fix in libhybris linker_tls.cpp has regressed\n\
         - exit 1 + `get_tls() = 0 (expected 42)`\n\
           => promote_tls_module_to_static is not pushing .tdata into the calling\n\
              thread's tls_static_tls, OR the bionic_tcb / tls_tp_base math has\n\
              drifted, OR a TLSDESC handler regressed back to the dynamic resolver\n\
              path (see hooks.c::tls_static_tls + linker_tls.cpp + linker.cpp\n\
              tls_tp_base + R_GENERIC_TLSDESC handler)\n\
         stdout: {stdout}\nstderr: {stderr}",
        output.status.code()
    );

    // Belt-and-braces: the repro prints these on a successful round-trip.
    // Catches the (unlikely) case where it exits 0 having taken a different
    // failure path entirely (e.g. dlopen returning a stub).
    assert!(
        stderr.contains("hybris_dlclose -> 0"),
        "libhybris-tls-repro exited 0 but never printed `hybris_dlclose -> 0` -- \
         did dlopen actually succeed?\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("thread isolation check OK"),
        "libhybris-tls-repro exited 0 but never completed the per-thread TLS \
         isolation check.\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("post-dlclose replay check OK"),
        "libhybris-tls-repro exited 0 but never completed the post-dlclose \
         promoted-TLS replay check.\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("survived; no abort, value correct"),
        "libhybris-tls-repro exited 0 but never reached the final post-dlclose \
         line -- did execution end before the value-correctness assert ran?\n\
         stdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("guard checks OK"),
        "libhybris-tls-repro exited 0 but the loud-error guards on dlsym(TLS) \
         and weak TLSDESC didn't pass. A regression here means the linker has \
         dropped the DL_ERR guards in do_dlsym (TLS) or R_GENERIC_TLSDESC \
         (unresolved weak), restoring silent corruption / wild pointers.\n\
         stdout: {stdout}\nstderr: {stderr}"
    );
}

/// Sanity-check that libhybris can load the Android Vulkan driver via
/// `android_dlopen` and that our `vulkanplatform_wayland.so` advertises
/// `VK_KHR_wayland_surface` by remapping `VK_KHR_android_surface`. Runs
/// `vulkaninfo --summary` in the chroot; `/usr/local/lib` is first on
/// `LD_LIBRARY_PATH` so libhybris's `libvulkan.so.1` shadows the standard
/// `vulkan-icd-loader` copy. Doesn't exercise swapchain/present — the
/// `test_vkcube_renders_via_ahb` test below does.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_vulkaninfo_loads_android_driver() {
    tawc_integration::helpers::test_init();
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

/// `eglinfo -B` in the chroot picks up libhybris's `libEGL.so` shim
/// (`android_dlopen` → bionic libEGL → vendor blob) and reports
/// `EGL_VENDOR=Android META-EGL` plus the device's GPU as the GLES
/// renderer. The `Android META-EGL` signature is the smoking gun that
/// our shim got loaded.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_eglinfo_loads_android_driver() {
    tawc_integration::helpers::test_init();
    require_compositor();

    let out = adb::rootfs_run_with(BACKEND, "eglinfo -B")
        .expect("failed to run eglinfo in chroot");

    // eglinfo's exit code tracks whether *every* platform initialized
    // — under libhybris it picks the single "Default display platform"
    // and exits 0, so check stdout regardless of status.
    let stdout = String::from_utf8_lossy(&out.stdout);

    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    // Distinctive Android EGL identifier; distro Mesa would say "Mesa
    // Project". This is also what tells us our libEGL.so shim was
    // loaded and that it dlopen'd the bionic libEGL behind it.
    assert!(
        stdout.contains("Android META-EGL"),
        "EGL vendor string is not Android — distro Mesa libEGL shadowed \
         our shim or libhybris failed to load the Android driver?\n\
         stdout:\n{stdout}"
    );
}

/// `weston-simple-egl` (canonical minimal EGL/GLES client) maps a window
/// via `wl_egl_window`, binds `android_wlegl`, allocates
/// `ANativeWindowBuffer`s through libhybris, and presents via AHB —
/// exactly the path the compositor's `wlegl` module imports.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
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

/// Vulkan client end-to-end: `vkcube` opens a Wayland surface via
/// `vulkanplatform_wayland.so`, the swapchain hands out
/// `ANativeWindowBuffer`s, and the compositor imports those as AHB
/// textures. Complements `test_vulkaninfo_loads_android_driver` (which
/// only inspects extension strings) by actually exercising present.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_vkcube_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    // `--c` caps the frame count; we still kill via stop() so the value
    // mostly just guards against the test runner hanging if stop() fails.
    let mut app = launch_and_wait_for_ahb(
        BACKEND,
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

/// GTK3 with the chroot's default `GDK_GL=gles:always` (set by
/// [`RootfsEnv`] for libhybris) renders through `android_wlegl` and
/// presents via AHB hardware buffers.
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_gtk3_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    assert_renders_via_ahb(
        BACKEND,
        "gtk3-demo-application",
        "gtk3-demo-application",
        GTK_LAUNCH_TIMEOUT,
    );
}

/// GTK4's default GSK renderer goes through `android_wlegl` on libhybris,
/// so launching `gtk4-widget-factory` must produce AHB imports. Catches
/// GTK4-specific regressions in the GL path.
///
/// (We use `gtk4-widget-factory` rather than `gtk4-demo-application`
/// because the latter pulls in a session-bus / GApplication setup that
/// errors out in this rootfs and never actually maps a window.)
#[test]
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_gtk4_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    assert_renders_via_ahb(
        BACKEND,
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );
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
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
fn test_firefox_renders_via_ahb() {
    tawc_integration::helpers::test_init();
    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog.
    let _ = adb::rootfs_run_with(
        BACKEND,
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups",
    );

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
#[cfg_attr(
    tawc_skip_libhybris_on_target,
    ignore = "libhybris skipped on x86 device"
)]
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
        .expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}
