//! XWayland integration: spawning the bionic-built `Xwayland` binary,
//! routing X11 surfaces through tawc's host/toplevel model, and feeding
//! the X11 window manager loop.
//!
//! The Xwayland binary, xkbcomp, and their DT_NEEDED libs are shipped
//! as `jniLibs/<abi>/lib*.so` so they end up in `nativeLibraryDir`,
//! which has the `apk_data_file` SELinux type — the only on-disk place
//! `untrusted_app` is allowed to exec from on Android 10+.
//! `CompositorService.ensureXwaylandExtracted` (Kotlin) lays down symlinks at
//! `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` pointing at those real files,
//! exports the dir to us via `TAWC_NATIVE_LIB_DIR`, and extracts the XKB data
//! tree into `<filesDir>/xwayland/share/`. We set Xwayland's cwd to
//! `<appData>` so the relative XKB paths baked by `scripts/build-xwayland.sh`
//! resolve correctly, and put `<install>/bin` on `PATH` for
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
use std::time::{Duration, Instant};
use std::os::fd::AsRawFd;

use log::{error, info, warn};
use smithay::reexports::calloop::generic::{FdWrapper, Generic};
use smithay::reexports::calloop::{Interest, LoopHandle, Mode, PostAction, RegistrationToken};
use smithay::utils::{Logical, Rectangle};
use smithay::wayland::xwayland_shell::{XWaylandShellHandler, XWaylandShellState};
use smithay::xwayland::{
    xwm::{Reorder, ResizeEdge, WmWindowProperty, XwmId},
    X11Surface, X11Wm, XWayland, XWaylandEvent, XwmHandler,
};
use smithay::reexports::wayland_server::{
    protocol::wl_surface::WlSurface,
    Resource,
};
use smithay::wayland::selection::data_device::{
    clear_data_device_selection, current_data_device_selection_userdata,
    set_data_device_selection,
};
use smithay::wayland::selection::SelectionTarget;

use crate::compositor::TawcState;
use crate::host::ActivityId;

const RESTART_GRACE: Duration = Duration::from_millis(500);
const START_RETRY_DELAY: Duration = Duration::from_millis(500);

enum StartResult {
    Started(RegistrationToken),
    RetryLater,
    Unavailable,
}

fn configure_x11_toplevel_for_host(
    state: &TawcState,
    surface: &X11Surface,
    host_id: &ActivityId,
) -> Option<(i32, i32)> {
    if surface.is_override_redirect() {
        return None;
    }

    let (w, h) = state.host_logical_size(host_id)?;
    let mut geo = surface.geometry();
    geo.loc = (0, 0).into();
    geo.size = (w, h).into();
    if let Err(e) = surface.configure(geo) {
        warn!("xwayland: configure failed: {}", e);
        return None;
    }
    Some((w, h))
}

pub fn configure_x11_toplevels_for_hosts(state: &TawcState) -> bool {
    let mut configured = false;
    for surface in &state.x11_surfaces {
        let Some(host_id) = state.x11_surface_host(surface) else {
            continue;
        };
        configured |= configure_x11_toplevel_for_host(state, surface, &host_id).is_some();
    }
    configured
}

pub fn set_enabled(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
    enabled: bool,
) {
    state.xwayland_enabled = enabled;
    if enabled {
        service_pending(handle, state);
    } else {
        state.xwayland_start_pending = false;
        state.xwayland_start_after = None;
        stop_activation_socket(handle, state);
        stop_xwayland(handle, state);
    }
}

