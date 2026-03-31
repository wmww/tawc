use std::collections::HashMap;
use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::UnixStream;
use log::{error, info};

use smithay::backend::renderer::gles::{GlesRenderer, GlesTexProgram, GlesTexture};
use smithay::backend::renderer::{buffer_type, BufferType, ImportMemWl};
use smithay::delegate_compositor;
use smithay::delegate_data_device;
use smithay::delegate_output;
use smithay::delegate_seat;
use smithay::delegate_shm;
use smithay::delegate_xdg_shell;
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::reexports::wayland_server::protocol::wl_seat;
use smithay::reexports::wayland_server::protocol::wl_buffer;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};
use smithay::reexports::wayland_server::backend::{ClientData, ClientId, DisconnectReason};
use smithay::reexports::wayland_server::Display;
use smithay::utils::{Rectangle, Serial, Size};
use smithay::wayland::buffer::BufferHandler;
use smithay::wayland::compositor::{
    CompositorClientState, CompositorHandler, CompositorState, SurfaceAttributes,
    with_states,
};
use smithay::wayland::selection::data_device::{
    ClientDndGrabHandler, DataDeviceHandler, DataDeviceState, ServerDndGrabHandler,
};
use smithay::wayland::selection::{SelectionHandler, SelectionTarget, SelectionSource};
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
};
use smithay::wayland::output::OutputHandler;
use smithay::wayland::shm::{ShmHandler, ShmState};

use crate::ahb::AhbBuffer;
use crate::gl_import::AhbTextureImporter;
use crate::protocol::tawc_buffer_v1::server::{
    tawc_ahb_channel_v1::{self, TawcAhbChannelV1},
    tawc_buffer_manager_v1::{self, TawcBufferManagerV1},
};

/// Per-surface AHB channel state.
pub struct SurfaceAhbState {
    /// Our end of the side-channel socketpair for receiving AHBs.
    pub recv_socket: UnixStream,
    /// Pending AHB dimensions (set by attach, applied on commit).
    pub pending_width: Option<i32>,
    pub pending_height: Option<i32>,
    /// The currently committed (imported) texture for this surface.
    pub texture: Option<GlesTexture>,
    /// The currently committed AHB (kept alive so the texture remains valid).
    pub ahb: Option<AhbBuffer>,
    /// Width/height of the committed buffer.
    pub committed_width: i32,
    pub committed_height: i32,
}

/// Per-surface SHM buffer state.
pub struct SurfaceShmState {
    /// The currently committed (imported) texture for this surface.
    pub texture: Option<GlesTexture>,
    /// Width/height of the committed buffer.
    pub committed_width: i32,
    pub committed_height: i32,
}

/// Data stored per tawc_ahb_channel_v1 resource.
pub struct ChannelData {
    pub surface: WlSurface,
}

/// Main compositor state.
pub struct TawcState {
    pub display_handle: DisplayHandle,
    pub compositor_state: CompositorState,
    pub shm_state: ShmState,
    pub xdg_shell_state: XdgShellState,
    pub data_device_state: DataDeviceState,
    pub seat_state: SeatState<Self>,
    pub seat: Seat<Self>,

    /// Per-surface AHB channel state, keyed by WlSurface id.
    pub surface_ahb: HashMap<WlSurface, SurfaceAhbState>,

    /// Per-surface SHM buffer state (for surfaces using wl_shm).
    pub surface_shm: HashMap<WlSurface, SurfaceShmState>,

    /// Custom shader for rendering SHM buffers with magenta tint.
    pub shm_tint_shader: Option<GlesTexProgram>,

    /// Toplevel surfaces (for rendering).
    pub toplevels: Vec<ToplevelSurface>,

    /// AHB texture importer (loaded once).
    pub importer: Option<AhbTextureImporter>,

    /// Raw EGL display pointer (needed for AHB import).
    pub raw_egl_display: *const std::ffi::c_void,
    /// Raw EGL context pointer.
    pub raw_egl_context: *const std::ffi::c_void,
}

