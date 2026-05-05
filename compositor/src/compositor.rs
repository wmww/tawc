//! Wayland protocol state and handler implementations.
//!
//! This module owns TawcState (the Wayland protocol side of the compositor)
//! and all the smithay handler trait impls. It does NOT own the renderer,
//! EGL context, or GPU textures — those live in render::RenderState.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use log::{error, info};

use smithay::backend::renderer::gles::GlesTexture;
use smithay::delegate_compositor;
use smithay::delegate_data_device;
use smithay::delegate_output;
use smithay::delegate_seat;
use smithay::delegate_shm;
use smithay::delegate_xdg_decoration;
use smithay::delegate_xdg_shell;
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::input::keyboard::XkbConfig;
use smithay::reexports::wayland_server::protocol::wl_seat;
use smithay::reexports::wayland_server::protocol::wl_buffer;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{
    Client, DisplayHandle, Resource,
};
use smithay::reexports::wayland_server::backend::{ClientData, ClientId, DisconnectReason};
use smithay::reexports::wayland_server::Display;
use smithay::utils::Serial;
use smithay::wayland::buffer::BufferHandler;
use smithay::wayland::compositor::{
    self, CompositorClientState, CompositorHandler, CompositorState,
};
use smithay::wayland::selection::data_device::{
    ClientDndGrabHandler, DataDeviceHandler, DataDeviceState, ServerDndGrabHandler,
};
use smithay::wayland::selection::SelectionHandler;
use smithay::desktop::PopupManager;
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
};
use smithay::wayland::shell::xdg::decoration::{XdgDecorationHandler, XdgDecorationState};
use smithay::wayland::output::OutputHandler;
use smithay::wayland::shm::{ShmHandler, ShmState};
use smithay::wayland::viewporter::ViewporterState;
use smithay::wayland::xwayland_shell::XWaylandShellState;
use smithay::xwayland::{X11Surface, X11Wm, XWaylandClientData};
use smithay::delegate_viewporter;
use smithay::delegate_xwayland_shell;

use crate::host::{ActivityId, OutputHost};
use crate::protocol::android_wlegl::server::android_wlegl::AndroidWlegl;
use crate::text_input::TextInputState;
use crate::wlegl;
use wayland_protocols::wp::text_input::zv3::server::zwp_text_input_manager_v3::ZwpTextInputManagerV3;

// ---------------------------------------------------------------------------
// Per-surface state
// ---------------------------------------------------------------------------

/// Per-surface state for an attached android_wlegl (AHB-backed) buffer.
///
/// Written by the CompositorHandler::commit callback when a wlegl wl_buffer
/// is attached. Texture import happens lazily in render.rs, cached on the
/// buffer's WleglBufferData user-data so reattaches reuse it.
///
/// `current_buffer` stays set until the client attaches a different buffer,
/// at which point Smithay's `SurfaceAttributes::merge_into` sends
/// `wl_buffer.release` for the old buffer as part of commit processing. We
/// therefore keep reading `current_buffer`'s texture every frame knowing the
/// client can't be overwriting it — it only learns the buffer is free on the
/// replacing commit, which already swapped `current_buffer` to the new one.
pub struct SurfaceWleglState {
    pub current_buffer: Option<wl_buffer::WlBuffer>,
    /// Buffer dimensions in buffer pixels (`wl_buffer.width/height`).
    pub committed_width: i32,
    pub committed_height: i32,
    /// `wl_surface.set_buffer_scale` value at last commit (default 1).
    pub buffer_scale: i32,
    /// `wp_viewport.set_destination` size in logical surface coords. When set
    /// it overrides `buffer / buffer_scale` for computing the on-screen size.
    pub viewport_dst: Option<(i32, i32)>,
}

