//! Real `wl_pointer` dispatch tests. Drive wayland-debug-app through the
//! compositor's mouse path and assert the client observes the expected
//! target, surface-local coordinates, buttons, and scroll units.
//!
//! Injection goes through the debug broker's `inject-pointer` action, which
//! builds `SOURCE_MOUSE` MotionEvents and dispatches them at the focused
//! SurfaceView — so `CompositorActivity`'s own decoding (the source split,
//! the button-mask diff, the hover rules) is part of what is under test.
//!
//! Also covers pointer emulation (notes/input.md "Pointer emulation"): what
//! touchscreen input does to `wl_pointer` in each mode. Tests count events
//! from a baseline taken after the window is up rather than from zero.

use tawc_integration::adb;
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    assert_compositor_clean, start_wayland_debug_popup, start_wayland_debug_subsurface,
    start_wayland_debug_subsurface_input_empty, start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::GraphicsBackend;

/// Pointer dispatch has no buffer-type stake; pick the most portable backend.
const INPUT_BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const WAYLAND_DEBUG_ENV: &str = "";

/// evdev button codes the five Android mouse buttons map to.
const BTN_LEFT: u32 = 0x110;
const BTN_RIGHT: u32 = 0x111;
const BTN_MIDDLE: u32 = 0x112;
const BTN_SIDE: u32 = 0x113;
const BTN_EXTRA: u32 = 0x114;

const PRESSED: u32 = 1;
const RELEASED: u32 = 0;

/// `wl_pointer.axis_source` enum values.
const AXIS_SOURCE_WHEEL: u32 = 0;

/// One wheel detent in `axis_value120` units, and in the legacy `axis`
/// value's logical pixels.
const V120_PER_DETENT: i32 = 120;
const LOGICAL_PX_PER_DETENT: f64 = 15.0;

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

#[derive(Clone, Debug)]
struct PointerPosition {
    target: String,
    x: f64,
    y: f64,
}

fn parse_position(payload: &str) -> PointerPosition {
    let mut parts = payload.split(':');
    let target = parts.next().expect("pointer target").to_string();
    let x = parts
        .next()
        .expect("pointer x")
        .parse()
        .expect("pointer x number");
    let y = parts
        .next()
        .expect("pointer y")
        .parse()
        .expect("pointer y number");
    assert!(
        parts.next().is_none(),
        "extra fields in pointer payload {payload:?}"
    );
    PointerPosition { target, x, y }
}

#[derive(Clone, Debug, PartialEq)]
struct PointerButton {
    target: String,
    button: u32,
    state: u32,
}

fn parse_button(payload: &str) -> PointerButton {
    let mut parts = payload.split(':');
    let target = parts.next().expect("button target").to_string();
    let button = parts
        .next()
        .expect("button code")
        .parse()
        .expect("button code integer");
    let state = parts
        .next()
        .expect("button state")
        .parse()
        .expect("button state integer");
    assert!(
        parts.next().is_none(),
        "extra fields in button payload {payload:?}"
    );
    PointerButton {
        target,
        button,
        state,
    }
}

/// `axis:value` for POINTER_AXIS / POINTER_AXIS_V120, where axis is `v` or `h`.
fn parse_axis(payload: &str) -> (String, f64) {
    let (axis, value) = payload.split_once(':').expect("axis payload");
    (
        axis.to_string(),
        value.parse().expect("axis value is a number"),
    )
}

fn positions(app: &DebugApp, tag: &str) -> Vec<PointerPosition> {
    app.payloads_with_tag(tag)
        .iter()
        .map(|p| parse_position(p))
        .collect()
}

fn buttons(app: &DebugApp) -> Vec<PointerButton> {
    app.payloads_with_tag("POINTER_BUTTON")
        .iter()
        .map(|p| parse_button(p))
        .collect()
}

