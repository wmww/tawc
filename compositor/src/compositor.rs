//! Wayland protocol state and handler implementations.
//!
//! This module owns TawcState and all the Smithay handler trait impls.
//! Renderer/EGL details stay grouped in render::RenderState, which is carried
//! by TawcState because Smithay callbacks use one concrete state type.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use log::{error, info, warn};

use smithay::backend::renderer::utils::with_renderer_surface_state;
use smithay::backend::renderer::{buffer_type, BufferType};
use smithay::delegate_dispatch2;
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::input::dnd::DndGrabHandler;
use smithay::input::keyboard::XkbConfig;
use smithay::reexports::wayland_server::protocol::{wl_output, wl_seat};
use smithay::reexports::wayland_server::protocol::wl_buffer;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{
    Client, DisplayHandle, Resource,
};
use smithay::reexports::wayland_server::backend::{ClientData, ClientId, DisconnectReason};
use smithay::reexports::wayland_server::Display;
use smithay::reexports::wayland_server::WEnum;
use smithay::reexports::wayland_protocols_misc::server_decoration::server::{
    org_kde_kwin_server_decoration::{
        Mode as KdeDecorationMode, OrgKdeKwinServerDecoration,
    },
    org_kde_kwin_server_decoration_manager::Mode as KdeDefaultDecorationMode,
};
use smithay::utils::Serial;
use smithay::wayland::buffer::BufferHandler;
use smithay::wayland::compositor::{
    self, CompositorClientState, CompositorHandler, CompositorState,
};
use smithay::wayland::fractional_scale::{
    self, FractionalScaleHandler, FractionalScaleManagerState,
};
use smithay::wayland::output::{OutputHandler, OutputManagerState};
use smithay::wayland::selection::data_device::{
    DataDeviceHandler, DataDeviceState, WaylandDndGrabHandler, set_data_device_focus,
};
use smithay::wayland::selection::{SelectionHandler, SelectionSource, SelectionTarget};
use std::os::fd::OwnedFd;
use smithay::desktop::{
    find_popup_root_surface, PopupGrab, PopupKeyboardGrab, PopupManager, PopupPointerGrab, Window,
};
use smithay::input::pointer::Focus as PointerFocusMode;
use smithay::wayland::shell::kde::decoration::{
    KdeDecorationHandler, KdeDecorationState,
};
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
    XdgToplevelSurfaceData,
};
use smithay::wayland::shell::xdg::decoration::{XdgDecorationHandler, XdgDecorationState};
use smithay::wayland::shm::{ShmHandler, ShmState};
use smithay::wayland::viewporter::ViewporterState;
use smithay::wayland::xwayland_shell::XWaylandShellState;
use smithay::xwayland::{X11Surface, X11Wm, XWaylandClientData};

use crate::host::{ActivityId, OutputHost};
use crate::launcher;
use crate::protocol::android_wlegl::server::android_wlegl::AndroidWlegl;
use crate::protocol::tawc_gfxstream::server::tawc_gfxstream::TawcGfxstream;
use crate::scale::OutputScale;
use crate::text_input::TextInputState;
use crate::wlegl;
use wayland_protocols::wp::text_input::zv3::server::zwp_text_input_manager_v3::ZwpTextInputManagerV3;

#[derive(Clone, PartialEq, Eq)]
pub struct WindowMetadata {
    pub title: String,
    pub app_id: String,
    pub desktop_id: String,
    pub desktop_name: String,
    pub icon_path: String,
}

// ---------------------------------------------------------------------------
// TawcState — Wayland protocol state
// ---------------------------------------------------------------------------

/// Wayland protocol state for the compositor.
///
/// This is what Smithay handler callbacks receive. It also carries
/// `RenderState` because Smithay's compositor pre-commit hooks require the
/// same state type that owns protocol handlers.
pub struct TawcState {
    pub display_handle: DisplayHandle,
    pub compositor_state: CompositorState,
    pub shm_state: ShmState,
    pub xdg_shell_state: XdgShellState,
    // Held to keep the xdg-output manager global registered for the
    // display lifetime; Smithay's delegate macro reaches it through the
    // `OutputHandler` impl.
    #[allow(dead_code)]
    pub output_manager_state: OutputManagerState,
    // Held to keep the zxdg_decoration_manager_v1 global registered for the
    // life of the display; smithay's delegate macro reaches it via the
    // `XdgDecorationHandler` impl, never through this field.
    #[allow(dead_code)]
    pub xdg_decoration_state: XdgDecorationState,
    // Legacy KDE server-decoration protocol used by Qt and older toolkits.
    // TAWC presents Linux windows as Android app surfaces, so it suppresses
    // desktop chrome instead of asking clients to draw titlebars.
    #[allow(dead_code)]
    pub kde_decoration_state: KdeDecorationState,
    // Held to keep wp_fractional_scale_manager_v1 registered.
    #[allow(dead_code)]
    pub fractional_scale_manager_state: FractionalScaleManagerState,
    pub data_device_state: DataDeviceState,
    pub seat_state: SeatState<Self>,
    pub seat: Seat<Self>,

