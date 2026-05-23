//! Runtime settings tests. Clients observe settings through the compositor
//! protocol path, so these cover app/broker -> server -> Wayland behavior
//! rather than compositor-internal state queries.

use std::io::{BufRead, BufReader};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    assert_broker_ok, assert_compositor_clean, ensure_wayland_debug_app,
    start_wayland_debug_scale, start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

const GTK3_DEMO_APPLICATION_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK3_MENU_OBSERVE_TIMEOUT: Duration = Duration::from_secs(5);

// Wayland logical coordinates for the second menubar item in gtk3-demo-application.
// Keep
// both GTK3 menu tests on this exact tap: if GTK3 fixes the cold-state bug,
// the "verify broken" test should fail instead of drifting to a different
// click target.
const GTK3_DEMO_APPLICATION_MENU_X: f32 = 170.0;
const GTK3_DEMO_APPLICATION_MENU_Y: f32 = 20.0;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Gtk3Menu {
    Leftmost,
    Tapped,
}

#[derive(Clone, Copy, Debug)]
struct AnchorRect {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
}

#[derive(Clone, Copy, Debug)]
struct TouchPoint {
    x: f64,
    y: f64,
}

struct Gtk3MenuObservation {
    touch: TouchPoint,
    anchors_after_touch: Vec<AnchorRect>,
    relevant_lines: Vec<String>,
}

fn payload_for_line<'a>(line: &'a str, tag: &str) -> Option<&'a str> {
    if line == tag {
        Some("")
    } else {
        line.strip_prefix(&format!("{tag}:"))
    }
}

fn first_payload_with_tag(lines: &[String], tag: &str) -> Option<String> {
    lines
        .iter()
        .find_map(|line| payload_for_line(line, tag).map(str::to_string))
}

fn first_tag_index(lines: &[String], tag: &str) -> Option<usize> {
    lines
        .iter()
        .position(|line| payload_for_line(line, tag).is_some())
}

fn parse_i32_pair(payload: &str) -> (i32, i32) {
    let values: Vec<i32> = payload
        .split_whitespace()
        .map(str::parse)
        .collect::<Result<_, _>>()
        .unwrap_or_else(|e| panic!("parse i32 pair payload {payload:?}: {e}"));
    let [a, b] = values.as_slice() else {
        panic!("expected i32 pair payload, got {payload:?}");
    };
    (*a, *b)
}

fn first_i32_pair_with_tag(app: &DebugApp, tag: &str) -> (i32, i32) {
    let payload = app
        .payloads_with_tag(tag)
        .into_iter()
        .next()
        .unwrap_or_else(|| panic!("missing {tag} payload"));
    parse_i32_pair(&payload)
}

fn assert_first_configure_size_matches_state(app: &DebugApp, label: &str) {
    let state = compositor::query_state(TIMEOUT)
        .unwrap_or_else(|e| panic!("query compositor state for {label}: {e}"));
    let size = first_i32_pair_with_tag(app, "CONFIGURE_SIZE");
    assert!(
        size.0 > 0 && size.1 > 0,
        "{label} first configure size must be positive, got {size:?}"
    );
    assert_eq!(
        size,
        (state.output_logical_w, state.output_logical_h),
        "{label} first configure size should match compositor logical output"
    );
}

fn parse_anchor_rect(line: &str) -> Option<AnchorRect> {
    let args = line.split("set_anchor_rect(").nth(1)?.split(')').next()?;
    let values: Vec<i32> = args
        .split(',')
        .map(str::trim)
        .map(str::parse)
        .collect::<Result<_, _>>()
        .ok()?;
    let [x, y, width, height] = values.as_slice() else {
        return None;
    };
    Some(AnchorRect {
        x: *x,
        y: *y,
        width: *width,
        height: *height,
    })
}

fn parse_touch_down(line: &str) -> Option<TouchPoint> {
    if !line.contains("wl_touch#") || !line.contains(".down(") {
        return None;
    }
    let args = line.split(".down(").nth(1)?.split(')').next()?;
    let parts: Vec<&str> = args.split(',').map(str::trim).collect();
    let x = parts.get(parts.len().checked_sub(2)?)?.parse::<f64>().ok()?;
    let y = parts.get(parts.len().checked_sub(1)?)?.parse::<f64>().ok()?;
    Some(TouchPoint { x, y })
}