/// Counts of every pointer tag a test cares about, taken before injecting.
#[derive(Clone, Copy, Debug)]
struct Baseline {
    enter: usize,
    leave: usize,
    motion: usize,
    button: usize,
    frame: usize,
    axis: usize,
    v120: usize,
    touch_down: usize,
}

/// Counted from a single snapshot of the client's output: separate
/// `count_with_tag` calls would each see a different point in the stream, and
/// a frame count that lags its own group by one read is indistinguishable
/// from a missing frame.
fn baseline(app: &DebugApp) -> Baseline {
    let lines = app.lines();
    let count = |tag: &str| {
        let prefix = format!("{tag}:");
        lines
            .iter()
            .filter(|l| *l == tag || l.starts_with(&prefix))
            .count()
    };
    Baseline {
        enter: count("POINTER_ENTER"),
        leave: count("POINTER_LEAVE"),
        motion: count("POINTER_MOTION"),
        button: count("POINTER_BUTTON"),
        frame: count("POINTER_FRAME"),
        axis: count("POINTER_AXIS"),
        v120: count("POINTER_AXIS_V120"),
        touch_down: count("TOUCH_DOWN"),
    }
}

/// Move the pointer into the window and wait until the client has seen it, so
/// later assertions start from a known focus. Returns the baseline taken after
/// the move settled.
fn settle_pointer_in_window(app: &DebugApp) -> Baseline {
    let before = baseline(app);
    adb::inject_pointer_move().expect("inject-pointer move");
    // Either an enter (first entry) or a motion (already inside) lands.
    app.wait_on_pointer_activity(before)
        .expect("pointer move reached the client");
    baseline(app)
}

trait PointerWait {
    fn wait_on_pointer_activity(&self, before: Baseline) -> Result<(), String>;
    fn wait_for_more(&self, tag: &str, than: usize) -> Result<(), String>;
}

impl PointerWait for DebugApp {
    fn wait_on_pointer_activity(&self, before: Baseline) -> Result<(), String> {
        let deadline = std::time::Instant::now() + TIMEOUT;
        loop {
            if self.count_with_tag("POINTER_ENTER") > before.enter
                || self.count_with_tag("POINTER_MOTION") > before.motion
            {
                return Ok(());
            }
            if std::time::Instant::now() >= deadline {
                return Err("no pointer enter or motion arrived".to_string());
            }
            std::thread::sleep(std::time::Duration::from_millis(25));
        }
    }

    fn wait_for_more(&self, tag: &str, than: usize) -> Result<(), String> {
        self.wait_for_tag_count(tag, than + 1, TIMEOUT)
    }
}

#[test]
fn test_pointer_motion_targets_toplevel() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_on_pointer_activity(before)
            .expect("pointer motion");

        let last = positions(app, "POINTER_ENTER")
            .into_iter()
            .chain(positions(app, "POINTER_MOTION"))
            .last()
            .expect("a pointer position event");
        assert_eq!(last.target, "toplevel", "pointer target");
        assert!(
            last.x >= 0.0 && last.y >= 0.0,
            "surface-local coordinates should be non-negative: {last:?}"
        );
    });
}

/// A mouse click must produce `wl_pointer.button` and *no* `wl_touch.down`.
/// Without the source split in `CompositorActivity`, Android's laundering of
/// mouse buttons through `onTouchEvent` delivers both.
#[test]
fn test_pointer_click_does_not_also_touch() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = settle_pointer_in_window(app);
        adb::inject_pointer_click("primary").expect("inject-pointer click");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("press and release");

        let new_buttons = buttons(app).split_off(before.button);
        assert_eq!(
            new_buttons,
            vec![
                PointerButton {
                    target: "toplevel".to_string(),
                    button: BTN_LEFT,
                    state: PRESSED,
                },
                PointerButton {
                    target: "toplevel".to_string(),
                    button: BTN_LEFT,
                    state: RELEASED,
                },
            ],
        );
        assert_eq!(
            app.count_with_tag("TOUCH_DOWN"),
            before.touch_down,
            "mouse click must not also be delivered as touch"
        );
    });
}

