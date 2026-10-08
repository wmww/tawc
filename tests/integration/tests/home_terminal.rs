//! The home screen's terminal tabs (notes/terminal.md "Lifecycle"): what
//! happens when a terminal goes away, per the selection at the time
//! (the recents swipe is in lazy_compositor.rs). Typed into through the
//! real IME path (`input`), so MainActivity must be visible.

use tawc_integration::helpers::{
    show_home_apps, show_home_tab, show_home_terminal, terminal_run, test_init,
    wait_terminal_shell, wait_terminal_state,
};
use tawc_integration::{adb, GraphicsBackend};

/// New terminal tab, then wait for its shell.
fn open_tab(want: &str) {
    adb::home_tab("new").expect("home-tab new");
    wait_terminal_state(want);
    wait_terminal_shell();
}

/// Type a command that exits the shell once [release] is called with
/// the same `name`.
fn exit_on_release(name: &str) {
    adb::rootfs_run_with(GraphicsBackend::Cpu, &format!("rm -f /tmp/tawc-ht-{name}"))
        .expect("rootfs run");
    terminal_run(&format!("until%s[%s-e%s/tmp/tawc-ht-{name}%s];do%ssleep%s0.1;done;exit"));
}

fn release(name: &str) {
    adb::rootfs_run_with(GraphicsBackend::Cpu, &format!("touch /tmp/tawc-ht-{name}"))
        .expect("rootfs run");
}

/// The last terminal exiting while selected closes the app; with others
/// left the right neighbour is selected, else the left one.
#[test]
fn test_terminal_exit_selects_neighbour_then_closes_app() {
    test_init();
    show_home_terminal();
    let reasons = adb::session_state().expect("session-state");
    assert!(reasons.iter().any(|r| r.starts_with("terminal ")), "terminal holds no session reason: {reasons:?}");
    open_tab("tabs:2 selected:1");
    open_tab("tabs:3 selected:2");

    // Middle tab exits: its right neighbour (now at its index) is selected.
    show_home_tab("1");
    wait_terminal_state("tabs:3 selected:1");
    terminal_run("exit");
    wait_terminal_state("tabs:2 selected:1");

    // Rightmost exits: the left neighbour.
    terminal_run("exit");
    wait_terminal_state("tabs:1 selected:0");

    // Last one: the app closes.
    terminal_run("exit");
    wait_terminal_state("tabs:0 selected:none");
    let reasons = adb::session_state().expect("session-state");
    assert!(!reasons.iter().any(|r| r.starts_with("terminal ")), "closed shell still holds: {reasons:?}");

    // Later tests expect a TAWC activity in front.
    show_home_apps();
}

/// A terminal exiting while the apps tab or another terminal is
/// selected only drops its tab; the selection stays and the app stays
/// open.
#[test]
fn test_unselected_terminal_exit_keeps_selection() {
    test_init();
    show_home_terminal();
    exit_on_release("a");
    adb::home_tab("apps").expect("home-tab apps");
    wait_terminal_state("tabs:1 selected:apps");
    release("a");
    wait_terminal_state("tabs:0 selected:apps");

    // Tab 0 exits behind tab 1, which keeps the selection at its new index.
    show_home_terminal();
    exit_on_release("b");
    open_tab("tabs:2 selected:1");
    release("b");
    wait_terminal_state("tabs:1 selected:0");
    terminal_run("exit");
    wait_terminal_state("tabs:0 selected:none");
    show_home_apps();
}
