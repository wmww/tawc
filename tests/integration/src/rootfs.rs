use std::io;

use crate::adb;

/// Verify a test app binary built by `scripts/install-test-deps.sh`
/// exists in the rootfs and return its path. The build itself runs at
/// install-test-deps time — tests no longer compile anything. If you
/// edited the source under `tests/apps/<name>/`, re-run install-test-deps
/// to pick up the change.
fn check_rootfs_app(name: &str) -> io::Result<String> {
    let binary_rootfs = format!("/tmp/{name}/{name}");
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
             `bash scripts/install-test-deps.sh` (re-run after editing \
             tests/apps/{name}/* to pick up the new source)."
        ),
    ))
}

/// `gtk4-debug-app` — the GTK4 driver used by `apps::` and `input::` tests.
pub fn ensure_debug_app() -> io::Result<String> {
    check_rootfs_app("gtk4-debug-app")
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
