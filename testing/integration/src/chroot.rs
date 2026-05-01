use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::adb;

/// Filesystem root of the in-app Arch chroot, as seen from outside.
/// Inside the chroot the same dir is just `/`.
const CHROOT_ROOTFS: &str = "/data/data/me.phie.tawc/distros/arch/rootfs";

/// Build the gtk4-debug-app in the chroot if the sources have changed
/// since the last successful build. Returns the in-chroot binary path.
///
/// Build deps (gtk4, pkg-config) are expected to already be installed —
/// run `testing/install-test-deps.sh` once per chroot install.
pub fn ensure_debug_app() -> io::Result<String> {
    ensure_chroot_app("gtk4-debug-app")
}

/// Build the TAWC-DRI Phase 1 round-trip test client in the chroot.
/// Returns the in-chroot binary path.
pub fn ensure_tawc_dri_test() -> io::Result<String> {
    ensure_chroot_app("tawc-dri-test")
}

/// Build the EGL-on-X11 driver test client in the chroot. Returns the
/// in-chroot binary path. Exercises the libhybris X11 EGL platform
/// plugin (eglplatform_x11.so) end-to-end.
pub fn ensure_eglx11_test() -> io::Result<String> {
    ensure_chroot_app("eglx11-test")
}

/// Build a small in-chroot test binary by name, if its sources have
/// changed since the last successful build. The source dir is
/// `testing/<name>/` (a sibling of the integration crate); the dir
/// must contain a `build.sh` that produces an executable at the same
/// path. Returns the in-chroot binary path.
fn ensure_chroot_app(name: &str) -> io::Result<String> {
    let chroot_build = format!("/tmp/{}", name);
    let fs_build_dir = format!("{}{}", CHROOT_ROOTFS, chroot_build);
    let binary_chroot = format!("{}/{}", chroot_build, name);
    let staging = format!("/data/local/tmp/{}-src", name);
    let source_dir: PathBuf = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join(name);

    let stamp = read_stamp(&chroot_build)?;
    let source_mtime = latest_mtime(&source_dir)?;
    let stamp_mtime: u64 = stamp.parse().unwrap_or(0);
    if stamp != "missing" && stamp_mtime == source_mtime {
        return Ok(binary_chroot);
    }

    adb::shell(&format!("rm -rf {}", staging))?;
    Command::new("adb")
        .args(["push", &source_dir.to_string_lossy(), &staging])
        .output()?;

    // Copy into chroot filesystem. su lets us reach the staging dir
    // (shell-uid-owned in /data/local/tmp) and the rootfs path
    // regardless of install method, but the resulting files land
    // owned by uid 0. For proot installs the in-chroot build runs as
    // the app uid (see [adb::chroot_run] dispatch), so chmod the tree
    // world-writable afterwards — otherwise `ld` can't replace the
    // build output. Harmless for chroot installs since the build runs
    // as root there.
    adb::shell(&format!(
        "su -c 'mkdir -p {dir} && cp {src}/* {dir} && chmod -R a+rwX {dir}'",
        dir = fs_build_dir,
        src = staging
    ))?;

    let output = adb::chroot_run(&format!("/bin/bash {}/build.sh", chroot_build))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "{name} build failed (missing test deps? \
                 try `bash testing/install-test-deps.sh`):\n\
                 stdout: {stdout}\nstderr: {stderr}",
            ),
        ));
    }

    adb::chroot_run(&format!(
        "echo {} > {}/build-stamp",
        source_mtime, chroot_build
    ))?;

    Ok(binary_chroot)
}

fn read_stamp(chroot_build: &str) -> io::Result<String> {
    let stamp_chroot = format!("{}/build-stamp", chroot_build);
    let cmd = format!(
        "printf 'STAMP:%s\\n' \"$(test -f {0} && cat {0} 2>/dev/null || echo missing)\"",
        stamp_chroot
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