/// Per-surface SHM buffer state.
///
/// The current_buffer field tracks which wl_buffer we last imported, so we
/// can detect new buffers and release old ones. Texture field is written by
/// render.rs during import_shm_buffers().
pub struct SurfaceShmState {
    pub texture: Option<GlesTexture>,
    pub current_buffer: Option<wl_buffer::WlBuffer>,
    pub committed_width: i32,
    pub committed_height: i32,
    /// `wl_surface.set_buffer_scale` value at last commit (default 1).
    pub buffer_scale: i32,
    /// `wp_viewport.set_destination` size in logical surface coords.
    pub viewport_dst: Option<(i32, i32)>,
}

// ---------------------------------------------------------------------------
// TawcState — Wayland protocol state
// ---------------------------------------------------------------------------

/// Wayland protocol state for the compositor.
///
/// Does NOT hold rendering state (renderer, EGL, shaders) — that's in
/// render::RenderState. This struct is what smithay handler callbacks receive.
pub struct TawcState {
    pub display_handle: DisplayHandle,
    pub compositor_state: CompositorState,
    pub shm_state: ShmState,
    pub xdg_shell_state: XdgShellState,
    // Held to keep the zxdg_decoration_manager_v1 global registered for the
    // life of the display; smithay's delegate macro reaches it via the
    // `XdgDecorationHandler` impl, never through this field.
    #[allow(dead_code)]
    pub xdg_decoration_state: XdgDecorationState,
    pub data_device_state: DataDeviceState,
    pub seat_state: SeatState<Self>,
    pub seat: Seat<Self>,

    /// Per-surface android_wlegl (AHB-backed GPU) buffer state.
    pub surface_wlegl: HashMap<WlSurface, SurfaceWleglState>,
    /// Per-surface SHM state, keyed by WlSurface.
    pub surface_shm: HashMap<WlSurface, SurfaceShmState>,

    /// Toplevel surfaces tracked for rendering and lifecycle.
    pub toplevels: Vec<ToplevelSurface>,

    /// Popup manager for tracking xdg_popup surfaces and their positions.
    pub popup_manager: PopupManager,

    /// Output scale factor (physical pixels per logical pixel). Canonical source
    /// of truth — lib.rs sets this at startup and render.rs reads it back.
    pub output_scale: i32,

    /// Logical output size (physical pixels / scale), used to configure toplevels.
    /// Tracks the primary display's geometry; per-host sizing is handled by
    /// each `OutputHost.logical_size`. Phase 0/1 always uses the primary
    /// display's metrics for both.
    pub output_logical_size: (i32, i32),

    /// Per-Activity render targets. One entry per Android `CompositorActivity`
    /// that has registered its `SurfaceView`. For phase 0-4 there is at most
    /// one host (the `"primary"` Activity). Phase 5 onward populates this
    /// with one entry per chroot toplevel that becomes a separate task.
    ///
    /// See `notes/multi-activity.md`.
    pub hosts: HashMap<ActivityId, OutputHost>,

    /// Toplevel → host assignment. Looked up by the render loop to decide
    /// which host's `EGLSurface` a given toplevel paints into. Subsurfaces
    /// and popups are NOT in this map — the renderer derives their host
    /// from their root toplevel.
    ///
    /// Populated by `assign_toplevel_to_host` from
    /// `XdgShellHandler::new_toplevel`. Cleaned up alongside dead toplevels
    /// in the frame timer.
    pub toplevel_to_host: HashMap<WlSurface, ActivityId>,

    /// When true, the policy assigns every toplevel to the same Activity
    /// (the existing host or the hardcoded `"primary"` if none exists yet)
    /// rather than spawning a new Activity per toplevel. Defaults to false
    /// (multi-window) since phase 5; will be exposed as a SharedPreference
    /// in the polish pass.
    pub single_activity_mode: bool,

    /// ActivityId of the host Android currently shows in the foreground.
    /// Used by per-host input / focus dispatch. Updated on
    /// `SurfaceEvent::FocusChanged`. None when no Activity has reported
    /// focus yet (e.g. cold start before the first
    /// `onWindowFocusChanged(true)`).
    pub foreground_host: Option<ActivityId>,