pub fn service_pending(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
) {
    let now = Instant::now();
    if state.xwayland_source_dead {
        if let Some(token) = state.xwayland_source.take() {
            handle.remove(token);
        }
        state.xwayland_source_dead = false;
        cleanup_xwayland_runtime_state(state);
        if state.xwayland_enabled {
            state.xwayland_start_after = Some(now + START_RETRY_DELAY);
        }
    }

    if !state.xwayland_enabled {
        return;
    }
    if state.xwayland_source.is_some() {
        state.xwayland_start_pending = false;
        state.xwayland_start_after = None;
        return;
    }
    if state.xwm.is_some() {
        state.xwayland_start_after = Some(now + START_RETRY_DELAY);
        return;
    }

    if !state.xwayland_start_pending {
        if state.xwayland_activation.is_none()
            && !state.xwayland_start_after.is_some_and(|when| now < when)
        {
            start_activation_socket(handle, state);
        }
        return;
    }
    if state.xwayland_start_after.is_some_and(|when| now < when) {
        return;
    }

    match start_xwayland(handle, state) {
        StartResult::Started(token) => {
            state.xwayland_source = Some(token);
            state.xwayland_start_pending = false;
            state.xwayland_start_after = None;
        }
        StartResult::RetryLater => {
            state.xwayland_start_pending = false;
            state.xwayland_start_after = Some(now + START_RETRY_DELAY);
        }
        StartResult::Unavailable => {
            state.xwayland_start_pending = false;
            state.xwayland_start_after = None;
        }
    }
}

fn prepare_xwayland_environment() -> Option<(String, String)> {
    if matches!(
        std::env::var("TAWC_XWAYLAND_ENABLED").as_deref(),
        Ok("0") | Ok("false") | Ok("FALSE")
    ) {
        info!("xwayland: disabled or unavailable");
        return None;
    }

    let paths = crate::app_paths::get();
    let xwl_runtime_dir = paths.xwayland_runtime_dir.clone();
    let xwl_install_dir = paths.xwayland_dir.clone();
    if let Err(e) = std::fs::create_dir_all(format!("{}/.X11-unix", xwl_runtime_dir)) {
        warn!("xwayland: mkdir {}: {}", xwl_runtime_dir, e);
    }
    std::env::set_var("TAWC_XWL_RUNTIME_DIR", &xwl_runtime_dir);

    let xwl_bin = format!("{}/bin", xwl_install_dir);
    let path_with_xwl = match std::env::var("PATH") {
        Ok(p) if p.split(':').any(|entry| entry == xwl_bin) => p,
        Ok(p) => format!("{}:{}", xwl_bin, p),
        Err(_) => xwl_bin,
    };
    std::env::set_var("PATH", &path_with_xwl);

    Some((xwl_runtime_dir, xwl_install_dir))
}

fn start_activation_socket(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
) {
    if state.xwayland_activation.is_some() || state.xwayland_activation_source.is_some() {
        return;
    }
    if prepare_xwayland_environment().is_none() {
        state.xwayland_start_pending = false;
        state.xwayland_start_after = None;
        return;
    }

    let activation = match XWayland::prepare_lazy(Some(0), false) {
        Ok(activation) => activation,
        Err(e) => {
            warn!("xwayland: failed to prepare activation socket: {}", e);
            state.xwayland_start_after = Some(Instant::now() + START_RETRY_DELAY);
            return;
        }
    };
    let display_number = activation.display_number();
    let fd = activation.poll_fd().as_raw_fd();
    state.xwayland_activation = Some(activation);

    let source = Generic::new(
        unsafe { FdWrapper::new(fd) },
        Interest::READ,
        Mode::Level,
    );
    match handle.insert_source(source, move |_, _, data: &mut TawcState| {
        info!("xwayland: X11 client connected; starting Xwayland");
        data.xwayland_activation_source = None;
        data.xwayland_start_pending = true;
        data.xwayland_start_after = None;
        Ok(PostAction::Remove)
    }) {
        Ok(token) => {
            state.xwayland_activation_source = Some(token);
            info!("xwayland: activation socket ready, DISPLAY=:{}", display_number);
        }
        Err(e) => {
            error!("xwayland: failed to insert activation socket source: {}", e);
            state.xwayland_activation = None;
            state.xwayland_start_after = Some(Instant::now() + START_RETRY_DELAY);
        }
    }
}