    /// Smithay desktop windows, host assignments, and per-host render
    /// projections.
    pub desktop: crate::desktop::DesktopRegistry,

    /// Popup manager for tracking xdg_popup surfaces and their positions.
    pub popup_manager: PopupManager,
    /// Active explicit xdg_popup grab. Touch input is not routed through
    /// Smithay's pointer grab, so the touch outside-dismiss path uses this
    /// to keep popup-grab bookkeeping in sync with `popup_done`.
    pub active_popup_grab: Option<PopupGrab<Self>>,

    /// GTK3 broken menus workaround.
    ///
    /// This deliberately contained compatibility path exposes a wl_pointer
    /// and briefly enters/leaves each new toplevel so GTK3 initializes its
    /// cold pointer-crossing state before touchscreen menubar taps. See
    /// notes/gtk3-broken-menus-workaround.md.
    pub gtk3_broken_menus_workaround_enabled: bool,

    /// Output scale factor (physical pixels per logical pixel). Canonical source
    /// of truth — lib.rs sets this at startup and render.rs reads it back.
    pub output_scale: OutputScale,

    /// Logical size of the host currently backing the single advertised output.
    /// Per-host configure sizing comes from each `OutputHost.logical_size`.
    pub output_logical_size: (i32, i32),

    /// Physical size behind the single advertised output. This follows the
    /// foreground host after startup, not arbitrary background host resizes.
    pub output_physical_size: (i32, i32),

    /// Per-Activity render targets. One entry per Android `CompositorActivity`
    /// that has registered its `SurfaceView`. For phase 0-4 there is at most
    /// one host (the `"primary"` Activity). Phase 5 onward populates this
    /// with one entry per chroot toplevel that becomes a separate task.
    ///
    /// See `notes/multi-activity.md`.
    pub hosts: HashMap<ActivityId, OutputHost>,

    /// When true, the policy assigns every toplevel to the same Activity
    /// (the existing host or the hardcoded `"primary"` if none exists yet)
    /// rather than spawning a new Activity per toplevel. Defaults to false
    /// (multi-window) since phase 5; will be exposed as a SharedPreference
    /// in the polish pass.
    pub single_activity_mode: bool,

    /// Canonical fullscreen state per ActivityId, retained even before
    /// the Activity registers its SurfaceView.
    pub host_fullscreen: HashMap<ActivityId, bool>,

    /// Last Android-facing metadata sent for each Activity. Used for
    /// recents labels/icons and as the seed for a future in-app switcher.
    pub window_metadata: HashMap<ActivityId, WindowMetadata>,

    /// Rootfs desktop-entry lookup cache. Includes misses so repeated
    /// title updates for unmatched app ids never rescan the filesystem.
    pub app_metadata_cache: HashMap<String, Option<launcher::DesktopAppMetadata>>,

    /// Text input protocol state.
    pub text_input_state: TextInputState,

    /// Number of connected Wayland clients. Shared with ClientState instances
    /// so they can increment/decrement from ClientData callbacks.
    pub client_count: Arc<AtomicU32>,

    /// Set when toplevels are added or removed; cleared by the frame timer
    /// after updating focus. Avoids per-frame focus scans when nothing changed.
    pub toplevels_changed: bool,

    /// Set by the compositor commit handler when any surface commits. Smithay
    /// imports textures while building render elements; this flag only wakes
    /// tawc's render loop for new buffers, damage, viewport changes, or
    /// same-buffer reattaches with fresh client-written content.
    pub buffer_commit_pending: bool,

    /// XWayland state. The shell state global is created on startup and
    /// lets X11 clients associate their X11 windows with backing
    /// wl_surfaces. `xwm` is None until Xwayland reports Ready.
    pub xwayland_shell_state: XWaylandShellState,
    pub xwm: Option<X11Wm>,
    /// X11 surfaces known to the window-manager side. Once an X11 surface
    /// gets a backing wl_surface it is mirrored into `desktop` as a Smithay
    /// `Window`; this list remains for XWM events and parent/selection policy.
    pub x11_surfaces: Vec<X11Surface>,
    /// X display number Xwayland is listening on. None until the
    /// XWayland Ready event arrives.
    pub xdisplay: Option<u32>,

    pub render: crate::render::RenderState,
    /// The single output object tracked today. Its `wl_output` global is
    /// advertised only after the first real Activity surface size arrives;
    /// multi-output support is left to a later phase (see
    /// `notes/multi-activity.md`).
    pub output: smithay::output::Output,
    pub output_advertised: bool,
    /// Host whose dimensions currently back the single advertised output.
    /// None until the first visible/bootstrap host registers.
    pub advertised_output_host: Option<ActivityId>,
    pub start_time: std::time::Instant,
    pub frame_count: u64,
    /// Set when buffer contents change; cleared after rendering.
    /// Skips GPU work when the screen hasn't changed.
    pub needs_render: bool,
    /// Number of toplevels visible in the last rendered frame.
    /// Used by the state query to verify the screen actually reflects cleanup.
    pub last_rendered_toplevels: usize,
}

