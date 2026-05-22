//! XWayland integration: spawning the bionic-built `Xwayland` binary,
//! routing X11 surfaces through tawc's host/toplevel model, and feeding
//! the X11 window manager loop.
//!
//! The Xwayland binary, xkbcomp, and their DT_NEEDED libs are shipped
//! as `jniLibs/<abi>/lib*.so` so they end up in `nativeLibraryDir`,
//! which has the `apk_data_file` SELinux type — the only on-disk place
//! `untrusted_app` is allowed to exec from on Android 10+.
//! `CompositorService.ensureXwaylandExtracted` (Kotlin) lays down
//! symlinks at `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` pointing at
//! those real files, exports the dir to us via `TAWC_NATIVE_LIB_DIR`,
//! and extracts the XKB data tree (read by fopen via Xwayland's
//! baked-in `-Dxkb_dir`) into `<filesDir>/xwayland/share/`. We just
//! have to put `<install>/bin` on `PATH` for
//! `Command::new("Xwayland")` and point `LD_LIBRARY_PATH` at
//! nativeLibraryDir so the linker finds the bionic-built `.so` deps.
//!
//! The X11 socket lives at `<appData>/share/xtmp/.X11-unix/X{N}` and
//! the lockfile at `<appData>/share/xtmp/.X{N}-lock`. Living under
//! `share/` (rather than directly under `<appData>/`) means it rides
//! into each rootfs through the same `<appData>/share/ →
//! /usr/share/tawc/` bind that exposes the wayland socket — we don't
//! bind any other part of `<appData>/` into the rootfs view (see
//! notes/installation.md "/usr/share/tawc"). Smithay reads
//! `TAWC_XWL_RUNTIME_DIR` to override its `/tmp` defaults — see the
//! `tawc-patches` branch on the smithay fork. Each install method
//! still surfaces `<appData>/share/xtmp/.X11-unix` at the canonical
//! `/tmp/.X11-unix` path inside the rootfs (asymmetric bind on
//! tawcroot/proot, real bind-mount on chroot) because libxcb hardcodes
//! `/tmp/.X11-unix/X<N>` for the `:N` form of `$DISPLAY`.

use std::process::Stdio;

use log::{error, info, warn};
use smithay::utils::{Logical, Rectangle};
use smithay::wayland::xwayland_shell::{XWaylandShellHandler, XWaylandShellState};
use smithay::xwayland::{
    xwm::{Reorder, ResizeEdge, WmWindowProperty, XwmId},
    X11Surface, X11Wm, XWayland, XWaylandEvent, XwmHandler,
};
use smithay::reexports::wayland_server::Resource;
use smithay::wayland::selection::data_device::{
    clear_data_device_selection, current_data_device_selection_userdata,
    set_data_device_selection,
};
use smithay::wayland::selection::SelectionTarget;

use crate::compositor::TawcState;
use crate::host::ActivityId;

/// Where Xwayland's listening socket / lockfile live. Patched smithay
/// reads `TAWC_XWL_RUNTIME_DIR` from env; cross-compiled libxcb /
/// xtrans have this baked in (see `scripts/build-xwayland.sh`).
pub const XWL_RUNTIME_DIR: &str = "/data/data/me.phie.tawc/share/xtmp";

/// Where the in-app extractor stages the Xwayland binary + libs.
pub const XWL_INSTALL_DIR: &str = "/data/data/me.phie.tawc/files/xwayland";

