//! Integration coverage for ando (notes/ando.md): the production
//! broker that runs Android commands for rootfs guests.
//!
//! Every spawn here goes through the debug exec broker's RUNINSIDE
//! form, so each test inherently runs its ando child concurrently with
//! an ART `ProcessBuilder` child (the guest session itself) — standing
//! regression cover for "nothing else in the app process steals the
//! ando broker's waitpid status".
//!
//! ando is a per-distro capability, default **off**. These tests flip
//! it with the in-memory test override (`set-ando`, see
//! [adb::set_ando]) rather than a durable metadata write: every test
//! that expects ando to *work* calls [enable_ando] first, and the
//! disabled-path tests call [disable_ando]. The override is discarded
//! on app-process death and by `test-init`, so nothing durable leaks.

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

/// Enable ando for the standing install and wait for the broker to
/// reconcile. The take-effect-on-next-spawn contract is satisfied
/// automatically: the subsequent `run(...)` is a fresh rootfs spawn, so
/// it carries the per-distro ando bind.
fn enable_ando() {
    adb::set_ando(true).expect("set-ando true");
}

/// Disable ando for the standing install. Immediate: the listener is
/// closed, its socket unlinked, and in-flight ando children SIGKILLed.
fn disable_ando() {
    adb::set_ando(false).expect("set-ando false");
}

/// Count Android-side processes whose command line matches `pattern`
/// (a grep regex; use a [b]racketed first char so the grep itself
/// doesn't match). `-o PID,ARGS` is required: the emulator's default
/// `ps -A` prints only the COMM (`sleep`), so a pattern carrying args
/// (`sleep 991`) would never match.
fn android_proc_count(pattern: &str) -> usize {
    let out = adb::shell(&format!("ps -A -o PID,ARGS 2>/dev/null | grep '{pattern}' | wc -l"))
        .expect("adb shell ps");
    String::from_utf8_lossy(&out.stdout).trim().parse().expect("ps count")
}

/// Poll until the Android-side process matching `pattern` is present
/// (or absent), panicking if the state isn't reached within `secs`.
fn wait_for_proc(pattern: &str, present: bool, secs: u64) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(secs);
    loop {
        if (android_proc_count(pattern) > 0) == present {
            return;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "process {pattern:?} present={present} not reached within {secs}s"
        );
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}

#[test]
fn test_ando_runs_android_command_on_guest_stdout() {
    enable_ando();
    let (rc, out, err) = run("ando /system/bin/getprop ro.product.cpu.abi");
    assert_eq!(rc, 0, "stderr: {err}");
    assert!(
        out == "x86_64" || out.starts_with("arm"),
        "unexpected abi from getprop: {out:?}"
    );
}

#[test]
fn test_ando_path_search_and_exit_code() {
    enable_ando();
    // `sh` is bare (PATH search hits /system/bin) and the exit code
    // must come back verbatim through broker + client.
    let (rc, _, err) = run("ando sh -c 'exit 7'");
    assert_eq!(rc, 7, "stderr: {err}");
}

#[test]
fn test_ando_command_not_found() {
    enable_ando();
    let (rc, _, err) = run("ando no-such-command-xyz");
    assert_eq!(rc, 127);
    assert!(err.contains("not found"), "stderr: {err:?}");
}

#[test]
fn test_ando_env_hygiene_and_extras() {
    enable_ando();
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
    enable_ando();
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
    enable_ando();
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
    enable_ando();
    // SIGKILL the client (no chance to forward): the broker sees
    // socket EOF before child exit and SIGKILLs the child's group.
    let (rc, out, _) = run("ando sleep 988 & p=$!; sleep 1; kill -KILL $p; wait $p; echo rc=$?");
    assert_eq!(rc, 0);
    assert_eq!(out, "rc=137");
    // The broker reaps the orphan asynchronously after the socket EOF —
    // poll instead of sleeping a fixed grace.
    wait_for_proc("[s]leep 988", false, 3);
}

#[test]
fn test_ando_cwd_travels_as_fd() {
    enable_ando();
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
        out.ends_with(&format!("/{}/share", tawc_integration::app_package())),
        "expected bind source of /usr/share/tawc, got {out:?}"
    );
}

#[test]
fn test_ando_option_parsing_stops_at_command() {
    enable_ando();
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
    enable_ando();
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
    enable_ando();
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
    enable_ando();
    // -e extras are sent last, so they win over -E-forwarded values
    // (broker applies ENV lines last-wins) regardless of flag order.
    let (rc, out, err) =
        run("TAWC_E_VAR=guest ando -e TAWC_E_VAR=extra -E sh -c 'echo $TAWC_E_VAR'");
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "extra");
}

