//! tawcroot **prod-env** device tests: production `libtawcroot.so`
//! spawned through the exec broker, so every run happens under the
//! real production sandbox — app uid, `untrusted_app`, the
//! zygote-installed seccomp filter — on whatever device the suite
//! targets. This is the layer that turns "what does Android policy
//! actually do to this syscall" into a standing test instead of a
//! hand-run smoke; see notes/tawcroot/testing.md ("Prod-env device
//! layer") for the placement rule.
//!
//! What this covers that `tawcroot/test.sh --device` (adb shell / su)
//! structurally cannot:
//!   - ground truth for the synthesized androidfilter: these guests run
//!     under the filter zygote actually installs;
//!   - production-SELinux interactions in the production domain (e.g.
//!     hardlink denial → v1 rename+symlink fallback, AF_UNIX bind in
//!     app data);
//!   - no adbd-mode variance: broker children are never real root, so
//!     the euid-0 skip class can't recur.
//!
//! Guest programs and rootfs staging: `tawc_integration::tawcroot_prodenv`.

use tawc_integration::helpers::test_init;
use tawc_integration::tawcroot_prodenv::{app_sh, app_sh_raw, assert_guest_exit, env, run_guest};

/// Static guest runs and exits under the real zygote filter — the
/// whole trap/translate pipeline works in the production sandbox.
#[test]
fn test_prodenv_static_exit42() {
    test_init();
    let out = run_guest(&[], &["/bin/static_exit42"], &[]).expect("broker spawn");
    assert_guest_exit("static_exit42", &out, 42);
}

/// Loader stack synthesis (argc/argv/envp/auxv incl. AT_RANDOM) is
/// intact in prod-env. The fixture requires argc == 3 and envp[0]
/// starting with 'X'; success encodes AT_RANDOM byte 0 as 100..227,
/// failures are 96..99.
#[test]
fn test_prodenv_stack_synth_argc_random() {
    test_init();
    let out = run_guest(&[], &["/bin/static_argc_random", "a1", "a2"], &["X=1"])
        .expect("broker spawn");
    let code = out.status.code();
    assert!(
        matches!(code, Some(c) if (100..=227).contains(&c)),
        "static_argc_random: expected success code 100..=227, got {:?}\nstderr={}",
        out.status,
        String::from_utf8_lossy(&out.stderr),
    );
}

/// Guest execve: the exec handler's memfd + `/proc/self/exe` re-exec
/// dance under the real filter and SELinux (`untrusted_app` re-execs
/// `libtawcroot.so` from nativeLibraryDir mid-guest).
#[test]
fn test_prodenv_guest_execve() {
    test_init();
    let out = run_guest(&[], &["/bin/static_execve_exit42"], &[]).expect("broker spawn");
    assert_guest_exit("static_execve_exit42", &out, 42);
}

/// Virtual identity: drop to uid/gid 994 is irreversible (setuid(0)
/// EPERMs) and survives execve. In prod-env the process is never real
/// root, so this holds identically on emulator and phone — the
/// rooted-adbd variance documented in notes/tawcroot/testing.md
/// doesn't exist here.
#[test]
fn test_prodenv_identity_survives_execve() {
    test_init();
    let out = run_guest(&[], &["/bin/static_drop_ids_execve"], &[]).expect("broker spawn");
    assert_guest_exit("static_drop_ids_execve", &out, 42);
}

