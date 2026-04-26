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
    CompositorClientState, CompositorHandler, CompositorState,
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
    /// We render the buffer 1:1 at these physical dimensions — matching the
    /// SHM draw path and what the legacy `surface_ahb` path did. Some
    /// clients (Firefox/WebRender's main wl_surface) render at the output's
    /// physical size but commit buffer_scale=1 anyway, so honouring
    /// buffer_scale would draw them at 2× and run them off-screen.
    pub committed_width: i32,
    pub committed_height: i32,
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
    pub output_logical_size: (i32, i32),

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
}

impl TawcState {
    pub fn new(display: &mut Display<Self>, output_scale: i32, output_logical_size: (i32, i32)) -> Self {
        let dh = display.handle();

        let compositor_state = CompositorState::new::<Self>(&dh);
        let xdg_shell_state = XdgShellState::new::<Self>(&dh);
        let xdg_decoration_state = XdgDecorationState::new::<Self>(&dh);
        let shm_state = ShmState::new::<Self>(&dh, []);
        let data_device_state = DataDeviceState::new::<Self>(&dh);
        let mut seat_state = SeatState::new();
        let mut seat = seat_state.new_wl_seat(&dh, "tawc");
        // Advertise pointer, keyboard, and touch capabilities
        seat.add_pointer();
        std::env::set_var("XKB_CONFIG_ROOT", "/data/data/me.phie.tawc/files/xkb");
        // Smithay falls back to writing the keymap to a tempfile under
        // XDG_RUNTIME_DIR (or std::env::temp_dir() = /tmp) for wl_keyboard
        // versions < 7. /tmp doesn't exist on a stock Android emulator and
        // an untrusted_app can't write to it where it does. Without the
        // keymap, smithay skips wl_keyboard.enter, GTK never activates the
        // wayland IM, and text-input.enable never fires.
        std::env::set_var("XDG_RUNTIME_DIR", "/data/data/me.phie.tawc");
        seat.add_keyboard(XkbConfig::default(), 200, 25)
            .expect("Failed to add keyboard to seat");
        seat.add_touch();

        dh.create_global::<Self, AndroidWlegl, ()>(2, ());
        dh.create_global::<Self, ZwpTextInputManagerV3, ()>(1, ());

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
        }
    }
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
        &client.get_data::<ClientState>().unwrap().compositor_state
    }

    fn commit(&mut self, surface: &WlSurface) {
        self.popup_manager.commit(surface);
        // Track the attached android_wlegl (AHB) buffer so the renderer can
        // find its texture. Smithay's `SurfaceAttributes::merge_into` already
        // sent `wl_buffer.release` for the old buffer before this handler runs
        // (see smithay/src/wayland/compositor/handlers.rs:125), so the client
        // has been told the old buffer is free exactly once. We just mirror
        // the current attachment into `surface_wlegl` for the renderer.
        use smithay::wayland::compositor::{with_states, SurfaceAttributes};
        use smithay::wayland::compositor::BufferAssignment;

        let mut new_buf_info: Option<(wl_buffer::WlBuffer, i32, i32)> = None;
        let mut removed = false;
        with_states(surface, |surf_states| {
            let mut guard = surf_states.cached_state.get::<SurfaceAttributes>();
            let attrs = guard.current();
            match &attrs.buffer {
                Some(BufferAssignment::NewBuffer(buf)) => {
                    if let Some(data) = wlegl::wlegl_buffer_data(buf) {
                        new_buf_info = Some((buf.clone(), data.width, data.height));
                    }
                }
                Some(BufferAssignment::Removed) => removed = true,
                None => {}
            }
        });

        if let Some((buf, w, h)) = new_buf_info {
            self.surface_wlegl.insert(
                surface.clone(),
                SurfaceWleglState {
                    current_buffer: Some(buf),
                    committed_width: w,
                    committed_height: h,
                },
            );
            // A wlegl surface replaces any prior SHM attachment.
            self.surface_shm.remove(surface);
            self.buffer_commit_pending = true;
        } else if removed {
            self.surface_wlegl.remove(surface);
            self.buffer_commit_pending = true;
        }
    }
}

impl XdgShellHandler for TawcState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        info!("New toplevel surface: {:?}", surface.wl_surface().id());
        let (w, h) = self.output_logical_size;
        surface.with_pending_state(|state| {
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Activated,
            );
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Maximized,
            );
            state.size = Some((w, h).into());
        });
        surface.send_configure();
        // Set keyboard focus to the new toplevel so text input works
        if let Some(keyboard) = self.seat.get_keyboard() {
            let serial = smithay::utils::SERIAL_COUNTER.next_serial();
            keyboard.set_focus(self, Some(surface.wl_surface().clone()), serial);
        }
        self.toplevels.push(surface);
        self.toplevels_changed = true;
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