fn opened_menu_from_final_anchor(touch: TouchPoint, anchor: AnchorRect) -> Gtk3Menu {
    let anchor_left = f64::from(anchor.x);
    let anchor_right = f64::from(anchor.x + anchor.width);
    let anchor_top = f64::from(anchor.y);
    let anchor_bottom = f64::from(anchor.y + anchor.height);
    assert!(
        touch.y >= anchor_top && touch.y <= anchor_bottom,
        "touch y-coordinate was outside the final GTK3 menu anchor: \
         touch={touch:?} anchor={anchor:?}"
    );
    if anchor.x == 0 && touch.x > anchor_right {
        Gtk3Menu::Leftmost
    } else if anchor.x > 0 && touch.x >= anchor_left && touch.x <= anchor_right {
        Gtk3Menu::Tapped
    } else {
        panic!(
            "final GTK3 menu anchor did not match leftmost or tapped menu: \
             touch={touch:?} anchor={anchor:?}"
        );
    }
}

fn spawn_relevant_stderr_reader(stderr: std::process::ChildStderr) -> mpsc::Receiver<String> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let reader = BufReader::new(stderr);
        for line in reader.lines().map_while(Result::ok) {
            if line.contains("wl_touch#")
                || line.contains("set_anchor_rect(")
                || line.contains("wl_pointer#")
            {
                if tx.send(line).is_err() {
                    break;
                }
            }
        }
    });
    rx
}