impl TawcState {
    pub fn new(
        display: &mut Display<Self>,
        output_scale: OutputScale,
        output_logical_size: (i32, i32),
        output_physical_size: (i32, i32),
        gtk3_broken_menus_workaround_enabled: bool,
        render: crate::render::RenderState,
        output: smithay::output::Output,
    ) -> Self {
        let dh = display.handle();

        // v6 so we can send wl_surface.preferred_buffer_scale per surface
        // as the integer fallback. Clients that support fractional scaling
        // get the real value through wp_fractional_scale_v1.
        let compositor_state = CompositorState::new_v6::<Self>(&dh);
        let xdg_shell_state = XdgShellState::new::<Self>(&dh);
        let output_manager_state = OutputManagerState::new_with_xdg_output::<Self>(&dh);
        let xdg_decoration_state = XdgDecorationState::new::<Self>(&dh);
        let kde_decoration_state =
            KdeDecorationState::new::<Self>(&dh, KdeDefaultDecorationMode::Server);
        let fractional_scale_manager_state = FractionalScaleManagerState::new::<Self>(&dh);
        let shm_state = ShmState::new::<Self>(&dh, []);
        let data_device_state = DataDeviceState::new::<Self>(&dh);
        // wp_viewporter lets clients set a logical destination size separate
        // from the buffer dimensions. Firefox/WebRender allocates HiDPI
        // buffers with buffer_scale=1 and uses viewport.set_destination to
        // tell the compositor the surface's logical size; without
        // viewporter, Firefox falls back to a path that ends up oversizing
        // the surface on a scaled output. The returned `ViewporterState` has no
        // Drop impl — the global lives for the lifetime of the Display.
        ViewporterState::new::<Self>(&dh);
        let mut seat_state = SeatState::new();
        let mut seat = seat_state.new_wl_seat(&dh, "tawc");
        // Advertise only input devices tawc actually has. Android touch
        // should not appear as a Wayland pointer; toolkits must use wl_touch
        // for touchscreen interactions.
        let xkb_root = crate::app_paths::get().xkb_config_root.clone();
        std::env::set_var("XKB_CONFIG_ROOT", &xkb_root);
        // Smithay falls back to writing the keymap to a tempfile under
        // XDG_RUNTIME_DIR (or std::env::temp_dir() = /tmp) for wl_keyboard
        // versions < 7. /tmp doesn't exist on a stock Android emulator and
        // an untrusted_app can't write to it where it does. Without the
        // keymap, smithay skips wl_keyboard.enter, GTK never activates the
        // wayland IM, and text-input.enable never fires.
        std::env::set_var("XDG_RUNTIME_DIR", &crate::app_paths::get().data_dir);
        // libxkbcommon's `xkb_context_new` returns NULL when none of its
        // include paths can be opened; xkbcommon-rs's `Context::new`
        // doesn't NULL-check that, and the C `xkb_context_ref` it later
        // hands the NULL to doesn't NULL-check either, so the failure
        // mode reaching us via smithay's `add_keyboard` is a SIGSEGV with
        // no useful log line. Catch the realistic precondition (xkb data
        // dir present) up front so the panic message lands in
        // tawc-native logcat instead of just `libc Fatal signal 11`.
        let evdev_rules = std::path::Path::new(&xkb_root).join("rules/evdev");
        if !evdev_rules.is_file() {
            panic!(
                "xkb data missing at {} — CompositorService.ensureXkbDataExtracted should have populated this before nativeStartCompositor",
                evdev_rules.display(),
            );
        }
        seat.add_keyboard(XkbConfig::default(), 200, 25)
            .expect("Failed to add keyboard to seat");
        seat.add_touch();
        // GTK3 broken menus workaround: when enabled, expose a pointer so
        // the isolated workaround helper can briefly enter/leave each new
        // toplevel and prime GTK3's crossing state.
        if gtk3_broken_menus_workaround_enabled {
            seat.add_pointer();
        }

        dh.create_global::<Self, AndroidWlegl, ()>(2, ());
        dh.create_global::<Self, ZwpTextInputManagerV3, ()>(1, ());
        // gfxstream-bridge custom Vulkan WSI: bind unconditionally
        // (libhybris-backend clients ignore it; gfxstream-backend
        // clients use it instead of `zwp_linux_dmabuf_v1`). See
        // `crate::gfxstream_present` and notes/gfxstream-bridge.md
        // "WSI plan: custom Vulkan WSI".
        dh.create_global::<Self, TawcGfxstream, ()>(1, ());

        let xwayland_shell_state = XWaylandShellState::new::<Self>(&dh);

        Self {
            display_handle: dh,
            compositor_state,
            shm_state,
            xdg_shell_state,
            output_manager_state,
            xdg_decoration_state,
            kde_decoration_state,
            fractional_scale_manager_state,
            data_device_state,
            seat_state,
            seat,
            desktop: crate::desktop::DesktopRegistry::new(),
            popup_manager: PopupManager::default(),
            active_popup_grab: None,
            gtk3_broken_menus_workaround_enabled,
            output_scale,
            output_logical_size,
            output_physical_size,
            text_input_state: TextInputState::new(),
            client_count: Arc::new(AtomicU32::new(0)),
            toplevels_changed: false,
            buffer_commit_pending: false,
            hosts: HashMap::new(),
            host_fullscreen: HashMap::new(),
            window_metadata: HashMap::new(),
            app_metadata_cache: HashMap::new(),
            // Phase 5: default to multi-window. Each non-child toplevel
            // gets its own Android task / recents card.
            single_activity_mode: false,
            xwayland_shell_state,
            xwm: None,
            x11_surfaces: Vec::new(),
            xdisplay: None,
            render,
            output,
            output_advertised: false,
            advertised_output_host: None,
            start_time: std::time::Instant::now(),
            frame_count: 0,
            needs_render: true,
            last_rendered_toplevels: 0,
        }
    }