/// Spawn Xwayland and insert it as a calloop event source. On the
/// `Ready` event the X11 window manager is constructed and stashed on
/// the state.
pub fn start_xwayland(
    handle: &smithay::reexports::calloop::LoopHandle<'static, TawcState>,
    state: &TawcState,
) {
    let handle_for_wm = handle.clone();
    // Make sure <appData>/share/xtmp/.X11-unix/ exists. Patched
    // smithay drops the listening socket here; bionic-built libxcb /
    // xtrans look up :0 here too (from the compositor side — rootfs
    // side gets it at /tmp/.X11-unix via each method's asymmetric bind,
    // see notes/installation.md "/usr/share/tawc").
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
    // PATH is consumed by `Command::new("Xwayland")` lookup.
    let path_with_xwl = match std::env::var("PATH") {
        Ok(p) => format!("{}/bin:{}", XWL_INSTALL_DIR, p),
        Err(_) => format!("{}/bin", XWL_INSTALL_DIR),
    };
    std::env::set_var("PATH", &path_with_xwl);

    let dh = state.display_handle.clone();
    // Xwayland is a pure bionic process. libhybris/lib is intentionally
    // NOT on its LD_LIBRARY_PATH: libhybris ships glibc-built stub .sos
    // (libui.so, libsync.so, libgralloc.so, libhardware.so) for
    // chroot-side use, and putting them ahead of /system/lib64 makes the
    // bionic linker pick them up when something pulls in libui — most
    // notably `dlopen("libnativewindow.so")` from xwayland-tawc, since
    // /system/lib64/libnativewindow.so DT_NEEDS libui.so. The libhybris
    // stub then fails to resolve glibc's libc.so.6 and the dlopen
    // collapses. Server-side AHB allocation needs only the bionic
    // libnativewindow.so already resident in /system/lib64.
    //
    // The DT_NEEDED .sos (libX11, libxcb, libpixman-1, …) live next to
    // the binaries in nativeLibraryDir, exported by Kotlin as
    // `TAWC_NATIVE_LIB_DIR`. If unset we'll fall back to the legacy
    // filesDir layout — Xwayland just won't find its libs and exec
    // will fail; logged here so the cause is obvious.
    let lib_dir = std::env::var("TAWC_NATIVE_LIB_DIR").unwrap_or_else(|_| {
        warn!("xwayland: TAWC_NATIVE_LIB_DIR not set; Xwayland will likely fail to load its .so deps");
        format!("{xwl}/lib", xwl = XWL_INSTALL_DIR)
    });
    let envs: Vec<(String, String)> = vec![("LD_LIBRARY_PATH".to_string(), lib_dir)];

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
    let dh_for_wm = dh.clone();
    let result = handle.insert_source(xwayland, move |event, _, data: &mut TawcState| match event {
        XWaylandEvent::Ready { x11_socket, display_number } => {
            info!("xwayland: Ready, display={}", display_number);
            match X11Wm::start_wm(
                handle_for_wm.clone(),
                &dh_for_wm,
                x11_socket,
                xwayland_client_for_handler.clone(),
            ) {
                Ok(wm) => {
                    data.xwm = Some(wm);
                    data.xdisplay = Some(display_number);
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
        let (host, spawn_activity) = pick_host_for_x11(self, &window);
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
        window.user_data().insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(host.clone()))));
        self.update_host_window_metadata(&host, window.title(), window.class());
        self.toplevels_changed = true;
        if spawn_activity {
            crate::spawn_activity_from_native(&host);
        }
    }

    fn mapped_override_redirect_window(&mut self, _xwm: XwmId, window: X11Surface) {
        info!("xwayland: mapped_OR {:?}", window);
        let (host, spawn_activity) = pick_host_for_x11(self, &window);
        self.x11_surfaces.push(window.clone());
        window.user_data().insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(host.clone()))));
        self.update_host_window_metadata(&host, window.title(), window.class());
        self.toplevels_changed = true;
        if spawn_activity {
            crate::spawn_activity_from_native(&host);
        }
    }

    fn unmapped_window(&mut self, _xwm: XwmId, window: X11Surface) {
        info!("xwayland: unmapped {:?}", window);
        let host = self.x11_surface_host(&window);
        if let Some(wl) = window.wl_surface() {
            self.x11_to_host.remove(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        if !window.is_override_redirect() {
            let _ = window.set_mapped(false);
        }
        self.toplevels_changed = true;
        if let Some(host) = host {
            self.finish_host_if_unused(&host);
        }
    }

    fn destroyed_window(&mut self, _xwm: XwmId, window: X11Surface) {
        let host = self.x11_surface_host(&window);
        if let Some(wl) = window.wl_surface() {
            self.x11_to_host.remove(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        self.toplevels_changed = true;
        if let Some(host) = host {
            self.finish_host_if_unused(&host);
        }
    }

    fn property_notify(&mut self, _xwm: XwmId, window: X11Surface, property: WmWindowProperty) {
        if !matches!(property, WmWindowProperty::Title | WmWindowProperty::Class) {
            return;
        }
        let Some(host) = x11_host(self, &window) else {
            return;
        };
        self.update_host_window_metadata(&host, window.title(), window.class());
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

    fn allow_selection_access(&mut self, xwm: XwmId, _selection: SelectionTarget) -> bool {
        let Some(keyboard) = self.seat.get_keyboard() else {
            return false;
        };
        let Some(focus) = keyboard.current_focus() else {
            return false;
        };
        self.x11_surfaces.iter().any(|surface| {
            surface.xwm_id() == Some(xwm)
                && surface
                    .wl_surface()
                    .as_ref()
                    .is_some_and(|wl| wl.id().same_client_as(&focus.id()))
        })
    }
    fn send_selection(
        &mut self,
        _xwm: XwmId,
        selection: SelectionTarget,
        mime_type: String,
        fd: std::os::unix::io::OwnedFd,
    ) {
        match selection {
            SelectionTarget::Clipboard => {
                let user_data = current_data_device_selection_userdata(&self.seat)
                    .as_deref()
                    .cloned();
                if let Some(crate::clipboard::SelectionUserData::AndroidText(text)) = user_data {
                    if crate::clipboard::is_supported_text_mime(&mime_type) {
                        crate::clipboard::write_text_to_fd(fd, text);
                    } else {
                        warn!("xwayland: refusing unsupported Android clipboard MIME {}", mime_type);
                    }
                    return;
                }
                if let Err(e) = smithay::wayland::selection::data_device::request_data_device_client_selection(
                    &self.seat,
                    mime_type,
                    fd,
                ) {
                    warn!("xwayland: failed to request Wayland clipboard for X11: {:?}", e);
                }
            }
            SelectionTarget::Primary => {}
        }
    }
    fn new_selection(
        &mut self,
        _xwm: XwmId,
        selection: SelectionTarget,
        mime_types: Vec<String>,
    ) {
        match selection {
            SelectionTarget::Clipboard => {
                let preferred_mime = crate::clipboard::preferred_text_mime(&mime_types);
                set_data_device_selection(
                    &self.display_handle,
                    &self.seat,
                    mime_types,
                    crate::clipboard::SelectionUserData::X11(selection),
                );
                if let Some(mime_type) = preferred_mime {
                    if let Some(xwm) = self.xwm.as_mut() {
                        match crate::clipboard::pipe() {
                            Ok((read_fd, write_fd)) => {
                                if let Err(e) = xwm.send_selection(selection, mime_type, write_fd) {
                                    warn!("xwayland: failed to request X11 clipboard for Android: {:?}", e);
                                } else {
                                    crate::clipboard::read_fd_for_android(read_fd, "x11");
                                }
                            }
                            Err(e) => warn!("xwayland: clipboard pipe failed: {}", e),
                        }
                    }
                }
            }
            SelectionTarget::Primary => {}
        }
    }
    fn cleared_selection(&mut self, _xwm: XwmId, selection: SelectionTarget) {
        match selection {
            SelectionTarget::Clipboard => {
                let clear = current_data_device_selection_userdata(&self.seat)
                    .as_deref()
                    .is_some_and(|user_data| {
                        matches!(
                            user_data,
                            crate::clipboard::SelectionUserData::X11(SelectionTarget::Clipboard)
                        )
                    });
                if clear {
                    clear_data_device_selection(&self.display_handle, &self.seat);
                }
            }
            SelectionTarget::Primary => {}
        }
    }
}

/// `PendingHost` is the host an X11Surface was assigned to at
/// map_window_request time. The wl_surface backing arrives later (via
/// xwayland_shell_v1.set_serial), at which point the compositor's
/// commit hook moves the host id from this user_data slot into
/// `state.x11_to_host`.
pub struct PendingHost(pub std::cell::RefCell<Option<ActivityId>>);

fn x11_host(state: &TawcState, surface: &X11Surface) -> Option<ActivityId> {
    state.x11_surface_host(surface)
}

/// Look up the host an existing X11Surface is on. Tries the live
/// wl_surface→host map first, falls back to the `PendingHost` user_data
/// stamped at `map_window_request` time (for surfaces that have a host
/// reserved but haven't yet had their wl_surface bound).
fn parent_host(state: &TawcState, parent: &X11Surface) -> Option<ActivityId> {
    x11_host(state, parent)
}

/// Pick a host for a freshly-mapped X11 surface, mirroring the
/// `assign_toplevel_to_host` policy that xdg toplevels use.
///
/// - **Override-redirect** popups (menus, tooltips, dropdowns) and
///   **transient_for** dialogs ride on the parent toplevel's host.
///   This keeps a Wine right-click menu or an xterm tooltip from
///   spawning its own recents card.
/// - **single_activity_mode**: collapse onto the first existing host;
///   if none exists, mint+spawn one.
/// - **Default (multi-window)**: every X11 toplevel gets its own
///   Activity, same as Wayland.
///
/// Returns `(host_id, should_spawn_activity)`. The caller fires
/// `spawn_activity_from_native(&host)` when the second component is
/// true. The reverse-JNI call doesn't borrow `state`, so it's fine to
/// invoke while still holding `&mut TawcState`.
fn pick_host_for_x11(state: &mut TawcState, surface: &X11Surface) -> (ActivityId, bool) {
    // Children/transients ride on the parent's host. Skip if we can't
    // find the parent (e.g. it was destroyed before the child mapped)
    // or if it has no host yet — fall through to the regular policy.
    if surface.is_override_redirect() || surface.is_transient_for().is_some() {
        let parent_id = surface.is_transient_for();
        if let Some(parent) = state
            .x11_surfaces
            .iter()
            .find(|s| Some(s.window_id()) == parent_id)
            .cloned()
        {
            if let Some(h) = parent_host(state, &parent) {
                return (h, false);
            }
        }
    }

    if state.single_activity_mode {
        let (id, spawn) = match state.hosts.keys().next().cloned() {
            Some(id) => (id, false),
            None => (crate::host::new_activity_id(), true),
        };
        return (id, spawn);
    }

    // Multi-window: every X11 toplevel gets its own Activity, exactly
    // as a Wayland toplevel would.
    (crate::host::new_activity_id(), true)
}

/// Called from `CompositorHandler::commit` for every wl_surface commit
/// (and from the render path before SHM imports). Scans
/// `state.x11_surfaces` and promotes any X11Surface whose wl_surface
/// is set but missing from `state.x11_to_host` into the host map
/// (using its `PendingHost` user_data, or `pick_host_for_x11` as
/// fallback).
///
/// We scan all x11_surfaces (rather than matching on the committing
/// wl_surface specifically) because there's a real race between
/// xclock's first commit and smithay's `WL_SURFACE_SERIAL` handler
/// setting `X11Surface.wl_surface`. If the commit lands first, no
/// X11Surface yet has the wl_surface bound; smithay sets it
/// asynchronously and the next commit might not fire before the
/// renderer needs the host mapping. Calling this from the render path
/// too closes the gap.
pub fn associate_pending_x11_surfaces(state: &mut TawcState) {
    if state.x11_surfaces.is_empty() {
        return;
    }
    // Two passes: collect unassociated surfaces, then process. Avoids
    // mutating state.x11_to_host while iterating state.x11_surfaces.
    let pending: Vec<X11Surface> = state
        .x11_surfaces
        .iter()
        .filter(|s| match s.wl_surface() {
            Some(wl) => !state.x11_to_host.contains_key(&wl),
            None => false,
        })
        .cloned()
        .collect();
    for surface in pending {
        let wl = match surface.wl_surface() {
            Some(s) => s,
            None => continue,
        };
        let (host, spawn_activity) = match surface
            .user_data()
            .get::<PendingHost>()
            .and_then(|p| p.0.borrow_mut().take())
        {
            Some(h) => (h, false),
            None => pick_host_for_x11(state, &surface),
        };
        info!(
            "xwayland: associated X11 surface {} to host {}",
            surface.window_id(),
            host,
        );
        state.x11_to_host.insert(wl, host.clone());
        state.toplevels_changed = true;
        if spawn_activity {
            crate::spawn_activity_from_native(&host);
        }
    }
}
