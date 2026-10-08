//! The broker's RUNINSIDE path (`rootfs-run.sh`, `adb::rootfs_run`).

use tawc_integration::adb;

/// The caller waits for the guest: late output arrives and the guest's
/// exit status comes back. Toybox `setsid` on Android 11/12 forks and
/// exits 0 at once, which lost both (old spawn shape; the reproducer
/// needs an API 30-32 target).
#[test]
fn test_rootfs_run_waits_for_guest() {
    tawc_integration::helpers::test_init();
    let out = adb::rootfs_run("sleep 1; echo late; exit 7").expect("rootfs_run");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.code() == Some(7) && stdout.contains("late"),
        "expected exit 7 + \"late\", got {:?}\nstdout:\n{stdout}\nstderr:\n{}",
        out.status.code(),
        String::from_utf8_lossy(&out.stderr),
    );
}
