use std::io;
use std::path::Path;
use std::process::Command;

use crate::adb;

const CHROOT_BUILD_DIR: &str = "/tmp/gtk3-debug-app";
const HOST_STAGING: &str = "/data/local/tmp/gtk3-debug-app-src";
const CHROOT_FS_BUILD_DIR: &str = "/data/local/arch-chroot/tmp/gtk3-debug-app";

/// Check deps and build freshness in a single adb call.
/// Returns (deps_ok, stamp_value).
fn check_status() -> io::Result<(bool, String)> {
    // pacman runs inside the chroot; the stamp/binary live in the chroot fs
    // which is also visible from inside, so one chroot_run covers both.
    let stamp_chroot = format!("{}/build-stamp", CHROOT_BUILD_DIR);
    let binary_chroot = format!("{}/gtk3-debug-app", CHROOT_BUILD_DIR);
    let cmd = format!(
        "pacman -Q gtk3 pkg-config >/dev/null 2>&1 && echo DEPS_OK; \
         test -f {} && cat {} 2>/dev/null || echo missing",
        binary_chroot, stamp_chroot
    );
    let output = adb::chroot_run(&cmd)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let deps_ok = stdout.contains("DEPS_OK");
    let stamp = stdout.lines()
        .find(|l| !l.contains("DEPS_OK") && !l.is_empty())
        .unwrap_or("missing")
        .trim()
        .to_string();
    Ok((deps_ok, stamp))
}

/// Ensure GTK3 and build tools are installed in the chroot.
pub fn ensure_build_deps() -> io::Result<()> {
    // Called after check_status confirmed deps are missing
    eprintln!("Installing build deps in chroot...");
    let output = adb::chroot_run("pacman -Sy --noconfirm gtk3 pkg-config")?;
    if !output.status.success() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "Failed to install build deps:\n{}",
                String::from_utf8_lossy(&output.stdout)
            ),
        ));
    }
    Ok(())
}

/// Ensure deps are installed and debug app is built.
/// Returns the path to the binary inside the chroot.
/// Skips work that's already done.
pub fn ensure_debug_app() -> io::Result<String> {
    let binary_chroot = format!("{}/gtk3-debug-app", CHROOT_BUILD_DIR);
    let source_dir = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("gtk3-debug-app");

    let (deps_ok, stamp) = check_status()?;

    if !deps_ok {
        ensure_build_deps()?;
    }

    // Check if build is fresh
    let source_mtime = latest_mtime(&source_dir)?;
    let stamp_mtime: u64 = stamp.parse().unwrap_or(0);
    if stamp != "missing" && stamp_mtime == source_mtime {
        return Ok(binary_chroot);
    }

    // Push source files to staging area
    adb::shell(&format!("rm -rf {}", HOST_STAGING))?;
    Command::new("adb")
        .args(["push", &source_dir.to_string_lossy(), HOST_STAGING])
        .output()?;

    // Copy into chroot filesystem
    adb::shell(&format!(
        "su -c 'mkdir -p {} && cp {}/* {}'",
        CHROOT_FS_BUILD_DIR, HOST_STAGING, CHROOT_FS_BUILD_DIR
    ))?;

    // Build
    let output = adb::chroot_run(&format!("/bin/bash {}/build.sh", CHROOT_BUILD_DIR))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("Build failed:\nstdout: {}\nstderr: {}", stdout, stderr),
        ));
    }

    // Write stamp with latest source mtime so we can skip next time
    adb::chroot_run(&format!(
        "echo {} > {}/build-stamp",
        source_mtime, CHROOT_BUILD_DIR
    ))?;

    Ok(binary_chroot)
}

/// Get the latest modification time (as unix seconds) of any file in the directory.
fn latest_mtime(dir: &Path) -> io::Result<u64> {
    let mut latest = 0u64;
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        if let Ok(meta) = entry.metadata() {
            if let Ok(mtime) = meta.modified() {
                let secs = mtime
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs();
                latest = latest.max(secs);
            }
        }
    }
    Ok(latest)
}