    /// Text input protocol state.
    pub text_input_state: TextInputState,

    /// Number of connected Wayland clients. Shared with ClientState instances
    /// so they can increment/decrement from ClientData callbacks.
    pub client_count: Arc<AtomicU32>,

    /// Set when toplevels are added or removed; cleared by the frame timer
    /// after updating focus. Avoids per-frame focus scans when nothing changed.
    pub toplevels_changed: bool,

    /// Set by the compositor commit handler when a wlegl/AHB surface attaches
    /// a buffer whose texture is already cached (re-attach of an existing
    /// wl_buffer object with fresh Adreno-written content). Without this,
    /// import_wlegl_buffers returns false on every commit after the first,
    /// needs_render stays false, and we stop redrawing even though the client
    /// keeps producing frames.
    pub buffer_commit_pending: bool,

    /// XWayland state. The shell state global is created on startup and
    /// lets X11 clients associate their X11 windows with backing
    /// wl_surfaces. `xwm` is None until Xwayland reports Ready.
    pub xwayland_shell_state: XWaylandShellState,
    pub xwm: Option<X11Wm>,
    /// X11 toplevels currently mapped. Iterated by the renderer the
    /// same way `toplevels` is. Override-redirect surfaces (popups,
    /// menus) live here too — the renderer doesn't yet stack them
    /// separately above their parent.
    pub x11_surfaces: Vec<X11Surface>,
    /// X11Surface's wl_surface → host id, mirror of `toplevel_to_host`
    /// for X11. Populated when an X11Surface gets a backing wl_surface
    /// (commit-time association via xwayland_shell_v1).
    pub x11_to_host: HashMap<WlSurface, ActivityId>,
    /// X display number Xwayland is listening on. None until the
    /// XWayland Ready event arrives.
    pub xdisplay: Option<u32>,
}

