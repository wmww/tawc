//! Pixel-level rendering checks for compositor coordinate/orientation paths.

use std::time::Duration;

use tawc_integration::adb;
use tawc_integration::compositor;
use tawc_integration::helpers::{
    assert_compositor_clean, start_wayland_debug_popup, start_wayland_debug_render_pattern,
    TIMEOUT,
};
use tawc_integration::GraphicsBackend;

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const WAYLAND_DEBUG_ENV: &str = "";

#[derive(Clone, Copy, Debug)]
struct Rgb {
    r: u8,
    g: u8,
    b: u8,
}

fn physical_coord(logical: i32, logical_extent: i32, physical_extent: i32) -> u32 {
    ((logical as f64 * physical_extent as f64 / logical_extent as f64).round() as i32)
        .clamp(0, physical_extent.saturating_sub(1)) as u32
}

fn sample_logical(
    shot: &adb::RawScreenshot,
    state: &compositor::CompositorState,
    logical_x: i32,
    logical_y: i32,
) -> Rgb {
    let x = physical_coord(logical_x, state.output_logical_w, state.output_physical_w);
    let y = physical_coord(logical_y, state.output_logical_h, state.output_physical_h);
    let [r, g, b, _a] = shot
        .pixel_rgba(x, y)
        .unwrap_or_else(|| panic!("screenshot missing pixel at physical {x},{y}"));
    Rgb { r, g, b }
}

fn assert_red(c: Rgb, label: &str) {
    assert!(c.r > 180 && c.g < 100 && c.b < 100, "{label}: {c:?}");
}

fn assert_green(c: Rgb, label: &str) {
    assert!(c.g > 130 && c.r < 100 && c.b < 120, "{label}: {c:?}");
}

fn assert_blue(c: Rgb, label: &str) {
    assert!(c.b > 170 && c.r < 100 && c.g < 120, "{label}: {c:?}");
}

fn assert_yellow(c: Rgb, label: &str) {
    assert!(c.r > 160 && c.g > 140 && c.b < 100, "{label}: {c:?}");
}

#[derive(Debug)]
struct PopupLayout {
    child_x: i32,
    child_y: i32,
    content_w: i32,
    content_h: i32,
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
        content_w: fields[3],
        content_h: fields[4],
    }
}

#[test]
fn test_shm_render_pattern_orientation_pixels() {
    tawc_integration::helpers::test_init();
    let mut app = start_wayland_debug_render_pattern(BACKEND, WAYLAND_DEBUG_ENV);
    compositor::wait_for_rendered_toplevels(1, TIMEOUT).expect("render-pattern visible");
    std::thread::sleep(Duration::from_millis(250));

    let state = compositor::query_state(TIMEOUT).expect("query compositor state");
    assert_eq!(state.surfaces_shm, 1, "render-pattern should use one SHM surface: {state:?}");
    assert_eq!(state.surfaces_wlegl, 0, "render-pattern should not use AHB: {state:?}");

    let shot = adb::screencap_raw().expect("raw screencap");
    assert_eq!(shot.width as i32, state.output_physical_w, "screencap width/state mismatch");
    assert_eq!(shot.height as i32, state.output_physical_h, "screencap height/state mismatch");
    assert_eq!(shot.format, 1, "expected RGBA_8888 screencap format");

    // The debug app draws four 80x80 logical color blocks with centers 136
    // logical pixels from each edge, outside tawc's 150-physical-pixel tint
    // fade band on the standing physical target. Sampling these centers
    // catches y-flips, x-flips, scale errors, and basic SHM draw placement.
    let inset_center = 136;
    let right_center = state.output_logical_w - inset_center;
    let bottom_center = state.output_logical_h - inset_center;

    assert_red(
        sample_logical(&shot, &state, inset_center, inset_center),
        "top-left block",
    );
    assert_green(
        sample_logical(&shot, &state, right_center, inset_center),
        "top-right block",
    );
    assert_blue(
        sample_logical(&shot, &state, inset_center, bottom_center),
        "bottom-left block",
    );
    assert_yellow(
        sample_logical(&shot, &state, right_center, bottom_center),
        "bottom-right block",
    );

    app.stop()
        .expect("render-pattern app crashed or failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_shm_xdg_popup_position_pixels() {
    tawc_integration::helpers::test_init();
    let mut app = start_wayland_debug_popup(BACKEND, WAYLAND_DEBUG_ENV);
    app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
        .expect("popup ready");
    std::thread::sleep(Duration::from_millis(250));

    let layout = parse_popup_layout(
        app.payloads_with_tag("POPUP_LAYOUT")
            .last()
            .expect("popup layout payload"),
    );
    let state = compositor::query_state(TIMEOUT).expect("query compositor state");
    let shot = adb::screencap_raw().expect("raw screencap");

    let sample = sample_logical(
        &shot,
        &state,
        layout.child_x + layout.content_w / 2,
        layout.child_y + layout.content_h / 2,
    );
    assert!(
        sample.r > 150 && sample.g > 90 && sample.b < 130,
        "popup center should be orange at its configured logical position: \
         sample={sample:?} layout={layout:?} state={state:?}"
    );

    app.stop()
        .expect("popup app crashed or failed to stop cleanly");
    assert_compositor_clean();
}
