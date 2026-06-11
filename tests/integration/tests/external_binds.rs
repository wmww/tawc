//! End-to-end coverage for external-storage binds
//! (notes/external-binds.md): a fresh tawcroot install seeded with the
//! default binds, shared-storage round-trips in both directions, a
//! metadata edit taking effect on the next spawn, fail-closed launch on
//! a revoked all-files grant, and bind-target contents surviving
//! uninstall.
//!
//! One `#[test]` because the steps are a strict lifecycle over one
//! disposable install (`extbinds`) — and because the install gate
//! serializes jobs anyway. The install rides the dev cache proxy
//! (mandatory for install paths — see CLAUDE.md); the test refuses to
//! run when the proxy is down rather than hammering upstream mirrors.
//!
//! UI-level editing (ManageBindsActivity) isn't driven here; the
//! metadata-edit step covers the same contract (the persisted list
//! drives the next spawn) one layer down.

use std::net::TcpStream;
use std::process::Output;
use std::time::Duration;

use tawc_integration::adb;
use tawc_integration::exec_broker::{self, Invocation, Request};

const PKG: &str = "me.phie.tawc";
const TEST_ID: &str = "extbinds";
const PROXY: &str = "http://127.0.0.1:8080/proxy/";
/// Deliberate exception to the "non-app-private test scratch only under
/// /data/local/tmp/tawc-dev/" rule (AGENTS.md): shared storage IS the
/// feature under test, and the app uid can't reach /data/local/tmp.
/// Created and removed by this test.
const SHARED_DIR: &str = "/storage/emulated/0/tawc-bind-test";

fn metadata_path() -> String {
    format!("/data/data/{PKG}/distros/{TEST_ID}/metadata.json")
}

fn combined(out: &Output) -> String {
    format!(
        "{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    )
}

/// Refuse to run without the dev cache proxy (CLAUDE.md: never start it
/// from automation; ask the user).
fn ensure_cache_proxy() {
    let addr = "127.0.0.1:8080".parse().unwrap();
    if TcpStream::connect_timeout(&addr, Duration::from_secs(2)).is_err() {
        panic!(
            "dev cache proxy is not reachable at 127.0.0.1:8080 — \
             ask the user to run `scripts/cache-proxy.sh run`, then re-run this test"
        );
    }
}

/// Flip the MANAGE_EXTERNAL_STORAGE appop (what
/// `Environment.isExternalStorageManager()` reads) for the app uid.
fn set_all_files_access(mode: &str) {
    let out = adb::shell(&format!(
        "appops set --uid {PKG} MANAGE_EXTERNAL_STORAGE {mode}"
    ))
    .expect("adb shell appops");
    assert!(out.status.success(), "appops set failed: {}", combined(&out));
}

fn action(name: &str, args: &[(&str, &str)]) -> Output {
    exec_broker::run_capture(Invocation {
        // Install/uninstall actions open the in-app log screen, which
        // needs the foreground-app BAL allowance.
        foreground_app: true,
        request: Request::Action {
            name: name.to_string(),
            args: args
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect(),
        },
    })
    .expect("broker action")
}

/// Run a command inside the disposable `extbinds` install (not the
/// suite's standing install, which `adb::rootfs_run` targets).
fn run_inside(cmd: &str) -> Output {
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::RunInside {
            install_id: TEST_ID.to_string(),
            cmd: cmd.to_string(),
            op_title: None,
            graphics: None,
        },
    })
    .expect("broker run-inside")
}

fn run_inside_ok(cmd: &str) -> String {
    let out = run_inside(cmd);
    assert!(
        out.status.success(),
        "in-rootfs command failed: `{cmd}`\n{}",
        combined(&out)
    );
    String::from_utf8_lossy(&out.stdout).to_string()
}

/// Host-side (app uid) shell via the broker's EXEC form.
fn host_sh(script: &str) -> Output {
    adb::rootfs_host_exec(&["/system/bin/sh", "-c", script]).expect("broker host exec")
}