    /// Move the seat's keyboard focus and the text-input v3 focus to the
    /// same surface, atomically. These two are conceptually one thing —
    /// "the surface receiving input from the user" — and historically each
    /// of the four call sites that touched focus updated only one of them,
    /// letting them drift apart. Always go through this helper.
    ///
    /// Idempotent: a redundant update with the same surface fans out to
    /// `KeyboardHandle::set_focus` (Smithay deduplicates) and to
    /// `TextInputState::update_focus` (early-return on equal focus).
    pub fn set_input_focus(&mut self, target: Option<&WlSurface>) {
        let target_owned = target.cloned();
        if let Some(keyboard) = self.seat.get_keyboard() {
            let serial = smithay::utils::SERIAL_COUNTER.next_serial();
            keyboard.set_focus(self, target_owned, serial);
        }
        let target_client = target.and_then(|surface| surface.client());
        set_data_device_focus(&self.display_handle, &self.seat, target_client);
        self.text_input_state.update_focus(target);
    }

    /// Advertise the current scale to one surface through both scale paths:
    /// the integer wl_surface v6 fallback and the fractional-scale protocol.
    /// Runtime scale changes can reuse this over every live surface before
    /// reconfiguring toplevels.
    pub fn send_surface_scale(&self, surface: &WlSurface) {
        let scale = self.output_scale;
        compositor::with_states(surface, |data| {
            compositor::send_surface_state(
                surface,
                data,
                scale.integer_fallback(),
                smithay::utils::Transform::Normal,
            );
            fractional_scale::with_fractional_scale(data, |fractional| {
                fractional.set_preferred_scale(scale.fractional());
            });
        });
    }

    pub fn host_logical_size(&self, host_id: &ActivityId) -> Option<(i32, i32)> {
        self.hosts
            .get(host_id)
            .map(|host| host.logical_size)
            .filter(|(w, h)| *w > 0 && *h > 0)
    }

    pub fn toplevel_host_ready(&self, toplevel: &ToplevelSurface) -> bool {
        self.desktop
            .assigned_host(toplevel.wl_surface())
            .and_then(|host_id| self.host_logical_size(host_id))
            .is_some()
    }

    pub fn configure_toplevel_for_host(
        &self,
        toplevel: &ToplevelSurface,
        host_id: &ActivityId,
    ) -> Option<(i32, i32)> {
        let (w, h) = self.host_logical_size(host_id)?;
        let fullscreen = self.host_fullscreen(host_id);
        toplevel.with_pending_state(|state| {
            state.size = Some((w, h).into());
            // Clients that ignore Maximized still need a cap for their
            // natural size. GTK4 otherwise picks buffers wider than the
            // logical screen and renders mostly off-screen.
            state.bounds = Some((w, h).into());
        });
        set_toplevel_fullscreen_state(toplevel, fullscreen, None);
        Some((w, h))
    }

    /// Outcome of a host assignment: the host the toplevel ends up on,
    /// and whether the policy decided a fresh Activity needs to be
    /// spawned for it. Phase 5+ uses `spawn_activity` to fire the
    /// reverse-JNI call after the borrow on `TawcState` is released.
    pub fn assign_toplevel_to_host(
        &mut self,
        toplevel: &ToplevelSurface,
    ) -> crate::desktop::HostAssignment {
        self.desktop.assign_wayland_toplevel(
            toplevel.wl_surface().clone(),
            toplevel.parent(),
            self.single_activity_mode,
            self.hosts.keys().next().cloned(),
        )
    }

