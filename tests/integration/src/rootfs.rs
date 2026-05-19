use std::io;

use crate::adb;

/// Verify a test app binary copied by `scripts/install-test-deps.sh`
/// exists in the rootfs and return its path. The binary is cross-built
/// on the host at install-test-deps time — tests never compile on the
/// device. If you edited the source under `tests/apps/<name>/`, re-run
/// install-test-deps to pick up the change.
fn check_rootfs_app(name: &str) -> io::Result<String> {
    let binary_rootfs = format!("/usr/local/bin/{name}");
    let probe = format!(
        "test -x {bin} && echo OK || echo MISSING",
        bin = binary_rootfs
    );
    let output = adb::rootfs_run(&probe)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    if stdout.lines().any(|l| l.trim() == "OK") {
        return Ok(binary_rootfs);
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!(
            "{name} not found at {binary_rootfs}. Run \
             `scripts/install-test-deps.sh` (re-run after editing \
             tests/apps/{name}/* to pick up the new source)."
        ),
    ))
}

/// `gtk4-debug-app` — the GTK4 driver used by `apps::` and `input::` tests.
pub fn ensure_debug_app() -> io::Result<String> {
    check_rootfs_app("gtk4-debug-app")
}

/// `wayland-debug-app` — toolkitless Wayland protocol test driver.
pub fn ensure_wayland_debug_app() -> io::Result<String> {
    let bin = check_rootfs_app("wayland-debug-app")?;
    let probe = format!("grep -a -q 'Fullscreen touch visualizer' {bin} && echo OK || echo STALE");
    let output = adb::rootfs_run(&probe)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    if stdout.lines().any(|l| l.trim() == "OK") {
        return Ok(bin);
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!(
            "{bin} is stale and does not include the `touch` mode. Run \
             `scripts/install-test-deps.sh` (re-run after editing \
             tests/apps/wayland-debug-app/* to pick up the new source)."
        ),
    ))
}

/// `tawc-dri-test` — TAWC-DRI Phase 1 round-trip client.
pub fn ensure_tawc_dri_test() -> io::Result<String> {
    check_rootfs_app("tawc-dri-test")
}

/// `eglx11-test` — EGL-on-X11 driver test exercising the libhybris
/// X11 EGL platform plugin (eglplatform_x11.so) end-to-end.
pub fn ensure_eglx11_test() -> io::Result<String> {
    check_rootfs_app("eglx11-test")
}

/// `libhybris-tls-repro` — regression test for the libhybris bionic
/// linker's TLS-module unregister assertion (`linker_tls.cpp:93`).
/// Returns the absolute path to the `libhybris-tls-repro` binary
/// inside the rootfs. The NDK-cross-built bionic companions live under
/// `/usr/local/lib/`.
pub fn ensure_libhybris_tls_repro() -> io::Result<String> {
    let bin = check_rootfs_app("libhybris-tls-repro")?;
    // Also confirm the bionic-side .so landed — install-test-deps cross-
    // builds it via NDK; if missing, the binary check above passes but
    // the test would fail with a confusing dlopen error.
    let so_dir = "/usr/local/lib";
    let so = format!("{so_dir}/tls_lib.so");
    let weak = format!("{so_dir}/weak_lib.so");
    let probe = format!("test -f {so} -a -f {weak} && echo OK || echo MISSING");
    let output = adb::rootfs_run(&probe)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    if !stdout.lines().any(|l| l.trim() == "OK") {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "{so} or {weak} not found. Run `scripts/install-test-deps.sh` \
                 (which cross-builds the bionic-ABI tls_lib.so via the \
                 Android NDK on the host)."
            ),
        ));
    }
    Ok(bin)
}
