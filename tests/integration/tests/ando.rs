//! Integration coverage for ando (notes/ando.md): the production
//! broker that runs Android commands for rootfs guests.
//!
//! Every spawn here goes through the debug exec broker's RUNINSIDE
//! form, so each test inherently runs its ando child concurrently with
//! an ART `ProcessBuilder` child (the guest session itself) — standing
//! regression cover for "nothing else in the app process steals the
//! ando broker's waitpid status".

use tawc_integration::adb;

/// Run `cmd` inside the rootfs; return (exit code, stdout, stderr)
/// with stdio trimmed.
fn run(cmd: &str) -> (i32, String, String) {
    let out = adb::rootfs_run(cmd).unwrap_or_else(|e| panic!("rootfs_run {cmd:?}: {e}"));
    (
        out.status.code().unwrap_or(-1),
        String::from_utf8_lossy(&out.stdout).trim().to_string(),
        String::from_utf8_lossy(&out.stderr).trim().to_string(),
    )
}

/// Count Android-side processes whose command line matches `pattern`
/// (a grep regex; use a [b]racketed first char so the grep itself
/// doesn't match).
fn android_proc_count(pattern: &str) -> usize {
    let out = adb::shell(&format!("ps -A | grep '{pattern}' | wc -l")).expect("adb shell ps");
    String::from_utf8_lossy(&out.stdout).trim().parse().expect("ps count")
}

#[test]
fn test_ando_runs_android_command_on_guest_stdout() {
    let (rc, out, err) = run("ando /system/bin/getprop ro.product.cpu.abi");
    assert_eq!(rc, 0, "stderr: {err}");
    assert!(
        out == "x86_64" || out.starts_with("arm"),
        "unexpected abi from getprop: {out:?}"
    );
}

#[test]
fn test_ando_path_search_and_exit_code() {
    // `sh` is bare (PATH search hits /system/bin) and the exit code
    // must come back verbatim through broker + client.
    let (rc, _, err) = run("ando sh -c 'exit 7'");
    assert_eq!(rc, 7, "stderr: {err}");
}

#[test]
fn test_ando_command_not_found() {
    let (rc, _, err) = run("ando no-such-command-xyz");
    assert_eq!(rc, 127);
    assert!(err.contains("not found"), "stderr: {err:?}");
}

#[test]
fn test_ando_env_hygiene_and_extras() {
    // Nothing from the guest env (libhybris LD_* baggage) may leak…
    let (rc, out, _) = run(r#"ando sh -c 'echo "[${LD_PRELOAD}${LD_LIBRARY_PATH}]"'"#);
    assert_eq!(rc, 0);
    assert_eq!(out, "[]", "guest env leaked into the Android child");
    // …while -e extras must arrive (long form included).
    let (rc, out, _) = run("ando -e TAWC_TEST_VAR=hello sh -c 'echo $TAWC_TEST_VAR'");
    assert_eq!(rc, 0);
    assert_eq!(out, "hello");
    let (rc, out, _) = run("ando --env TAWC_TEST_VAR=long sh -c 'echo $TAWC_TEST_VAR'");
    assert_eq!(rc, 0);
    assert_eq!(out, "long");
}

#[test]
fn test_ando_identity_is_app_uid_not_fake_root() {
    let (rc, out, _) = run("echo guest=$(id -u) android=$(ando id -u)");
    assert_eq!(rc, 0);
    let android_uid: u32 = out
        .strip_prefix("guest=0 android=")
        .unwrap_or_else(|| panic!("unexpected identity output: {out:?}"))
        .parse()
        .expect("android uid");
    assert!(android_uid >= 10000, "expected app uid, got {android_uid}");
}

#[test]
fn test_ando_signal_forwarding() {
    // TERM the in-rootfs client: its handler forwards SIG 15, the
    // broker kills the Android-side process group, sleep dies by
    // signal, and the client exits with the broker's 128+15.
    let (rc, out, _) = run("ando sleep 987 & p=$!; sleep 1; kill -TERM $p; wait $p; echo rc=$?");
    assert_eq!(rc, 0);
    assert_eq!(out, "rc=143");
    assert_eq!(android_proc_count("[s]leep 987"), 0, "Android-side sleep survived");
}

#[test]
fn test_ando_client_death_reaps_android_child() {
    // SIGKILL the client (no chance to forward): the broker sees
    // socket EOF before child exit and SIGKILLs the child's group.
    let (rc, out, _) = run("ando sleep 988 & p=$!; sleep 1; kill -KILL $p; wait $p; echo rc=$?");
    assert_eq!(rc, 0);
    assert_eq!(out, "rc=137");
    std::thread::sleep(std::time::Duration::from_secs(1));
    assert_eq!(android_proc_count("[s]leep 988"), 0, "orphaned Android-side sleep");
}

#[test]
fn test_ando_cwd_travels_as_fd() {
    // The child starts in the host directory the guest cwd resolves
    // to — pwd reports the untranslated host path.
    let (rc, out, _) = run("cd /root && ando sh -c pwd");
    assert_eq!(rc, 0);
    assert!(
        out.ends_with("/rootfs/root"),
        "expected host path of guest /root, got {out:?}"
    );
    // Files created there are visible in the guest cwd.
    let (rc, out, _) = run(
        "cd /root && rm -f ando-cwd-x && ando sh -c 'touch ando-cwd-x' && ls ando-cwd-x && rm ando-cwd-x",
    );
    assert_eq!(rc, 0);
    assert_eq!(out, "ando-cwd-x");
    // A bind-mounted cwd resolves to the bind source.
    let (rc, out, _) = run("cd /usr/share/tawc && ando sh -c pwd");
    assert_eq!(rc, 0);
    assert!(
        out.ends_with("/me.phie.tawc/share"),
        "expected bind source of /usr/share/tawc, got {out:?}"
    );
}

#[test]
fn test_ando_option_parsing_stops_at_command() {
    // Everything after the first non-option belongs to the command:
    // sh gets -e and -c (if ando consumed -e, "-c" fails K=V → 125).
    let (rc, out, err) = run("ando sh -e -c 'echo boundary-ok'");
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "boundary-ok");
    // -- terminates options: the next word is the command even if it
    // starts with '-' (broker 127, not an ando usage error).
    let (rc, _, err) = run("ando -- -no-such-cmd-xyz");
    assert_eq!(rc, 127);
    assert!(err.contains("not found"), "stderr: {err:?}");
}