    pub fn set_host_fullscreen(&mut self, host_id: &ActivityId, fullscreen: bool) {
        self.host_fullscreen.insert(host_id.clone(), fullscreen);
        if let Some(host) = self.hosts.get_mut(host_id) {
            host.fullscreen = fullscreen;
        }
        let host_ready = self.host_logical_size(host_id).is_some();

        for toplevel in self.wayland_toplevels_for_host(host_id) {
            set_toplevel_fullscreen_state(&toplevel, fullscreen, None);
            if host_ready {
                toplevel.send_pending_configure();
            }
        }
    }

    pub fn host_fullscreen(&self, host_id: &ActivityId) -> bool {
        self.host_fullscreen
            .get(host_id)
            .copied()
            .or_else(|| self.hosts.get(host_id).map(|host| host.fullscreen))
            .unwrap_or(false)
    }

    pub fn desktop_visible_host_id(&self) -> Option<ActivityId> {
        self.desktop.visible_host_id(&self.hosts)
    }

    pub fn sync_primary_output_to_host(&mut self, host_id: &ActivityId) {
        let Some(host) = self.hosts.get(host_id) else {
            return;
        };
        let size = host.physical_size;
        self.output_physical_size = (size.w, size.h);
        self.output_logical_size = host.logical_size;
        let mode = smithay::output::Mode { size, refresh: 60_000 };
        self.output.change_current_state(
            Some(mode),
            Some(smithay::utils::Transform::Normal),
            Some(self.output_scale.smithay_scale()),
            Some((0, 0).into()),
        );
        self.output.set_preferred(mode);
        self.advertised_output_host = Some(host_id.clone());
    }

    pub fn sync_advertised_output_to_host_if_visible(&mut self, host_id: &ActivityId) {
        if !self.hosts.contains_key(host_id) {
            return;
        }
        let foreground = self.desktop.foreground_host();
        let bootstrap = foreground.is_none()
            && self
                .advertised_output_host
                .as_ref()
                .is_none_or(|advertised| advertised == host_id);
        if foreground != Some(host_id) && !bootstrap {
            return;
        }

        self.sync_primary_output_to_host(host_id);
        if !self.output_advertised {
            self.output.create_global::<TawcState>(&self.display_handle);
            self.output_advertised = true;
        }
    }

    pub fn sync_desktop_hosts(&mut self) {
        self.desktop
            .sync_hosts(&self.hosts, &self.output);
    }

    pub fn attached_buffer_counts(&self) -> (usize, usize) {
        let mut surfaces = Vec::new();
        for window in self.desktop.windows() {
            window.with_surfaces(|surface, _| {
                if !surfaces.iter().any(|s: &WlSurface| s == surface) {
                    surfaces.push(surface.clone());
                }
            });
        }

        let mut wlegl = 0;
        let mut shm = 0;
        for surface in surfaces {
            let Some(buffer) = with_renderer_surface_state(&surface, |renderer_state| {
                renderer_state.buffer().cloned()
            })
            .flatten()
            else {
                continue;
            };
            if wlegl::wlegl_buffer_data(&buffer).is_some() {
                wlegl += 1;
            } else if matches!(buffer_type(&buffer), Some(BufferType::Shm)) {
                shm += 1;
            }
        }
        (wlegl, shm)
    }

    pub fn update_wayland_window_metadata(&mut self, toplevel: &ToplevelSurface) {
        let Some(host_id) = self.desktop.assigned_host(toplevel.wl_surface()).cloned() else {
            return;
        };
        let (title, app_id) = xdg_toplevel_metadata(toplevel);
        self.update_host_window_metadata(&host_id, title, app_id);
    }

    pub fn x11_surface_host(&self, surface: &X11Surface) -> Option<ActivityId> {
        if let Some(wl) = surface.wl_surface() {
            if let Some(h) = self.desktop.assigned_host(&wl) {
                return Some(h.clone());
            }
        }
        surface
            .user_data()
            .get::<crate::xwayland::PendingHost>()
            .and_then(|p| p.0.borrow().clone())
    }

    pub fn host_has_windows(&self, host_id: &ActivityId) -> bool {
        self.desktop.any_window_for_host(host_id)
            || self
                .x11_surfaces
                .iter()
                .any(|surface| self.x11_surface_host(surface).as_ref() == Some(host_id))
    }

    pub fn finish_host_if_unused(&mut self, host_id: &ActivityId) -> bool {
        if self.host_has_windows(host_id) {
            return false;
        }
        self.finish_host(host_id);
        true
    }

    pub fn finish_host(&mut self, host_id: &ActivityId) {
        let should_finish_activity = self.hosts.contains_key(host_id)
            || self.window_metadata.contains_key(host_id)
            || self.host_fullscreen.contains_key(host_id);
        self.window_metadata.remove(host_id);
        self.host_fullscreen.remove(host_id);
        self.desktop.clear_foreground_host_if(host_id);
        if self.advertised_output_host.as_ref() == Some(host_id) {
            self.advertised_output_host = None;
        }
        self.hosts.remove(host_id);
        self.sync_desktop_hosts();
        if should_finish_activity {
            crate::finish_activity_from_native(host_id);
        }
    }

