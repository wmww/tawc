//! Launch splash (notes/launcher.md "Launch splash"): a launcher tap opens
//! its task at once and the program's first window maps into that same
//! host. Driven through the debug `launcher-launch` action (the tap path)
//! with fixture `.desktop` entries in the managed dir, observed through
//! `launch-state` and `query-state`'s `pending_launches`/`host_windows`.

use std::cell::RefCell;
use std::time::{Duration, Instant};

use tawc_integration::compositor::{self, CompositorState};
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{self, assert_broker_ok};
use tawc_integration::{adb, GraphicsBackend};

const APPS_DIR: &str = "root/.local/share/applications";
const WAYLAND_APP: &str = "/usr/local/bin/wayland-debug-app";
const X11_APP: &str = "/usr/local/bin/x11-debug-app";
/// Long enough for a cold compositor plus Xwayland start on the emulator.
const SHOWN_TIMEOUT: Duration = Duration::from_secs(30);

fn rootfs() -> String {
    format!(
        "{}/distros/{}/rootfs",
        tawc_integration::app_data_dir(),
        tawc_integration::install_id()
    )
}

/// Plants entries, tracks launches, and on drop closes their splash
/// tasks, kills what they started and removes the entries.
struct Fixture {
    ids: Vec<&'static str>,
    launches: RefCell<Vec<String>>,
}

impl Fixture {
    fn new() -> Self {
        helpers::test_init();
        Self { ids: Vec::new(), launches: RefCell::new(Vec::new()) }
    }

    /// A GUI entry running `exec` through `sh -c` (no single quotes in it).
    fn plant(&mut self, id: &'static str, exec: &str) {
        let path = format!("{}/{APPS_DIR}/{id}.desktop", rootfs());
        let plant = format!(
            "mkdir -p \"$(dirname '{path}')\" && printf '%s\\n' \
             '[Desktop Entry]' 'Type=Application' 'Name=Splash {id}' \
             'Exec=sh -c \"{exec}\"' > '{path}'"
        );
        assert_broker_ok(
            adb::rootfs_host_exec(&["/system/bin/sh", "-c", &plant]).expect("plant .desktop"),
            "plant .desktop",
        );
        self.ids.push(id);
    }

    fn launch(&self, id: &str, timeout_ms: Option<u64>) -> String {
        let launch = adb::launcher_launch(id, timeout_ms).expect("launcher-launch");
        assert!(launch.starts_with("launch-"), "unexpected launch id {launch:?}");
        self.launches.borrow_mut().push(launch.clone());
        launch
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        for launch in self.launches.borrow().iter() {
            if let Some(task) = adb::launch_state(launch).ok().and_then(|s| task_id(&s)) {
                remove_task(task);
            }
        }
        let _ = adb::cleanup_rootfs();
        for id in &self.ids {
            let _ = adb::set_entry_pointer(id, "");
            let rm = format!("rm -f '{}/{APPS_DIR}/{id}.desktop'", rootfs());
            let _ = adb::rootfs_host_exec(&["/system/bin/sh", "-c", &rm]);
        }
    }
}

fn state_name(json: &str) -> String {
    adb::json_str(json, "state").unwrap_or_default()
}

fn task_id(json: &str) -> Option<i64> {
    let start = json.find("\"taskId\":")? + "\"taskId\":".len();
    let end = json[start..].find(|c: char| c != '-' && !c.is_ascii_digit())? + start;
    json[start..end].parse().ok().filter(|id| *id >= 0)
}

/// `am stack remove` on the task: the recents swipe.
fn remove_task(task: i64) {
    let _ = adb::shell(&format!("am stack remove {task}"));
}

/// Poll `launch-state` until its state is `want`; returns the JSON.
fn wait_launch_state(launch: &str, want: &str, timeout: Duration) -> String {
    let deadline = Instant::now() + timeout;
    loop {
        let json = adb::launch_state(launch).expect("launch-state");
        if state_name(&json) == want {
            return json;
        }
        assert!(Instant::now() < deadline, "launch {launch} never reached {want:?}; last {json}");
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn wait_compositor(what: &str, mut pred: impl FnMut(&CompositorState) -> bool) -> CompositorState {
    let deadline = Instant::now() + helpers::TIMEOUT;
    loop {
        let state = compositor::query_state(Duration::from_secs(2)).expect("query-state");
        if pred(&state) {
            return state;
        }
        assert!(Instant::now() < deadline, "timed out waiting for {what}; last {state:?}");
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// The launch's window is on its own host and nothing else is up: no
/// second Activity was spawned, and the reservation is spent.
fn assert_window_on_launch_host(launch: &str) {
    wait_compositor("one window on the launch host only", |s| {
        s.hosts == 1 && s.host_windows(launch) == Some(1) && s.pending_launches == 0
    });
    // A wrongly spawned Activity would register its host a moment later.
    std::thread::sleep(Duration::from_millis(500));
    let state = compositor::query_state(Duration::from_secs(2)).expect("query-state");
    assert_eq!(state.hosts, 1, "extra host appeared: {state:?}");
}

#[test]
fn test_wayland_window_maps_into_splash_task() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-wl", &format!("sleep 1; exec {WAYLAND_APP} render-pattern"));
    let launch = fx.launch("tawc-splash-wl", None);
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
}

/// A wrapper that forks the real program still matches: same session.
#[test]
fn test_wrapper_script_matches_by_session() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-wrap", &format!("{WAYLAND_APP} render-pattern & wait"));
    let launch = fx.launch("tawc-splash-wrap", None);
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
}

/// A launcher script that backgrounds the program and exits 0 at once
/// leaves the program in its session: keep waiting, don't quit.
#[test]
fn test_backgrounding_launcher_keeps_waiting() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-bg", &format!("(sleep 1; exec {WAYLAND_APP} render-pattern) &"));
    let launch = fx.launch("tawc-splash-bg", None);
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
}

/// X11 matches by `_NET_WM_PID`'s session (the Wayland client is Xwayland).
#[test]
fn test_x11_window_maps_into_splash_task() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-x11", &format!("DISPLAY=:0 exec {X11_APP} window"));
    let launch = fx.launch("tawc-splash-x11", None);
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
}