fn stop_activation_socket(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
) {
    if let Some(token) = state.xwayland_activation_source.take() {
        handle.remove(token);
    }
    state.xwayland_activation = None;
}

/// Spawn Xwayland and insert it as a calloop event source. On the
/// `Ready` event the X11 window manager is constructed and stashed on
/// the state.
fn start_xwayland(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
) -> StartResult {
    let paths = crate::app_paths::get();
    let Some((xwl_runtime_dir, xwl_install_dir)) = prepare_xwayland_environment() else {
        return StartResult::Unavailable;
    };
    let Some(activation) = state.xwayland_activation.take() else {
        warn!("xwayland: start requested without an activation socket");
        return StartResult::RetryLater;
    };
    if let Some(token) = state.xwayland_activation_source.take() {
        handle.remove(token);
    }

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
        format!("{xwl}/lib", xwl = xwl_install_dir)
    });
    let envs: Vec<(String, String)> = vec![
        ("LD_LIBRARY_PATH".to_string(), lib_dir),
        ("XDG_RUNTIME_DIR".to_string(), xwl_runtime_dir.clone()),
    ];

    // Pipe Xwayland's stderr/stdout to a log file under our xtmp dir so
    // we can post-mortem startup failures (Stdio::null() drops them on
    // the floor and android_logger is JVM-only). This is a debug
    // convenience; can be removed once Xwayland is stable.
    let log_path = format!("{}/xwayland.log", xwl_runtime_dir);
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

    let old_cwd = std::env::current_dir().ok();
    if let Err(e) = std::env::set_current_dir(&paths.data_dir) {
        error!("xwayland: chdir {} failed: {}", paths.data_dir, e);
        return StartResult::RetryLater;
    }
    let spawn_result = XWayland::spawn_with_activation(
        &dh,
        activation,
        envs,
        stdout,
        stderr,
        |_| (),
    );
    if let Some(old_cwd) = old_cwd {
        let _ = std::env::set_current_dir(old_cwd);
    }
    let (xwayland, client) = match spawn_result {
        Ok(pair) => pair,
        Err(e) => {
            error!("xwayland: failed to spawn Xwayland: {}", e);
            return StartResult::RetryLater;
        }
    };

    info!("xwayland: spawned Xwayland; waiting for Ready");

    let xwayland_client_for_handler = client.clone();
    let dh_for_wm = dh.clone();
    let result = handle.insert_source(xwayland, move |event, _, data: &mut TawcState| match event {
        XWaylandEvent::Ready { x11_socket, display_number } => {
            info!("xwayland: Ready, display={}", display_number);
            match X11Wm::start_wm(
                data.loop_handle(),
                &dh_for_wm,
                x11_socket,
                xwayland_client_for_handler.clone(),
            ) {
                Ok(mut wm) => {
                    let android_clipboard_active = current_data_device_selection_userdata(&data.seat)
                        .as_deref()
                        .is_some_and(|user_data| {
                            matches!(user_data, crate::clipboard::SelectionUserData::Android)
                        });
                    if android_clipboard_active {
                        if let Err(e) = wm.new_selection(
                            SelectionTarget::Clipboard,
                            Some(crate::clipboard::text_mime_types()),
                        ) {
                            warn!(
                                "clipboard: failed to notify XWayland of existing Android selection: {:?}",
                                e
                            );
                        }
                    }
                    data.xwm = Some(wm);
                    data.xdisplay = Some(display_number);
                    info!("xwayland: X11Wm attached, DISPLAY=:{}", display_number);
                }
                Err(e) => {
                    error!("xwayland: X11Wm::start_wm failed: {}", e);
                    data.xwayland_source_dead = true;
                }
            }
        }
        XWaylandEvent::Error => {
            error!("xwayland: Xwayland crashed during startup");
            data.xwayland_source_dead = true;
        }
    });
    match result {
        Ok(token) => StartResult::Started(token),
        Err(e) => {
            error!("xwayland: insert_source failed: {}", e);
            StartResult::RetryLater
        }
    }
}

