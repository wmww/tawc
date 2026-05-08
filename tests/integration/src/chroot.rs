use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::adb;

/// Optional override for which install runs `build.sh`. Falls back to
/// `TAWC_INSTALL_ID` (the same install we run tests in) when unset.
/// Useful when the test target can't compile — e.g. a tawcroot install
/// where gcc currently fails (cc1 dispatch is unimplemented). Set to a
/// chroot/proot install id on the same device that has gcc + the build
/// deps installed; the build output is copied across into the test
/// install's build dir before the run.
fn build_install_id() -> String {
    std::env::var("TAWC_BUILD_INSTALL_ID").unwrap_or_else(|_| crate::install_id())
}

fn rootfs_for(install_id: &str) -> String {
    format!("/data/data/me.phie.tawc/distros/{}/rootfs", install_id)
}

/// Build the gtk4-debug-app in the chroot if the sources have changed
/// since the last successful build. Returns the in-chroot binary path.
///
/// Build deps (gtk4, pkg-config) are expected to already be installed —
/// run `scripts/install-test-deps.sh` once per chroot install.
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
/// `tests/apps/<name>/` (a sibling of the integration crate); the dir
/// must contain a `build.sh` that produces an executable at the same
/// path. Returns the in-chroot binary path.
fn ensure_chroot_app(name: &str) -> io::Result<String> {
    let chroot_build = format!("/tmp/{}", name);
    let test_install = crate::install_id();
    let build_install = build_install_id();
    let test_fs_build_dir = format!("{}{}", rootfs_for(&test_install), chroot_build);
    let build_fs_build_dir = format!("{}{}", rootfs_for(&build_install), chroot_build);
    let binary_chroot = format!("{}/{}", chroot_build, name);
    let staging = format!("{}/{}-src", crate::TAWC_SCRATCH, name);
    let source_dir: PathBuf = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("apps")
        .join(name);

    let stamp = read_stamp(&chroot_build)?;
    let source_mtime = latest_mtime(&source_dir)?;
    let stamp_mtime: u64 = stamp.parse().unwrap_or(0);
    if stamp != "missing" && stamp_mtime == source_mtime {
        return Ok(binary_chroot);
    }

    adb::shell(&format!("mkdir -p {} && rm -rf {}", crate::TAWC_SCRATCH, staging))?;
    Command::new("adb")
        .args(["push", &source_dir.to_string_lossy(), &staging])
        .output()?;

    // Copy sources into the build install's rootfs via the broker
    // (runs as the app uid, which owns the rootfs tree — no su, no
    // ownership-flip, no chmod dance). The chmod -R a+rwX is left
    // belt-and-suspenders so it keeps working if the broker is later
    // bypassed for any reason.
    adb::chroot_host_exec(&[
        "/system/bin/sh", "-c",
        &format!(
            "mkdir -p {dir} && cp {src}/* {dir} && chmod -R a+rwX {dir}",
            dir = build_fs_build_dir,
            src = staging
        ),
    ])?;

    // Build inside the build install. When TAWC_BUILD_INSTALL_ID
    // overrides the test install, run inside the build install via
    // `scripts/tawc-chroot-run.sh` rather than reusing
    // `adb::chroot_run` (which is bound to the test install's id).
    let output = if build_install == test_install {
        adb::chroot_run(&format!("/bin/bash {}/build.sh", chroot_build))?
    } else {
        let script = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("scripts")
            .join("tawc-chroot-run.sh");
        Command::new("bash")
            .arg(&script)
            .arg(format!("/bin/bash {}/build.sh", chroot_build))
            .env("TAWC_INSTALL_ID", &build_install)
            .output()?
    };
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "{name} build failed in install '{build_install}' (missing test deps? \
                 try `bash scripts/install-test-deps.sh`):\n\
                 stdout: {stdout}\nstderr: {stderr}",
            ),
        ));
    }

    // If we built in a different install than we'll run in, copy the
    // binary across. Both rootfs's are the same arch (all installs on
    // a given device share `host_arch`), so the binary is portable.
    if build_install != test_install {
        adb::chroot_host_exec(&[
            "/system/bin/sh", "-c",
            &format!(
                "mkdir -p {test_dir} && cp {build_dir}/{name} {test_dir}/{name} && chmod a+rx {test_dir}/{name}",
                test_dir = test_fs_build_dir,
                build_dir = build_fs_build_dir,
                name = name,
            ),
        ])?;
    }

    // Stamp lives in the test install (the binary alongside it is
    // what we actually run).
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