impl TawcState {
    pub fn new(display: &mut Display<Self>, output_scale: i32, output_logical_size: (i32, i32)) -> Self {
        let dh = display.handle();

        // v6 so we can send wl_surface.preferred_buffer_scale per surface
        // (done from `new_surface`). GTK4 ≥ 4.18 needs that signal to
        // allocate HiDPI buffers — see notes/rendering.md.
        let compositor_state = CompositorState::new_v6::<Self>(&dh);
        let xdg_shell_state = XdgShellState::new::<Self>(&dh);
        let xdg_decoration_state = XdgDecorationState::new::<Self>(&dh);
        let shm_state = ShmState::new::<Self>(&dh, []);
        let data_device_state = DataDeviceState::new::<Self>(&dh);
        // wp_viewporter lets clients set a logical destination size separate
        // from the buffer dimensions. Firefox/WebRender allocates HiDPI
        // buffers with buffer_scale=1 and uses viewport.set_destination to
        // tell the compositor the surface's logical size; without
        // viewporter, Firefox falls back to a path that ends up oversizing
        // the surface on a 2x output. The returned `ViewporterState` has no
        // Drop impl — the global lives for the lifetime of the Display.
        ViewporterState::new::<Self>(&dh);
        let mut seat_state = SeatState::new();
        let mut seat = seat_state.new_wl_seat(&dh, "tawc");
        // Advertise pointer, keyboard, and touch capabilities
        seat.add_pointer();
        let xkb_root = "/data/data/me.phie.tawc/files/xkb";
        std::env::set_var("XKB_CONFIG_ROOT", xkb_root);
        // Smithay falls back to writing the keymap to a tempfile under
        // XDG_RUNTIME_DIR (or std::env::temp_dir() = /tmp) for wl_keyboard
        // versions < 7. /tmp doesn't exist on a stock Android emulator and
        // an untrusted_app can't write to it where it does. Without the
        // keymap, smithay skips wl_keyboard.enter, GTK never activates the
        // wayland IM, and text-input.enable never fires.
        std::env::set_var("XDG_RUNTIME_DIR", "/data/data/me.phie.tawc");
        // libxkbcommon's `xkb_context_new` returns NULL when none of its
        // include paths can be opened; xkbcommon-rs's `Context::new`
        // doesn't NULL-check that, and the C `xkb_context_ref` it later
        // hands the NULL to doesn't NULL-check either, so the failure
        // mode reaching us via smithay's `add_keyboard` is a SIGSEGV with
        // no useful log line. Catch the realistic precondition (xkb data
        // dir present) up front so the panic message lands in
        // tawc-native logcat instead of just `libc Fatal signal 11`.
        let evdev_rules = std::path::Path::new(xkb_root).join("rules/evdev");
        if !evdev_rules.is_file() {
            panic!(
                "xkb data missing at {} — CompositorService.ensureXkbDataExtracted should have populated this before nativeStartCompositor",
                evdev_rules.display(),
            );
        }
        seat.add_keyboard(XkbConfig::default(), 200, 25)
            .expect("Failed to add keyboard to seat");
        seat.add_touch();

        dh.create_global::<Self, AndroidWlegl, ()>(2, ());
        dh.create_global::<Self, ZwpTextInputManagerV3, ()>(1, ());
        let xwayland_shell_state = XWaylandShellState::new::<Self>(&dh);

        Self {
            display_handle: dh,
            compositor_state,
            shm_state,
            xdg_shell_state,
            xdg_decoration_state,
            data_device_state,
            seat_state,
            seat,
            surface_wlegl: HashMap::new(),
            surface_shm: HashMap::new(),
            toplevels: Vec::new(),
            popup_manager: PopupManager::default(),
            output_scale,
            output_logical_size,
            text_input_state: TextInputState::new(),
            client_count: Arc::new(AtomicU32::new(0)),
            toplevels_changed: false,
            buffer_commit_pending: false,
            hosts: HashMap::new(),
            toplevel_to_host: HashMap::new(),
            // Phase 5: default to multi-window. Each non-child toplevel
            // gets its own Android task / recents card.
            single_activity_mode: false,
            foreground_host: None,
            xwayland_shell_state,
            xwm: None,
            x11_surfaces: Vec::new(),
            x11_to_host: HashMap::new(),
            xdisplay: None,
        }
    }

    /// Outcome of a host assignment: the host the toplevel ends up on,
    /// and whether the policy decided a fresh Activity needs to be
    /// spawned for it. Phase 5+ uses `spawn_activity` to fire the
    /// reverse-JNI call after the borrow on `TawcState` is released.
    pub fn assign_toplevel_to_host(&mut self, toplevel: &ToplevelSurface) -> HostAssignment {
        let surface = toplevel.wl_surface().clone();

        // Children of an existing toplevel always share that toplevel's
        // host (e.g. a dialog opens in the same Android task as its parent).
        if let Some(parent_surf) = toplevel.parent() {
            if let Some(parent_host) = self.toplevel_to_host.get(&parent_surf).cloned() {
                self.toplevel_to_host.insert(surface, parent_host.clone());
                return HostAssignment { host: parent_host, spawn_activity: false };
            }
        }

        if self.single_activity_mode {
            // Collapse all toplevels onto the first existing host. If no
            // Activity has registered yet, mint a fresh id and ask Kotlin
            // to spawn one — single-Activity mode still needs at least
            // one CompositorActivity to render into.
            let (id, spawn_activity) = match self.hosts.keys().next().cloned() {
                Some(id) => (id, false),
                None => (crate::host::new_activity_id(), true),
            };
            self.toplevel_to_host.insert(surface, id.clone());
            HostAssignment { host: id, spawn_activity }
        } else {
            // Multi-window: stage a fresh host id and ask Kotlin to spawn
            // an Activity for it. The Activity will register its surface
            // asynchronously via `nativeRegisterActivitySurface`.
            let new_id = crate::host::new_activity_id();
            self.toplevel_to_host.insert(surface, new_id.clone());
            HostAssignment { host: new_id, spawn_activity: true }
        }
    }
}

