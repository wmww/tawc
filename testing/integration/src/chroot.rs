use std::io;
use std::path::Path;
use std::process::Command;

use crate::adb;

const APP_NAME: &str = "gtk4-debug-app";
const BUILD_PKGS: &[&str] = &["gtk4", "pkg-config"];

fn chroot_build_dir() -> String {
    format!("/tmp/{}", APP_NAME)
}

fn chroot_fs_build_dir() -> String {
    format!("/data/local/arch-chroot/tmp/{}", APP_NAME)
}

fn host_staging() -> String {
    format!("/data/local/tmp/{}-src", APP_NAME)
}

fn binary_path() -> String {
    format!("{}/{}", chroot_build_dir(), APP_NAME)
}

/// Check deps and build freshness in a single adb call.
/// Returns (deps_ok, stamp_value).
fn check_status() -> io::Result<(bool, String)> {
    let build_dir = chroot_build_dir();
    let stamp_chroot = format!("{}/build-stamp", build_dir);
    let binary_chroot = binary_path();
    let pkgs = BUILD_PKGS.join(" ");
    // Use sentinel-framed output so unrelated warnings (e.g. from pacman) can't
    // be mistaken for the stamp value.
    let cmd = format!(
        "pacman -Q {} >/dev/null 2>&1 && echo DEPS_OK; \
         printf 'STAMP:%s\\n' \"$(test -f {} && cat {} 2>/dev/null || echo missing)\"",
        pkgs, binary_chroot, stamp_chroot
    );
    let output = adb::chroot_run(&cmd)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let deps_ok = stdout.lines().any(|l| l.trim() == "DEPS_OK");
    let stamp = stdout
        .lines()
        .find_map(|l| l.trim().strip_prefix("STAMP:"))
        .unwrap_or("missing")
        .trim()
        .to_string();
    Ok((deps_ok, stamp))
}

/// Ensure the named packages are installed in the chroot (pacman).
/// Idempotent — skips the install if pacman -Q already succeeds for all.
pub fn ensure_pkgs(pkgs: &[&str]) -> io::Result<()> {
    let joined = pkgs.join(" ");
    let check = adb::chroot_run(&format!("pacman -Q {} >/dev/null 2>&1 && echo OK", joined))?;
    if String::from_utf8_lossy(&check.stdout).contains("OK") {
        return Ok(());
    }
    eprintln!("Installing chroot packages: {}", joined);
    let output = adb::chroot_run(&format!("pacman -Sy --noconfirm {}", joined))?;
    if !output.status.success() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "Failed to install packages {}:\n{}",
                joined,
                String::from_utf8_lossy(&output.stdout)
            ),
        ));
    }
    Ok(())
}

/// Ensure deps are installed and the gtk4-debug-app is built.
/// Returns the path to the binary inside the chroot.
/// Skips work that's already done.
pub fn ensure_debug_app() -> io::Result<String> {
    let binary_chroot = binary_path();
    let source_dir = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join(APP_NAME);

    let (deps_ok, stamp) = check_status()?;

    if !deps_ok {
        ensure_pkgs(BUILD_PKGS)?;
    }

    // Check if build is fresh
    let source_mtime = latest_mtime(&source_dir)?;
    let stamp_mtime: u64 = stamp.parse().unwrap_or(0);
    if stamp != "missing" && stamp_mtime == source_mtime {
        return Ok(binary_chroot);
    }

    let staging = host_staging();
    let fs_build_dir = chroot_fs_build_dir();

    // Push source files to staging area
    adb::shell(&format!("rm -rf {}", staging))?;
    Command::new("adb")
        .args(["push", &source_dir.to_string_lossy(), &staging])
        .output()?;

    // Copy into chroot filesystem
    adb::shell(&format!(
        "su -c 'mkdir -p {} && cp {}/* {}'",
        fs_build_dir, staging, fs_build_dir
    ))?;

    // Build
    let output = adb::chroot_run(&format!("/bin/bash {}/build.sh", chroot_build_dir()))?;
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
        source_mtime,
        chroot_build_dir()
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
