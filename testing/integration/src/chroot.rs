use std::io;
use std::path::Path;
use std::process::Command;

use crate::adb;

const APP_NAME: &str = "gtk4-debug-app";

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

/// Read the build stamp value (mtime of the source tree at last build),
/// or "missing" if no successful build is on the device yet.
fn read_stamp() -> io::Result<String> {
    let stamp_chroot = format!("{}/build-stamp", chroot_build_dir());
    // Sentinel-framed so unrelated warnings can't be mistaken for the value.
    let cmd = format!(
        "printf 'STAMP:%s\\n' \"$(test -f {} && cat {} 2>/dev/null || echo missing)\"",
        stamp_chroot, stamp_chroot
    );
    let output = adb::chroot_run(&cmd)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(stdout
        .lines()
        .find_map(|l| l.trim().strip_prefix("STAMP:"))
        .unwrap_or("missing")
        .trim()
        .to_string())
}

/// Build the gtk4-debug-app in the chroot if the sources have changed
/// since the last successful build. Returns the in-chroot binary path.
///
/// Build deps (gtk4, pkg-config) are expected to already be installed —
/// run `testing/install-test-deps.sh` once per chroot install.
pub fn ensure_debug_app() -> io::Result<String> {
    let binary_chroot = binary_path();
    let source_dir = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join(APP_NAME);

    let stamp = read_stamp()?;

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
            format!(
                "gtk4-debug-app build failed (missing test deps? \
                 try `bash testing/install-test-deps.sh`):\n\
                 stdout: {}\nstderr: {}",
                stdout, stderr
            ),
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