fn launch_gtk3_demo_application_with_wayland_debug() -> (RootfsProcess, mpsc::Receiver<String>) {
    let mut app = RootfsProcess::spawn_with(
        BACKEND,
        "env GDK_BACKEND=wayland WAYLAND_DEBUG=1 gtk3-demo-application",
    )
    .expect("spawn gtk3-demo-application");
    let stderr = app
        .take_stderr()
        .expect("gtk3-demo-application stderr should be piped");
    let rx = spawn_relevant_stderr_reader(stderr);
    app.ensure_pgid();

    let deadline = std::time::Instant::now() + GTK3_DEMO_APPLICATION_LAUNCH_TIMEOUT;
    let mut painted = false;
    while std::time::Instant::now() < deadline {
        if !app.is_running() {
            app.stop().ok();
            panic!("gtk3-demo-application crashed/exited before first paint");
        }
        if let Ok(state) = compositor::query_state(TIMEOUT) {
            if state.toplevels >= 1 && (state.surfaces_wlegl + state.surfaces_shm) >= 1 {
                painted = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(200));
    }
    assert!(
        painted,
        "gtk3-demo-application did not reach first paint within {:?}",
        GTK3_DEMO_APPLICATION_LAUNCH_TIMEOUT
    );
    thread::sleep(Duration::from_secs(1));
    (app, rx)
}

fn observe_gtk3_demo_application_menu_tap(workaround_enabled: bool) -> Gtk3MenuObservation {
    assert_broker_ok(adb::set_output_scale(2.0).expect("set output scale"), "set-output-scale");
    assert_broker_ok(
        adb::set_gtk3_broken_menus_workaround(workaround_enabled)
            .unwrap_or_else(|e| panic!("set gtk3 broken menus workaround: {e}")),
        "set-gtk3-broken-menus-workaround",
    );
    adb::logcat_clear().expect("Failed to clear logcat");

    let (mut app, rx) = launch_gtk3_demo_application_with_wayland_debug();
    assert_broker_ok(
        adb::inject_touch_logical(GTK3_DEMO_APPLICATION_MENU_X, GTK3_DEMO_APPLICATION_MENU_Y)
            .expect("tap gtk3-demo-application menu"),
        "tap gtk3-demo-application menu",
    );

    let mut relevant_lines = Vec::new();
    let mut touch = None;
    let mut anchors_after_touch = Vec::new();
    let deadline = std::time::Instant::now() + GTK3_MENU_OBSERVE_TIMEOUT;
    let mut first_anchor_at = None;
    while std::time::Instant::now() < deadline {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(line) => {
                if touch.is_none() {
                    touch = parse_touch_down(&line);
                }
                if touch.is_some() {
                    if let Some(anchor) = parse_anchor_rect(&line) {
                        anchors_after_touch.push(anchor);
                        first_anchor_at.get_or_insert_with(std::time::Instant::now);
                    }
                }
                relevant_lines.push(line);
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
        if first_anchor_at
            .map(|t| t.elapsed() >= Duration::from_millis(750))
            .unwrap_or(false)
        {
            break;
        }
    }

    app.stop()
        .expect("gtk3-demo-application failed to stop cleanly");
    assert_compositor_clean();

    let touch = touch.unwrap_or_else(|| {
        panic!(
            "gtk3-demo-application did not receive the menu tap; relevant WAYLAND_DEBUG lines:\n{}",
            relevant_lines.join("\n")
        )
    });
    assert!(
        !anchors_after_touch.is_empty(),
        "gtk3-demo-application did not open a menu after touch {touch:?}; relevant WAYLAND_DEBUG lines:\n{}",
        relevant_lines.join("\n")
    );

    Gtk3MenuObservation {
        touch,
        anchors_after_touch,
        relevant_lines,
    }
}

fn assert_gtk3_demo_application_menu_tap(
    workaround_enabled: bool,
    expected_final_menu: Gtk3Menu,
) {
    let observation = observe_gtk3_demo_application_menu_tap(workaround_enabled);
    let final_anchor = *observation
        .anchors_after_touch
        .last()
        .expect("anchor list checked non-empty");
    let opened = opened_menu_from_final_anchor(observation.touch, final_anchor);
    assert_eq!(
        opened,
        expected_final_menu,
        "unexpected GTK3 menu opened after identical logical tap at ({}, {}), \
         touch={:?}, anchors_after_touch={:?}; relevant WAYLAND_DEBUG lines:\n{}",
        GTK3_DEMO_APPLICATION_MENU_X,
        GTK3_DEMO_APPLICATION_MENU_Y,
        observation.touch,
        observation.anchors_after_touch,
        observation.relevant_lines.join("\n")
    );
}

#[test]
fn test_test_init_resets_settings_and_closes_existing_clients() {
    tawc_integration::helpers::test_init();

    assert_broker_ok(adb::set_output_scale(3.25).expect("set output scale"), "set-output-scale");
    assert_broker_ok(
        adb::set_gtk3_broken_menus_workaround(false)
            .expect("set gtk3 broken menus workaround"),
        "set-gtk3-broken-menus-workaround",
    );
    assert_eq!(adb::get_output_scale().expect("get output scale"), 3.25);
    assert!(
        !adb::get_gtk3_broken_menus_workaround()
            .expect("get gtk3 broken menus workaround")
    );

    let leaked = start_wayland_debug_scale(BACKEND, "");
    tawc_integration::helpers::test_init();
    drop(leaked);

    assert_eq!(adb::get_output_scale().expect("get reset output scale"), 2.0);
    assert!(
        adb::get_gtk3_broken_menus_workaround()
            .expect("get reset gtk3 broken menus workaround")
    );

    let mut app = start_wayland_debug_scale(BACKEND, "");
    app.wait_for_tag_value("SCALE_CHANGED", "2.00", TIMEOUT)
        .expect("new client should observe reset factory output scale");
    app.stop().expect("scale debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_fractional_scale_updates_reach_wayland_client() {
    tawc_integration::helpers::test_init();
    assert_broker_ok(adb::set_output_scale(2.25).expect("set output scale"), "set-output-scale");

    let mut app = start_wayland_debug_scale(BACKEND, "");
    app.wait_for("SCALE_CHANGED", TIMEOUT)
        .expect("initial fractional scale event");

    for step in 0..=6 {
        let scale = 0.5 + step as f32 * 0.25;
        set_scale_and_expect(&app, scale);
    }

    app.stop().expect("scale debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_initial_configure_waits_for_real_host_size() {
    tawc_integration::helpers::test_init();
    assert_broker_ok(adb::set_output_scale(2.0).expect("set output scale"), "set-output-scale");
    let binary = ensure_wayland_debug_app();
    let state_before = compositor::query_state(TIMEOUT)
        .expect("query compositor state before initial configure test");

    let mut app = DebugApp::start(BACKEND, &binary, "initial-configure", "")
        .expect("start initial-configure debug app");
    app.wait_for("INITIAL_OUTPUT_GLOBALS", TIMEOUT)
        .expect("initial registry marker");
    app.wait_for_tag_value("CONFIGURE_STATE", "maximized", TIMEOUT)
        .expect("initial configure should include maximized state");
    app.wait_for_tag_value("CONFIGURE_ACTIVE", "1", TIMEOUT)
        .expect("initial configure should include pending active focus state");
    app.wait_for_tag_value("DECORATION_MODE", "server-side", TIMEOUT)
        .expect("initial configure should include pending server-side decoration");
    app.wait_for_tag_count("CONFIGURE_BOUNDS", 1, TIMEOUT)
        .expect("initial configure should include bounds");
    app.wait_ready()
        .expect("initial-configure debug app did not become ready");

    let lines = app.lines();
    let initial_idx = first_tag_index(&lines, "INITIAL_OUTPUT_GLOBALS")
        .expect("missing INITIAL_OUTPUT_GLOBALS");
    let configure_idx = first_tag_index(&lines, "CONFIGURE_SIZE")
        .expect("missing CONFIGURE_SIZE");
    assert!(
        initial_idx < configure_idx,
        "client received a toplevel configure before the pre-Activity registry phase: {lines:?}"
    );

    let initial_output_globals = first_payload_with_tag(&lines, "INITIAL_OUTPUT_GLOBALS")
        .expect("missing INITIAL_OUTPUT_GLOBALS payload")
        .parse::<u32>()
        .expect("parse INITIAL_OUTPUT_GLOBALS payload");
    if !state_before.output_advertised {
        assert_eq!(
            initial_output_globals, 0,
            "fresh compositor should not advertise wl_output before Activity surface registration"
        );
        let output_global_idx = first_tag_index(&lines, "OUTPUT_GLOBAL")
            .expect("missing post-registration OUTPUT_GLOBAL");
        assert!(
            initial_idx < output_global_idx,
            "wl_output global arrived before the pre-Activity registry phase: {lines:?}"
        );
    }

    let state = compositor::query_state(TIMEOUT)
        .expect("query compositor state after initial configure");
    assert!(state.output_advertised, "output should be advertised after host size");
    let configure_size = first_i32_pair_with_tag(&app, "CONFIGURE_SIZE");
    let configure_bounds = first_i32_pair_with_tag(&app, "CONFIGURE_BOUNDS");
    assert_eq!(
        configure_size,
        (state.output_logical_w, state.output_logical_h),
        "first configure should use the Activity logical size"
    );
    assert_eq!(
        configure_bounds,
        (state.output_logical_w, state.output_logical_h),
        "first configure bounds should use the Activity logical size"
    );

    app.stop()
        .expect("initial-configure debug app failed to stop cleanly");
    assert_compositor_clean();
}

fn set_scale_and_expect(app: &tawc_integration::debug_app::DebugApp, scale: f32) {
    let before = app.count_with_tag("SCALE_CHANGED");
    let expected = format!("{scale:.2}");
    assert_broker_ok(adb::set_output_scale(scale).expect("set output scale"), "set-output-scale");
    app.wait_for_tag_count("SCALE_CHANGED", before + 1, Duration::from_secs(5))
        .unwrap_or_else(|e| panic!("client did not observe {expected}x scale: {e}"));
    let last = app
        .payloads_with_tag("SCALE_CHANGED")
        .last()
        .cloned()
        .unwrap_or_default();
    assert_eq!(last, expected, "latest client scale event");
}

#[test]
fn test_xdg_configure_state_maximized_vs_fullscreen() {
    tawc_integration::helpers::test_init();
    let binary = ensure_wayland_debug_app();

    let mut normal = DebugApp::start(BACKEND, &binary, "scale", "")
        .expect("start non-fullscreen debug app");
    normal
        .wait_for_tag_value("CONFIGURE_STATE", "maximized", TIMEOUT)
        .expect("non-fullscreen app should be configured maximized");
    normal
        .wait_for_tag_value("CONFIGURE_ACTIVE", "1", TIMEOUT)
        .expect("non-fullscreen app should be configured active");
    assert_first_configure_size_matches_state(&normal, "non-fullscreen app");
    assert!(
        normal.payloads_with_tag("CONFIGURE_STATE").iter().all(|s| s != "fullscreen"),
        "non-fullscreen app received fullscreen configure"
    );
    normal.stop().expect("normal debug app failed to stop cleanly");
    assert_compositor_clean();

    let mut fullscreen = start_wayland_debug_touch(BACKEND, "");
    fullscreen
        .wait_for_tag_value("CONFIGURE_STATE", "fullscreen", TIMEOUT)
        .expect("fullscreen-requesting app should be configured fullscreen");
    fullscreen
        .wait_for_tag_value("CONFIGURE_ACTIVE", "1", TIMEOUT)
        .expect("fullscreen-requesting app should be configured active");
    assert_first_configure_size_matches_state(&fullscreen, "fullscreen-requesting app");
    fullscreen
        .stop()
        .expect("fullscreen debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_gtk3_demo_application_menu_opens_leftmost_without_workaround() {
    tawc_integration::helpers::test_init();
    assert_gtk3_demo_application_menu_tap(false, Gtk3Menu::Leftmost);
}

#[test]
fn test_gtk3_demo_application_menu_opens_tapped_menu_with_workaround() {
    tawc_integration::helpers::test_init();
    assert_gtk3_demo_application_menu_tap(true, Gtk3Menu::Tapped);
}
