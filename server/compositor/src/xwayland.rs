//! XWayland integration: spawning the bionic-built `Xwayland` binary,
//! routing X11 surfaces through tawc's host/toplevel model, and feeding
//! the X11 window manager loop.
//!
//! The Xwayland binary itself, plus its DT_NEEDED libs, are extracted
//! from the APK to `/data/data/me.phie.tawc/files/xwayland/` by
//! `CompositorService.ensureXwaylandExtracted` (Kotlin). The compositor
//! sets `PATH` and `LD_LIBRARY_PATH` to find them.
//!
//! The X11 socket lives at `/data/data/me.phie.tawc/xtmp/.X11-unix/X{N}`
//! and the lockfile at `/data/data/me.phie.tawc/xtmp/.X{N}-lock`. Smithay
//! reads `TAWC_XWL_RUNTIME_DIR` to override its `/tmp` defaults — see
//! the `tawc-patches` branch on the smithay fork. The chroot side bind-
//! mounts our xtmp dir so X clients inside the chroot find `:0` at the
//! standard `/tmp/.X11-unix/X0` path (via a symlink set up by
//! `ChrootMounter`).

use std::process::Stdio;

use log::{error, info, warn};
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::utils::{Logical, Rectangle};
use smithay::wayland::compositor::CompositorHandler;
use smithay::wayland::xwayland_shell::{XWaylandShellHandler, XWaylandShellState};
use smithay::xwayland::{
    xwm::{Reorder, ResizeEdge, XwmId},
    X11Surface, X11Wm, XWayland, XWaylandEvent, XwmHandler,
};
use smithay::wayland::selection::SelectionTarget;

use crate::compositor::TawcState;
use crate::event_loop::LoopData;
use crate::host::ActivityId;

/// Where Xwayland's listening socket / lockfile live. Patched smithay
/// reads `TAWC_XWL_RUNTIME_DIR` from env; bionic-built libxcb / xtrans
/// have this baked in (see `client/build-xwayland-aarch64`).
pub const XWL_RUNTIME_DIR: &str = "/data/data/me.phie.tawc/xtmp";

/// Where the in-app extractor stages the Xwayland binary + libs.
pub const XWL_INSTALL_DIR: &str = "/data/data/me.phie.tawc/files/xwayland";

/// Spawn Xwayland and insert it as a calloop event source. On the
/// `Ready` event the X11 window manager is constructed and stashed on
/// the state.
pub fn start_xwayland(
    handle: &smithay::reexports::calloop::LoopHandle<'static, LoopData>,
    state: &TawcState,
) {
    let handle_for_wm = handle.clone();
    // Make sure /data/data/me.phie.tawc/xtmp/.X11-unix/ exists. Patched
    // smithay drops the listening socket here; bionic-built libxcb /
    // xtrans look up :0 here too (from the compositor side — chroot side
    // is symlinked into here from /tmp/.X11-unix).
    if let Err(e) = std::fs::create_dir_all(format!("{}/.X11-unix", XWL_RUNTIME_DIR)) {
        warn!("xwayland: mkdir {}: {}", XWL_RUNTIME_DIR, e);
    }
    // Best-effort cleanup of stale lockfiles from a previous compositor
    // run. Smithay's lock-grab logic recovers from a stale PID lock too,
    // but that path is slower.
    for n in 0..3 {
        let _ = std::fs::remove_file(format!("{}/.X{}-lock", XWL_RUNTIME_DIR, n));
        let _ = std::fs::remove_file(format!("{}/.X11-unix/X{}", XWL_RUNTIME_DIR, n));
    }

    // Tell our patched smithay where to put X11 sockets.
    std::env::set_var("TAWC_XWL_RUNTIME_DIR", XWL_RUNTIME_DIR);
    // Tell Xwayland (via execve) where to find its bionic libs and
    // friends. PATH is consumed by `Command::new("Xwayland")` lookup;
    // LD_LIBRARY_PATH by the runtime linker for the spawned binary.
    let path_with_xwl = match std::env::var("PATH") {
        Ok(p) => format!("{}/bin:{}", XWL_INSTALL_DIR, p),
        Err(_) => format!("{}/bin", XWL_INSTALL_DIR),
    };
    std::env::set_var("PATH", &path_with_xwl);

    let dh = state.display_handle.clone();
    let envs: Vec<(String, String)> = vec![(
        "LD_LIBRARY_PATH".to_string(),
        format!("{}/lib", XWL_INSTALL_DIR),
    )];

    // Pipe Xwayland's stderr/stdout to a log file under our xtmp dir so
    // we can post-mortem startup failures (Stdio::null() drops them on
    // the floor and android_logger is JVM-only). This is a debug
    // convenience; can be removed once Xwayland is stable.
    let log_path = format!("{}/xwayland.log", XWL_RUNTIME_DIR);
    let (stdout, stderr): (Stdio, Stdio) = match std::fs::File::create(&log_path) {
        Ok(f) => {
            let f2 = f.try_clone().unwrap_or_else(|_| std::fs::File::create(&log_path).unwrap());
            (f.into(), f2.into())
        }
        Err(e) => {
            warn!("xwayland: cannot open {} for log: {}", log_path, e);
            (Stdio::null(), Stdio::null())
        }
    };

    let (xwayland, client) = match XWayland::spawn(
        &dh,
        None,
        envs,
        true, // open_abstract_socket — Linux only, costs nothing on Android
        stdout,
        stderr,
        |_| (),
    ) {
        Ok(pair) => pair,
        Err(e) => {
            error!("xwayland: failed to spawn Xwayland: {}", e);
            return;
        }
    };

    info!("xwayland: spawned Xwayland; waiting for Ready");

    let xwayland_client_for_handler = client.clone();
    let result = handle.insert_source(xwayland, move |event, _, data: &mut LoopData| match event {
        XWaylandEvent::Ready { x11_socket, display_number } => {
            info!("xwayland: Ready, display={}", display_number);
            match X11Wm::start_wm(
                handle_for_wm.clone(),
                x11_socket,
                xwayland_client_for_handler.clone(),
            ) {
                Ok(wm) => {
                    data.state.xwm = Some(wm);
                    data.state.xdisplay = Some(display_number);
                    info!("xwayland: X11Wm attached, DISPLAY=:{}", display_number);
                }
                Err(e) => {
                    error!("xwayland: X11Wm::start_wm failed: {}", e);
                }
            }
        }
        XWaylandEvent::Error => {
            error!("xwayland: Xwayland crashed during startup");
        }
    });
    if let Err(e) = result {
        error!("xwayland: insert_source failed: {}", e);
    }
}