#[test]
fn test_ando_preserve_env_forwards_and_blocklists() {
    // -E forwards an arbitrary guest var…
    let (rc, out, err) = run("TAWC_E_VAR=fwd ando -E sh -c 'echo $TAWC_E_VAR'");
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "fwd");
    // …but never the blocklist: LD_* stays clean under -E…
    let (rc, out, _) = run(
        "LD_PRELOAD=/guest/p.so LD_LIBRARY_PATH=/guest/l \
         ando -E sh -c 'echo \"[${LD_PRELOAD}${LD_LIBRARY_PATH}]\"'",
    );
    assert_eq!(rc, 0);
    assert_eq!(out, "[]", "blocklisted LD_* leaked through -E");
    // …and a poisoned guest PATH doesn't arrive (the bare `sh` spawn
    // would 127 if it did; absolute ando path since the guest shell's
    // own lookup may use the prefix assignment).
    let (rc, out, err) =
        run("PATH=/poisoned /usr/local/bin/ando -E sh -c 'echo \"[$PATH]\"'");
    assert_eq!(rc, 0, "guest PATH leaked through -E; stderr: {err}");
    assert!(!out.contains("/poisoned"), "guest PATH leaked: {out:?}");
}

#[test]
fn test_ando_preserve_env_list() {
    // =LIST forwards exactly the named vars; unset names are skipped.
    let (rc, out, err) = run(
        "TA=1 TB=2 TC=3 ando --preserve-env=TA,TB,TUNSET sh -c 'echo \"[$TA][$TB][$TC]\"'",
    );
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "[1][2][]");
    // Naming a blocklisted var is as deliberate as -e: it goes through.
    // (LD_LIBRARY_PATH, not LD_PRELOAD — a forwarded preload would be
    // honored by the bionic linker and kill the child.)
    let (rc, out, _) = run(
        "LD_LIBRARY_PATH=/guest/l ando --preserve-env=LD_LIBRARY_PATH sh -c 'echo $LD_LIBRARY_PATH'",
    );
    assert_eq!(rc, 0);
    assert_eq!(out, "/guest/l");
    // Bare --preserve-env never consumes a separate word as LIST.
    let (rc, out, _) = run("ando --preserve-env echo separate-ok");
    assert_eq!(rc, 0);
    assert_eq!(out, "separate-ok");
}