fn stop_xwayland(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
) {
    let Some(token) = state.xwayland_source.take() else {
        state.xwm = None;
        state.xdisplay = None;
        return;
    };

    info!("xwayland: stopping Xwayland");
    handle.remove(token);
    kill_xwayland_processes();
    state.xwayland_start_after = Some(Instant::now() + RESTART_GRACE);
    cleanup_xwayland_runtime_state(state);
}

fn cleanup_xwayland_runtime_state(state: &mut TawcState) {
    state.xdisplay = None;

    let clear_x11_clipboard = current_data_device_selection_userdata(&state.seat)
        .as_deref()
        .is_some_and(|user_data| {
            matches!(
                user_data,
                crate::clipboard::SelectionUserData::X11(SelectionTarget::Clipboard)
            )
        });
    if clear_x11_clipboard {
        clear_data_device_selection(&state.display_handle, &state.seat);
    }

    let hosts: Vec<ActivityId> = state
        .x11_surfaces
        .iter()
        .filter_map(|surface| state.x11_surface_host(surface))
        .collect();
    let surfaces = std::mem::take(&mut state.x11_surfaces);
    for surface in surfaces {
        if let Some(wl) = surface.wl_surface() {
            state.desktop.remove_surface(&wl);
        }
    }
    state.toplevels_changed = true;
    state.sync_desktop_hosts();
    for host in hosts {
        state.finish_host_if_unused(&host);
    }
}

/// Enumerate live Xwayland processes owned by our uid (same /proc walk
/// the kill sweep uses). Also surfaced through the `query-state` debug
/// payload so host tests can watch launch/idle-stop/restart without
/// `adb shell pidof`.
pub fn xwayland_pids() -> Vec<libc::pid_t> {
    let uid = unsafe { libc::getuid() };
    let mut pids = Vec::new();
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return pids;
    };
    for entry in entries.flatten() {
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let Ok(pid) = name.parse::<libc::pid_t>() else {
            continue;
        };
        let comm_path = format!("/proc/{pid}/comm");
        let Ok(comm) = std::fs::read_to_string(comm_path) else {
            continue;
        };
        if comm.trim() != "Xwayland" {
            continue;
        }
        if proc_uid(pid) != Some(uid) {
            continue;
        }
        // Skip zombies (empty cmdline): a SIGKILLed Xwayland lingers in
        // /proc until reaped, but it's gone for every purpose callers
        // care about. Matches `pidof` semantics, which tests relied on
        // before this enumeration was exposed via query-state.
        match std::fs::read(format!("/proc/{pid}/cmdline")) {
            Ok(cmdline) if !cmdline.is_empty() => {}
            _ => continue,
        }
        pids.push(pid);
    }
    pids
}

fn kill_xwayland_processes() {
    for pid in xwayland_pids() {
        let rc = unsafe { libc::kill(pid, libc::SIGKILL) };
        if rc == 0 {
            info!("xwayland: killed Xwayland pid {}", pid);
        } else {
            warn!(
                "xwayland: failed to kill Xwayland pid {}: {}",
                pid,
                std::io::Error::last_os_error(),
            );
        }
    }
}