// ---------------------------------------------------------------------------
// XWaylandShellHandler — protocol object that associates X11 windows with
// their backing wl_surface.
// ---------------------------------------------------------------------------

impl XWaylandShellHandler for TawcState {
    fn xwayland_shell_state(&mut self) -> &mut XWaylandShellState {
        &mut self.xwayland_shell_state
    }
}

// X11Wm::start_wm needs the calloop data type to implement
// XwmHandler + XWaylandShellHandler. Our calloop data is `LoopData`
// (which wraps `TawcState`); these forwarding impls delegate
// straight to the inner state.
impl XWaylandShellHandler for LoopData {
    fn xwayland_shell_state(&mut self) -> &mut XWaylandShellState {
        self.state.xwayland_shell_state()
    }
}

// ---------------------------------------------------------------------------
// XwmHandler — X11 window manager events. Mirrors the parts of
// `anvil/src/shell/x11.rs` that are relevant to a no-decoration,
// touch-driven, single-output compositor like tawc; everything around
// pointer move/resize grabs and selection bridging is left as stubs
// because we don't have an X11-aware seat path yet.
// ---------------------------------------------------------------------------

impl XwmHandler for TawcState {
    fn xwm_state(&mut self, _xwm: XwmId) -> &mut X11Wm {
        // Only one xwm, so we can just unwrap. Smithay never calls this
        // with an id we don't own.
        self.xwm.as_mut().expect("xwm_state called before X11Wm::start_wm")
    }

    fn new_window(&mut self, _xwm: XwmId, _window: X11Surface) {
        // Window object created on the X side — wait for map_request
        // before committing to any host placement.
    }

    fn new_override_redirect_window(&mut self, _xwm: XwmId, _window: X11Surface) {}