#[test]
fn test_ando_chdir() {
    enable_ando();
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
    enable_ando();
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
    enable_ando();
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
    // Usage errors (125) and -h (0) are handled entirely client-side
    // before any connect, so they need no broker — no enable_ando().
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
fn test_ando_socket_override_and_default_path() {
    enable_ando();
    // Empty override falls through to the built-in per-distro path,
    // which is live because ando is enabled.
    let (rc, out, _) = run("TAWC_ANDO_SOCKET= ando echo default-path-ok");
    assert_eq!(rc, 0);
    assert_eq!(out, "default-path-ok");
    // A bogus path has no node → ENOENT → the client prints the
    // ando-disabled instructions and exits 127.
    let (rc, _, err) = run("TAWC_ANDO_SOCKET=/run/no-such-ando.sock ando true");
    assert_eq!(rc, 127);
    assert!(err.contains("ando is disabled"), "stderr: {err:?}");
}

#[test]
fn test_ando_node_present_but_dead_reports_broker_not_running() {
    // The non-ENOENT connect-failure branch: a node that exists but
    // refuses (here /dev/null — connect(2) to a non-socket inode gives
    // ECONNREFUSED) means an *enabled* distro whose broker/app died,
    // so the client must keep the "is the TAWC app alive?" diagnosis,
    // not send the user to a toggle that is already on. Client-side
    // only — no broker state needed.
    let (rc, _, err) = run("TAWC_ANDO_SOCKET=/dev/null ando true");
    assert_eq!(rc, 127, "stderr: {err:?}");
    assert!(err.contains("broker not running"), "stderr: {err:?}");
    assert!(!err.contains("ando is disabled"), "stderr: {err:?}");
}

#[test]
fn test_ando_disabled_prints_enable_instructions() {
    disable_ando();
    // No per-distro bind → the default socket path doesn't resolve to
    // any node → client prints the multi-line enable instructions, 127.
    let (rc, _, err) = run("ando true");
    assert_eq!(rc, 127, "stderr: {err:?}");
    assert!(err.contains("ando is disabled for this distro"), "stderr: {err:?}");
    assert!(err.contains("Enable it in the TAWC app"), "stderr: {err:?}");
}

#[test]
fn test_ando_disabled_direct_socket_is_dead() {
    disable_ando();
    // The guest ando dir/socket is unreachable when disabled: there is
    // no bind, so the path resolves inside the rootfs where nothing
    // created it.
    let (rc, out, _) = run("test -e /run/tawc-ando/ando.sock; echo $?");
    assert_eq!(rc, 0);
    assert_eq!(out, "1", "ando socket node reachable while disabled");
    // Even naming the exact default path directly, a raw connect fails.
    let (rc, _, err) =
        run("TAWC_ANDO_SOCKET=/run/tawc-ando/ando.sock ando true");
    assert_eq!(rc, 127, "stderr: {err:?}");
    assert!(err.contains("ando is disabled"), "stderr: {err:?}");
}

#[test]
fn test_ando_disable_kills_in_flight_child() {
    enable_ando();
    // Launch an ando child that outlives this rootfs session: `setsid` +
    // full fd detach reparents the client away from the session tree, so
    // the RUNINSIDE teardown's descendant-kill can't reach it. The
    // client stays connected to the broker, keeping the Android-side
    // sleep alive.
    let (rc, _, err) =
        run("setsid ando sleep 991 </dev/null >/dev/null 2>&1 & sleep 1; echo launched");
    assert_eq!(rc, 0, "stderr: {err}");
    // Anchor ARGS to start with "sleep": since tawcroot's proctitle work
    // the guest-side `ando sleep 991` CLIENT also shows its real cmdline
    // in Android's same-uid ps, and a bare "[s]leep 991" matches both.
    // "<digit> " pins the match to the end of the PID column.
    let sleep_pat = "[0-9] [s]leep 991$";
    wait_for_proc(sleep_pat, true, 5);

    // Confirm it genuinely survived session teardown — if the client had
    // died with the session, disconnect-kill (not disable) would have
    // already reaped the sleep (within milliseconds), making a later
    // "gone" a false pass.
    std::thread::sleep(std::time::Duration::from_millis(500));
    assert_eq!(
        android_proc_count(sleep_pat),
        1,
        "ando child died with the session, before the disable under test"
    );

    // Disable: stop_listener SIGKILLs the in-flight child's pgid even
    // though the (detached) client is still connected.
    disable_ando();
    wait_for_proc(sleep_pat, false, 5);
}

#[test]
fn test_ando_reenable_next_spawn_works() {
    // Disabled first…
    disable_ando();
    let (rc, _, _) = run("ando true");
    assert_eq!(rc, 127);
    // …then enabling makes the very next spawn work (bind + listener).
    enable_ando();
    let (rc, out, err) = run("ando echo reenabled-ok");
    assert_eq!(rc, 0, "stderr: {err}");
    assert_eq!(out, "reenabled-ok");
}

#[test]
fn test_ando_legacy_shared_path_gone_even_when_enabled() {
    enable_ando();
    // The pre-per-distro shared socket path (/usr/share/tawc/ando.sock,
    // a file directly under the share dir) is never bound now — the
    // per-distro socket lives at its own /run/tawc-ando path. So it
    // fails even while ando is enabled and the real socket works.
    let (rc, _, err) = run("TAWC_ANDO_SOCKET=/usr/share/tawc/ando.sock ando true");
    assert_eq!(rc, 127, "stderr: {err:?}");
    assert!(err.contains("ando is disabled"), "stderr: {err:?}");
    // Sanity: the real per-distro path still works in the same session.
    let (rc, out, _) = run("ando echo real-path-ok");
    assert_eq!(rc, 0);
    assert_eq!(out, "real-path-ok");
}

#[test]
fn test_ando_get_reflects_override() {
    enable_ando();
    assert!(adb::get_ando().expect("get-ando"));
    disable_ando();
    assert!(!adb::get_ando().expect("get-ando"));
}