/// Result of `TawcState::assign_toplevel_to_host`. Caller owns the
/// reverse-JNI side-effect so it happens outside the `&mut TawcState`
/// borrow.
pub struct HostAssignment {
    pub host: ActivityId,
    pub spawn_activity: bool,
}

// ---------------------------------------------------------------------------
// Smithay handler impls
// ---------------------------------------------------------------------------

impl BufferHandler for TawcState {
    fn buffer_destroyed(&mut self, _buffer: &wl_buffer::WlBuffer) {}
}

impl ShmHandler for TawcState {
    fn shm_state(&self) -> &ShmState {
        &self.shm_state
    }
}

impl CompositorHandler for TawcState {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }

    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        // XWayland clients carry their own ClientData type; falling
        // through to ClientState::unwrap would panic on the very first
        // surface commit from Xwayland.
        if let Some(state) = client.get_data::<XWaylandClientData>() {
            return &state.compositor_state;
        }
        &client.get_data::<ClientState>().unwrap().compositor_state
    }

    fn new_surface(&mut self, surface: &WlSurface) {
        // Send preferred_buffer_scale up front so HiDPI clients commit at
        // native size from the first frame. No-op on pre-v6 surfaces.
        let scale = self.output_scale;
        compositor::with_states(surface, |data| {
            compositor::send_surface_state(surface, data, scale, smithay::utils::Transform::Normal);
        });
    }

    fn commit(&mut self, surface: &WlSurface) {
        self.popup_manager.commit(surface);
        // Catch up on any X11Surface ↔ wl_surface ↔ host associations
        // that smithay's xwayland_shell handler has resolved since the
        // last commit. Helper scans all x11_surfaces internally — it
        // doesn't need the committed wl_surface to do its work.
        crate::xwayland::associate_pending_x11_surfaces(self);
        // Track the attached android_wlegl (AHB) buffer so the renderer can
        // find its texture. Smithay's `SurfaceAttributes::merge_into` already
        // sent `wl_buffer.release` for the old buffer before this handler runs
        // (see deps/smithay/src/wayland/compositor/handlers.rs:125), so the client
        // has been told the old buffer is free exactly once. We just mirror
        // the current attachment into `surface_wlegl` for the renderer.
        use smithay::wayland::compositor::{with_states, SurfaceAttributes};
        use smithay::wayland::compositor::BufferAssignment;

        let mut new_buf_info: Option<(wl_buffer::WlBuffer, i32, i32)> = None;
        let mut removed = false;
        let mut commit_buffer_scale: i32 = 1;
        let mut viewport_dst: Option<(i32, i32)> = None;
        with_states(surface, |surf_states| {
            let mut guard = surf_states.cached_state.get::<SurfaceAttributes>();
            let attrs = guard.current();
            commit_buffer_scale = attrs.buffer_scale.max(1);
            match &attrs.buffer {
                Some(BufferAssignment::NewBuffer(buf)) => {
                    if let Some(data) = wlegl::wlegl_buffer_data(buf) {
                        new_buf_info = Some((buf.clone(), data.width, data.height));
                    }
                }
                Some(BufferAssignment::Removed) => removed = true,
                None => {}
            }
            drop(guard);
            let mut vp_guard = surf_states
                .cached_state
                .get::<smithay::wayland::viewporter::ViewportCachedState>();
            viewport_dst = vp_guard.current().dst.map(|s| (s.w, s.h));
        });

        if let Some((buf, w, h)) = new_buf_info {
            self.surface_wlegl.insert(
                surface.clone(),
                SurfaceWleglState {
                    current_buffer: Some(buf),
                    committed_width: w,
                    committed_height: h,
                    buffer_scale: commit_buffer_scale,
                    viewport_dst,
                },
            );
            // A wlegl surface replaces any prior SHM attachment.
            self.surface_shm.remove(surface);
            self.buffer_commit_pending = true;
        } else if removed {
            // wl_surface.attach(NULL): drop both wlegl and SHM state for this
            // surface. Without the SHM removal an SHM-only surface that
            // unmaps via a null commit would keep its last texture rendering
            // until the surface itself dies.
            self.surface_wlegl.remove(surface);
            self.surface_shm.remove(surface);
            self.buffer_commit_pending = true;
        } else {
            // No buffer change — but buffer_scale or viewport may have moved.
            // Refresh both on whichever surface entry exists, and request a
            // redraw if anything actually changed (a viewport-only commit
            // wouldn't otherwise wake the renderer).
            if let Some(ws) = self.surface_wlegl.get_mut(surface) {
                if ws.buffer_scale != commit_buffer_scale || ws.viewport_dst != viewport_dst {
                    ws.buffer_scale = commit_buffer_scale;
                    ws.viewport_dst = viewport_dst;
                    self.buffer_commit_pending = true;
                }
            }
            if let Some(ss) = self.surface_shm.get_mut(surface) {
                if ss.buffer_scale != commit_buffer_scale || ss.viewport_dst != viewport_dst {
                    ss.buffer_scale = commit_buffer_scale;
                    ss.viewport_dst = viewport_dst;
                    self.buffer_commit_pending = true;
                }
            }
        }
    }
}