/// Tap the launched window and return the pointer emulation mode the
/// touch resolved to.
fn touch_emulation_on(launch: &str) -> String {
    wait_launch_state(launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(launch);
    let before = compositor::query_state(Duration::from_secs(2)).expect("query-state").touch_downs;
    assert_broker_ok(adb::inject_touch("tap").expect("inject-touch"), "inject-touch");
    wait_compositor("touch resolved", |s| s.touch_downs > before).last_touch_emulation
}

/// The editor's per-entry override reaches the launched window (session
/// match); entries without one follow the global setting.
#[test]
fn test_entry_pointer_emulation_override() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-ptr", &format!("exec {WAYLAND_APP} render-pattern"));
    assert_broker_ok(adb::set_entry_pointer("tawc-splash-ptr", "full").expect("set-entry-pointer"), "set-entry-pointer");
    let launch = fx.launch("tawc-splash-ptr", None);
    assert_eq!(touch_emulation_on(&launch), "full");
}

#[test]
fn test_entry_without_pointer_override_uses_global() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-noptr", &format!("exec {WAYLAND_APP} render-pattern"));
    let launch = fx.launch("tawc-splash-noptr", None);
    assert_eq!(touch_emulation_on(&launch), "hover");
}

/// X11 windows all belong to Xwayland's client, so the override follows
/// the matched launch host.
#[test]
fn test_x11_entry_pointer_emulation_override() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-x11ptr", &format!("DISPLAY=:0 exec {X11_APP} window"));
    assert_broker_ok(adb::set_entry_pointer("tawc-splash-x11ptr", "none").expect("set-entry-pointer"), "set-entry-pointer");
    let launch = fx.launch("tawc-splash-x11ptr", None);
    assert_eq!(touch_emulation_on(&launch), "none");
}

#[test]
fn test_failed_launch_shows_log_and_code() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-boom", "echo boom >&2; exit 3");
    let launch = fx.launch("tawc-splash-boom", None);
    let json = wait_launch_state(&launch, "exited", SHOWN_TIMEOUT);
    assert!(json.contains("\"code\":3"), "exit code missing: {json}");
    assert!(json.contains("boom"), "stderr missing from log: {json}");
}

/// After the timeout the log shows, the reservation stays, and a window
/// the same session opens later still lands in the splash task.
#[test]
fn test_timeout_then_late_window_still_matches() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-late", &format!("echo waiting; sleep 2; exec {WAYLAND_APP} render-pattern"));
    let launch = fx.launch("tawc-splash-late", Some(500));
    let json = wait_launch_state(&launch, "timedout", SHOWN_TIMEOUT);
    assert!(json.contains("waiting"), "live log missing: {json}");
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
}

/// A program that hands the activation token to an already-running
/// instance and exits: that instance's window moves into the splash task
/// and its own Activity goes away.
#[test]
fn test_activation_token_handoff() {
    let mut fx = Fixture::new();
    let fifo = "/root/tawc-activation.fifo";
    let mut running = DebugApp::start(
        GraphicsBackend::Cpu,
        WAYLAND_APP,
        &format!("activation-listener {fifo}"),
        "",
    )
    .expect("start activation listener");
    running.wait_ready().expect("listener ready");
    let first = wait_compositor("the running instance's own host", |s| s.hosts == 1 && s.toplevels == 1);

    fx.plant("tawc-splash-handoff", &format!("echo $XDG_ACTIVATION_TOKEN > {fifo}"));
    let launch = fx.launch("tawc-splash-handoff", None);
    running.wait_for("ACTIVATED", SHOWN_TIMEOUT).expect("token handed to the running instance");
    wait_launch_state(&launch, "shown", SHOWN_TIMEOUT);
    assert_window_on_launch_host(&launch);
    assert_ne!(first.host_windows, format!("{launch}:1"), "window was never elsewhere");
    let _ = running.stop();
}

/// Swiping the splash away drops the reservation, so the compositor may
/// idle-stop again; the program keeps running.
#[test]
fn test_swiping_splash_releases_reservation() {
    let mut fx = Fixture::new();
    fx.plant("tawc-splash-swipe", "sleep 999");
    let launch = fx.launch("tawc-splash-swipe", None);
    wait_compositor("the reserved splash host", |s| s.pending_launches == 1 && s.host_windows(&launch) == Some(0));
    let json = wait_launch_state(&launch, "waiting", SHOWN_TIMEOUT);
    remove_task(task_id(&json).expect("splash task id"));
    wait_launch_state(&launch, "released", helpers::TIMEOUT);
    wait_compositor("the reservation released", |s| s.pending_launches == 0 && s.hosts == 0);
}