unsafe impl Send for TawcState {}

impl TawcState {
    pub fn new(display: &mut Display<Self>) -> Self {
        let dh = display.handle();

        let compositor_state = CompositorState::new::<Self>(&dh);
        let xdg_shell_state = XdgShellState::new::<Self>(&dh);
        let shm_state = ShmState::new::<Self>(&dh, []);
        let data_device_state = DataDeviceState::new::<Self>(&dh);
        let mut seat_state = SeatState::new();
        let seat = seat_state.new_wl_seat(&dh, "tawc");

        // Register tawc_buffer_manager_v1 global
        dh.create_global::<Self, TawcBufferManagerV1, ()>(1, ());

        Self {
            display_handle: dh,
            compositor_state,
            shm_state,
            xdg_shell_state,
            data_device_state,
            seat_state,
            seat,
            surface_ahb: HashMap::new(),
            surface_shm: HashMap::new(),
            shm_tint_shader: None,
            toplevels: Vec::new(),
            importer: None,
            raw_egl_display: std::ptr::null(),
            raw_egl_context: std::ptr::null(),
        }
    }

    /// Import pending AHB for a surface (called during commit).
    pub fn import_pending_ahb(&mut self, surface: &WlSurface, renderer: &GlesRenderer) {
        let Some(ahb_state) = self.surface_ahb.get_mut(surface) else {
            return;
        };

        let (Some(width), Some(height)) = (ahb_state.pending_width.take(), ahb_state.pending_height.take()) else {
            return;
        };

        // Receive one AHB from side channel (blocking socket, but we only
        // call this when pending_width is set, meaning client sent an attach).
        // Use non-blocking mode temporarily to drain all pending AHBs.
        let recv_fd = ahb_state.recv_socket.as_raw_fd();
        ahb_state.recv_socket.set_nonblocking(true).ok();
        let mut latest_ahb = None;
        loop {
            match AhbBuffer::recv_from_socket(recv_fd) {
                Ok(ahb) => {
                    latest_ahb = Some(ahb);
                }
                Err(_) => break,
            }
        }
        ahb_state.recv_socket.set_nonblocking(false).ok();

        match latest_ahb {
            Some(ahb) => {
                info!("Received AHB {}x{} for surface {:?}", ahb.width(), ahb.height(), surface.id());

                // Import as texture
                if let Some(ref importer) = self.importer {
                    // Make context current for GL operations
                    unsafe {
                        smithay::backend::egl::ffi::egl::MakeCurrent(
                            self.raw_egl_display,
                            smithay::backend::egl::ffi::egl::NO_SURFACE,
                            smithay::backend::egl::ffi::egl::NO_SURFACE,
                            self.raw_egl_context,
                        );
                    }
                    match importer.import_ahb(
                        renderer,
                        self.raw_egl_display,
                        ahb.as_raw(),
                        width,
                        height,
                    ) {
                        Ok(texture) => {
                            info!("AHB imported as texture for surface {:?}", surface.id());
                            ahb_state.texture = Some(texture);
                            ahb_state.ahb = Some(ahb);
                            ahb_state.committed_width = width;
                            ahb_state.committed_height = height;
                        }
                        Err(e) => error!("Failed to import AHB: {}", e),
                    }
                } else {
                    error!("No AHB importer available");
                }
            }
            None => {
                // No AHB available yet -- client sent attach but hasn't sent the AHB
            }
        }
    }