impl XdgShellHandler for TawcState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        info!("New toplevel surface: {:?}", surface.wl_surface().id());

        // Run the assignment policy (which inserts into toplevel_to_host).
        // The result tells us whether to spawn a new Activity (phase 5+).
        let assignment = self.assign_toplevel_to_host(&surface);

        // Configure with the host's logical size if it exists, otherwise
        // fall back to the cached primary-output size.
        let (w, h) = self
            .hosts
            .get(&assignment.host)
            .map(|h| h.logical_size)
            .unwrap_or(self.output_logical_size);
        surface.with_pending_state(|state| {
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Activated,
            );
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Maximized,
            );
            state.size = Some((w, h).into());
            // Send xdg_toplevel.configure_bounds so clients that ignore
            // Maximized still cap their natural size to the screen. GTK4
            // 4.18 widget-factory ignores Maximized and produces a
            // 1369x1200 buffer (gtk4-widget-factory's natural width) on a
            // 540x1200 logical screen, ending up rendered as a 2738x2400
            // physical surface most of which is off-screen — symptom is
            // a "blank" window. Sending bounds tells GTK4 the maximum
            // size it should pick.
            state.bounds = Some((w, h).into());
        });
        surface.send_configure();

        // Move keyboard focus to the new toplevel only if its host is
        // currently in the foreground. Otherwise the FocusChanged event
        // will set focus when the host's Activity gains focus.
        //
        // For phase 0-4 / single-Activity mode, foreground_host is None
        // until the first Android focus event fires. To avoid losing
        // focus during the brief startup window, fall back to "the
        // first host" when foreground_host is unset — that matches the
        // pre-multi-window behaviour and keeps text input working.
        let host_is_foreground = match &self.foreground_host {
            Some(fg) => *fg == assignment.host,
            None => self.hosts.keys().next() == Some(&assignment.host),
        };
        if host_is_foreground {
            if let Some(keyboard) = self.seat.get_keyboard() {
                let serial = smithay::utils::SERIAL_COUNTER.next_serial();
                keyboard.set_focus(self, Some(surface.wl_surface().clone()), serial);
            }
        }
        self.toplevels.push(surface);
        self.toplevels_changed = true;

        // Reverse-JNI side effects after the &mut self borrow is released.
        if assignment.spawn_activity {
            crate::spawn_activity_from_native(&assignment.host);
        }
    }

    fn new_popup(&mut self, surface: PopupSurface, positioner: PositionerState) {
        let (output_w, output_h) = self.output_logical_size;
        let target = smithay::utils::Rectangle::from_size(smithay::utils::Size::from((output_w, output_h)));
        let geometry = positioner.get_unconstrained_geometry(target);
        surface.with_pending_state(|state| {
            state.geometry = geometry;
        });
        if let Err(e) = surface.send_configure() {
            error!("Failed to send popup configure: {:?}", e);
        }
        if let Err(e) = self.popup_manager.track_popup(surface.into()) {
            error!("Failed to track popup: {:?}", e);
        }
    }
    fn grab(&mut self, _surface: PopupSurface, _seat: wl_seat::WlSeat, _serial: Serial) {}
    fn reposition_request(
        &mut self,
        surface: PopupSurface,
        positioner: PositionerState,
        token: u32,
    ) {
        let (output_w, output_h) = self.output_logical_size;
        let target = smithay::utils::Rectangle::from_size(smithay::utils::Size::from((output_w, output_h)));
        surface.with_pending_state(|state| {
            state.geometry = positioner.get_unconstrained_geometry(target);
        });
        surface.send_repositioned(token);
        if let Err(e) = surface.send_configure() {
            error!("Failed to send popup reposition configure: {:?}", e);
        }
    }
}

