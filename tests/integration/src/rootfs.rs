use std::io;
use std::sync::OnceLock;

use crate::adb;

/// Memoize an `ensure_*` probe result for the life of the test binary.
/// The probes only verify that `scripts/run-integration-tests.sh`
/// deployed a binary into the rootfs — that can't change mid-run, and
/// each probe costs a full chroot entry (`RUNINSIDE`), so re-running it
/// per test is pure overhead. Mirrors `helpers::ensure_wayland_debug_app`.
fn memoized(cell: &OnceLock<io::Result<String>>, probe: impl FnOnce() -> io::Result<String>) -> io::Result<String> {
    cell.get_or_init(probe)
        .as_ref()
        .map(String::clone)
        .map_err(|e| io::Error::new(e.kind(), e.to_string()))
}

/// Verify a test app binary deployed by `scripts/run-integration-tests.sh`
/// exists in the rootfs and return its path. The binary is cross-built
/// on the host before the Rust harness starts; tests never compile on
/// the device.
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
             `scripts/run-integration-tests.sh` so test apps are built \
             and deployed before cargo starts."
        ),
    ))
}

/// `wayland-debug-app` — toolkitless Wayland protocol test driver.
pub fn ensure_wayland_debug_app() -> io::Result<String> {
    let bin = check_rootfs_app("wayland-debug-app")?;
    let probe = format!("grep -a -q 'small-popup' {bin} && echo OK || echo STALE");
    let output = adb::rootfs_run(&probe)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    if stdout.lines().any(|l| l.trim() == "OK") {
        return Ok(bin);
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!(
            "{bin} is stale and does not include the newest scene modes. Run \
             `scripts/run-integration-tests.sh` so the changed test app \
             is rebuilt and deployed."
        ),
    ))
}

/// `tawc-dri-test` — TAWC-DRI Phase 1 round-trip client.
pub fn ensure_tawc_dri_test() -> io::Result<String> {
    static BIN: OnceLock<io::Result<String>> = OnceLock::new();
    memoized(&BIN, || check_rootfs_app("tawc-dri-test"))
}

/// `x11-debug-app` — Xlib protocol test driver.
pub fn ensure_x11_debug_app() -> io::Result<String> {
    static BIN: OnceLock<io::Result<String>> = OnceLock::new();
    memoized(&BIN, || check_rootfs_app("x11-debug-app"))
}

/// `eglx11-test` — EGL-on-X11 driver test exercising the libhybris
/// X11 EGL platform plugin (eglplatform_x11.so) end-to-end.
pub fn ensure_eglx11_test() -> io::Result<String> {
    static BIN: OnceLock<io::Result<String>> = OnceLock::new();
    memoized(&BIN, || check_rootfs_app("eglx11-test"))
}

/// `libhybris-tls-repro` — regression test for the libhybris bionic
/// linker's TLS-module unregister assertion (`linker_tls.cpp:93`).
/// Returns the absolute path to the `libhybris-tls-repro` binary
/// inside the rootfs. The NDK-cross-built bionic companions live under
/// [`LIBHYBRIS_TLS_REPRO_LIB_DIR`].
pub fn ensure_libhybris_tls_repro() -> io::Result<String> {
    static BIN: OnceLock<io::Result<String>> = OnceLock::new();
    memoized(&BIN, ensure_libhybris_tls_repro_uncached)
}

/// Guest dir holding the repro's bionic-side .so files. Must be a
/// /data-prefixed guest path: the libhybris bionic linker applies the
/// device linker config's namespace isolation, which permits dlopen
/// from /data but rejects rootfs paths like /usr/local/lib. Kept in
/// sync with `copy_test_app` in `scripts/run-integration-tests.sh`.
pub const LIBHYBRIS_TLS_REPRO_LIB_DIR: &str = "/data/tawc-tests";

fn ensure_libhybris_tls_repro_uncached() -> io::Result<String> {
    let bin = check_rootfs_app("libhybris-tls-repro")?;
    // Also confirm the bionic-side .so landed — the integration runner
    // cross-builds it via NDK; if missing, the binary check above passes but
    // the test would fail with a confusing dlopen error.
    let so_dir = LIBHYBRIS_TLS_REPRO_LIB_DIR;
    let so = format!("{so_dir}/tls_lib.so");
    let weak = format!("{so_dir}/weak_lib.so");
    let probe = format!("test -f {so} -a -f {weak} && echo OK || echo MISSING");
    let output = adb::rootfs_run(&probe)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    if !stdout.lines().any(|l| l.trim() == "OK") {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "{so} or {weak} not found. Run `scripts/run-integration-tests.sh` \
                 (which cross-builds the bionic-ABI tls_lib.so via the \
                 Android NDK on the host)."
            ),
        ));
    }
    Ok(bin)
}
