//! Socket-activated compositor lifecycle and the session holds around it.
//!
//! The suite runs with the compositor pinned (`compositor-hold`); each
//! test here drops the pin for its duration so the production rule —
//! start on first connection, stop a second after the last client — is
//! what runs.

use std::time::{Duration, Instant};

use tawc_integration::helpers::{
    app_fd_targets, close_home_terminal, ensure_wayland_debug_app, fd_growth, has_shm_surface,
    show_home_apps,
    show_home_terminal, terminal_run, wait_for_rootfs_file, TIMEOUT,
};
use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

/// Idle grace (1 s) plus slack for teardown and a slow device.
const STOP_TIMEOUT: Duration = Duration::from_secs(6);
/// Xwayland lingers 5 s after its last client before the rule can apply.
const X11_STOP_TIMEOUT: Duration = Duration::from_secs(20);
const START_TIMEOUT: Duration = Duration::from_secs(15);

/// Unpinned for the test, re-pinned (and so restarted) afterwards even
/// on panic, so later tests find the compositor they expect.
struct Unpinned;

impl Unpinned {
    fn new() -> Self {
        tawc_integration::helpers::test_init();
        let guard = Unpinned;
        adb::compositor_hold(false).expect("compositor-hold off");
        compositor::wait_for_stopped(X11_STOP_TIMEOUT).expect("idle compositor should stop");
        guard
    }
}

impl Drop for Unpinned {
    fn drop(&mut self) {
        let _ = adb::compositor_hold(true);
    }
}