    /// Import a pending SHM buffer for a surface.
    /// Must be called with the renderer available (from the render loop).
    /// Returns true if a new buffer was imported.
    pub fn import_pending_shm(&mut self, surface: &WlSurface, renderer: &mut GlesRenderer) -> bool {
        // Check if there's a pending SHM buffer on this surface
        let buffer = with_states(surface, |states| {
            let mut guard = states.cached_state.get::<SurfaceAttributes>();
            let attrs = guard.current();
            match &attrs.buffer {
                Some(smithay::wayland::compositor::BufferAssignment::NewBuffer(buf)) => {
                    Some(buf.clone())
                }
                _ => None,
            }
        });

        let Some(buffer) = buffer else { return false };

        // Check if it's actually an SHM buffer
        if !matches!(buffer_type(&buffer), Some(BufferType::Shm)) {
            return false;
        }

        // Get buffer dimensions
        let dims = smithay::wayland::shm::with_buffer_contents(&buffer, |_, _, data| {
            (data.width, data.height)
        });
        let (width, height) = match dims {
            Ok(d) => d,
            Err(e) => {
                error!("Failed to get SHM buffer contents: {}", e);
                return false;
            }
        };

        // Use full buffer as damage region
        let damage = [Rectangle::from_size(Size::from((width, height)))];
        match renderer.import_shm_buffer(&buffer, None, &damage) {
            Ok(texture) => {
                let is_new = !self.surface_shm.contains_key(surface);
                let shm_state = self.surface_shm.entry(surface.clone()).or_insert(SurfaceShmState {
                    texture: None,
                    committed_width: 0,
                    committed_height: 0,
                });
                shm_state.texture = Some(texture);
                shm_state.committed_width = width;
                shm_state.committed_height = height;
                if is_new {
                    info!("SHM buffer imported as texture {}x{} for surface {:?}", width, height, surface.id());
                }
                true
            }
            Err(e) => {
                error!("Failed to import SHM buffer: {}", e);
                false
            }
        }
    }

}

// --- BufferHandler ---

impl BufferHandler for TawcState {
    fn buffer_destroyed(&mut self, _buffer: &wl_buffer::WlBuffer) {}
}

// --- ShmHandler ---

impl ShmHandler for TawcState {
    fn shm_state(&self) -> &ShmState {
        &self.shm_state
    }
}

// --- CompositorHandler ---

impl CompositorHandler for TawcState {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }

    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }

    fn commit(&mut self, _surface: &WlSurface) {
        // Buffer import is handled in the render loop to avoid borrow issues
        // with renderer access. We just note that a commit happened.
    }
}

// --- XdgShellHandler ---

impl XdgShellHandler for TawcState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        info!("New toplevel surface: {:?}", surface.wl_surface().id());
        surface.with_pending_state(|state| {
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Activated,
            );
        });
        surface.send_configure();
        self.toplevels.push(surface);
    }

    fn new_popup(&mut self, _surface: PopupSurface, _positioner: PositionerState) {}

    fn grab(&mut self, _surface: PopupSurface, _seat: wl_seat::WlSeat, _serial: Serial) {}

    fn reposition_request(
        &mut self,
        _surface: PopupSurface,
        _positioner: PositionerState,
        _token: u32,
    ) {
    }
}

// --- OutputHandler ---

impl OutputHandler for TawcState {}

// --- SeatHandler ---

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

// --- tawc_buffer_manager_v1 GlobalDispatch ---

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

// --- tawc_buffer_manager_v1 Dispatch ---

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

                // Create socketpair for AHB side channel
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

                // Send the client's end of the socketpair
                channel.channel_fd(client_sock.as_fd());
                info!("Sent side-channel fd to client");

                // Store compositor's end (keep blocking -- AHB recv needs it)
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

// --- tawc_ahb_channel_v1 Dispatch ---

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
                info!("AHB attach: {}x{} for surface {:?}", width, height, data.surface.id());
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

// --- Client state ---

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

// --- DataDeviceHandler (stub for GTK3 compatibility) ---

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

// --- Delegate macros ---

delegate_compositor!(TawcState);
delegate_data_device!(TawcState);
delegate_output!(TawcState);
delegate_shm!(TawcState);
delegate_xdg_shell!(TawcState);
delegate_seat!(TawcState);
