//! Staging + spawn helpers for the tawcroot **prod-env** device tests
//! (`tests/tawcroot_prodenv.rs`).
//!
//! Those tests run production `libtawcroot.so` in the real production
//! sandbox — app uid, `untrusted_app` SELinux domain, the
//! zygote-installed seccomp filter — by exec'ing it through the exec
//! broker's ARGV form, exactly the way production launches work. No
//! test binary is pushed to the device: `libtawcroot.so` is already in
//! the APK's `nativeLibraryDir` (the one app-readable location
//! `untrusted_app` may execve), and the guest programs + fake rootfs
//! are pure data that tawcroot's manual ELF loader mmaps.
//!
//! The fake rootfs is staged into app-private cache
//! (`/data/data/me.phie.tawc/cache/tawcroot-prodtest/`) through the
//! broker (`sh -c 'cat > …'` per file, app-owned), so uninstall — or
//! Android clearing the cache — removes it. Staging re-runs from
//! scratch once per test-binary invocation.
//!
//! Guest programs are the NDK cross-builds from
//! `build/tawcroot-<abi>/programs/`, produced by
//! `tawcroot/build-fixtures.sh` (run-integration-tests.sh builds them
//! via `ensure_tawcroot_device_tests`).

use std::io;
use std::path::PathBuf;
use std::process::Output;
use std::sync::OnceLock;

use crate::exec_broker::{self, Invocation, Request};

/// Device-side staging root. App cache: app-owned, uninstall-cleaned.
const STAGE_DIR: &str = "/data/data/me.phie.tawc/cache/tawcroot-prodtest";

/// Guest programs staged into the fake rootfs at `/bin/<name>`.
/// All are freestanding static fixtures except `dynamic_exit42`,
/// which links bionic and exercises the PT_INTERP loader path against
/// the device's real `/system` + `/apex` (bound by the test).
const STAGED_PROGRAMS: &[&str] = &[
    "static_exit42",
    "static_argc_random",
    "static_execve_exit42",
    "static_drop_ids_execve",
    "static_check_ids_exit42",
    "static_open_creat_argv1",
    "static_open_rdonly_argv1",
    "static_unix_bind_argv1",
    "static_check_proc_self_fd",
    "static_io_uring_deny",
    "static_drop_ids_devnull_eperm",
    "static_link_publish_argv12",
    "static_mknod_chr_fake",
    "dynamic_exit42",
];

pub struct ProdEnv {
    /// Absolute device path of production tawcroot: `<nativeLibraryDir>/libtawcroot.so`.
    pub tawcroot: String,
    /// Absolute device path of the staged fake rootfs.
    pub rootfs: String,
}

/// Resolve `libtawcroot.so` and stage the fake rootfs. Once per test
/// binary; panics loudly on any setup failure (missing fixtures on the
/// host, broker errors) since every prod-env test depends on it.
pub fn env() -> &'static ProdEnv {
    static ENV: OnceLock<ProdEnv> = OnceLock::new();
    ENV.get_or_init(|| {
        let lib_dir = crate::adb::native_lib_dir().expect("broker app-info nativeLibraryDir");
        let tawcroot = format!("{lib_dir}/libtawcroot.so");
        let rootfs = stage_rootfs().expect("stage tawcroot prod-env fake rootfs");
        ProdEnv { tawcroot, rootfs }
    })
}

/// Run production tawcroot in the production sandbox:
/// `libtawcroot.so -r <staged-rootfs> [binds...] -- <guest argv...>`,
/// spawned via the broker's ARGV form (fork from the app process, so
/// the child inherits uid, `untrusted_app`, and the real zygote
/// seccomp filter). `env` becomes the child's whole environment (the
/// broker replaces, not merges) and is what the guest sees as envp.
pub fn run_guest(binds: &[&str], guest_argv: &[&str], env: &[&str]) -> io::Result<Output> {
    let e = self::env();
    let mut argv: Vec<String> = vec![
        e.tawcroot.clone(),
        "-r".to_string(),
        e.rootfs.clone(),
    ];
    for b in binds {
        argv.push("-b".to_string());
        argv.push((*b).to_string());
    }
    argv.push("--".to_string());
    argv.extend(guest_argv.iter().map(|s| (*s).to_string()));
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::Exec {
            argv,
            env: env.iter().map(|s| (*s).to_string()).collect(),
            cwd: None,
            op_title: None,
        },
    })
}

