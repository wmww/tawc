//! Window placement: oversized windows are fit-scaled and centered, small
//! ones centered, dialogs centered over a dimmed parent, and input lands
//! where the user sees each window. Layout is read from `query-state`'s
//! `windows=` placements; pixels and client-reported coordinates check that
//! rendering and input agree with it.

use std::time::Duration;

use tawc_integration::adb;
use tawc_integration::compositor::{self, CompositorState, WindowPlacement};
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{assert_compositor_clean, start_wayland_debug_scene, TIMEOUT};
use tawc_integration::GraphicsBackend;

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const ENV: &str = "";

/// wayland-debug-app's fixed sizes.
const SMALL_W: f64 = 200.0;
const SMALL_H: f64 = 150.0;
const DIALOG_W: f64 = 240.0;
const DIALOG_H: f64 = 180.0;
const POPUP_H: i32 = 140;

fn with_scene(scene: &str, run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_scene(BACKEND, ENV, scene);
    run(&app);
    app.stop()
        .unwrap_or_else(|e| panic!("{scene} app crashed or failed to stop cleanly: {e}"));
    assert_compositor_clean();
}

fn host_size(state: &CompositorState) -> (f64, f64) {
    (state.output_logical_w as f64, state.output_logical_h as f64)
}

/// Wait until the visible host shows `count` windows and the one with `role`
/// has committed a `w`×`h` geometry.
fn wait_for_window(count: usize, role: &str, size: (i32, i32)) -> (CompositorState, WindowPlacement) {
    let deadline = std::time::Instant::now() + TIMEOUT;
    loop {
        let state = compositor::query_state(TIMEOUT).expect("query compositor state");
        if let Some(window) = state.window_with_role(role) {
            if state.windows.len() == count && (window.geometry.2, window.geometry.3) == size {
                let window = window.clone();
                return (state, window);
            }
        }
        if std::time::Instant::now() > deadline {
            panic!("no {size:?} {role} window among {count}: {state:?}");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

fn assert_close(actual: f64, expected: f64, tolerance: f64, label: &str) {
    assert!(
        (actual - expected).abs() <= tolerance,
        "{label}: got {actual:.2}, expected {expected:.2}"
    );
}

/// The fit + center rule, checked against the compositor's placement.
fn assert_fit_centered(window: &WindowPlacement, state: &CompositorState) {
    let (w, h) = host_size(state);
    let (gx, gy, gw, gh) = window.geometry;
    let (gw, gh) = (gw as f64, gh as f64);
    let scale = (w / gw).min(h / gh).min(1.0);
    assert_close(window.scale, scale, 0.001, "placement scale");
    assert_close(window.offset.0, (w - gw * scale) / 2.0 - gx as f64 * scale, 0.01, "offset x");
    assert_close(window.offset.1, (h - gh * scale) / 2.0 - gy as f64 * scale, 0.01, "offset y");
}

#[derive(Clone, Copy, Debug)]
struct Rgb {
    r: u8,
    g: u8,
    b: u8,
}

fn sample(shot: &adb::RawScreenshot, state: &CompositorState, (x, y): (f64, f64)) -> Rgb {
    let px = |logical: f64, logical_extent: i32, physical_extent: i32| {
        ((logical * physical_extent as f64 / logical_extent as f64).round() as i32)
            .clamp(0, physical_extent - 1) as u32
    };
    let x = px(x, state.output_logical_w, state.output_physical_w);
    let y = px(y, state.output_logical_h, state.output_physical_h);
    let [r, g, b, _] = shot
        .pixel_rgba(x, y)
        .unwrap_or_else(|| panic!("screenshot missing pixel at physical {x},{y}"));
    Rgb { r, g, b }
}

/// A screenshot of the fullscreen host with the state it shows. Retries
/// while the Activity is still settling into fullscreen.
fn screenshot(
    count: usize,
    role: &str,
    size: impl Fn(&CompositorState) -> (i32, i32),
) -> (CompositorState, WindowPlacement, adb::RawScreenshot) {
    let deadline = std::time::Instant::now() + TIMEOUT;
    loop {
        let state = compositor::query_state(TIMEOUT).expect("query compositor state");
        let (state, window) = wait_for_window(count, role, size(&state));
        // Let the last commit reach the screen.
        std::thread::sleep(Duration::from_millis(250));
        let shot = adb::screencap_raw().expect("raw screencap");
        let after = compositor::query_state(TIMEOUT).expect("query compositor state");
        let settled = (shot.width as i32, shot.height as i32)
            == (state.output_physical_w, state.output_physical_h)
            && after.windows == state.windows;
        if settled {
            return (state, window, shot);
        }
        if std::time::Instant::now() > deadline {
            panic!(
                "screencap {}x{} never matched the host: {state:?}",
                shot.width, shot.height
            );
        }
    }
}

/// The compositor's empty background (#1B1B22).
fn assert_background(c: Rgb, label: &str) {
    assert!(c.r < 60 && c.g < 60 && c.b < 70, "{label} should be background: {c:?}");
}

fn tap(x: f64, y: f64) {
    let output = adb::inject_touch_logical(x as f32, y as f32).expect("inject tap");
    assert!(output.status.success(), "tap-logical failed: {output:?}");
}

/// `label:id:x:y:active` (scene modes) or `id:x:y:active` (plain toplevel).
fn touch_xy(payload: &str) -> (String, f64, f64) {
    let parts: Vec<&str> = payload.split(':').collect();
    let (label, rest) = match parts.len() {
        5 => (parts[0].to_string(), &parts[1..]),
        4 => ("toplevel".to_string(), &parts[..]),
        _ => panic!("unexpected touch payload {payload:?}"),
    };
    (label, rest[1].parse().unwrap(), rest[2].parse().unwrap())
}

fn last_touch(app: &DebugApp, tag: &str) -> (String, f64, f64) {
    touch_xy(app.payloads_with_tag(tag).last().unwrap_or_else(|| panic!("no {tag}")))
}

#[test]
fn test_oversize_window_is_fit_scaled_and_centered() {
    tawc_integration::helpers::test_init();
    with_scene("oversize", |_app| {
        let (state, window, shot) = screenshot(1, "toplevel", |state| {
            (state.output_logical_w * 2, state.output_logical_h)
        });
        assert_fit_centered(&window, &state);
        assert!(window.scale < 0.6, "a 2x-wide window should be scaled down: {window:?}");
        assert!(!state.scrim, "no scrim without a dialog");

        let (gw, gh) = (window.geometry.2 as f64, window.geometry.3 as f64);
        // render-pattern's 80x80 corner blocks, centered 136 px in from each
        // buffer edge.
        let corners = [
            ((136.0, 136.0), "top-left", [true, false, false]),
            ((gw - 136.0, 136.0), "top-right", [false, true, false]),
            ((136.0, gh - 136.0), "bottom-left", [false, false, true]),
        ];
        for ((x, y), label, [red, green, blue]) in corners {
            let c = sample(&shot, &state, window.to_screen(x, y));
            let ok = if red {
                c.r > 180 && c.g < 100 && c.b < 100
            } else if green {
                c.g > 130 && c.r < 100 && c.b < 120
            } else {
                assert!(blue);
                c.b > 170 && c.r < 100 && c.g < 120
            };
            assert!(ok, "{label} block at its scaled position: {c:?} window={window:?}");
        }
        let c = sample(&shot, &state, window.to_screen(gw - 136.0, gh - 136.0));
        assert!(c.r > 160 && c.g > 140 && c.b < 100, "bottom-right block: {c:?}");

        // Letterbox bars above and below the scaled window.
        let (w, _) = host_size(&state);
        assert!(window.offset.1 > 20.0, "expected vertical letterboxing: {window:?}");
        assert_background(sample(&shot, &state, (w / 2.0, window.offset.1 / 2.0)), "top bar");
    });
}

#[test]
fn test_small_window_is_centered() {
    tawc_integration::helpers::test_init();
    with_scene("small", |app| {
        let (state, window, shot) =
            screenshot(1, "toplevel", |_| (SMALL_W as i32, SMALL_H as i32));
        assert_fit_centered(&window, &state);
        assert_eq!(window.scale, 1.0, "small windows are not scaled");

        let (w, h) = host_size(&state);
        let c = sample(&shot, &state, (w / 2.0, h / 2.0));
        assert!(c.g > 110 && c.b > 110 && c.r < 90, "small window center should be teal: {c:?}");
        assert_background(
            sample(&shot, &state, (window.offset.0 - 10.0, h / 2.0)),
            "left of the window",
        );

        // A tap at the screen center lands at the window's center.
        tap(w / 2.0, h / 2.0);
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT).expect("tap on window");
        let (_, x, y) = last_touch(app, "TOUCH_DOWN");
        assert_close(x, SMALL_W / 2.0, 1.0, "surface-local x");
        assert_close(y, SMALL_H / 2.0, 1.0, "surface-local y");

        // Background reaches nobody: the next tap on the window is only the
        // second down.
        tap(10.0, 10.0);
        tap(window.offset.0 + 20.0, window.offset.1 + 30.0);
        app.wait_for_tag_count("TOUCH_UP", 2, TIMEOUT).expect("second tap on window");
        assert_eq!(app.count_with_tag("TOUCH_DOWN"), 2, "background tap reached the client");
        let (_, x, y) = last_touch(app, "TOUCH_DOWN");
        assert_close(x, 20.0, 1.0, "second tap x");
        assert_close(y, 30.0, 1.0, "second tap y");
    });
}

#[test]
fn test_pointer_on_centered_window() {
    tawc_integration::helpers::test_init();
    with_scene("small", |app| {
        let (_, window) = wait_for_window(1, "toplevel", (SMALL_W as i32, SMALL_H as i32));
        let (sx, sy) = window.to_screen(40.0, 25.0);
        let before = app.count_with_tag("POINTER_MOTION") + app.count_with_tag("POINTER_ENTER");
        adb::inject_pointer_move_logical(sx as f32, sy as f32).expect("inject-pointer move");
        let deadline = std::time::Instant::now() + TIMEOUT;
        while app.count_with_tag("POINTER_MOTION") + app.count_with_tag("POINTER_ENTER") <= before {
            assert!(std::time::Instant::now() < deadline, "no pointer event arrived");
            std::thread::sleep(Duration::from_millis(25));
        }
        let last = app
            .payloads_with_tag("POINTER_ENTER")
            .into_iter()
            .chain(app.payloads_with_tag("POINTER_MOTION"))
            .last()
            .expect("pointer position");
        let parts: Vec<&str> = last.split(':').collect();
        assert_eq!(parts[0], "toplevel", "pointer target: {last}");
        assert_close(parts[1].parse().unwrap(), 40.0, 1.0, "pointer x");
        assert_close(parts[2].parse().unwrap(), 25.0, 1.0, "pointer y");

        let reported = String::from_utf8_lossy(&adb::query_state().expect("query-state").stdout)
            .to_string();
        let field = |key: &str| -> f64 {
            reported
                .split_whitespace()
                .find_map(|kv| kv.strip_prefix(key)?.strip_prefix('=')?.parse().ok())
                .unwrap_or_else(|| panic!("{key} missing from {reported:?}"))
        };
        assert_close(field("pointer_x"), sx, 1.0, "query-state pointer_x is screen space");
        assert_close(field("pointer_y"), sy, 1.0, "query-state pointer_y is screen space");
    });
}

/// Popups follow their parent's placement and are constrained to the visible
/// screen, not to the parent's frame.
#[test]
fn test_popup_constrained_to_screen_on_centered_window() {
    tawc_integration::helpers::test_init();
    with_scene("small-popup", |app| {
        app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
            .expect("popup ready");
        let (state, window) = wait_for_window(1, "toplevel", (SMALL_W as i32, SMALL_H as i32));
        let configure = app
            .payloads_with_tag("POPUP_CONFIGURE")
            .last()
            .cloned()
            .expect("popup configure");
        let fields: Vec<i32> = configure.split(':').map(|v| v.parse().unwrap()).collect();
        // Slid up until its bottom meets the screen bottom, in the parent's
        // window-geometry frame (its surface origin).
        let (_, h) = host_size(&state);
        let (_, bottom) = window.to_window(0.0, h);
        assert_close(
            fields[1] as f64,
            bottom - POPUP_H as f64,
            1.0,
            &format!("popup y (configure {configure}, window {window:?})"),
        );
    });
}

#[test]
fn test_dialog_centered_over_scrim() {
    tawc_integration::helpers::test_init();
    with_scene("dialog", |app| {
        app.wait_for_tag_value("SURFACE_READY", "dialog", TIMEOUT)
            .expect("dialog ready");
        assert_eq!(
            app.payloads_with_tag("DIALOG_CONFIGURE_STATE").last().map(String::as_str),
            Some("other"),
            "dialogs are not maximized"
        );
        assert_eq!(
            app.payloads_with_tag("DIALOG_CONFIGURE_SIZE").last().map(String::as_str),
            Some("0 0"),
            "dialogs pick their own size"
        );

        let (state, dialog) = wait_for_window(2, "child", (DIALOG_W as i32, DIALOG_H as i32));
        assert!(state.scrim, "a dialog draws a scrim: {state:?}");
        assert_fit_centered(&dialog, &state);
        assert_eq!(state.windows.last(), Some(&dialog), "dialog is on top: {state:?}");
        let (w, h) = host_size(&state);
        let outside = (w / 2.0, h * 0.2);
        assert!(outside.1 < dialog.offset.1, "sample point must be outside the dialog");

        let (state, dialog, shot) =
            screenshot(2, "child", |_| (DIALOG_W as i32, DIALOG_H as i32));
        let c = sample(&shot, &state, dialog.to_screen(DIALOG_W / 2.0, DIALOG_H / 2.0 + 30.0));
        assert!(c.b > 170 && c.r < 100, "dialog center should be blue: {c:?}");
        // The parent fills with 90% gray; the scrim halves it.
        let c = sample(&shot, &state, outside);
        assert!(
            (90..140).contains(&c.r) && (90..140).contains(&c.g) && (90..140).contains(&c.b),
            "parent outside the dialog should be dimmed: {c:?} state={state:?}"
        );

        // A tap on the scrim reaches neither surface; one on the dialog
        // arrives in its frame.
        tap(outside.0, outside.1);
        let (cx, cy) = dialog.to_screen(DIALOG_W / 2.0, DIALOG_H / 2.0);
        tap(cx, cy);
        app.wait_for_tag_count("SURFACE_TOUCH_UP", 1, TIMEOUT).expect("tap on dialog");
        assert_eq!(app.count_with_tag("SURFACE_TOUCH_DOWN"), 1, "scrim tap reached the client");
        let (label, x, y) = last_touch(app, "SURFACE_TOUCH_DOWN");
        assert_eq!(label, "dialog");
        assert_close(x, DIALOG_W / 2.0, 1.0, "dialog-local x");
        assert_close(y, DIALOG_H / 2.0, 1.0, "dialog-local y");

        // A drag that starts on the dialog stays in its frame after leaving it.
        let output = adb::inject_drag_logical((cx as f32, cy as f32), (outside.0 as f32, outside.1 as f32))
            .expect("inject drag");
        assert!(output.status.success(), "drag-logical failed: {output:?}");
        app.wait_for_tag_count("SURFACE_TOUCH_UP", 2, TIMEOUT).expect("drag up");
        let (label, x, y) = last_touch(app, "SURFACE_TOUCH_MOTION");
        assert_eq!(label, "dialog", "drag motion target");
        let (ex, ey) = dialog.to_window(outside.0, outside.1);
        assert_close(x, ex, 1.5, "drag end x in dialog frame");
        assert_close(y, ey, 1.5, "drag end y in dialog frame");
    });
}

/// Back sends Escape to the focused dialog; once it closes, keyboard focus
/// returns to the parent and the scrim goes away.
#[test]
fn test_dialog_close_returns_focus_to_parent() {
    tawc_integration::helpers::test_init();
    with_scene("dialog", |app| {
        app.wait_for_tag_value("SURFACE_READY", "dialog", TIMEOUT)
            .expect("dialog ready");
        app.wait_for_tag_value("KEYBOARD_ENTER", "dialog", TIMEOUT)
            .expect("dialog has keyboard focus");
        let parent_enters = app
            .payloads_with_tag("KEYBOARD_ENTER")
            .iter()
            .filter(|label| *label == "toplevel")
            .count();

        // The scene is fullscreen, and Back leaves fullscreen first.
        let output = adb::back().expect("back");
        assert!(output.status.success(), "back failed: {output:?}");
        app.wait_for_tag_value("CONFIGURE_STATE", "maximized", TIMEOUT)
            .expect("first Back leaves fullscreen");
        let output = adb::back().expect("back");
        assert!(output.status.success(), "back failed: {output:?}");
        app.wait_for_tag_count("DIALOG_CLOSED", 1, TIMEOUT).expect("dialog closed by Escape");
        let deadline = std::time::Instant::now() + TIMEOUT;
        loop {
            let enters = app
                .payloads_with_tag("KEYBOARD_ENTER")
                .iter()
                .filter(|label| *label == "toplevel")
                .count();
            if enters > parent_enters {
                break;
            }
            assert!(std::time::Instant::now() < deadline, "focus did not return to the parent");
            std::thread::sleep(Duration::from_millis(25));
        }

        let deadline = std::time::Instant::now() + TIMEOUT;
        loop {
            let state = compositor::query_state(TIMEOUT).expect("query compositor state");
            if state.windows.len() == 1 && !state.scrim {
                break;
            }
            assert!(std::time::Instant::now() < deadline, "dialog still shown: {state:?}");
            std::thread::sleep(Duration::from_millis(50));
        }
    });
}