    pub fn request_close_windows_for_host(&mut self, host_id: &ActivityId) -> usize {
        let mut closed = 0;
        for toplevel in self.wayland_toplevels_for_host(host_id) {
            toplevel.send_close();
            closed += 1;
        }

        let x11_surfaces: Vec<_> = self
            .x11_surfaces
            .iter()
            .filter(|surface| self.x11_surface_host(surface).as_ref() == Some(host_id))
            .cloned()
            .collect();
        for surface in x11_surfaces {
            if let Err(e) = surface.close() {
                warn!("xwayland: failed to close window {}: {}", surface.window_id(), e);
            }
            closed += 1;
        }
        closed
    }

    pub fn request_close_all_client_windows_for_test(&mut self) -> usize {
        let mut closed = 0;
        for toplevel in self.xdg_shell_state.toplevel_surfaces() {
            if toplevel.alive() {
                toplevel.send_close();
                closed += 1;
            }
        }

        let x11_surfaces = self.x11_surfaces.clone();
        for surface in x11_surfaces {
            if let Err(e) = surface.close() {
                warn!("xwayland: failed to close window {}: {}", surface.window_id(), e);
            }
            closed += 1;
        }
        closed
    }

    pub fn update_host_window_metadata(
        &mut self,
        host_id: &ActivityId,
        title: String,
        app_id: String,
    ) {
        let old = self.window_metadata.get(host_id);
        let desktop = if old.is_some_and(|m| m.app_id == app_id) {
            old.and_then(|m| {
                (!m.desktop_id.is_empty() || !m.desktop_name.is_empty() || !m.icon_path.is_empty())
                    .then(|| launcher::DesktopAppMetadata {
                        desktop_id: m.desktop_id.clone(),
                        name: m.desktop_name.clone(),
                        icon_path: m.icon_path.clone(),
                    })
            })
        } else {
            self.resolve_cached_app_metadata(&app_id)
        };
        let metadata = WindowMetadata {
            title,
            app_id,
            desktop_id: desktop
                .as_ref()
                .map(|d| d.desktop_id.clone())
                .unwrap_or_default(),
            desktop_name: desktop
                .as_ref()
                .map(|d| d.name.clone())
                .unwrap_or_default(),
            icon_path: desktop
                .map(|d| d.icon_path)
                .unwrap_or_default(),
        };

        if self.window_metadata.get(host_id) == Some(&metadata) {
            return;
        }
        self.window_metadata.insert(host_id.clone(), metadata.clone());
        crate::update_window_metadata_from_native(host_id, &metadata);
    }

    fn resolve_cached_app_metadata(&mut self, app_id: &str) -> Option<launcher::DesktopAppMetadata> {
        let key = app_id.trim().to_ascii_lowercase();
        if key.is_empty() {
            return None;
        }
        if !self.app_metadata_cache.contains_key(&key) {
            self.app_metadata_cache
                .insert(key.clone(), launcher::resolve_metadata_for_app_id(app_id));
        }
        self.app_metadata_cache.get(&key).cloned().flatten()
    }

    pub fn wayland_toplevels_for_host(&self, host_id: &ActivityId) -> Vec<ToplevelSurface> {
        self.xdg_shell_state
            .toplevel_surfaces()
            .iter()
            .filter(|t| t.alive())
            .filter(|t| self.desktop.assigned_host(t.wl_surface()) == Some(host_id))
            .cloned()
            .collect()
    }

    pub fn first_wayland_toplevel_for_host(&self, host_id: &ActivityId) -> Option<WlSurface> {
        self.wayland_toplevels_for_host(host_id)
            .into_iter()
            .next()
            .map(|t| t.wl_surface().clone())
    }
}

fn xdg_toplevel_metadata(toplevel: &ToplevelSurface) -> (String, String) {
    compositor::with_states(toplevel.wl_surface(), |states| {
        let attributes = states
            .data_map
            .get::<XdgToplevelSurfaceData>()
            .unwrap()
            .lock()
            .unwrap();
        (
            attributes.title.clone().unwrap_or_default(),
            attributes.app_id.clone().unwrap_or_default(),
        )
    })
}

pub fn set_toplevel_fullscreen_state(
    toplevel: &ToplevelSurface,
    fullscreen: bool,
    output: Option<wl_output::WlOutput>,
) {
    use wayland_protocols::xdg::shell::server::xdg_toplevel::State as XdgState;

    toplevel.with_pending_state(|state| {
        if fullscreen {
            state.states.set(XdgState::Fullscreen);
            state.states.unset(XdgState::Maximized);
            state.fullscreen_output = output;
        } else {
            state.states.unset(XdgState::Fullscreen);
            state.states.set(XdgState::Maximized);
            state.fullscreen_output = None;
        }
    });
}

