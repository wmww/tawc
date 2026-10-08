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

/// libudev's uevent-monitor bring-up must complete at the app uid.
/// `socket(AF_NETLINK, …, NETLINK_KOBJECT_UEVENT)` is EACCES for
/// untrusted_app, which libudev reports as a NULL monitor — and that
/// takes down SDL_INIT_HAPTIC and, through SDL3's atomic SDL_Init, the
/// guest's video subsystem with it (issue:
/// sdl-haptic-udev-netlink-kills-video-init). Only meaningful here: the
/// hosted suite and `tawcroot/test.sh --device` run where the kernel
/// may hand out a real netlink socket, so they cannot prove the stub.
#[test]
fn test_prodenv_uevent_socket_stub() {
    test_init();
    let out = run_guest(&[], &["/bin/static_uevent_socket"], &[]).expect("broker spawn");
    assert_guest_exit("static_uevent_socket", &out, 42);
}

/// Interface enumeration at the app uid, where Android denies rtnetlink
/// bind() and RTM_GETLINK and allows SIOCGIFNAME only on inet sockets:
/// glibc getifaddrs' netlink sequence, libtorrent's plain-send dump and
/// glibc if_indextoname's AF_UNIX ioctl must all see `lo`. The hosted
/// `test_rtnl_emu` suite only simulates the denials; this is the real
/// policy.
#[test]
fn test_prodenv_rtnl_interface_enumeration() {
    test_init();
    let out = run_guest(
        &["/system:/system", "/apex:/apex"],
        &["/bin/dynamic_rtnl_probe"],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("dynamic_rtnl_probe", &out, 42);
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

/// Legacy-NR trap-set audit (issues/tawcroot-x86_64-legacy-trapset-
/// audit.md). Under the REAL zygote filter, a guest can only see
/// -ENOSYS from a kernel-implemented legacy NR if Android trapped it
/// and tawcroot has no handler — the trapped-but-unhandled gap. The
/// probe issues a table of legacy NRs with benign args and exits 42
/// iff no gap (and its controls hold). x86_64-only; on aarch64 the
/// fixture is a no-op that also exits 42. Full per-NR table prints to
/// stdout — run with `--nocapture` to read it.
#[test]
fn test_prodenv_legacy_nr_trapset_audit() {
    test_init();
    let out = run_guest(
        &["/system:/system", "/apex:/apex"],
        &["/bin/dynamic_legacy_nr_probe"],
        &[],
    )
    .expect("broker spawn");
    println!(
        "----- dynamic_legacy_nr_probe stdout -----\n{}",
        String::from_utf8_lossy(&out.stdout)
    );
    assert_guest_exit("dynamic_legacy_nr_probe", &out, 42);
}

/// The gpgme/glibc pre-exec `closefrom` shape under the real zygote
/// filter: fork, close the reserved-fd numbers, `close_range(0, ~0u)`,
/// then execve. `close_range` is NR 436, which bionic only gained at
/// API 34 — every older policy RET_TRAPs it, and since SIGSYS is masked
/// inside our own handler a raw re-issue from there is a silent
/// force-kill (wmww/tawc#14). The handler emulates instead. On an
/// API >= 34 target this passes either way (the policy allows 436); it
/// is the no-regression check on the standing target and the
/// reproducer on an older-API AVD.
#[test]
fn test_prodenv_fork_closefrom_execve() {
    test_init();
    let out = run_guest(
        &[],
        &[
            "/bin/static_fork_closefrom_exec_argv1",
            "/bin/static_exit42",
        ],
        &[],
    )
    .expect("broker spawn");
    assert_guest_exit("static_fork_closefrom_exec_argv1", &out, 42);
}

/// Host-side `Sh.run` scripts can use here-documents. mksh spills each
/// heredoc to a file under `$TMPDIR`; app processes have none, and the
/// `/data/local` fallback is unwritable by the app uid, which failed
/// every install at Configure (wmww/tawc#13, #16).
#[test]
fn test_prodenv_host_sh_heredoc() {
    test_init();
    let out = tawc_integration::adb::host_sh("set -eu\ncat <<'EOF'\nheredoc-ok\nEOF\n")
        .expect("broker host-sh");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success() && stdout.contains("heredoc-ok"),
        "host-sh heredoc failed: {:?} stdout={stdout:?}",
        out.status,
    );
}