/// The mirror of the above: a touchscreen tap must stay on `wl_touch` and
/// never become a pointer button.
#[test]
fn test_touch_tap_does_not_become_pointer() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_touch("tap").expect("inject-touch tap");
        app.wait_for_tag_count("TOUCH_DOWN", before.touch_down + 1, TIMEOUT)
            .expect("touch down");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");
        assert_eq!(
            app.count_with_tag("POINTER_BUTTON"),
            before.button,
            "touchscreen tap must not produce a pointer button"
        );
    });
}

fn set_pointer_emulation(mode: &str) {
    let output = adb::set_pointer_emulation(mode).expect("set-pointer-emulation");
    assert!(
        output.status.success(),
        "set-pointer-emulation {mode}: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}

/// Client lines from `TOUCH_DOWN`/`POINTER_*` tags after the first `skip`.
fn input_lines(app: &DebugApp, skip: usize) -> Vec<String> {
    app.lines()
        .into_iter()
        .filter(|l| l.starts_with("TOUCH_") || l.starts_with("POINTER_"))
        .skip(skip)
        .collect()
}

/// Hover (the default): the pointer follows a touch drag with motion only,
/// no buttons and no leave. Nemo's rubberband polls the client pointer and
/// froze without this.
#[test]
fn test_hover_emulation_touch_drag_moves_pointer() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_touch("drag").expect("inject-touch drag");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");
        app.wait_for_more("POINTER_MOTION", before.motion)
            .expect("pointer motion during drag");

        let up = app.payloads_with_tag("TOUCH_UP").remove(0);
        let mut up = up.split(':').skip(1).map(|v| v.parse::<f64>().expect("touch coord"));
        let (up_x, up_y) = (up.next().expect("up x"), up.next().expect("up y"));
        let last = positions(app, "POINTER_MOTION").pop().expect("last motion");
        assert_eq!(last.target, "toplevel");
        assert!(
            (last.x - up_x).abs() < 1.0 && (last.y - up_y).abs() < 1.0,
            "pointer {:.1},{:.1} should rest at touch up {up_x:.1},{up_y:.1}",
            last.x,
            last.y,
        );
        let after = baseline(app);
        assert_eq!(after.button, before.button, "touch must not press a pointer button");
        assert_eq!(after.leave, before.leave, "touch must not send a pointer leave");
    });
}

#[test]
fn test_no_emulation_touch_leaves_pointer_alone() {
    tawc_integration::helpers::test_init();
    set_pointer_emulation("none");
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_touch("drag").expect("inject-touch drag");
        app.wait_for_tag_count("TOUCH_UP", 1, TIMEOUT)
            .expect("touch up");
        let after = baseline(app);
        assert_eq!(
            (after.enter, after.motion, after.button),
            (before.enter, before.motion, before.button),
            "pointer emulation none must not touch wl_pointer"
        );
    });
}

#[test]
fn test_full_emulation_tap_clicks() {
    tawc_integration::helpers::test_init();
    set_pointer_emulation("full");
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_touch("tap").expect("inject-touch tap");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("click");
        let clicks = buttons(app).split_off(before.button);
        let left = |state| PointerButton { target: "toplevel".into(), button: BTN_LEFT, state };
        assert_eq!(clicks, vec![left(PRESSED), left(RELEASED)]);
        assert_eq!(baseline(app).touch_down, before.touch_down, "full emulation sends no wl_touch");
    });
}