/// Privilege-gated chmod/chown against root-owned /dev/null: faked
/// success at virtual root, real EPERM/EACCES after a genuine drop.
/// Ported from the testhost smoke's euid-sensitive steps (de5f95d);
/// as the app uid the real kernel/SELinux answer always surfaces.
#[test]
fn test_prodenv_identity_dropped_devnull_eperm() {
    test_init();
    let out = run_guest(
        &["/dev:/dev"],
        &["/bin/static_drop_ids_devnull_eperm"],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("static_drop_ids_devnull_eperm", &out, 42);
}

/// Device-node mknod under fake root: untrusted_app never has
/// CAP_MKNOD, so this is where the S_IFCHR refusal actually fires
/// (rooted test environments succeed and can't see it). The handler
/// must degrade to a regular-file placeholder in rootfs paths, swallow
/// to bare success in the host-/dev bind, keep EEXIST honest, and
/// surface the real error after a genuine identity drop.
#[test]
fn test_prodenv_mknod_chr_fake_root() {
    test_init();
    let out = run_guest(&["/dev:/dev"], &["/bin/static_mknod_chr_fake"], &[]).expect("broker spawn");
    assert_guest_exit("static_mknod_chr_fake", &out, 42);
}

/// The hardlink publish idiom (git's object finalize shape). Under
/// `untrusted_app`, Android SELinux denies hardlink creation in app
/// data on both targets, so this exercises the v1 rename+symlink
/// fallback end to end — previously only the physical device's shell
/// domain hit it, incidentally.
#[test]
fn test_prodenv_link_publish_fallback() {
    test_init();
    let out = run_guest(
        &[],
        &["/bin/static_link_publish_argv12", "/pub-src", "/pub-dst"],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("static_link_publish_argv12", &out, 42);
}

/// Path translation for O_CREAT lands the file at the translated
/// host path inside the staged rootfs (verified from outside the
/// guest via the broker).
#[test]
fn test_prodenv_open_creat_translates() {
    test_init();
    let rootfs = &env().rootfs;
    let marker = "/prodenv-creat-marker";
    let host_path = format!("{rootfs}{marker}");
    app_sh(&format!("rm -f '{host_path}'"));
    let out = run_guest(&[], &["/bin/static_open_creat_argv1", marker], &[])
        .expect("broker spawn");
    assert_guest_exit("static_open_creat_argv1", &out, 0);
    app_sh(&format!("test -f '{host_path}' && rm '{host_path}'"));
}

/// Read path: an absolute guest path opens the rootfs file.
#[test]
fn test_prodenv_open_rdonly_etc_probe() {
    test_init();
    let out = run_guest(&[], &["/bin/static_open_rdonly_argv1", "/etc/probe"], &[])
        .expect("broker spawn");
    assert_guest_exit("static_open_rdonly_argv1", &out, 0);
}

/// AF_UNIX bind: sun_path is translated and the socket inode lands
/// inside the rootfs. The adb-shell device suite must skip this
/// (Android denies socket creation on `shell_data_file`); app data as
/// the app uid is the production case and works.
#[test]
fn test_prodenv_unix_bind_translates_sun_path() {
    test_init();
    let rootfs = &env().rootfs;
    let guest_sock = "/run/prodenv-agent.sock";
    let host_sock = format!("{rootfs}{guest_sock}");
    app_sh(&format!("rm -f '{host_sock}'"));
    let out = run_guest(&[], &["/bin/static_unix_bind_argv1", guest_sock], &[])
        .expect("broker spawn");
    assert_guest_exit("static_unix_bind_argv1", &out, 42);
    let probe = app_sh_raw(&format!("test -S '{host_sock}'")).expect("broker sh");
    assert!(
        probe.status.success(),
        "expected a socket inode at {host_sock} after guest bind"
    );
    app_sh(&format!("rm -f '{host_sock}'"));
}

/// /proc/self/fd dirent filter hides tawcroot's reserved fds from the
/// guest, against the device's real procfs.
#[test]
fn test_prodenv_proc_self_fd_hides_reserved() {
    test_init();
    let out = run_guest(
        &["/proc:/proc"],
        &["/bin/static_check_proc_self_fd"],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("static_check_proc_self_fd", &out, 42);
}

/// io_uring defense-in-depth deny: all three io_uring syscalls return
/// ENOSYS to the guest. Runs under the real zygote filter, so this is
/// also ground truth that Android's own filter doesn't preempt
/// tawcroot's handling with something harsher.
#[test]
fn test_prodenv_io_uring_deny() {
    test_init();
    let out = run_guest(&[], &["/bin/static_io_uring_deny"], &[]).expect("broker spawn");
    assert_guest_exit("static_io_uring_deny", &out, 42);
}

/// Dynamic (bionic-linked) guest: the manual loader follows PT_INTERP
/// to the device's real linker through `/system` + `/apex` binds —
/// the same bind shape production launches use.
#[test]
fn test_prodenv_dynamic_exit42() {
    test_init();
    let out = run_guest(
        &["/system:/system", "/apex:/apex"],
        &["/bin/dynamic_exit42"],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("dynamic_exit42", &out, 42);
}