fn force_server_side_decoration(toplevel: &ToplevelSurface) {
    use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode;
    toplevel.with_pending_state(|state| {
        state.decoration_mode = Some(Mode::ServerSide);
    });
}

// ---------------------------------------------------------------------------
// Smithay handler impls
// ---------------------------------------------------------------------------

impl BufferHandler for TawcState {
    fn buffer_destroyed(&mut self, _buffer: &wl_buffer::WlBuffer) {
        // Smithay drops wl_buffer userdata, including any WleglBufferData
        // AHB ref. SHM state is keyed by surface and cleaned up elsewhere.
    }
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
        // Send scale up front so HiDPI clients commit at native size from
        // the first frame. Fractional-aware clients use
        // wp_fractional_scale_v1; integer-only clients use the rounded-up
        // wl_surface.preferred_buffer_scale fallback.
        self.send_surface_scale(surface);
    }

    fn commit(&mut self, surface: &WlSurface) {
        self.popup_manager.commit(surface);
        // Catch up only the X11Surface backed by this committed
        // wl_surface. The frame timer does the wider Xwayland race-closing
        // scan without making unrelated commits mutate other windows.
        crate::xwayland::associate_committed_x11_surface(self, surface);
        smithay::backend::renderer::utils::on_commit_buffer_handler::<TawcState>(surface);
        self.desktop.commit_surface(surface);
        self.sync_desktop_hosts();
        self.buffer_commit_pending = true;
    }
}

impl XdgShellHandler for TawcState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        info!("New toplevel surface: {:?}", surface.wl_surface().id());

        // Run the desktop registry's assignment policy.
        // The result tells us whether to spawn a new Activity (phase 5+).
        let assignment = self.assign_toplevel_to_host(&surface);

        surface.with_pending_state(|state| {
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Activated,
            );
        });
        set_toplevel_fullscreen_state(&surface, self.host_fullscreen(&assignment.host), None);
        if let Some((w, h)) = self.configure_toplevel_for_host(&surface, &assignment.host) {
            surface.send_configure();
            crate::gtk3_menus_workaround::prime_toplevel(self, surface.wl_surface(), w, h);
        } else {
            info!(
                "Deferring initial configure for {:?} until host {} registers",
                surface.wl_surface().id(),
                assignment.host,
            );
        }

        // Move input focus to the new toplevel only if its host is
        // currently in the foreground. Otherwise the FocusChanged event
        // will set focus when the host's Activity gains focus.
        //
        // For phase 0-4 / single-Activity mode, foreground_host is None
        // until the first Android focus event fires. To avoid losing
        // focus during the brief startup window, fall back to "the
        // first host" when foreground_host is unset — that matches the
        // pre-multi-window behaviour and keeps text input working.
        let host_is_foreground = match self.desktop.foreground_host() {
            Some(fg) => *fg == assignment.host,
            None => self.hosts.keys().next() == Some(&assignment.host),
        };
        let new_focus = host_is_foreground.then(|| surface.wl_surface().clone());
        let window = Window::new_wayland_window(surface.clone());
        self.desktop
            .add_wayland_window(surface.wl_surface().clone(), window);
        self.sync_desktop_hosts();
        self.toplevels_changed = true;
        if let Some(focus) = new_focus.as_ref() {
            self.set_input_focus(Some(focus));
        }
        self.update_wayland_window_metadata(&surface);

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
    fn grab(&mut self, surface: PopupSurface, _seat: wl_seat::WlSeat, serial: Serial) {
        let popup = surface.into();
        let root = match find_popup_root_surface(&popup) {
            Ok(root) => root,
            Err(e) => {
                error!("Failed to find popup root for grab: {:?}", e);
                return;
            }
        };
        let grab = match self
            .popup_manager
            .grab_popup::<Self>(root, popup, &self.seat, serial)
        {
            Ok(grab) => grab,
            Err(e) => {
                error!("Failed to grab popup: {:?}", e);
                return;
            }
        };
        self.active_popup_grab = Some(grab.clone());
        if let Some(keyboard) = self.seat.get_keyboard() {
            keyboard.set_grab(self, PopupKeyboardGrab::new(&grab), serial);
        }
        if let Some(pointer) = self.seat.get_pointer() {
            pointer.set_grab(
                self,
                PopupPointerGrab::new(&grab),
                serial,
                PointerFocusMode::Keep,
            );
        }
    }
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

    fn fullscreen_request(&mut self, surface: ToplevelSurface, output: Option<wl_output::WlOutput>) {
        let host_id = self.desktop.assigned_host(surface.wl_surface()).cloned();
        if let Some(host_id) = host_id {
            self.set_host_fullscreen(&host_id, true);
            crate::set_activity_fullscreen_from_native(&host_id, true);
        } else {
            set_toplevel_fullscreen_state(&surface, true, output);
            surface.send_pending_configure();
        }
    }

    fn unfullscreen_request(&mut self, surface: ToplevelSurface) {
        let host_id = self.desktop.assigned_host(surface.wl_surface()).cloned();
        if let Some(host_id) = host_id {
            self.set_host_fullscreen(&host_id, false);
            crate::set_activity_fullscreen_from_native(&host_id, false);
        } else {
            set_toplevel_fullscreen_state(&surface, false, None);
            surface.send_pending_configure();
        }
    }

    fn app_id_changed(&mut self, surface: ToplevelSurface) {
        self.update_wayland_window_metadata(&surface);
    }

    fn title_changed(&mut self, surface: ToplevelSurface) {
        self.update_wayland_window_metadata(&surface);
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let host = self.desktop.remove_wayland_toplevel(surface.wl_surface());
        self.sync_desktop_hosts();
        self.toplevels_changed = true;
        if let Some(host) = host {
            self.finish_host_if_unused(&host);
        }
    }
}

