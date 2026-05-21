//! Application-level smoke tests. Verify that real desktop programs
//! launch, map a toplevel, and (for some) do something simple in response
//! to input. Deliberately do **not** assert which buffer path the client
//! uses — that's the `cpu_graphics::` / `hybris::` / `gfxstream::`
//! modules' job. The same app may appear here (launch smoke) and in one
//! of those (buffer-path coverage); see `notes/testing.md` "Test layout"
//! for the partition.
//!
//! Every test here runs under [`GraphicsBackend::Cpu`]: the launch path
//! is the most portable backend and these tests don't care which buffer
//! type ends up on the wire. Backend-specific launches (Firefox/STK
//! under libhybris / gfxstream) live in the per-backend modules.
//!
//! Requires the compositor to be up and an in-app distro installed.

use std::io::{BufRead, BufReader};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use tawc_integration::helpers::{
    assert_compositor_clean, launch_and_wait_for_toplevel, wait_for_keyboard_shown, TIMEOUT,
};
use tawc_integration::rootfs_process::RootfsProcess;
use tawc_integration::{adb, compositor, GraphicsBackend};

const BACKEND: GraphicsBackend = GraphicsBackend::Cpu;

const FIREFOX_CMD: &str = "firefox --no-remote";

const FIREFOX_LAUNCH_TIMEOUT: Duration = Duration::from_secs(30);
const STK_LAUNCH_TIMEOUT: Duration = Duration::from_secs(60);
const LXTERMINAL_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const LXTERMINAL_EXIT_TIMEOUT: Duration = Duration::from_secs(10);
const GTK_WIDGET_FACTORY_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK3_DEMO_APPLICATION_LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
const GTK3_MENU_OBSERVE_TIMEOUT: Duration = Duration::from_secs(5);

// Physical coordinates inside a GtkEntry on gtk4-widget-factory's first page.
// The stock app opens on page 1, whose first column contains editable entries
// near the upper left. Tapping this point focuses one of those GtkEntry widgets
// and makes GTK enable text-input-v3.
const GTK_WIDGET_FACTORY_ENTRY_X: u32 = 170;
const GTK_WIDGET_FACTORY_ENTRY_Y: u32 = 220;

// Physical coordinates for the second menubar item in gtk3-demo-application
// on the standard integration-test phone layout with output scale 2.0. Keep
// both GTK3 menu tests on this exact tap: if GTK3 fixes the cold-state bug,
// the "verify broken" test should fail instead of drifting to a different
// click target.
const GTK3_DEMO_APPLICATION_MENU_X: u32 = 340;
const GTK3_DEMO_APPLICATION_MENU_Y: u32 = 170;

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

struct OutputScaleGuard {
    previous: f32,
}

impl OutputScaleGuard {
    fn set(scale: f32) -> Self {
        let previous = adb::get_output_scale().expect("get output scale");
        assert_broker_ok(
            adb::set_output_scale(scale).unwrap_or_else(|e| panic!("set output scale: {e}")),
            "set-output-scale",
        );
        Self { previous }
    }
}

impl Drop for OutputScaleGuard {
    fn drop(&mut self) {
        let _ = adb::set_output_scale(self.previous);
    }
}

struct Gtk3BrokenMenusWorkaroundGuard {
    previous: bool,
}

impl Gtk3BrokenMenusWorkaroundGuard {
    fn set(enabled: bool) -> Self {
        let previous = adb::get_gtk3_broken_menus_workaround()
            .expect("get gtk3 broken menus workaround setting");
        assert_broker_ok(
            adb::set_gtk3_broken_menus_workaround(enabled)
                .unwrap_or_else(|e| panic!("set gtk3 broken menus workaround: {e}")),
            "set-gtk3-broken-menus-workaround",
        );
        Self { previous }
    }
}

impl Drop for Gtk3BrokenMenusWorkaroundGuard {
    fn drop(&mut self) {
        let _ = adb::set_gtk3_broken_menus_workaround(self.previous);
    }
}

