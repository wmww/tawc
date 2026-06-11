//! Wipe-engine ([RootfsCleaner]) edge cases that the happy-path
//! uninstalls elsewhere don't reach: the uniform mount gate refusing
//! to delete a tawcroot slot with a leaked bind mount under it (a
//! same-filesystem bind that `find -xdev` would walk straight
//! through), and the one-`su`-retry ladder clearing root-owned
//! droppings from an app-uid rootfs.
//!
//! Slots are fabricated (mkdir + metadata.json via the broker, as the
//! app uid) rather than installed — uninstall only needs metadata, and
//! a full install would cost minutes per case. Needs a rooted target
//! (emulator): the gate test mounts/unmounts via `su -mm`, the
//! droppings test plants root-owned files.

use std::process::Output;

use tawc_integration::adb;
use tawc_integration::exec_broker::{self, Invocation, Request};

const PKG: &str = "me.phie.tawc";
const TEST_ID: &str = "wipetest";
/// Bind source under the sanctioned scratch dir — same `/data`
/// filesystem as the rootfs, so `-xdev` alone would not stop the
/// delete from traversing into it.
const BIND_SRC: &str = "/data/local/tmp/tawc-dev/wipe-bind-src";

fn slot_dir() -> String {
    format!("/data/data/{PKG}/distros/{TEST_ID}")
}

fn combined(out: &Output) -> String {
    format!(
        "{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    )
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

/// Host-side (app uid) shell via the broker's EXEC form.
fn host_sh(script: &str) -> Output {
    adb::rootfs_host_exec(&["/system/bin/sh", "-c", script]).expect("broker host exec")
}

fn host_sh_ok(script: &str) {
    let out = host_sh(script);
    assert!(
        out.status.success(),
        "app-uid script failed: `{script}`\n{}",
        combined(&out)
    );
}

/// Root shell on the device (Magisk su from the adb shell uid). The
/// script rides inside single quotes through two shells — keep it
/// quote-free.
fn su(script: &str) -> Output {
    assert!(!script.contains('\''), "su script must not contain single quotes: {script}");
    adb::shell(&format!("su -c '{script}'")).expect("adb shell su")
}

fn su_ok(script: &str) {
    let out = su(script);
    assert!(out.status.success(), "su script failed: `{script}`\n{}", combined(&out));
}

/// Root shell in the global mount namespace (`su -mm`), for mounts the
/// app's namespace inherits via propagation. Same quoting caveat as [su].
fn su_mm(script: &str) -> Output {
    assert!(!script.contains('\''), "su script must not contain single quotes: {script}");
    adb::shell(&format!("su -mm -c '{script}'")).expect("adb shell su -mm")
}

fn require_root() {
    let out = su("id -u");
    let uid = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success() && uid.trim() == "0",
        "this test needs a rooted target (su unavailable): {}",
        combined(&out)
    );
}

/// Lay down a fake READY tawcroot slot: rootfs with a few files
/// (including a mode-0500 dir, the bootstrap shape the wipe's chmod
/// step exists for) plus minimal metadata. App-uid throughout, like a
/// real tawcroot install.
fn fabricate_slot() {
    let d = slot_dir();
    let metadata = format!(
        r#"{{"id": "{TEST_ID}", "arch": "x86_64", "distro": "arch", "method": "tawcroot", "state": "READY", "schemaVersion": 1}}"#
    );
    host_sh_ok(&format!(
        "set -eu; \
         mkdir -p {d}/rootfs/etc {d}/rootfs/locked; \
         echo canary > {d}/rootfs/etc/canary; \
         touch {d}/rootfs/locked/inner; \
         chmod 500 {d}/rootfs/locked; \
         printf '%s' '{metadata}' > {d}/metadata.json"
    ));
}

fn slot_exists() -> bool {
    let out = host_sh(&format!(
        "test -d {} && echo EXISTS || echo ABSENT",
        slot_dir()
    ));
    String::from_utf8_lossy(&out.stdout).contains("EXISTS")
}

#[test]
fn test_wipe_gate_and_su_retry() {
    require_root();
    let d = slot_dir();

    // Pre-clean leftovers from an aborted previous run. The umount
    // must come first or the uninstall below would (correctly) refuse.
    su_mm(&format!("umount {d}/rootfs/leak 2>/dev/null; true"));
    // chmod the 0500 dir back so a fabricated leftover doesn't need
    // the su ladder just to pre-clean.
    host_sh(&format!("chmod -R u+rwX {d} 2>/dev/null; true"));
    action("uninstall", &[("id", TEST_ID)]);
    su(&format!("rm -rf {BIND_SRC}"));
    assert!(!slot_exists(), "pre-clean failed to remove the test slot");

    // --- Mount gate: a leaked same-filesystem bind mount under the
    // rootfs must refuse the wipe for a *tawcroot* install (no kernel
    // mounts of its own; only the uniform gate stands between the
    // delete and the bind source).
    fabricate_slot();
    su_ok(&format!("mkdir -p {BIND_SRC} && echo precious > {BIND_SRC}/precious.txt"));
    host_sh_ok(&format!("mkdir -p {d}/rootfs/leak"));
    let out = su_mm(&format!("mount --bind {BIND_SRC} {d}/rootfs/leak"));
    assert!(out.status.success(), "bind mount failed: {}", combined(&out));

    let out = action("uninstall", &[("id", TEST_ID)]);
    assert!(
        !out.status.success(),
        "uninstall over a live bind mount unexpectedly succeeded:\n{}",
        combined(&out)
    );
    assert!(
        combined(&out).contains("wipe refused"),
        "expected the mount-gate refusal in the op log:\n{}",
        combined(&out)
    );
    assert!(slot_exists(), "refused wipe deleted the slot anyway");
    let probe = host_sh(&format!(
        "test -f {d}/metadata.json && echo META_OK || echo META_GONE"
    ));
    assert!(
        String::from_utf8_lossy(&probe.stdout).contains("META_OK"),
        "refused wipe lost metadata.json"
    );
    let probe = su(&format!("cat {BIND_SRC}/precious.txt"));
    assert_eq!(
        String::from_utf8_lossy(&probe.stdout).trim(),
        "precious",
        "bind-source contents damaged by a refused wipe"
    );

    // Unmounted, the same slot uninstalls cleanly (slot is FAILED
    // after the refusal; uninstall is the recovery edge).
    su_mm(&format!("umount {d}/rootfs/leak"));
    let out = action("uninstall", &[("id", TEST_ID)]);
    assert!(out.status.success(), "post-umount uninstall failed:\n{}", combined(&out));
    assert!(!slot_exists(), "uninstall left the slot behind");
    let probe = su(&format!("cat {BIND_SRC}/precious.txt"));
    assert_eq!(
        String::from_utf8_lossy(&probe.stdout).trim(),
        "precious",
        "bind-source contents did not survive uninstall"
    );
    su(&format!("rm -rf {BIND_SRC}"));

    // --- su-retry ladder: root-owned droppings inside an app-uid
    // rootfs (interleaved debug `su` use). A root-owned mode-0700 dir
    // blocks the app-uid `find` from unlinking its children; the one
    // `su` retry must clear it.
    fabricate_slot();
    su_ok(&format!(
        "mkdir {d}/rootfs/rootdir && touch {d}/rootfs/rootdir/dropping && chmod 700 {d}/rootfs/rootdir"
    ));
    let out = action("uninstall", &[("id", TEST_ID)]);
    assert!(
        out.status.success(),
        "uninstall with root-owned droppings failed:\n{}",
        combined(&out)
    );
    assert!(!slot_exists(), "su retry left the slot behind");
}