/// Assert `output` exited with `expected`, panicking with the full
/// broker stdio otherwise (tawcroot prints its diagnostics on stderr).
pub fn assert_guest_exit(name: &str, output: &Output, expected: i32) {
    assert_eq!(
        output.status.code(),
        Some(expected),
        "{name}: expected exit {expected}, got {:?}\n----- stdout -----\n{}\n----- stderr -----\n{}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr),
    );
}

/// Run `sh -c <cmd>` as the app uid, panicking on non-zero exit.
pub fn app_sh(cmd: &str) -> Output {
    let output = app_sh_raw(cmd).unwrap_or_else(|e| panic!("broker sh -c {cmd:?}: {e}"));
    assert!(
        output.status.success(),
        "sh -c {cmd:?} failed: {:?} stderr={}",
        output.status,
        String::from_utf8_lossy(&output.stderr),
    );
    output
}

/// Run `sh -c <cmd>` as the app uid, returning the raw result (for
/// probes where non-zero is an answer, not an error).
pub fn app_sh_raw(cmd: &str) -> io::Result<Output> {
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::Exec {
            argv: vec![
                "/system/bin/sh".to_string(),
                "-c".to_string(),
                cmd.to_string(),
            ],
            env: Vec::new(),
            cwd: None,
            op_title: None,
        },
    })
}

/// The tawcroot fixture ABI for the connected device, from the app
/// process's `uname -m` (the broker child is the app's arch, which is
/// what `libtawcroot.so` was packaged for).
fn device_abi() -> io::Result<String> {
    let output = app_sh_raw("uname -m")?;
    let arch = String::from_utf8_lossy(&output.stdout).trim().to_string();
    match arch.as_str() {
        "aarch64" | "x86_64" => Ok(arch),
        other => Err(io::Error::other(format!(
            "unsupported device arch {other:?} for tawcroot fixtures"
        ))),
    }
}

fn host_programs_dir(abi: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join(format!("build/tawcroot-{abi}/programs"))
}

/// Deliver `bytes` to `device_path` through the broker (app-owned).
fn stage_file(device_path: &str, bytes: &[u8], mode: &str) -> io::Result<()> {
    let cmd = format!("cat > '{device_path}' && chmod {mode} '{device_path}'");
    let output = exec_broker::run_capture_with_input(
        Invocation {
            foreground_app: false,
            request: Request::Exec {
                argv: vec![
                    "/system/bin/sh".to_string(),
                    "-c".to_string(),
                    cmd,
                ],
                env: Vec::new(),
                cwd: None,
                op_title: None,
            },
        },
        bytes,
    )?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "staging {device_path} failed: {:?} stderr={}",
            output.status,
            String::from_utf8_lossy(&output.stderr),
        )));
    }
    Ok(())
}

/// Build the fake rootfs on-device. Tree shape mirrors the cleat
/// suites' `rootfs_helpers` fixture: guest programs under `/bin`,
/// `/etc/probe` with known content, an empty `/run` for socket tests.
fn stage_rootfs() -> io::Result<String> {
    let abi = device_abi()?;
    let programs = host_programs_dir(&abi);
    if !programs.is_dir() {
        return Err(io::Error::other(format!(
            "missing {} — run scripts/run-integration-tests.sh without --no-build \
             (or tawcroot/build-fixtures.sh {abi})",
            programs.display()
        )));
    }

    let rootfs = format!("{STAGE_DIR}/rootfs");
    let output = app_sh_raw(&format!(
        "rm -rf '{STAGE_DIR}' && mkdir -p '{rootfs}/bin' '{rootfs}/etc' '{rootfs}/run'"
    ))?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "creating {rootfs} failed: {:?} stderr={}",
            output.status,
            String::from_utf8_lossy(&output.stderr),
        )));
    }

    for name in STAGED_PROGRAMS {
        let host_path = programs.join(name);
        let bytes = std::fs::read(&host_path).map_err(|e| {
            io::Error::other(format!(
                "read fixture {} failed ({e}) — stale build/tawcroot-{abi}? \
                 re-run tawcroot/build-fixtures.sh {abi}",
                host_path.display()
            ))
        })?;
        stage_file(&format!("{rootfs}/bin/{name}"), &bytes, "755")?;
    }
    stage_file(&format!("{rootfs}/etc/probe"), b"from-rootfs\n", "644")?;
    Ok(rootfs)
}