fn assert_broker_ok(output: std::process::Output, action: &str) {
    assert!(
        output.status.success(),
        "{action} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn ctrl_key(keycode: u32) {
    assert_broker_ok(
        adb::ic_send_modified_key_event(keycode, true, false, false)
            .unwrap_or_else(|e| panic!("Ctrl key action failed: {e}")),
        "ic-send-modified-key-event",
    );
    std::thread::sleep(Duration::from_millis(150));
}

fn wait_for_android_clipboard(expected: &str, timeout: Duration) {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        let got = adb::clipboard_get_text().expect("get Android clipboard");
        if got == expected {
            return;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "Android clipboard did not become {:?}; last={:?}",
            expected,
            got
        );
        std::thread::sleep(Duration::from_millis(100));
    }
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

fn spawn_relevant_stderr_reader(
    stderr: std::process::ChildStderr,
) -> mpsc::Receiver<String> {
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
    let _scale_guard = OutputScaleGuard::set(2.0);
    let _workaround_guard = Gtk3BrokenMenusWorkaroundGuard::set(workaround_enabled);
    adb::logcat_clear().expect("Failed to clear logcat");

    let (mut app, rx) = launch_gtk3_demo_application_with_wayland_debug();
    adb::input_tap(GTK3_DEMO_APPLICATION_MENU_X, GTK3_DEMO_APPLICATION_MENU_Y)
        .expect("tap gtk3-demo-application menu");

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
        "unexpected GTK3 menu opened after identical tap at ({}, {}), \
         touch={:?}, anchors_after_touch={:?}; relevant WAYLAND_DEBUG lines:\n{}",
        GTK3_DEMO_APPLICATION_MENU_X,
        GTK3_DEMO_APPLICATION_MENU_Y,
        observation.touch,
        observation.anchors_after_touch,
        observation.relevant_lines.join("\n")
    );
}

/// Firefox launches, maps a toplevel, and the compositor sees a client.
/// Buffer-path coverage lives in `hybris::test_firefox_renders_via_ahb` and
/// `gfxstream::test_firefox_renders_via_ahb`.
#[test]
fn test_firefox_launches() {
    // Remove lock/crash files so Firefox doesn't show the troubleshoot-mode dialog
    // (killall doesn't give Firefox a clean shutdown, leaving these behind)
    let _ = adb::rootfs_run(
        "rm -f ~/.config/mozilla/firefox/*/.parentlock \
              ~/.config/mozilla/firefox/*/lock \
              ~/.config/mozilla/firefox/*/sessionstore.jsonlz4 \
              ~/.config/mozilla/firefox/*/sessionCheckpoints.json && \
         rm -rf ~/.config/mozilla/firefox/*/sessionstore-backups",
    );

    let mut firefox = launch_and_wait_for_toplevel(BACKEND, FIREFOX_CMD, "Firefox", FIREFOX_LAUNCH_TIMEOUT);

    let state = compositor::query_state(TIMEOUT)
        .expect("Failed to query compositor state while Firefox running");
    assert!(
        state.clients >= 1,
        "Compositor should see at least 1 client while Firefox running, got {:?}",
        state
    );

    firefox.stop().expect("Firefox process group failed to stop cleanly");
    assert_compositor_clean();
}

/// supertuxkart launches and maps a toplevel; sticks around past first
/// paint (some GL init failures only manifest a moment after the window
/// appears). Buffer-path coverage lives in
/// `hybris::test_supertuxkart_renders_via_ahb` and
/// `gfxstream::test_supertuxkart_renders_via_ahb`.
#[test]
fn test_supertuxkart_launches() {
    let mut stk = launch_and_wait_for_toplevel(BACKEND, "supertuxkart", "supertuxkart", STK_LAUNCH_TIMEOUT);

    std::thread::sleep(Duration::from_secs(1));
    assert!(
        stk.is_running(),
        "supertuxkart exited shortly after mapping its window"
    );

    stk.stop()
        .expect("supertuxkart process group failed to stop cleanly");
    assert_compositor_clean();
}

