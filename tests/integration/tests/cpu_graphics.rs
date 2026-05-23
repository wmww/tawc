//! Tests for the CPU software-rendering backend. Every spawn pins
//! [`GraphicsBackend::Cpu`]: no libhybris on `LD_LIBRARY_PATH`, no
//! gfxstream env — the distro Mesa loads with `LIBGL_ALWAYS_SOFTWARE=1`
//! + `GALLIUM_DRIVER=llvmpipe`, the Vulkan loader picks up lavapipe if
//! `vulkan-swrast` is installed. Clients that have a GPU fast path
//! (real toolkits, EGL/Vulkan demos) fall back to `wl_shm` — those
//! tests live in the per-backend `libhybris::` / `gfxstream::` modules
//! instead, since exercising AHB is the whole point of *those*
//! backends.
//!
//! What lives here: the buffer paths that **stay the same shape**
//! regardless of GPU — `wl_shm` plumbing, GTK's software fallbacks
//! when explicitly forced, and the static `eglinfo` sanity that
//! confirms CPU mode actually picked a software renderer (not a stale
//! libhybris LD_LIBRARY_PATH).

use std::time::Duration;

use tawc_integration::helpers::{
    assert_client_animating, assert_compositor_clean, assert_renders_via_shm,
    has_shm_surface, require_compositor, saw_ahb_import, saw_shm_import, TIMEOUT,
};
use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

const WESTON_LAUNCH_TIMEOUT: Duration = Duration::from_secs(15);
const GTK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

/// `eglinfo -B` under CPU must report a software rasteriser
/// (`llvmpipe` / `softpipe` / `swrast`) — that's the whole point of
/// the backend. A failure here means the env didn't take effect (e.g.
/// libhybris's `LD_LIBRARY_PATH` leaked through) and we're silently
/// hitting a hardware path the user explicitly opted out of.
#[test]
fn test_eglinfo_reports_software_renderer() {
    tawc_integration::helpers::test_init();
    require_compositor();

    let out = adb::rootfs_run_with(BACKEND, "eglinfo -B")
        .expect("failed to run eglinfo in chroot");
    // GBM platform probe fails inside the chroot (no /dev/dri/cardN);
    // ignore exit status and inspect stdout.
    let stdout = String::from_utf8_lossy(&out.stdout);

    assert!(
        stdout.contains("OpenGL ES profile renderer:"),
        "no GLES renderer reported — driver init failed?\nstdout:\n{stdout}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(
        stdout.contains("llvmpipe")
            || stdout.contains("softpipe")
            || stdout.contains("swrast"),
        "CPU backend did not pick a software rasteriser. \
         LIBGL_ALWAYS_SOFTWARE / GALLIUM_DRIVER env didn't reach the \
         child? Or libhybris's LD_LIBRARY_PATH leaked through and a \
         vendor GLES blob loaded?\nstdout:\n{stdout}"
    );
    assert!(
        !stdout.contains("Android META-EGL"),
        "CPU backend loaded the libhybris/Android EGL shim — env \
         shouldn't have put libhybris on LD_LIBRARY_PATH.\nstdout:\n{stdout}"
    );
}

/// `weston-simple-shm` is the canonical minimal `wl_shm` client: a few
/// hundred lines that allocate a pool, attach a buffer, draw a moving
/// pattern. If this fails the SHM plumbing in the compositor is broken
/// regardless of what GTK/Firefox do.
#[test]
fn test_weston_simple_shm_renders_via_shm() {
    tawc_integration::helpers::test_init();
    require_compositor();
    adb::logcat_clear().expect("Failed to clear logcat");

    let mut app = RootfsProcess::spawn_with(BACKEND, "weston-simple-shm")
        .expect("spawn weston-simple-shm");
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + WESTON_LAUNCH_TIMEOUT;
    let mut saw_buffer = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("weston-simple-shm crashed/exited before rendering");
        }
        let logs = adb::logcat_dump_tawc().expect("Failed to dump logcat");
        if saw_shm_import(&logs) || has_shm_surface() {
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
    assert_eq!(
        state.surfaces_shm, 1,
        "weston-simple-shm should have one SHM-backed surface: {state:?}"
    );
    assert_eq!(
        state.surfaces_wlegl, 0,
        "weston-simple-shm should not attach AHB buffers: {state:?}"
    );

    assert_client_animating("weston-simple-shm", Duration::from_millis(1500), 10);

    app.stop()
        .expect("weston-simple-shm failed to stop cleanly");
    assert_compositor_clean();
}

/// GTK3 with `GDK_GL=disabled` falls back to its software renderer and
/// presents via `wl_shm`. Uses the stock `gtk3-demo-application` so
/// this exercises the SHM client path with a real, non-debug program.
/// Forcing the override under CPU is redundant (CPU has no GL anyway)
/// but the env override is itself worth regression-guarding.
#[test]
fn test_gtk3_renders_via_shm() {
    tawc_integration::helpers::test_init();
    assert_renders_via_shm(
        BACKEND,
        "GDK_GL=disabled gtk3-demo-application",
        "gtk3-demo-application",
        GTK_LAUNCH_TIMEOUT,
    );
}

/// GTK4 with `GSK_RENDERER=cairo` uses the software cairo renderer and
/// presents via `wl_shm`. Exercises the GTK4 SHM path, which is distinct
/// from GTK3's (GTK4 ping-pongs fresh wl_buffers per frame and caches
/// the first released cairo surface — a regression that double-releases
/// SHM buffers will crash GTK4 here while leaving GTK3 working).
#[test]
fn test_gtk4_renders_via_shm() {
    tawc_integration::helpers::test_init();
    assert_renders_via_shm(
        BACKEND,
        "GSK_RENDERER=cairo gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_LAUNCH_TIMEOUT,
    );
}
