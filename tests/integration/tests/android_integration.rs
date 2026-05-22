//! Android system integration tests. These drive Android-facing paths such as
//! Back dispatch and fullscreen restoration, then assert the Wayland client
//! observes the expected compositor behavior.

use tawc_integration::adb;
use tawc_integration::debug_app::DebugApp;
use tawc_integration::helpers::{
    assert_compositor_clean, ensure_wayland_debug_app, start_wayland_debug_popup_switch,
    start_wayland_debug_touch, TIMEOUT,
};
use tawc_integration::GraphicsBackend;

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;
const WAYLAND_DEBUG_ENV: &str = "";

fn inject_touch(kind: &str) {
    let output = adb::inject_touch(kind).unwrap_or_else(|e| panic!("inject-touch {kind}: {e}"));
    assert!(
        output.status.success(),
        "inject-touch {kind} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn press_back(label: &str) {
    let output = adb::input_back().unwrap_or_else(|e| panic!("{label}: {e}"));
    assert!(
        output.status.success(),
        "{label} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn test_xdg_configure_state_maximized_vs_fullscreen() {
    let binary = ensure_wayland_debug_app();

    let mut normal = DebugApp::start(BACKEND, &binary, "scale", "")
        .expect("start non-fullscreen debug app");
    normal
        .wait_for_tag_value("CONFIGURE_STATE", "maximized", TIMEOUT)
        .expect("non-fullscreen app should be configured maximized");
    assert!(
        normal
            .payloads_with_tag("CONFIGURE_STATE")
            .iter()
            .all(|s| s != "fullscreen"),
        "non-fullscreen app received fullscreen configure"
    );
    normal.stop().expect("normal debug app failed to stop cleanly");
    assert_compositor_clean();

    let mut fullscreen = start_wayland_debug_touch(BACKEND, WAYLAND_DEBUG_ENV);
    fullscreen
        .wait_for_tag_value("CONFIGURE_STATE", "fullscreen", TIMEOUT)
        .expect("fullscreen-requesting app should be configured fullscreen");
    fullscreen
        .stop()
        .expect("fullscreen debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_back_restores_fullscreen_then_sends_escape() {
    let mut app = start_wayland_debug_touch(BACKEND, WAYLAND_DEBUG_ENV);
    app.wait_for_tag_value("CONFIGURE_STATE", "fullscreen", TIMEOUT)
        .expect("fullscreen-requesting app should be configured fullscreen");

    press_back("input back");
    app.wait_for_tag_value("CONFIGURE_STATE", "maximized", TIMEOUT)
        .expect("back should restore fullscreen app to maximized");
    assert!(
        app.payloads_with_tag("KEY").iter().all(|s| s != "1"),
        "first back should restore fullscreen, not inject Escape"
    );

    press_back("second input back");
    app.wait_for_tag_value("KEY", "1", TIMEOUT)
        .expect("second back should inject Escape");

    app.stop()
        .expect("fullscreen debug app failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_back_dismisses_grabbed_popup_before_restoring_fullscreen() {
    let mut app = start_wayland_debug_popup_switch(BACKEND, WAYLAND_DEBUG_ENV);
    inject_touch("tap-menu-a");
    app.wait_for_tag_value("CONFIGURE_STATE", "fullscreen", TIMEOUT)
        .expect("popup switch scene should start fullscreen");
    app.wait_for_tag_value("SURFACE_READY", "popup", TIMEOUT)
        .expect("first grabbed popup ready");
    let maximized_before = app
        .payloads_with_tag("CONFIGURE_STATE")
        .iter()
        .filter(|s| *s == "maximized")
        .count();

    press_back("input back");
    app.wait_for_tag_value("POPUP_DONE", "", TIMEOUT)
        .expect("grabbed popup dismissed after back");
    let maximized_after = app
        .payloads_with_tag("CONFIGURE_STATE")
        .iter()
        .filter(|s| *s == "maximized")
        .count();
    assert_eq!(
        maximized_after, maximized_before,
        "back should dismiss popup before restoring fullscreen"
    );

    app.stop()
        .expect("popup switch debug app failed to stop cleanly");
    assert_compositor_clean();
}