#[test]
fn test_full_emulation_drag_holds_left_button() {
    tawc_integration::helpers::test_init();
    set_pointer_emulation("full");
    with_wayland_touch(|app| {
        let skip = input_lines(app, 0).len();
        let before = baseline(app);
        adb::inject_touch("drag").expect("inject-touch drag");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("press and release");
        let lines = input_lines(app, skip);
        let press = lines.iter().position(|l| l.starts_with("POINTER_BUTTON:")).expect("press");
        let release = lines.iter().rposition(|l| l.starts_with("POINTER_BUTTON:")).expect("release");
        assert!(
            lines[press + 1..release].iter().any(|l| l.starts_with("POINTER_MOTION:")),
            "drag must move with the button held: {lines:?}"
        );
        assert!(!lines.iter().any(|l| l.starts_with("TOUCH_")), "no wl_touch: {lines:?}");
        let held = buttons(app).split_off(before.button);
        assert_eq!(held.iter().map(|b| (b.button, b.state)).collect::<Vec<_>>(), [
            (BTN_LEFT, PRESSED),
            (BTN_LEFT, RELEASED)
        ]);
    });
}

#[test]
fn test_full_emulation_long_press_right_clicks() {
    tawc_integration::helpers::test_init();
    set_pointer_emulation("full");
    with_wayland_touch(|app| {
        let before = baseline(app);
        adb::inject_touch("press").expect("inject-touch press");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("right click while held");
        adb::inject_touch("release").expect("inject-touch release");
        // The lift must not click: give a stray press time to land.
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_for_more("POINTER_MOTION", baseline(app).motion)
            .expect("motion after release");
        let clicks = buttons(app).split_off(before.button);
        assert_eq!(clicks.iter().map(|b| (b.button, b.state)).collect::<Vec<_>>(), [
            (BTN_RIGHT, PRESSED),
            (BTN_RIGHT, RELEASED)
        ]);
    });
}

/// Android synthesizes an `ACTION_HOVER_EXIT` before every mouse press and an
/// `ACTION_HOVER_ENTER` after the release. Mapping those to leave/enter would
/// wrap every click in a crossing pair, which closes GTK menus mid-click.
#[test]
fn test_click_sequence_sends_no_leave() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = settle_pointer_in_window(app);
        adb::inject_pointer_click("primary").expect("inject-pointer click");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("press and release");
        adb::inject_pointer_hover_exit().expect("inject-pointer hover-exit");
        // Give a stray leave a chance to arrive before asserting it didn't.
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_for_more("POINTER_MOTION", before.motion)
            .expect("motion after hover-exit");

        assert_eq!(
            app.count_with_tag("POINTER_LEAVE"),
            before.leave,
            "hover exit must not become wl_pointer.leave"
        );
    });
}

#[test]
fn test_pointer_buttons_map_to_evdev_codes() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let mut expected = Vec::new();
        let mut before = settle_pointer_in_window(app);
        for (name, code) in [
            ("primary", BTN_LEFT),
            ("secondary", BTN_RIGHT),
            ("tertiary", BTN_MIDDLE),
            ("back", BTN_SIDE),
            ("forward", BTN_EXTRA),
        ] {
            adb::inject_pointer_click(name).unwrap_or_else(|e| panic!("click {name}: {e}"));
            app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
                .unwrap_or_else(|e| panic!("click {name}: {e}"));
            expected.push((code, PRESSED));
            expected.push((code, RELEASED));
            before = baseline(app);
        }

        let seen: Vec<(u32, u32)> = buttons(app)
            .into_iter()
            .rev()
            .take(expected.len())
            .rev()
            .map(|b| (b.button, b.state))
            .collect();
        assert_eq!(seen, expected, "button codes and states");
    });
}

/// A mouse Back click must not also run the Android Back policy. Android
/// raises `KEYCODE_BACK` alongside `BUTTON_BACK`; if the view let it through,
/// the compositor would send Escape (or dismiss a popup) on top of BTN_SIDE.
#[test]
fn test_back_button_does_not_run_android_back() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = settle_pointer_in_window(app);
        let keys_before = app.count_with_tag("KEY");
        adb::inject_pointer_click("back").expect("inject-pointer back click");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("back press and release");
        assert_eq!(
            app.count_with_tag("KEY"),
            keys_before,
            "mouse Back must not also deliver a key event"
        );
    });
}