    fn map_window_request(&mut self, _xwm: XwmId, window: X11Surface) {
        info!(
            "xwayland: map_window_request title={:?} class={:?} geo={:?}",
            window.title(),
            window.class(),
            window.geometry(),
        );
        // Pick a host the same way xdg toplevels do.
        let host = pick_host_for_x11(self);
        if let Err(e) = window.set_mapped(true) {
            warn!("xwayland: set_mapped(true) failed: {}", e);
            return;
        }
        // Honour the X client's requested size — most X-only apps
        // (xeyes, xclock, xterm) request a small window and forcing
        // them to fullscreen produces unintelligible output. If the
        // requested geometry is empty (override-redirect, fresh
        // window), seed with a small placeholder; the client will
        // reconfigure on its first commit.
        let mut geo = window.geometry();
        if geo.size.w <= 0 || geo.size.h <= 0 {
            geo.size = (150, 100).into();
        }
        if let Err(e) = window.configure(geo) {
            warn!("xwayland: configure failed: {}", e);
        }
        self.x11_surfaces.push(window.clone());
        // Defer host assignment until the X11Surface gets a backing
        // wl_surface (commit hook captures it). For now record the
        // pending host on the X11Surface's user_data so we don't lose
        // it.
        window.user_data().insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(host))));
        self.toplevels_changed = true;
    }

    fn mapped_override_redirect_window(&mut self, _xwm: XwmId, window: X11Surface) {
        info!("xwayland: mapped_OR {:?}", window);
        let host = pick_host_for_x11(self);
        self.x11_surfaces.push(window.clone());
        window.user_data().insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(host))));
        self.toplevels_changed = true;
    }

    fn unmapped_window(&mut self, _xwm: XwmId, window: X11Surface) {
        info!("xwayland: unmapped {:?}", window);
        if let Some(wl) = window.wl_surface() {
            self.x11_to_host.remove(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        if !window.is_override_redirect() {
            let _ = window.set_mapped(false);
        }
        self.toplevels_changed = true;
    }

    fn destroyed_window(&mut self, _xwm: XwmId, window: X11Surface) {
        if let Some(wl) = window.wl_surface() {
            self.x11_to_host.remove(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        self.toplevels_changed = true;
    }

    fn configure_request(
        &mut self,
        _xwm: XwmId,
        window: X11Surface,
        _x: Option<i32>,
        _y: Option<i32>,
        w: Option<u32>,
        h: Option<u32>,
        _reorder: Option<Reorder>,
    ) {
        // We never let X11 clients move themselves; honour size requests
        // but pin position to the host origin. This matches what
        // server-side decoration looks like (always full-screen-ish).
        let mut geo = window.geometry();
        if let Some(w) = w {
            geo.size.w = (w as i32).max(1);
        }
        if let Some(h) = h {
            geo.size.h = (h as i32).max(1);
        }
        let _ = window.configure(geo);
    }

    fn configure_notify(
        &mut self,
        _xwm: XwmId,
        _window: X11Surface,
        _geometry: Rectangle<i32, Logical>,
        _above: Option<u32>,
    ) {
        // Override-redirect windows update their on-screen rect via this
        // path. We don't track per-X11-surface positions yet; everything
        // renders at the host origin.
    }

    fn maximize_request(&mut self, _xwm: XwmId, _window: X11Surface) {}
    fn unmaximize_request(&mut self, _xwm: XwmId, _window: X11Surface) {}
    fn fullscreen_request(&mut self, _xwm: XwmId, _window: X11Surface) {}
    fn unfullscreen_request(&mut self, _xwm: XwmId, _window: X11Surface) {}
    fn resize_request(
        &mut self,
        _xwm: XwmId,
        _window: X11Surface,
        _button: u32,
        _edges: ResizeEdge,
    ) {
    }
    fn move_request(&mut self, _xwm: XwmId, _window: X11Surface, _button: u32) {}

    // Selection (clipboard / primary) bridging — left as no-ops; tawc
    // doesn't currently bridge wayland↔X11 clipboards.
    fn allow_selection_access(&mut self, _xwm: XwmId, _selection: SelectionTarget) -> bool {
        false
    }
    fn send_selection(
        &mut self,
        _xwm: XwmId,
        _selection: SelectionTarget,
        _mime_type: String,
        _fd: std::os::unix::io::OwnedFd,
    ) {
    }
    fn new_selection(
        &mut self,
        _xwm: XwmId,
        _selection: SelectionTarget,
        _mime_types: Vec<String>,
    ) {
    }
    fn cleared_selection(&mut self, _xwm: XwmId, _selection: SelectionTarget) {}
}

// Forwarding impl on LoopData (the calloop-data type), delegating to
// the inner TawcState. Required because X11Wm::start_wm bounds on the
// LoopHandle's data type implementing XwmHandler.
impl XwmHandler for LoopData {
    fn xwm_state(&mut self, xwm: XwmId) -> &mut X11Wm {
        self.state.xwm_state(xwm)
    }
    fn new_window(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.new_window(xwm, window)
    }
    fn new_override_redirect_window(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.new_override_redirect_window(xwm, window)
    }
    fn map_window_request(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.map_window_request(xwm, window)
    }
    fn mapped_override_redirect_window(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.mapped_override_redirect_window(xwm, window)
    }
    fn unmapped_window(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.unmapped_window(xwm, window)
    }
    fn destroyed_window(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.destroyed_window(xwm, window)
    }
    fn configure_request(
        &mut self,
        xwm: XwmId,
        window: X11Surface,
        x: Option<i32>,
        y: Option<i32>,
        w: Option<u32>,
        h: Option<u32>,
        reorder: Option<Reorder>,
    ) {
        self.state.configure_request(xwm, window, x, y, w, h, reorder)
    }
    fn configure_notify(
        &mut self,
        xwm: XwmId,
        window: X11Surface,
        geometry: Rectangle<i32, Logical>,
        above: Option<u32>,
    ) {
        self.state.configure_notify(xwm, window, geometry, above)
    }
    fn maximize_request(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.maximize_request(xwm, window)
    }
    fn unmaximize_request(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.unmaximize_request(xwm, window)
    }
    fn fullscreen_request(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.fullscreen_request(xwm, window)
    }
    fn unfullscreen_request(&mut self, xwm: XwmId, window: X11Surface) {
        self.state.unfullscreen_request(xwm, window)
    }
    fn resize_request(&mut self, xwm: XwmId, window: X11Surface, button: u32, edges: ResizeEdge) {
        self.state.resize_request(xwm, window, button, edges)
    }
    fn move_request(&mut self, xwm: XwmId, window: X11Surface, button: u32) {
        self.state.move_request(xwm, window, button)
    }
    fn allow_selection_access(&mut self, xwm: XwmId, selection: SelectionTarget) -> bool {
        self.state.allow_selection_access(xwm, selection)
    }
    fn send_selection(
        &mut self,
        xwm: XwmId,
        selection: SelectionTarget,
        mime_type: String,
        fd: std::os::unix::io::OwnedFd,
    ) {
        self.state.send_selection(xwm, selection, mime_type, fd)
    }
    fn new_selection(&mut self, xwm: XwmId, selection: SelectionTarget, mime_types: Vec<String>) {
        self.state.new_selection(xwm, selection, mime_types)
    }
    fn cleared_selection(&mut self, xwm: XwmId, selection: SelectionTarget) {
        self.state.cleared_selection(xwm, selection)
    }
}

/// `PendingHost` is the host an X11Surface was assigned to at
/// map_window_request time. The wl_surface backing arrives later (via
/// xwayland_shell_v1.set_serial), at which point the compositor's
/// commit hook moves the host id from this user_data slot into
/// `state.x11_to_host`.
pub struct PendingHost(pub std::cell::RefCell<Option<ActivityId>>);

/// Pick a host for a freshly-mapped X11 surface using the same policy
/// as xdg toplevels. Currently single-Activity for X11 clients (every
/// X11 toplevel is a child of the same Android task) — XWayland is one
/// glibc-style display and most X clients assume DISPLAY identity, so
/// per-window Activity spawning would be confusing.
fn pick_host_for_x11(state: &mut TawcState) -> ActivityId {
    if let Some(fg) = &state.foreground_host {
        if state.hosts.contains_key(fg) {
            return fg.clone();
        }
    }
    if let Some(any) = state.hosts.keys().next().cloned() {
        return any;
    }
    // No host registered yet — mint a fresh id and let the surface's
    // host stay pending until an Activity arrives.
    crate::host::new_activity_id()
}

/// Called from CompositorHandler::commit when an X11 surface's
/// wl_surface gets associated. Promotes the PendingHost user_data slot
/// to a real entry in `state.x11_to_host`.
pub fn associate_x11_surface_if_pending(state: &mut TawcState, wl: &WlSurface) {
    if state.x11_surfaces.is_empty() {
        return;
    }
    if state.x11_to_host.contains_key(wl) {
        return;
    }
    let surface = match state
        .x11_surfaces
        .iter()
        .find(|s| s.wl_surface().as_ref() == Some(wl))
    {
        Some(s) => s.clone(),
        None => {
            log::debug!(
                "xwayland: commit doesn't match any of {} X11 surfaces",
                state.x11_surfaces.len()
            );
            return;
        }
    };
    let host = surface
        .user_data()
        .get::<PendingHost>()
        .and_then(|p| p.0.borrow_mut().take())
        .unwrap_or_else(|| pick_host_for_x11(state));
    info!(
        "xwayland: associated X11 surface {} to host {}",
        surface.window_id(),
        host,
    );
    state.x11_to_host.insert(wl.clone(), host);
    state.toplevels_changed = true;
}
