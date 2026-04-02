//! Wayland protocol state and handler implementations.
//!
//! This module owns TawcState (the Wayland protocol side of the compositor)
//! and all the smithay handler trait impls. It does NOT own the renderer,
//! EGL context, or GPU textures — those live in render::RenderState.
//!
//! The per-surface state structs (SurfaceAhbState, SurfaceShmState) contain
//! both protocol fields (pending_width, current_buffer) written by handlers
//! here, and texture fields written by render.rs during buffer import.
//! This cross-cutting is intentional: keeping them together avoids a separate
//! lookup table keyed by the same WlSurface.

use std::collections::HashMap;
use std::os::fd::AsFd;
use std::os::unix::net::UnixStream;
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
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
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

use crate::ahb::AhbBuffer;
use crate::protocol::tawc_buffer_v1::server::{
    tawc_ahb_channel_v1::{self, TawcAhbChannelV1},
    tawc_buffer_manager_v1::{self, TawcBufferManagerV1},
};
use crate::text_input::TextInputState;
use wayland_protocols::wp::text_input::zv3::server::zwp_text_input_manager_v3::ZwpTextInputManagerV3;

// ---------------------------------------------------------------------------
// Per-surface state
// ---------------------------------------------------------------------------

/// Per-surface AHB channel state.
///
/// Protocol fields (recv_socket, pending_width/height) are written by the
/// tawc_ahb_channel_v1 handler. Texture fields are written by render.rs
/// during import_pending_ahbs().
pub struct SurfaceAhbState {
    pub recv_socket: UnixStream,
    pub pending_width: Option<i32>,
    pub pending_height: Option<i32>,
    /// Set by render.rs after importing the AHB as a GL texture.
    pub texture: Option<GlesTexture>,
    /// Kept alive so the GL texture remains valid.
    pub ahb: Option<AhbBuffer>,
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

/// Data stored per tawc_ahb_channel_v1 resource.
pub struct ChannelData {
    pub surface: WlSurface,
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

    /// Per-surface AHB state, keyed by WlSurface.
    pub surface_ahb: HashMap<WlSurface, SurfaceAhbState>,
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
        // xkbcommon needs XKB data files; point it at the chroot's copy
        std::env::set_var("XKB_CONFIG_ROOT", "/data/local/arch-chroot/usr/share/xkeyboard-config-2");
        seat.add_keyboard(XkbConfig::default(), 200, 25)
            .expect("Failed to add keyboard to seat");
        seat.add_touch();

        dh.create_global::<Self, TawcBufferManagerV1, ()>(1, ());
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
            surface_ahb: HashMap::new(),
            surface_shm: HashMap::new(),
            toplevels: Vec::new(),
            popup_manager: PopupManager::default(),
            output_scale,
            output_logical_size,
            text_input_state: TextInputState::new(),
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
// tawc_buffer_manager_v1 / tawc_ahb_channel_v1 protocol
// ---------------------------------------------------------------------------

impl GlobalDispatch<TawcBufferManagerV1, ()> for TawcState {
    fn bind(
        _state: &mut Self,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: New<TawcBufferManagerV1>,
        _global_data: &(),
        data_init: &mut DataInit<'_, Self>,
    ) {
        data_init.init(resource, ());
        info!("Client bound tawc_buffer_manager_v1");
    }
}

impl Dispatch<TawcBufferManagerV1, ()> for TawcState {
    fn request(
        state: &mut Self,
        _client: &Client,
        _resource: &TawcBufferManagerV1,
        request: tawc_buffer_manager_v1::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            tawc_buffer_manager_v1::Request::GetChannel { id, surface } => {
                info!("get_channel for surface {:?}", surface.id());

                let (compositor_sock, client_sock) = match UnixStream::pair() {
                    Ok(pair) => pair,
                    Err(e) => {
                        error!("Failed to create socketpair: {}", e);
                        return;
                    }
                };

                let channel_data = ChannelData {
                    surface: surface.clone(),
                };
                let channel = data_init.init(id, channel_data);
                channel.channel_fd(client_sock.as_fd());
                info!("Sent side-channel fd to client");

                state.surface_ahb.insert(
                    surface,
                    SurfaceAhbState {
                        recv_socket: compositor_sock,
                        pending_width: None,
                        pending_height: None,
                        texture: None,
                        ahb: None,
                        committed_width: 0,
                        committed_height: 0,
                    },
                );
            }
            tawc_buffer_manager_v1::Request::Destroy => {}
        }
    }
}

impl Dispatch<TawcAhbChannelV1, ChannelData> for TawcState {
    fn request(
        state: &mut Self,
        _client: &Client,
        _resource: &TawcAhbChannelV1,
        request: tawc_ahb_channel_v1::Request,
        data: &ChannelData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            tawc_ahb_channel_v1::Request::Attach { width, height } => {
                info!(
                    "AHB attach: {}x{} for surface {:?}",
                    width, height, data.surface.id()
                );
                if let Some(ahb_state) = state.surface_ahb.get_mut(&data.surface) {
                    ahb_state.pending_width = Some(width);
                    ahb_state.pending_height = Some(height);
                }
            }
            tawc_ahb_channel_v1::Request::Destroy => {
                info!("AHB channel destroyed for surface {:?}", data.surface.id());
                state.surface_ahb.remove(&data.surface);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Client state + delegate macros
// ---------------------------------------------------------------------------

#[derive(Default)]
pub struct ClientState {
    pub compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, _client_id: ClientId) {
        info!("Wayland client initialized");
    }

    fn disconnected(&self, _client_id: ClientId, _reason: DisconnectReason) {
        info!("Wayland client disconnected: {:?}", _reason);
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