/// lxterminal hosts a VTE terminal — the canonical surroundingless
/// text-input-v3 client (see `test_wayland_surroundingless_client_uses_keyboard_for_backspace`
/// in tests/input.rs for the protocol-level coverage). This test wires
/// up the full real-world stack: launch lxterminal, verify it stays up
/// past first paint, type `exit` followed by Enter, and assert the
/// process actually exits. That implicitly proves:
///   - `commit_string` (text-input-v3) reaches VTE → PTY → shell as
///     keystrokes; without it the shell never sees `exit`.
///   - `wl_keyboard` Enter reaches the shell as a newline; without it
///     the shell never executes the typed command.
///   - Compositor cleans up on a normal client exit (no leftover
///     toplevel/client).
///
/// `lxterminal` ships in the distro's [DEFAULT_BASE_PACKAGES], so a
/// fresh install already has it — no extra test package install
/// rerun needed.
#[test]
fn test_lxterminal_input_and_exit() {
    // Stub out the system IME so it doesn't amplify our `commitText` into
    // the lxterminal IC and produce extra characters.
    adb::enable_test_input().expect("enable-test-input action");

    let mut term = launch_and_wait_for_toplevel(
        BACKEND,
        "lxterminal",
        "lxterminal",
        LXTERMINAL_LAUNCH_TIMEOUT,
    );

    // launch_and_wait_for_toplevel already panics if the process exits
    // before its first window, but a terminal that opens then closes
    // shortly after (e.g. shell crash on startup) wouldn't be caught by
    // the in-loop check. Re-verify after a grace period.
    std::thread::sleep(Duration::from_secs(1));
    assert!(
        term.is_running(),
        "lxterminal exited shortly after first frame — shell crash on startup?"
    );

    // Drive the shell: "exit" via the IC's commitText, newline via
    // wl_keyboard Enter. Same path a soft-keyboard user typing into
    // the terminal would take — the IC translates `commitText` into
    // text-input-v3 `commit_string` and `sendKeyEvent(KEYCODE_ENTER)`
    // into a `wl_keyboard` Enter.
    adb::ic_commit_text("exit").expect("commit 'exit'");
    adb::ic_send_key_event(adb::KEYCODE_ENTER).expect("Enter");

    let deadline = std::time::Instant::now() + LXTERMINAL_EXIT_TIMEOUT;
    while std::time::Instant::now() < deadline {
        if !term.is_running() {
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(
        !term.is_running(),
        "lxterminal still running {:?} after `exit`+Enter — input didn't \
         reach the shell. commit_string lost between text-input-v3 and \
         VTE's PTY, or KEYCODE_ENTER never delivered as a wl_keyboard key.",
        LXTERMINAL_EXIT_TIMEOUT
    );

    // term already exited; stop() returns Err for already-gone
    // processes — discard it. Drop tears down the lingering local adb
    // shell wrapper.
    let _ = term.stop();
    assert_compositor_clean();
}

/// The GTK debug app gives us protocol-level assertions; this is the
/// stock GTK4 app smoke for the same user-facing path. It drives a real
/// `gtk4-widget-factory` GtkEntry:
///   - Android clipboard -> Wayland selection -> GTK paste (`Ctrl+V`)
///   - Android IC text input and editing inside GTK
///   - GTK copy (`Ctrl+C`) -> Wayland selection -> Android clipboard
#[test]
fn test_gtk4_widget_factory_copy_paste_and_text_input() {
    let paste_text = "gtk4 widget factory paste";
    let expected = "gtk4 widget factory paste edited";

    adb::logcat_clear().expect("Failed to clear logcat");
    adb::enable_test_input().expect("enable-test-input action");
    adb::clipboard_set_text(paste_text).expect("set Android clipboard");

    let mut factory = launch_and_wait_for_toplevel(
        BACKEND,
        "gtk4-widget-factory",
        "gtk4-widget-factory",
        GTK_WIDGET_FACTORY_LAUNCH_TIMEOUT,
    );

    adb::input_tap(GTK_WIDGET_FACTORY_ENTRY_X, GTK_WIDGET_FACTORY_ENTRY_Y)
        .expect("tap gtk4-widget-factory entry");
    wait_for_keyboard_shown(TIMEOUT);

    ctrl_key(adb::KEYCODE_A);
    ctrl_key(adb::KEYCODE_V);

    adb::ic_commit_text(" input").expect("commit GTK input text");
    std::thread::sleep(Duration::from_millis(250));
    adb::ic_delete_surrounding_text(5, 0).expect("delete GTK input text");
    std::thread::sleep(Duration::from_millis(250));
    adb::ic_commit_text("edited").expect("commit GTK edited text");
    std::thread::sleep(Duration::from_millis(250));

    ctrl_key(adb::KEYCODE_A);
    ctrl_key(adb::KEYCODE_C);
    wait_for_android_clipboard(expected, TIMEOUT);

    factory
        .stop()
        .expect("gtk4-widget-factory failed to stop cleanly");
    assert_compositor_clean();
}

#[test]
fn test_gtk3_demo_application_menu_opens_leftmost_without_workaround() {
    assert_gtk3_demo_application_menu_tap(false, Gtk3Menu::Leftmost);
}

#[test]
fn test_gtk3_demo_application_menu_opens_tapped_menu_with_workaround() {
    assert_gtk3_demo_application_menu_tap(true, Gtk3Menu::Tapped);
}