#[test]
fn test_external_binds_lifecycle() {
    ensure_cache_proxy();

    // Pre-clean: a previous aborted run may have left the install or
    // the shared-storage scratch dir behind. Uninstall of a missing
    // slot is a no-op.
    set_all_files_access("allow");
    action("uninstall", &[("id", TEST_ID)]);
    adb::shell(&format!("rm -rf {SHARED_DIR}")).unwrap();

    // --- Structurally invalid externalBinds fast-reject, before any
    // disk state exists. Distinct id: a rejected install leaves a
    // transient FAILED op in the registry for ~2s
    // (InstallationService.TRANSIENT_REJECT_HOLD_MS), and a same-id
    // install started inside that window would mirror the stale
    // transient instead of its own op.
    let reject_id = "extbinds-rej";
    let out = action(
        "install",
        &[
            ("id", reject_id),
            ("mirrorProxy", PROXY),
            ("externalBinds", r#"[{"hostPath":"relative","guestPath":"/x"}]"#),
        ],
    );
    assert!(
        !out.status.success(),
        "install with invalid binds unexpectedly succeeded:\n{}",
        combined(&out)
    );
    assert!(
        combined(&out).contains("invalid externalBinds"),
        "missing reject reason:\n{}",
        combined(&out)
    );
    let probe = host_sh(&format!(
        "test -d /data/data/{PKG}/distros/{reject_id} && echo EXISTS || echo ABSENT"
    ));
    assert!(
        String::from_utf8_lossy(&probe.stdout).contains("ABSENT"),
        "rejected install left a slot dir behind"
    );

    // --- Fresh install with no externalBinds arg: the service seeds
    // the default /android + /home/android binds (all-files access is
    // granted above).
    let out = action("install", &[("id", TEST_ID), ("mirrorProxy", PROXY)]);
    assert!(out.status.success(), "install failed:\n{}", combined(&out));
    // Android's JSONStringer escapes `/` as `\/`; normalise before the
    // literal-path asserts below.
    let meta = String::from_utf8_lossy(&host_sh(&format!("cat {}", metadata_path())).stdout)
        .replace("\\/", "/");
    assert!(
        meta.contains("\"guestPath\": \"/home/android\"") && meta.contains("\"guestPath\": \"/android\""),
        "fresh install missing default binds in metadata:\n{meta}"
    );

    // --- Guest → host: a file created under /home/android lands in
    // shared storage.
    run_inside_ok(&format!(
        "mkdir -p /home/android/{d} && echo from-guest > /home/android/{d}/from-guest.txt",
        d = "tawc-bind-test"
    ));
    let host_read = adb::shell(&format!("cat {SHARED_DIR}/from-guest.txt")).unwrap();
    assert_eq!(
        String::from_utf8_lossy(&host_read.stdout).trim(),
        "from-guest",
        "guest-written file not visible in shared storage: {}",
        combined(&host_read)
    );

    // --- Host → guest: a shared-storage file appears inside the rootfs.
    let out = adb::shell(&format!("echo from-host > {SHARED_DIR}/from-host.txt")).unwrap();
    assert!(out.status.success(), "host-side write failed: {}", combined(&out));
    let guest_read = run_inside_ok("cat /home/android/tawc-bind-test/from-host.txt");
    assert_eq!(guest_read.trim(), "from-host");

    // --- Default /android bind exposes the Android root.
    run_inside_ok("test -d /android/system && echo ANDROID_ROOT_OK");

    // --- Edited metadata drives the next spawn: move the home bind to
    // /home/droid and confirm the old guest path stops resolving.
    // First expression strips JSONStringer's `\/` escaping (still
    // valid JSON) so the path patterns match literally.
    let sed = host_sh(&format!(
        "sed -i 's|\\\\/|/|g; s|/home/android|/home/droid|' {}",
        metadata_path()
    ));
    assert!(sed.status.success(), "metadata edit failed: {}", combined(&sed));
    let moved = run_inside_ok(
        "cat /home/droid/tawc-bind-test/from-host.txt; \
         test ! -e /home/android/tawc-bind-test/from-host.txt && echo OLD_PATH_GONE",
    );
    assert!(moved.contains("from-host"), "edited bind not live: {moved}");
    assert!(moved.contains("OLD_PATH_GONE"), "old guest path still bound: {moved}");
    let sed = host_sh(&format!(
        "sed -i 's|/home/droid|/home/android|' {}",
        metadata_path()
    ));
    assert!(sed.status.success(), "metadata restore failed: {}", combined(&sed));

    // --- Revoked all-files access: launch fails closed with an
    // actionable error instead of silently dropping the bind.
    set_all_files_access("deny");
    let out = run_inside("true");
    assert!(
        !out.status.success(),
        "launch with revoked all-files access unexpectedly succeeded:\n{}",
        combined(&out)
    );
    assert!(
        combined(&out).contains("all-files access"),
        "fail-closed error not surfaced:\n{}",
        combined(&out)
    );
    set_all_files_access("allow");

    // --- Uninstall removes the rootfs but not the bind target's
    // contents (tawcroot binds are path rewrites, not mounts).
    let out = action("uninstall", &[("id", TEST_ID)]);
    assert!(out.status.success(), "uninstall failed:\n{}", combined(&out));
    let probe = host_sh(&format!(
        "test -d /data/data/{PKG}/distros/{TEST_ID} && echo EXISTS || echo ABSENT"
    ));
    assert!(String::from_utf8_lossy(&probe.stdout).contains("ABSENT"));
    let host_read = adb::shell(&format!("cat {SHARED_DIR}/from-guest.txt")).unwrap();
    assert_eq!(
        String::from_utf8_lossy(&host_read.stdout).trim(),
        "from-guest",
        "shared-storage contents did not survive uninstall"
    );

    // Cleanup the shared-storage scratch.
    adb::shell(&format!("rm -rf {SHARED_DIR}")).unwrap();
}