/// One wheel detent: Android's `AXIS_VSCROLL` is positive scrolling away from
/// the user, Wayland's vertical axis is positive downward, so scrolling up
/// must arrive negative. Magnitude is one detent: 120 in `axis_value120`, and
/// 15 logical pixels in the legacy `axis` value.
#[test]
fn test_wheel_scroll_direction_and_units() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let before = settle_pointer_in_window(app);
        adb::inject_pointer_scroll(1.0).expect("inject-pointer scroll up");
        app.wait_for_more("POINTER_AXIS_V120", before.v120)
            .expect("axis_value120");
        app.wait_for_more("POINTER_AXIS", before.axis)
            .expect("axis");

        let (axis, v120) = parse_axis(
            app.payloads_with_tag("POINTER_AXIS_V120")
                .last()
                .expect("v120 payload"),
        );
        assert_eq!(axis, "v", "wheel scroll is on the vertical axis");
        assert_eq!(
            v120 as i32, -V120_PER_DETENT,
            "one detent away from the user is -120 in Wayland's direction"
        );

        let (axis, value) = parse_axis(
            app.payloads_with_tag("POINTER_AXIS")
                .last()
                .expect("axis payload"),
        );
        assert_eq!(axis, "v");
        assert!(
            (value + LOGICAL_PX_PER_DETENT).abs() < 0.5,
            "legacy axis value should be one detent up: got {value}"
        );

        let source: u32 = app
            .payloads_with_tag("POINTER_AXIS_SOURCE")
            .last()
            .expect("axis source payload")
            .parse()
            .expect("axis source integer");
        assert_eq!(source, AXIS_SOURCE_WHEEL, "a mouse wheel is axis_source 0");
    });
}

/// Scrolling the other way flips the sign; the horizontal axis does not flip
/// at all, because Android and Wayland agree on positive-right.
#[test]
fn test_wheel_scroll_signs() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        let mut before = settle_pointer_in_window(app);
        adb::inject_pointer_scroll(-1.0).expect("inject-pointer scroll down");
        app.wait_for_more("POINTER_AXIS_V120", before.v120)
            .expect("vertical value120");
        let (axis, v120) = parse_axis(
            app.payloads_with_tag("POINTER_AXIS_V120")
                .last()
                .expect("v120 payload"),
        );
        assert_eq!(axis, "v");
        assert_eq!(
            v120 as i32, V120_PER_DETENT,
            "scrolling toward the user is positive in Wayland"
        );

        before = baseline(app);
        adb::inject_pointer_hscroll(1.0).expect("inject-pointer hscroll right");
        app.wait_for_more("POINTER_AXIS_V120", before.v120)
            .expect("horizontal value120");
        let (axis, v120) = parse_axis(
            app.payloads_with_tag("POINTER_AXIS_V120")
                .last()
                .expect("v120 payload"),
        );
        assert_eq!(axis, "h", "hscroll lands on the horizontal axis");
        assert_eq!(
            v120 as i32, V120_PER_DETENT,
            "horizontal scroll keeps Android's sign"
        );
    });
}