fn wait_for_clients(min: u32, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    loop {
        if let Ok(state) = compositor::query_state_once() {
            if state.clients >= min {
                return;
            }
        }
        assert!(Instant::now() < deadline, "no client reached the compositor in {timeout:?}");
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// `(fds, threads)` of the app process, read by a child of it.
fn app_fds_and_threads() -> (Vec<String>, u32) {
    let out = adb::host_sh("ls /proc/$PPID/task | wc -l").expect("host-sh thread count");
    let threads = String::from_utf8_lossy(&out.stdout).trim().parse().expect("thread count");
    (app_fd_targets(), threads)
}

/// A terminal-style spawn — nothing warms the compositor up — of a
/// Wayland client starts it; it stops after the client leaves; and a
/// second client starts it again.
#[test]
fn test_wayland_client_starts_and_idle_stops_compositor() {
    let binary = ensure_wayland_debug_app();
    let _unpinned = Unpinned::new();

    for round in 0..2 {
        let mut app = RootfsProcess::spawn_with(BACKEND, &format!("{binary} scale"))
            .expect("spawn debug app");
        wait_for_clients(1, START_TIMEOUT);
        let deadline = Instant::now() + START_TIMEOUT;
        while !has_shm_surface() {
            assert!(app.is_running(), "debug app exited before rendering (round {round})");
            assert!(Instant::now() < deadline, "debug app never rendered (round {round})");
            std::thread::sleep(Duration::from_millis(100));
        }
        let reasons = adb::session_state().expect("session-state");
        assert!(
            reasons.iter().any(|r| r.starts_with("compositor ")),
            "running compositor should hold a session reason, got {reasons:?}"
        );
        app.stop().expect("debug app failed to stop cleanly");
        compositor::wait_for_stopped(STOP_TIMEOUT)
            .unwrap_or_else(|e| panic!("round {round}: {e}"));
        let reasons = adb::session_state().expect("session-state");
        assert!(
            !reasons.iter().any(|r| r.starts_with("compositor ")),
            "stopped compositor should release its session reason, got {reasons:?}"
        );
    }
}

/// An X11-only client connects to `:0` and nothing else; the holder must
/// watch that socket too.
#[test]
#[cfg_attr(tawc_skip_libhybris_on_target, ignore = "xwayland skipped on x86 device")]
fn test_x11_client_starts_and_idle_stops_compositor() {
    let _unpinned = Unpinned::new();

    let mut app = RootfsProcess::spawn_with(
        BACKEND,
        "env -u WAYLAND_DISPLAY DISPLAY=:0 xclock -update 1",
    )
    .expect("spawn xclock");
    let deadline = Instant::now() + START_TIMEOUT;
    while !has_shm_surface() {
        assert!(app.is_running(), "xclock exited before rendering");
        assert!(Instant::now() < deadline, "xclock never rendered via a lazily started compositor");
        std::thread::sleep(Duration::from_millis(100));
    }
    app.stop().expect("xclock failed to stop cleanly");
    compositor::wait_for_stopped(X11_STOP_TIMEOUT).expect("compositor should stop after Xwayland");

    // `:0` is listening again: a second X11 client restarts everything.
    let mut again = RootfsProcess::spawn_with(
        BACKEND,
        "env -u WAYLAND_DISPLAY DISPLAY=:0 xclock -update 1",
    )
    .expect("spawn second xclock");
    let deadline = Instant::now() + START_TIMEOUT;
    while !has_shm_surface() {
        assert!(again.is_running(), "second xclock exited before rendering");
        assert!(Instant::now() < deadline, "second xclock never rendered");
        std::thread::sleep(Duration::from_millis(100));
    }
    again.stop().expect("second xclock failed to stop cleanly");
}

/// Start/stop cycles must not leak: every run's Display, client fds, GL
/// context and helper threads go away with it.
#[test]
fn test_compositor_cycles_do_not_leak() {
    let _unpinned = Unpinned::new();
    const CONNECT: &str = "python3 -c 'import socket; s = socket.socket(socket.AF_UNIX); \
        s.connect(\"/usr/share/tawc/wayland-0\"); s.close()'";

    let cycle = || {
        let out = adb::rootfs_run_with(BACKEND, CONNECT).expect("connect to wayland-0");
        assert!(
            out.status.success(),
            "connection refused by an idle compositor socket: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        compositor::wait_for_stopped(STOP_TIMEOUT).expect("compositor should stop after cycle");
    };
    // Warm-up: one-time lazy initialisation is not a leak.
    cycle();
    cycle();
    let (fds_before, threads_before) = app_fds_and_threads();
    for _ in 0..8 {
        cycle();
    }
    let (fds_after, threads_after) = app_fds_and_threads();
    assert!(
        fds_after.len() <= fds_before.len() + 2,
        "fds grew over 8 compositor cycles: {} -> {} ({})",
        fds_before.len(),
        fds_after.len(),
        fd_growth(&fds_before, &fds_after)
    );
    assert!(
        threads_after <= threads_before + 2,
        "threads grew over 8 compositor cycles: {threads_before} -> {threads_after}"
    );
}

/// Holds released and re-acquired back to back: the session service's
/// stop must never swallow a pending `startForegroundService`. Android
/// answers that with `ForegroundServiceDidNotStartInTimeException`, which
/// kills the app process and every guest with it. Each short command is
/// one acquire/release, and each compositor cycle releases the
/// compositor hold right as the next command acquires.
#[test]
fn test_session_service_survives_hold_churn() {
    let _unpinned = Unpinned::new();
    let app_pid = || {
        let out = adb::host_sh("echo $PPID").expect("host-sh pid");
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    };
    let pid_before = app_pid();
    assert!(!pid_before.is_empty(), "could not read app pid");

    const CONNECT: &str = "python3 -c 'import socket; s = socket.socket(socket.AF_UNIX); \
        s.connect(\"/usr/share/tawc/wayland-0\"); s.close()'";
    for i in 0..12 {
        for _ in 0..3 {
            let out = adb::rootfs_run_with(BACKEND, "true").expect("run true");
            assert!(out.status.success(), "iteration {i}: `true` failed — app died?");
        }
        let out = adb::rootfs_run_with(BACKEND, CONNECT).expect("connect");
        assert!(out.status.success(), "iteration {i}: connect failed — app died?");
        // Land the next acquire right around the compositor's auto-stop.
        std::thread::sleep(Duration::from_millis(900 + (i * 25) as u64));
    }
    assert_eq!(pid_before, app_pid(), "app process was restarted during hold churn");
}

/// A rootfs command holds a session reason exactly while it runs, and
/// Exit kills it — and a detached child that holds nothing — outright.
#[test]
fn test_session_holds_follow_commands_and_exit_kills_everything() {
    let _unpinned = Unpinned::new();

    let mut cmd = RootfsProcess::spawn_with(
        BACKEND,
        "setsid sleep 3917 >/dev/null 2>&1 & exec sleep 3918",
    )
    .expect("spawn sleeper");
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let reasons = adb::session_state().expect("session-state");
        if reasons.iter().any(|r| r.starts_with("command ")) {
            break;
        }
        assert!(Instant::now() < deadline, "command never held a session reason: {reasons:?}");
        std::thread::sleep(Duration::from_millis(50));
    }

    // Both must really be running first, or "gone after Exit" proves
    // nothing: a shell that exits at once can beat `setsid` to its fork.
    let sleepers = || {
        let ps = adb::host_sh("ps -A -o ARGS | grep -E 'sleep 391[78]' | grep -v grep; true")
            .expect("ps");
        String::from_utf8_lossy(&ps.stdout).trim().to_string()
    };
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let alive = sleepers();
        if alive.contains("sleep 3917") && alive.contains("sleep 3918") {
            break;
        }
        assert!(Instant::now() < deadline, "sleepers never both started: {alive:?}");
        std::thread::sleep(Duration::from_millis(50));
    }

    adb::session_exit().expect("session-exit");
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let alive = sleepers();
        let reasons = adb::session_state().expect("session-state");
        if alive.is_empty() && reasons.is_empty() {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "exit left things alive: processes={alive:?} reasons={reasons:?}"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
    let _ = cmd.stop();
}

/// `am stack remove` on MainActivity's root task: the recents swipe.
fn remove_main_task() {
    let out = adb::shell("am stack list").expect("am stack list");
    let list = String::from_utf8_lossy(&out.stdout);
    let mut root = None;
    let mut found = None;
    for line in list.lines() {
        if let Some(rest) = line.trim().strip_prefix("RootTask id=") {
            root = rest.split_whitespace().next().map(str::to_string);
        } else if line.contains(&tawc_integration::main_activity()) {
            found = root.clone();
        }
    }
    let id = found.expect("MainActivity task");
    adb::shell(&format!("am stack remove {id}")).expect("am stack remove");
}

/// Swiping the home screen away hangs up its shells like closing
/// desktop terminal windows: a plain background job dies, a `nohup`
/// one survives and keeps the service up as a stray until Exit.
#[test]
fn test_swipe_hangs_up_terminals() {
    let _unpinned = Unpinned::new();
    show_home_terminal();
    let wait_until = |what: &str, f: &mut dyn FnMut() -> bool| {
        let deadline = Instant::now() + Duration::from_secs(30);
        while !f() {
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            std::thread::sleep(Duration::from_millis(250));
        }
    };
    let state = || adb::terminal_state().expect("terminal-state");

    let sleepers = || {
        let ps = adb::host_sh("ps -A -o ARGS | grep -E 'sleep 392[12]' | grep -v grep; true").expect("ps");
        String::from_utf8_lossy(&ps.stdout).trim().to_string()
    };
    for line in ["sleep%s3921%s&", "nohup%ssleep%s3922%s>/dev/null%s2>&1%s&"] {
        adb::shell(&format!("input text '{line}'")).expect("input text");
        adb::shell("input keyevent 66").expect("enter");
    }
    wait_until("both sleepers", &mut || {
        let s = sleepers();
        s.contains("sleep 3921") && s.contains("sleep 3922")
    });

    remove_main_task();
    wait_until("the shell to close", &mut || state() == "tabs:0 selected:none");
    wait_until("the plain job to die", &mut || !sleepers().contains("sleep 3921"));
    assert!(sleepers().contains("sleep 3922"), "nohup child died with the shell");
    let reasons = adb::session_state().expect("session-state");
    assert!(reasons.is_empty(), "hung-up shell still holds: {reasons:?}");
    assert!(adb::session_service_running().expect("service state"), "stray tail should keep the service up");

    adb::session_exit().expect("session-exit");
    wait_until("the service to stop", &mut || !adb::session_service_running().expect("service state"));
    assert!(sleepers().is_empty(), "exit left {}", sleepers());

    // Later tests expect a TAWC activity in front: compositor windows
    // can't launch from the background.
    show_home_apps();
}

/// `wl-copy` from a cold terminal starts the compositor, which mirrors
/// the text into Android, takes the selection over from the surfaceless
/// daemon (which then exits) and stops; `wl-paste` restarts it and reads
/// the text back from Android.
#[test]
fn test_wl_copy_survives_compositor_stop() {
    let _unpinned = Unpinned::new();
    let _ = adb::rootfs_run_with(BACKEND, "rm -f /tmp/tawc-wlp-lazy*");
    show_home_terminal();

    terminal_run("wl-copy%slazy-wl-copy");
    let deadline = Instant::now() + START_TIMEOUT;
    while adb::clipboard_get_text().expect("get Android clipboard") != "lazy-wl-copy" {
        assert!(Instant::now() < deadline, "wl-copy text never reached Android");
        std::thread::sleep(Duration::from_millis(100));
    }
    compositor::wait_for_stopped(STOP_TIMEOUT).expect("compositor should stop after wl-copy");

    terminal_run("wl-paste%s-n>/tmp/tawc-wlp-lazy;touch%s/tmp/tawc-wlp-lazy.done");
    wait_for_rootfs_file(BACKEND, "/tmp/tawc-wlp-lazy.done", START_TIMEOUT);
    assert_eq!(wait_for_rootfs_file(BACKEND, "/tmp/tawc-wlp-lazy", TIMEOUT), "lazy-wl-copy");
    let _ = adb::rootfs_run_with(BACKEND, "rm -f /tmp/tawc-wlp-lazy*");
    close_home_terminal();
}

/// "Keep awake" holds `tawc:session` only while on; off, Exit and the
/// service stopping all drop it, and a new service starts released.
#[test]
fn test_session_wake_follows_toggle_and_exit() {
    let _unpinned = Unpinned::new();
    let wake_lock_held = || {
        let out = adb::shell("dumpsys power").expect("dumpsys power");
        String::from_utf8_lossy(&out.stdout)
            .lines()
            .any(|l| l.contains("PARTIAL_WAKE_LOCK") && l.contains("'tawc:session'"))
    };
    let wait_lock = |want: bool| {
        let deadline = Instant::now() + Duration::from_secs(10);
        while wake_lock_held() != want {
            assert!(Instant::now() < deadline, "tawc:session held != {want}");
            std::thread::sleep(Duration::from_millis(100));
        }
    };

    let mut cmd = RootfsProcess::spawn_with(BACKEND, "exec sleep 3919").expect("spawn sleeper");
    let deadline = Instant::now() + TIMEOUT;
    while adb::session_wake(None).expect("session-wake") != "released" {
        assert!(Instant::now() < deadline, "session service never came up released");
        std::thread::sleep(Duration::from_millis(50));
    }
    assert!(!wake_lock_held(), "lock held before the toggle");

    assert_eq!(adb::session_wake(Some(true)).expect("wake on"), "held");
    wait_lock(true);
    assert_eq!(adb::session_wake(Some(false)).expect("wake off"), "released");
    wait_lock(false);

    assert_eq!(adb::session_wake(Some(true)).expect("wake on"), "held");
    wait_lock(true);
    adb::session_exit().expect("session-exit");
    wait_lock(false);
    let deadline = Instant::now() + Duration::from_secs(20);
    while adb::session_service_running().expect("dumpsys") {
        assert!(Instant::now() < deadline, "session service stayed up after Exit");
        std::thread::sleep(Duration::from_millis(250));
    }
    assert_eq!(adb::session_wake(Some(true)).expect("wake on"), "unavailable");
    let _ = cmd.stop();

    // A new service lifetime starts released.
    let out = adb::rootfs_run_with(BACKEND, "true").expect("run true");
    assert!(out.status.success(), "`true` failed: {out:?}");
    assert!(!wake_lock_held(), "lock carried over into a new service");
}