fn proc_uid(pid: libc::pid_t) -> Option<libc::uid_t> {
    let status = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
    let uid_line = status.lines().find(|line| line.starts_with("Uid:"))?;
    uid_line
        .split_whitespace()
        .nth(1)?
        .parse::<libc::uid_t>()
        .ok()
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

    fn disconnected(&mut self, xwm: XwmId) {
        if self.xwm.as_ref().is_some_and(|active| active.id() == xwm) {
            info!("xwayland: X11Wm disconnected");
            self.xwm = None;
            if self.xwayland_source.is_some() {
                self.xwayland_source_dead = true;
            }
            if self.xwayland_enabled {
                self.xwayland_start_pending = false;
                self.xwayland_start_after = Some(Instant::now() + START_RETRY_DELAY);
            }
            cleanup_xwayland_runtime_state(self);
        }
    }

    fn new_override_redirect_window(&mut self, _xwm: XwmId, _window: X11Surface) {}

    fn map_window_request(&mut self, _xwm: XwmId, window: X11Surface) {
        // Translate X11 parent/transient state into the shared desktop
        // host-placement policy.
        let assignment = assign_host_for_x11(self, &window);
        if let Err(e) = window.set_mapped(true) {
            warn!("xwayland: set_mapped(true) failed: {}", e);
            return;
        }
        if configure_x11_toplevel_for_host(self, &window, &assignment.host).is_none() {
            let mut geo = window.geometry();
            geo.loc = (0, 0).into();
            if geo.size.w <= 0 || geo.size.h <= 0 {
                geo.size = (150, 100).into();
            }
            if let Err(e) = window.configure(geo) {
                warn!("xwayland: configure failed: {}", e);
            }
        }
        self.x11_surfaces.push(window.clone());
        // Defer host assignment until the X11Surface gets a backing
        // wl_surface (commit hook captures it). For now record the
        // pending host on the X11Surface's user_data so we don't lose
        // it.
        window
            .user_data()
            .insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(assignment.host.clone()))));
        self.update_host_window_metadata(&assignment.host, window.title(), window.class());
        self.toplevels_changed = true;
        if assignment.spawn_activity {
            crate::spawn_activity_from_native(&assignment.host);
        }
    }

    fn mapped_override_redirect_window(&mut self, _xwm: XwmId, window: X11Surface) {
        let assignment = assign_host_for_x11(self, &window);
        self.x11_surfaces.push(window.clone());
        window
            .user_data()
            .insert_if_missing(|| PendingHost(std::cell::RefCell::new(Some(assignment.host.clone()))));
        self.update_host_window_metadata(&assignment.host, window.title(), window.class());
        self.toplevels_changed = true;
        if assignment.spawn_activity {
            crate::spawn_activity_from_native(&assignment.host);
        }
    }

    fn unmapped_window(&mut self, _xwm: XwmId, window: X11Surface) {
        let host = self.x11_surface_host(&window);
        if let Some(wl) = window.wl_surface() {
            self.desktop.remove_surface(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        if !window.is_override_redirect() {
            let _ = window.set_mapped(false);
        }
        self.toplevels_changed = true;
        self.sync_desktop_hosts();
        if let Some(host) = host {
            self.finish_host_if_unused(&host);
        }
    }

    fn destroyed_window(&mut self, _xwm: XwmId, window: X11Surface) {
        let host = self.x11_surface_host(&window);
        if let Some(wl) = window.wl_surface() {
            self.desktop.remove_surface(&wl);
        }
        self.x11_surfaces.retain(|w| w != &window);
        self.toplevels_changed = true;
        self.sync_desktop_hosts();
        if let Some(host) = host {
            self.finish_host_if_unused(&host);
        }
    }

    fn property_notify(&mut self, _xwm: XwmId, window: X11Surface, property: WmWindowProperty) {
        if !matches!(property, WmWindowProperty::Title | WmWindowProperty::Class) {
            return;
        }
        let Some(host) = self.x11_surface_host(&window) else {
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
        if let Some(host) = self.x11_surface_host(&window) {
            if configure_x11_toplevel_for_host(self, &window, &host).is_some() {
                return;
            }
        }

        let mut geo = window.geometry();
        geo.loc = (0, 0).into();
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
                let android_owned = current_data_device_selection_userdata(&self.seat)
                    .as_deref()
                    .is_some_and(|user_data| {
                        matches!(user_data, crate::clipboard::SelectionUserData::Android)
                    });
                if android_owned {
                    if crate::clipboard::is_supported_text_mime(&mime_type) {
                        crate::clipboard::write_android_clipboard_to_fd(fd);
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
                crate::clipboard::queue_selection_pull(
                    crate::clipboard::PullSource::X11,
                    &mime_types,
                );
                set_data_device_selection(
                    &self.display_handle,
                    &self.seat,
                    mime_types,
                    crate::clipboard::SelectionUserData::X11(selection),
                );
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
/// `state.desktop`.
pub struct PendingHost(pub std::cell::RefCell<Option<ActivityId>>);

/// Pick a host for a freshly-mapped X11 surface using the same
/// parent/single-activity/new-activity policy as xdg toplevels.
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
fn assign_host_for_x11(
    state: &TawcState,
    surface: &X11Surface,
) -> crate::desktop::HostAssignment {
    // Children/transients ride on the parent's host. Skip if we can't
    // find the parent (e.g. it was destroyed before the child mapped)
    // or if it has no host yet — fall through to the regular policy.
    let parent_host = if surface.is_override_redirect() || surface.is_transient_for().is_some() {
        let parent_id = surface.is_transient_for();
        state
            .x11_surfaces
            .iter()
            .find(|s| Some(s.window_id()) == parent_id)
            .and_then(|parent| state.x11_surface_host(parent))
    } else {
        None
    };

    crate::desktop::DesktopRegistry::choose_host(
        parent_host,
        state.single_activity_mode,
        state.hosts.keys().next().cloned(),
    )
}

/// Called from `CompositorHandler::commit` for every wl_surface commit.
/// Promotes only the X11Surface backed by the committing wl_surface so commits
/// do not mutate unrelated windows.
pub fn associate_committed_x11_surface(state: &mut TawcState, committed: &WlSurface) -> bool {
    let Some(surface) = state
        .x11_surfaces
        .iter()
        .find(|surface| surface.wl_surface().as_ref() == Some(committed))
        .cloned()
    else {
        return false;
    };
    associate_x11_surface(state, surface)
}

/// Called from the frame timer. Scans `state.x11_surfaces` and promotes any
/// X11Surface whose wl_surface is set but missing from the desktop registry
/// into the host map (using its `PendingHost` user_data, or
/// `assign_host_for_x11` as fallback).
///
/// We scan all x11_surfaces here because there's a real race between xclock's
/// first commit and smithay's `WL_SURFACE_SERIAL` handler setting
/// `X11Surface.wl_surface`. If the commit lands first, no X11Surface yet has
/// the wl_surface bound; smithay sets it asynchronously and the next commit
/// might not fire before the renderer needs the host mapping. The frame timer
/// closes that gap without letting one commit mutate a different window.
pub fn associate_pending_x11_surfaces(state: &mut TawcState) -> bool {
    if state.x11_surfaces.is_empty() {
        return false;
    }
    // Two passes: collect unassociated surfaces, then process. Avoids
    // mutating the registry while iterating state.x11_surfaces.
    let pending: Vec<X11Surface> = state
        .x11_surfaces
        .iter()
        .filter(|s| match s.wl_surface() {
            Some(wl) => !state.desktop.has_host_assignment(&wl),
            None => false,
        })
        .cloned()
        .collect();
    let mut associated_any = false;
    for surface in pending {
        associated_any |= associate_x11_surface(state, surface);
    }
    associated_any
}

fn associate_x11_surface(state: &mut TawcState, surface: X11Surface) -> bool {
    let Some(wl) = surface.wl_surface() else {
        return false;
    };
    if state.desktop.has_host_assignment(&wl) {
        return false;
    }

    let (host, spawn_activity) = match surface
        .user_data()
        .get::<PendingHost>()
        .and_then(|p| p.0.borrow_mut().take())
    {
        Some(h) => (h, false),
        None => {
            let assignment = assign_host_for_x11(state, &surface);
            (assignment.host, assignment.spawn_activity)
        }
    };
    state.desktop.assign_surface_to_host(wl.clone(), host.clone());
    state.desktop.ensure_x11_window(wl, surface.clone());
    state.toplevels_changed = true;
    state.sync_desktop_hosts();
    if spawn_activity {
        crate::spawn_activity_from_native(&host);
    }
    true
}