impl OutputHandler for TawcState {}

impl XdgDecorationHandler for TawcState {
    fn new_decoration(&mut self, toplevel: ToplevelSurface) {
        use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode;
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(Mode::ServerSide);
        });
        toplevel.send_configure();
    }

    fn request_mode(
        &mut self,
        toplevel: ToplevelSurface,
        _mode: wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode,
    ) {
        use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode;
        // Always force server-side (no decorations since we don't draw any)
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(Mode::ServerSide);
        });
        toplevel.send_configure();
    }

    fn unset_mode(&mut self, toplevel: ToplevelSurface) {
        use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode;
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(Mode::ServerSide);
        });
        toplevel.send_configure();
    }
}

impl SeatHandler for TawcState {
    type KeyboardFocus = WlSurface;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;

    fn seat_state(&mut self) -> &mut SeatState<Self> {
        &mut self.seat_state
    }

    fn focus_changed(&mut self, _seat: &Seat<Self>, _focused: Option<&WlSurface>) {}
    fn cursor_image(
        &mut self,
        _seat: &Seat<Self>,
        _image: smithay::input::pointer::CursorImageStatus,
    ) {
    }
}

// ---------------------------------------------------------------------------
// Client state + delegate macros
// ---------------------------------------------------------------------------

pub struct ClientState {
    pub compositor_state: CompositorClientState,
    pub client_count: Arc<AtomicU32>,
}

impl ClientState {
    pub fn new(client_count: Arc<AtomicU32>) -> Self {
        Self {
            compositor_state: CompositorClientState::default(),
            client_count,
        }
    }
}

impl ClientData for ClientState {
    fn initialized(&self, _client_id: ClientId) {
        self.client_count.fetch_add(1, Ordering::Relaxed);
        info!("Wayland client initialized (total: {})", self.client_count.load(Ordering::Relaxed));
    }

    fn disconnected(&self, _client_id: ClientId, _reason: DisconnectReason) {
        self.client_count.fetch_sub(1, Ordering::Relaxed);
        info!("Wayland client disconnected: {:?} (total: {})", _reason, self.client_count.load(Ordering::Relaxed));
    }
}

impl DataDeviceHandler for TawcState {
    fn data_device_state(&self) -> &DataDeviceState {
        &self.data_device_state
    }
}

impl ClientDndGrabHandler for TawcState {}
impl ServerDndGrabHandler for TawcState {}

impl SelectionHandler for TawcState {
    type SelectionUserData = ();
}

delegate_compositor!(TawcState);
delegate_data_device!(TawcState);
delegate_output!(TawcState);
delegate_shm!(TawcState);
delegate_xdg_decoration!(TawcState);
delegate_xdg_shell!(TawcState);
delegate_seat!(TawcState);
delegate_viewporter!(TawcState);
delegate_xwayland_shell!(TawcState);