impl OutputHandler for TawcState {}

impl FractionalScaleHandler for TawcState {
    fn new_fractional_scale(&mut self, surface: WlSurface) {
        self.send_surface_scale(&surface);
    }
}

impl XdgDecorationHandler for TawcState {
    fn new_decoration(&mut self, toplevel: ToplevelSurface) {
        force_server_side_decoration(&toplevel);
        if self.toplevel_host_ready(&toplevel) {
            toplevel.send_pending_configure();
        }
    }

    fn request_mode(
        &mut self,
        toplevel: ToplevelSurface,
        _mode: wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode,
    ) {
        force_server_side_decoration(&toplevel);
        if self.toplevel_host_ready(&toplevel) {
            toplevel.send_pending_configure();
        }
    }

    fn unset_mode(&mut self, toplevel: ToplevelSurface) {
        force_server_side_decoration(&toplevel);
        if self.toplevel_host_ready(&toplevel) {
            toplevel.send_pending_configure();
        }
    }
}

impl KdeDecorationHandler for TawcState {
    fn kde_decoration_state(&self) -> &KdeDecorationState {
        &self.kde_decoration_state
    }

    fn new_decoration(
        &mut self,
        _surface: &WlSurface,
        decoration: &OrgKdeKwinServerDecoration,
    ) {
        decoration.mode(KdeDecorationMode::Server);
    }

    fn request_mode(
        &mut self,
        _surface: &WlSurface,
        decoration: &OrgKdeKwinServerDecoration,
        mode: WEnum<KdeDecorationMode>,
    ) {
        match mode {
            WEnum::Value(KdeDecorationMode::Server | KdeDecorationMode::None) => {
                decoration.mode(KdeDecorationMode::Server);
            }
            // Firefox/GTK re-requests client-side decorations each time the
            // compositor repeats "server". Ignore that request so desktop
            // chrome stays suppressed without creating a protocol ping-pong.
            WEnum::Value(KdeDecorationMode::Client) | WEnum::Unknown(_) => {}
            _ => {}
        }
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
    fn data_device_state(&mut self) -> &mut DataDeviceState {
        &mut self.data_device_state
    }
}

impl WaylandDndGrabHandler for TawcState {}
impl DndGrabHandler for TawcState {}

impl SelectionHandler for TawcState {
    type SelectionUserData = crate::clipboard::SelectionUserData;

    fn new_selection(
        &mut self,
        ty: SelectionTarget,
        source: Option<SelectionSource>,
        _seat: Seat<Self>,
    ) {
        let mime_types = source.as_ref().map(|source| source.mime_types());
        if let Some(xwm) = self.xwm.as_mut() {
            if let Err(e) = xwm.new_selection(ty, mime_types.clone()) {
                log::warn!("clipboard: failed to notify XWayland of Wayland selection: {:?}", e);
            }
        }

        if ty == SelectionTarget::Clipboard {
            if let Some(mime_types) = mime_types {
                log::debug!("clipboard: Wayland client offered clipboard mimes: {:?}", mime_types);
                crate::clipboard::queue_wayland_pull(mime_types);
            }
        }
    }

    fn send_selection(
        &mut self,
        ty: SelectionTarget,
        mime_type: String,
        fd: OwnedFd,
        _seat: Seat<Self>,
        user_data: &Self::SelectionUserData,
    ) {
        match user_data {
            crate::clipboard::SelectionUserData::AndroidText(text) => {
                if ty != SelectionTarget::Clipboard {
                    return;
                }
                crate::clipboard::write_text_to_fd(fd, text.clone());
            }
            crate::clipboard::SelectionUserData::X11(selection) => {
                if let Some(xwm) = self.xwm.as_mut() {
                    if let Err(e) = xwm.send_selection(*selection, mime_type, fd) {
                        log::warn!("clipboard: failed to send X11 selection to Wayland: {:?}", e);
                    }
                }
            }
        }
    }
}

delegate_dispatch2!(TawcState);