/// Every group of pointer events must be terminated by exactly one
/// `wl_pointer.frame`. Counted across every group kind the client can see,
/// crossings included.
#[test]
fn test_every_event_group_ends_with_one_frame() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        fn groups(b: Baseline) -> usize {
            b.enter + b.leave + b.motion + b.button + b.axis
        }

        let before = settle_pointer_in_window(app);
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_for_more("POINTER_MOTION", before.motion)
            .expect("motion");
        adb::inject_pointer_scroll(1.0).expect("inject-pointer scroll");
        app.wait_for_more("POINTER_AXIS", before.axis)
            .expect("axis");
        adb::inject_pointer_click("primary").expect("inject-pointer click");
        app.wait_for_tag_count("POINTER_BUTTON", before.button + 2, TIMEOUT)
            .expect("press and release");

        // The last group's frame can still be a read behind; settle first,
        // then assert, so a lagging read isn't reported as a missing frame.
        let deadline = std::time::Instant::now() + TIMEOUT;
        let mut after = baseline(app);
        while after.frame - before.frame != groups(after) - groups(before)
            && std::time::Instant::now() < deadline
        {
            std::thread::sleep(std::time::Duration::from_millis(25));
            after = baseline(app);
        }

        assert!(
            groups(after) > groups(before),
            "expected new pointer event groups: {before:?} -> {after:?}"
        );
        assert_eq!(
            after.frame - before.frame,
            groups(after) - groups(before),
            "one frame per pointer event group: {before:?} -> {after:?}"
        );
    });
}

/// A `wl_subsurface` gets pointer input in child-local coordinates, exactly
/// like touch. The scene places the subsurface under the injection point.
#[test]
fn test_pointer_subsurface_target() {
    tawc_integration::helpers::test_init();
    with_wayland_subsurface(|app| {
        app.wait_for_tag_value("SURFACE_READY", "subsurface", TIMEOUT)
            .expect("subsurface ready");
        let before = baseline(app);
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_on_pointer_activity(before).expect("pointer motion");

        let last = positions(app, "POINTER_ENTER")
            .into_iter()
            .chain(positions(app, "POINTER_MOTION"))
            .last()
            .expect("a pointer position event");
        assert_eq!(last.target, "subsurface", "pointer target");
    });
}

/// A child surface with an empty `wl_surface.set_input_region` must not take
/// the pointer — it falls through to the parent toplevel. Same rule as touch,
/// and the reason Firefox/WebRender works at all.
#[test]
fn test_pointer_respects_empty_input_region() {
    tawc_integration::helpers::test_init();
    with_wayland_subsurface_input_empty(|app| {
        app.wait_for_tag_value("SURFACE_READY", "subsurface", TIMEOUT)
            .expect("subsurface ready");
        let before = baseline(app);
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_on_pointer_activity(before).expect("pointer motion");

        let last = positions(app, "POINTER_ENTER")
            .into_iter()
            .chain(positions(app, "POINTER_MOTION"))
            .last()
            .expect("a pointer position event");
        assert_eq!(
            last.target, "toplevel",
            "an empty input region must not swallow the pointer"
        );
    });
}

/// An `xdg_popup` takes pointer input over its parent, and the surface-local
/// coordinates include its shadow / window-geometry offset.
#[test]
fn test_pointer_popup_target() {
    tawc_integration::helpers::test_init();
    with_wayland_popup(|app| {
        app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
            .expect("popup ready");
        let before = baseline(app);
        adb::inject_pointer_move().expect("inject-pointer move");
        app.wait_on_pointer_activity(before).expect("pointer motion");

        let last = positions(app, "POINTER_ENTER")
            .into_iter()
            .chain(positions(app, "POINTER_MOTION"))
            .last()
            .expect("a pointer position event");
        assert_eq!(last.target, "popup", "pointer target");
        assert!(
            last.x >= 0.0 && last.y >= 0.0,
            "surface-local coordinates should be non-negative: {last:?}"
        );
    });
}

/// The compositor reports the pointer through the state query rather than
/// through logs — motion and axis are far too high-volume to log.
#[test]
fn test_state_query_reports_pointer() {
    tawc_integration::helpers::test_init();
    with_wayland_touch(|app| {
        settle_pointer_in_window(app);
        let state = String::from_utf8_lossy(
            &adb::query_state().expect("query-state").stdout,
        )
        .to_string();
        assert!(
            state.contains("pointer_present=true"),
            "expected a pointer capability in {state:?}"
        );
        assert!(
            state.contains("pointer_focus=yes"),
            "expected the pointer to be focused in {state:?}"
        );
    });
}