#[test]
fn test_ando_env_precedence_e_beats_preserve() {
    // -e extras are sent last, so they win over -E-forwarded values
    // (broker applies ENV lines last-wins) regardless of flag order.
    let (rc, out, err) =
        run("TAWC_E_VAR=guest ando -e TAWC_E_VAR=extra -E sh -c 'echo $TAWC_E_VAR'");
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "extra");
}

#[test]
fn test_ando_chdir() {
    // -D replaces the caller's cwd before the cwd-fd open; same host
    // path resolution as the cwd test above.
    let (rc, out, err) = run("ando -D /root sh -c pwd");
    assert_eq!(rc, 0, "stderr: {err}");
    assert!(out.ends_with("/rootfs/root"), "got {out:?}");
    // Relative dirs resolve against the caller's cwd.
    let (rc, out, _) = run("cd / && ando -D root sh -c pwd");
    assert_eq!(rc, 0);
    assert!(out.ends_with("/rootfs/root"), "got {out:?}");
    // Nonexistent dir: one error line, exit 125, nothing spawned.
    let (rc, _, err) = run("ando -D /no-such-dir-xyz true");
    assert_eq!(rc, 125);
    assert!(err.contains("chdir"), "stderr: {err:?}");
}

#[test]
fn test_ando_shell_flag() {
    // -s with args runs /system/bin/sh -c with the sudo-style join.
    let (rc, _, err) = run("ando -s exit 9");
    assert_eq!(rc, 9, "stderr: {err}");
    // Escaping: 'a b' stays one word through sh -c.
    let (rc, out, _) = run("ando -s echo 'a b'");
    assert_eq!(rc, 0);
    assert_eq!(out, "a b");
    // -s with no command is the bare shell; feed it via stdin instead
    // of a tty (interactive use stays a manual check).
    let (rc, _, err) = run("printf 'exit 5\\n' | ando -s");
    assert_eq!(rc, 5, "stderr: {err}");
}

#[test]
fn test_ando_su_argv_construction() {
    // TAWC_ANDO_SU (test hook) swaps su for echo so an unrooted run
    // can assert the exact argv the -u/-r rewrite constructs.
    let su = "TAWC_ANDO_SU=/system/bin/echo";
    let (rc, out, err) = run(&format!("{su} ando -u shell id -u"));
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "shell -c id -u");
    let (rc, out, _) = run(&format!("{su} ando -r id"));
    assert_eq!(rc, 0);
    assert_eq!(out, "root -c id");
    // Repeated -u/-r: last one wins.
    let (rc, out, _) = run(&format!("{su} ando -u nobody -r id"));
    assert_eq!(rc, 0);
    assert_eq!(out, "root -c id");
    // -s with no command: bare `su USER` (su's default is a shell).
    let (rc, out, _) = run(&format!("{su} ando -r -s"));
    assert_eq!(rc, 0);
    assert_eq!(out, "root");
    // The -c string carries the escaped join.
    let (rc, out, _) = run(&format!("{su} ando -r echo 'a b'"));
    assert_eq!(rc, 0);
    assert_eq!(out, r"root -c echo a\ b");
}

#[test]
fn test_ando_flag_errors_and_help() {
    for bad in [
        "ando -Z true",   // unknown option
        "ando -D",        // missing arg
        "ando -u",        // missing arg
        "ando -e noequal true", // -e needs K=V
        "ando -r",        // user but no command and no -s
        "ando",           // no command at all
    ] {
        let (rc, _, err) = run(bad);
        assert_eq!(rc, 125, "{bad:?} should be a usage error; stderr: {err:?}");
    }
    let (rc, _, err) = run("ando -h");
    assert_eq!(rc, 0);
    for flag in ["--preserve-env", "--chdir", "--shell", "--user", "-r"] {
        assert!(err.contains(flag), "-h misses {flag}; stderr: {err:?}");
    }
}

#[test]
fn test_ando_broker_absent_and_socket_override() {
    // TAWC_ANDO_SOCKET overrides the socket path (test hook): a bogus
    // path gives one clear error line and exit 127.
    let (rc, _, err) = run("TAWC_ANDO_SOCKET=/usr/share/tawc/no-such-ando.sock ando true");
    assert_eq!(rc, 127);
    assert!(err.contains("broker not running"), "stderr: {err:?}");
    // Empty override falls through to the built-in share-dir path.
    let (rc, out, _) = run("TAWC_ANDO_SOCKET= ando echo default-path-ok");
    assert_eq!(rc, 0);
    assert_eq!(out, "default-path-ok");
}
