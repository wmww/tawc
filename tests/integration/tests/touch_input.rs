//! Touch input dispatch tests. Drive wayland-debug-app through compositor
//! wl_touch routing paths and assert the client observes the expected touch
//! target, slot, and surface-local coordinates.
//!
//! Deliberately avoid asserting anything about buffer types (AHB vs SHM):
//! the point is how the compositor dispatches touch, not how clients render.

use tawc_integration::adb;
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    assert_compositor_clean, start_wayland_debug_popup, start_wayland_debug_popup_switch,
    start_wayland_debug_subsurface, start_wayland_debug_subsurface_input_empty,
    start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::GraphicsBackend;

/// Touch dispatch has no buffer-type stake; pick the most portable backend.
const INPUT_BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const WAYLAND_DEBUG_ENV: &str = "";

fn with_wayland_touch(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_touch(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_subsurface(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_subsurface(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_subsurface_input_empty(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_subsurface_input_empty(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_popup(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_popup(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

fn with_wayland_popup_switch(run: impl FnOnce(&DebugApp)) {
    let mut app = start_wayland_debug_popup_switch(INPUT_BACKEND, WAYLAND_DEBUG_ENV);
    run(&app);
    app.stop()
        .expect("debug app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[derive(Clone, Debug)]
struct TouchDebugEvent {
    id: i32,
    x: f64,
    y: f64,
    active: u32,
}

fn parse_touch_event(payload: &str) -> TouchDebugEvent {
    let mut parts = payload.split(':');
    let id = parts
        .next()
        .expect("touch id")
        .parse()
        .expect("touch id integer");
    let x = parts
        .next()
        .expect("touch x")
        .parse()
        .expect("touch x number");
    let y = parts
        .next()
        .expect("touch y")
        .parse()
        .expect("touch y number");
    let active = parts
        .next()
        .expect("touch active count")
        .parse()
        .expect("touch active integer");
    assert!(
        parts.next().is_none(),
        "extra fields in touch payload {payload:?}"
    );
    TouchDebugEvent { id, x, y, active }
}

fn touch_events(app: &DebugApp, tag: &str) -> Vec<TouchDebugEvent> {
    app.payloads_with_tag(tag)
        .iter()
        .map(|payload| parse_touch_event(payload))
        .collect()
}

fn inject_touch(kind: &str) {
    let output = adb::inject_touch(kind).unwrap_or_else(|e| panic!("inject-touch {kind}: {e}"));
    assert!(
        output.status.success(),
        "inject-touch {kind} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[derive(Clone, Debug)]
struct SurfaceTouchDebugEvent {
    target: String,
    id: i32,
    x: f64,
    y: f64,
    active: u32,
}

#[derive(Clone, Debug)]
struct PopupLayout {
    child_x: i32,
    child_y: i32,
    shadow: i32,
    content_w: i32,
    content_h: i32,
    configure_x: i32,
    configure_y: i32,
    configure_w: i32,
    configure_h: i32,
    parent_geom_x: i32,
    parent_geom_y: i32,
}

fn parse_surface_touch_event(payload: &str) -> SurfaceTouchDebugEvent {
    let mut parts = payload.split(':');
    let target = parts.next().expect("touch target").to_string();
    let id = parts
        .next()
        .expect("touch id")
        .parse()
        .expect("touch id integer");
    let x = parts
        .next()
        .expect("touch x")
        .parse()
        .expect("touch x number");
    let y = parts
        .next()
        .expect("touch y")
        .parse()
        .expect("touch y number");
    let active = parts
        .next()
        .expect("touch active count")
        .parse()
        .expect("touch active integer");
    assert!(
        parts.next().is_none(),
        "extra fields in surface touch payload {payload:?}"
    );
    SurfaceTouchDebugEvent {
        target,
        id,
        x,
        y,
        active,
    }
}

fn parse_popup_layout(payload: &str) -> PopupLayout {
    let fields: Vec<i32> = payload
        .split(':')
        .map(|part| part.parse().expect("popup layout integer"))
        .collect();
    assert_eq!(
        fields.len(),
        11,
        "unexpected popup layout payload {payload:?}"
    );
    PopupLayout {
        child_x: fields[0],
        child_y: fields[1],
        shadow: fields[2],
        content_w: fields[3],
        content_h: fields[4],
        configure_x: fields[5],
        configure_y: fields[6],
        configure_w: fields[7],
        configure_h: fields[8],
        parent_geom_x: fields[9],
        parent_geom_y: fields[10],
    }
}

fn surface_touch_events(app: &DebugApp, tag: &str) -> Vec<SurfaceTouchDebugEvent> {
    app.payloads_with_tag(tag)
        .iter()
        .map(|payload| parse_surface_touch_event(payload))
        .collect()
}

fn assert_surface_tap_delivered(app: &DebugApp, target: &str) {
    inject_touch("tap");
    app.wait_for_tag_count("SURFACE_TOUCH_DOWN", 1, TIMEOUT)
        .expect("surface touch down");
    app.wait_for_tag_count("SURFACE_TOUCH_UP", 1, TIMEOUT)
        .expect("surface touch up");

    let downs = surface_touch_events(app, "SURFACE_TOUCH_DOWN");
    let ups = surface_touch_events(app, "SURFACE_TOUCH_UP");
    assert_eq!(downs.len(), 1, "expected one down, got {downs:?}");
    assert_eq!(ups.len(), 1, "expected one up, got {ups:?}");
    assert_eq!(downs[0].target, target, "touch down target");
    assert_eq!(ups[0].target, target, "touch up target");
    assert_eq!(downs[0].id, ups[0].id, "tap up used a different slot");
    assert_eq!(downs[0].active, 1, "tap down active count");
    assert_eq!(ups[0].active, 0, "tap up active count");
    assert!(
        downs[0].x >= 0.0 && downs[0].y >= 0.0,
        "surface-local coordinates should be non-negative: {:?}",
        downs[0]
    );
}

fn assert_popup_shadow_geometry_tap_delivered(app: &DebugApp) {
    app.wait_for_tag_count("POPUP_LAYOUT", 1, TIMEOUT)
        .expect("popup layout");
    let layout = parse_popup_layout(
        app.payloads_with_tag("POPUP_LAYOUT")
            .last()
            .expect("popup layout payload"),
    );

    assert_eq!(
        layout.configure_x,
        layout.child_x - layout.parent_geom_x,
        "popup configure x should be relative to parent window geometry"
    );
    assert_eq!(
        layout.configure_y,
        layout.child_y - layout.parent_geom_y,
        "popup configure y should be relative to parent window geometry"
    );
    assert_eq!(layout.configure_w, layout.content_w, "popup configure width");
    assert_eq!(layout.configure_h, layout.content_h, "popup configure height");

    assert_surface_tap_delivered(app, "popup");
    let down = surface_touch_events(app, "SURFACE_TOUCH_DOWN")
        .pop()
        .expect("popup touch down");
    let expected_x = f64::from(layout.shadow) + f64::from(layout.content_w) / 2.0;
    let expected_y = f64::from(layout.shadow) + f64::from(layout.content_h) / 2.0;
    assert!(
        (down.x - expected_x).abs() <= 2.0 && (down.y - expected_y).abs() <= 2.0,
        "popup surface-local touch should include shadow/window-geometry offset: \
         down={down:?} expected=({expected_x:.1}, {expected_y:.1}) layout={layout:?}"
    );
}

#[test]
fn test_touch_tap() {
    with_wayland_touch(|app| {
        inject_touch("tap");
        app.wait_for_tag_count("TOUCH_DOWN", 1, TIMEOUT)
            .expect("touch down");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");

        let downs = touch_events(app, "TOUCH_DOWN");
        let ups = touch_events(app, "TOUCH_UP");
        assert_eq!(downs.len(), 1, "expected one TOUCH_DOWN, got {downs:?}");
        assert_eq!(ups.len(), 1, "expected one TOUCH_UP, got {ups:?}");
        assert_eq!(downs[0].id, ups[0].id, "tap up used a different slot");
        assert_eq!(downs[0].active, 1, "tap down active count");
        assert_eq!(ups[0].active, 0, "tap up active count");
        assert!(
            (downs[0].x - ups[0].x).abs() < 1.0 && (downs[0].y - ups[0].y).abs() < 1.0,
            "tap moved unexpectedly: down={:?} up={:?}",
            downs[0],
            ups[0]
        );
    });
}

/// A wl_subsurface is rendered as part of the parent surface tree, but input
/// must still be delivered to the child wl_surface with child-local
/// coordinates. The debug scene places the subsurface under the broker's
/// normalized tap point.
#[test]
fn test_touch_subsurface_tap() {
    with_wayland_subsurface(|app| {
        app.wait_for_tag_value("SURFACE_READY", "subsurface", TIMEOUT)
            .expect("subsurface ready");
        assert_surface_tap_delivered(app, "subsurface");
    });
}

/// The core wl_subsurface protocol says a sub-surface never has keyboard
/// focus. Touch still targets the subsurface, but keyboard/text-input focus
/// must stay on the compound window's main surface.
#[test]
fn test_touch_subsurface_tap_does_not_move_keyboard_focus_to_subsurface() {
    with_wayland_subsurface(|app| {
        app.wait_for_tag_value("SURFACE_READY", "subsurface", TIMEOUT)
            .expect("subsurface ready");
        app.wait_for_tag_value("KEYBOARD_ENTER", "toplevel", TIMEOUT)
            .expect("initial toplevel keyboard focus");
        let enters_before = app.count_with_tag("KEYBOARD_ENTER");
        let leaves_before = app.count_with_tag("KEYBOARD_LEAVE");

        assert_surface_tap_delivered(app, "subsurface");

        assert_eq!(
            app.count_with_tag("KEYBOARD_ENTER"),
            enters_before,
            "subsurface touch should not move keyboard focus into the subsurface"
        );
        assert_eq!(
            app.count_with_tag("KEYBOARD_LEAVE"),
            leaves_before,
            "subsurface touch should not move keyboard focus away from the toplevel"
        );
    });
}

/// Render-only subsurfaces can be visible but set an empty input region.
/// Firefox/WebRender uses that shape: the child surface must not steal the
/// touch from the browser toplevel.
#[test]
fn test_touch_ignores_input_empty_subsurface() {
    with_wayland_subsurface_input_empty(|app| {
        app.wait_for_tag_value("SURFACE_READY", "subsurface", TIMEOUT)
            .expect("subsurface ready");
        assert_surface_tap_delivered(app, "toplevel");
    });
}

/// Basic xdg_popup coverage: the first tap must land on the popup with
/// popup-local coordinates, including non-zero window-geometry/shadow
/// offsets. A later tap outside the popup must dismiss it; GTK menu bars
/// depend on that `popup_done` before mapping a different menu popup.
#[test]
fn test_touch_popup_tap() {
    with_wayland_popup(|app| {
        app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
            .expect("popup ready");
        assert_popup_shadow_geometry_tap_delivered(app);

        inject_touch("tap-outside-popup");
        app.wait_for_tag_value("POPUP_DONE", "", TIMEOUT)
            .expect("popup dismissed after outside tap");
    });
}

/// Touch focus may enter an xdg_popup, but keyboard/text-input focus must stay
/// on the owning toplevel. Firefox desktop menus close without activating an
/// item if a touchscreen tap first moves keyboard focus into the popup.
#[test]
fn test_touch_popup_tap_does_not_move_keyboard_focus_to_popup() {
    with_wayland_popup(|app| {
        app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
            .expect("popup ready");
        app.wait_for_tag_value("KEYBOARD_ENTER", "toplevel", TIMEOUT)
            .expect("initial toplevel keyboard focus");
        let enters_before = app.count_with_tag("KEYBOARD_ENTER");
        let leaves_before = app.count_with_tag("KEYBOARD_LEAVE");

        assert_popup_shadow_geometry_tap_delivered(app);

        assert_eq!(
            app.count_with_tag("KEYBOARD_ENTER"),
            enters_before,
            "popup touch should not move keyboard focus into the popup"
        );
        assert_eq!(
            app.count_with_tag("KEYBOARD_LEAVE"),
            leaves_before,
            "popup touch should not move keyboard focus away from the toplevel"
        );
    });
}

/// GTK menu bars use grabbed xdg_popups. When a tap outside the current
/// popup opens another menu popup on the same toplevel, the compositor must
/// dismiss the old popup through the grab path; a bare `popup_done` leaves
/// Smithay's active-grab stack pointing at the old menu and the next
/// `xdg_popup.grab` is rejected as not-topmost.
#[test]
fn test_touch_grabbed_popup_switches_to_next_popup() {
    with_wayland_popup_switch(|app| {
        inject_touch("tap-menu-a");
        app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
            .expect("first grabbed popup ready");

        inject_touch("tap-menu-b");
        app.wait_for_tag_value("POPUP_DONE", "", TIMEOUT)
            .expect("first grabbed popup dismissed before second popup");
        app.wait_for_tag_value("SURFACE_READY", "popup2", TIMEOUT)
            .expect("second grabbed popup ready after outside touch");
    });
}

/// Drag is touch.down + a stream of wl_touch.motion events + touch.up. The
/// assertions only compare the client's own observed coordinates, avoiding
/// any baked-in screen dimensions or density assumptions.
#[test]
fn test_touch_drag() {
    with_wayland_touch(|app| {
        inject_touch("drag");
        app.wait_for_tag_count("TOUCH_DOWN", 1, TIMEOUT)
            .expect("touch down");
        app.wait_for_tag_count("TOUCH_MOTION", 3, TIMEOUT)
            .expect("touch motion");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");

        let down = touch_events(app, "TOUCH_DOWN").remove(0);
        let motions = touch_events(app, "TOUCH_MOTION");
        let up = touch_events(app, "TOUCH_UP").remove(0);
        let last_motion = motions.last().expect("last motion");

        assert!(
            motions.iter().all(|m| m.id == down.id),
            "drag slot changed: {motions:?}"
        );
        assert!(up.id == down.id, "drag up slot changed");
        assert!(
            last_motion.x > down.x && last_motion.y > down.y,
            "drag did not move down/right: down={down:?} last={last_motion:?}"
        );
        assert!(
            (up.x - last_motion.x).abs() < 1.0 && (up.y - last_motion.y).abs() < 1.0,
            "drag up should report the final motion position: up={up:?} last={last_motion:?}"
        );
    });
}

/// Two-finger delivery is the path most likely to rot because each Android
/// pointer ID must remain a distinct Wayland touch slot through down,
/// motion, and up. The debug broker builds a real multi-pointer MotionEvent
/// stream against the focused SurfaceView; the client verifies both slots.
#[test]
fn test_touch_multitouch() {
    with_wayland_touch(|app| {
        inject_touch("multitouch");
        app.wait_for_tag_count("TOUCH_DOWN", 2, TIMEOUT)
            .expect("two touch downs");
        app.wait_for_tag_count("TOUCH_MOTION", 6, TIMEOUT)
            .expect("multi-touch motion");
        app.wait_for_tag_count("TOUCH_UP", 2, TIMEOUT)
            .expect("two touch ups");

        let downs = touch_events(app, "TOUCH_DOWN");
        let motions = touch_events(app, "TOUCH_MOTION");
        let ups = touch_events(app, "TOUCH_UP");
        assert_eq!(downs.len(), 2, "expected two downs, got {downs:?}");
        assert_eq!(ups.len(), 2, "expected two ups, got {ups:?}");
        assert_ne!(downs[0].id, downs[1].id, "two fingers shared one slot");
        assert_eq!(downs[0].active, 1, "first down active count");
        assert_eq!(downs[1].active, 2, "second down active count");
        assert_eq!(ups.last().unwrap().active, 0, "all fingers should be up");

        for id in [downs[0].id, downs[1].id] {
            let first = downs.iter().find(|e| e.id == id).unwrap();
            let last = motions
                .iter()
                .rev()
                .find(|e| e.id == id)
                .unwrap_or_else(|| panic!("missing motion for touch id {id}"));
            assert!(
                (last.x - first.x).abs() > 5.0 || (last.y - first.y).abs() > 5.0,
                "touch id {id} did not move enough: first={first:?} last={last:?}"
            );
        }
    });
}
