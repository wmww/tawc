//! Wayland protocol state and handler implementations.
//!
//! This module owns TawcState and all the Smithay handler trait impls.
//! Renderer/EGL details stay grouped in render::RenderState, which is carried
//! by TawcState because Smithay callbacks use one concrete state type.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicU32, Ordering};
use log::{error, info, warn};

use smithay::reexports::calloop::{LoopHandle, RegistrationToken};
use smithay::backend::renderer::utils::with_renderer_surface_state;
use smithay::backend::renderer::{buffer_type, BufferType};
use smithay::delegate_dispatch2;
use smithay::wayland::presentation::PresentationState;
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
use smithay::utils::{Logical, Point, Serial};
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
use smithay::wayland::selection::ext_data_control::{
    DataControlHandler as ExtDataControlHandler, DataControlState as ExtDataControlState,
};
use smithay::wayland::selection::wlr_data_control::{
    DataControlHandler as WlrDataControlHandler, DataControlState as WlrDataControlState,
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
use smithay::wayland::cursor_shape::CursorShapeManagerState;
use smithay::wayland::tablet_manager::TabletSeatHandler;
use smithay::wayland::viewporter::ViewporterState;
use smithay::wayland::xwayland_shell::XWaylandShellState;
use smithay::xwayland::{X11Surface, X11Wm, XWaylandActivation, XWaylandClientData};

use crate::host::{ActivityId, OutputHost};
use crate::launcher;
use crate::protocol::android_wlegl::server::android_wlegl::AndroidWlegl;
#[cfg(feature = "gfxstream")]
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
/// Refresh rate the compositor advertises before Settings pushes one.
///
/// 60 Hz is what every previous build hard-coded, so an app that never
/// touches the new setting behaves exactly as before.
const DEFAULT_OUTPUT_REFRESH_MHZ: u32 = 60_000;

/// Bounds for an advertised rate. Below 10 Hz no client stays sane; above
/// 240 Hz nothing real exists, and a bogus huge value would overflow the
/// `i32` mHz field `wl_output.mode` carries.
const MIN_OUTPUT_REFRESH_MHZ: u32 = 10_000;
const MAX_OUTPUT_REFRESH_MHZ: u32 = 240_000;

/// One frame at `mhz`.
///
/// Single source for everything that has to agree on how fast the output
/// runs: the render loop's re-arm interval, the `wp_presentation` refresh
/// field, and the "is it time to draw again" check. Hard-coding 16 ms here
/// is what capped the whole pipeline at ~60 fps on a 120 Hz panel — the
/// timer is the only thing pacing renders, so frame callbacks (and with
/// them every client's animation) inherited that ceiling.
pub fn frame_period(mhz: u32) -> std::time::Duration {
    let clamped = mhz.clamp(MIN_OUTPUT_REFRESH_MHZ, MAX_OUTPUT_REFRESH_MHZ);
    std::time::Duration::from_nanos(1_000_000_000_000u64 / u64::from(clamped))
}

pub struct TawcState {
    pub display_handle: DisplayHandle,
    /// Calloop handle, set by `event_loop::run` before any source can fire.
    /// Callbacks must reach the loop through here (`data.loop_handle()`),
    /// never by capturing a `LoopHandle` clone: captured handles live inside
    /// the loop's own source list, forming an Rc cycle that keeps every
    /// source alive after the loop is dropped — including the Wayland
    /// listening socket, whose still-locked lock file makes the next
    /// in-process compositor start fail with "socket name already in use".
    pub loop_handle: Option<LoopHandle<'static, TawcState>>,
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
    /// Data-control (wl-copy/wl-paste without a surface); reads are gated
    /// in `SelectionHandler::allow_data_control_read`.
    pub ext_data_control_state: ExtDataControlState,
    pub wlr_data_control_state: WlrDataControlState,
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

    /// True while Android reports at least one mouse-class `InputDevice`.
    /// One of the two reasons the seat advertises `wl_pointer`; see
    /// [`TawcState::sync_pointer_capability`].
    pub mouse_attached: bool,

    /// Last real pointer position (logical, compositor space) and the focus
    /// it resolved to (surface + surface origin) — i.e. what was last handed
    /// to `PointerHandle::motion`, so the GTK3 prime can restore it instead
    /// of leaving to `None`. Not authoritative while a grab is active:
    /// smithay's own `current_focus` is, and the state query reads that.
    pub pointer_location: Point<f64, Logical>,
    pub pointer_focus: Option<(WlSurface, Point<f64, Logical>)>,

    /// Cursor presentation state. Android draws the pointer sprite; this
    /// tracks what was last pushed to the Activity's `PointerIcon`.
    pub cursor: crate::cursor::State,

    /// GTK3 broken menus workaround.
    ///
    /// This deliberately contained compatibility path asks for a wl_pointer
    /// (through [`TawcState::sync_pointer_capability`], which owns the
    /// capability) and briefly enters/leaves each new toplevel so GTK3
    /// initializes its cold pointer-crossing state before touchscreen
    /// menubar taps. See notes/gtk3-broken-menus-workaround.md.
    pub gtk3_broken_menus_workaround: crate::gtk3_menus_workaround::State,

    /// Output scale factor (physical pixels per logical pixel). Canonical source
    /// of truth — lib.rs sets this at startup and render.rs reads it back.
    pub output_scale: OutputScale,

    /// Logical size of the advertised output. Derived from
    /// `output_physical_size` and the scale by `set_output_mode`; per-host
    /// configure sizing comes from each `OutputHost.logical_size`.
    pub output_logical_size: (i32, i32),

    /// Physical size behind the single advertised output. Starts at the
    /// Android display metrics passed to `nativeStartCompositor` and then
    /// follows the foreground host, not arbitrary background host resizes.
    pub output_physical_size: (i32, i32),

    /// `wp_presentation` global. Clients that pace themselves by
    /// presentation feedback — Firefox's Wayland vsync source, `fifo`-style
    /// frame timing — cannot see how fast the panel really is without it and
    /// fall back to a fixed 60 Hz timer (visible as `Target Frame Rate: 60`
    /// in Firefox's `about:support` on a 120 Hz panel).
    pub presentation: PresentationState,

    /// Refresh rate advertised in the output's single mode, in mHz
    /// (60000 = 60 Hz). Starts at 60 Hz — what the compositor claimed
    /// before this was configurable — and is pushed from Settings via
    /// `nativeSetOutputRefreshRate`, which is where the panel's real
    /// maximum (120 Hz on the test tablet) comes in. Clients that pace
    /// themselves by `wl_output.mode` (WebRender, games, frame callbacks)
    /// render at this rate, so a stale 60 here means a 60 fps ceiling on a
    /// 120 Hz panel even though Android presents faster.
    pub output_refresh_mhz: u32,

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

    /// In-flight mirror of a client-owned clipboard selection into
    /// Android. A newer selection (or fresh Android clipboard text)
    /// cancels and replaces it; see clipboard.rs.
    pub clipboard_pull: Option<crate::clipboard::ActivePull>,

    /// `ClipDescription` timestamp of the last Android clip announced as
    /// a Wayland selection, for deduping repeat announces (focus syncs
    /// re-announce the same clip). Compositor restarts reset it.
    pub last_announced_android_clip_ts: Option<i64>,
    /// The live selection is a Wayland client's and its text is already
    /// in Android's clipboard, so the client is no longer needed to serve
    /// it (see `event_loop::check_idle`). Cleared on every new selection.
    pub selection_mirrored: bool,
    /// Live `wl_surface`s, Xwayland's included.
    pub surface_count: usize,
    /// Since when nothing has been connected; drives auto-stop.
    pub idle_since: Option<std::time::Instant>,

    /// Hardware keys accepted from Android and currently held in the Smithay
    /// seat. Releases must be honored even if Android Activity foreground
    /// bookkeeping changes between key-down and key-up.
    pub hardware_keys_down: HashSet<(ActivityId, u32)>,

    /// Number of connected Wayland clients. Shared with ClientState instances
    /// so they can increment/decrement from ClientData callbacks.
    pub client_count: Arc<AtomicU32>,
    pub client_ids: Arc<Mutex<Vec<ClientId>>>,

    /// Set when toplevels are added or removed; cleared by the frame timer
    /// after updating focus. Avoids per-frame focus scans when nothing changed.
    pub toplevels_changed: bool,

    /// Set by the compositor commit handler when any surface commits. Smithay
    /// imports textures while building render elements; this flag only wakes
    /// TAWC's render loop for new buffers, damage, viewport changes, or
    /// same-buffer reattaches with fresh client-written content.
    pub buffer_commit_pending: bool,

    /// XWayland state. The shell state global is created on startup and
    /// lets X11 clients associate their X11 windows with backing
    /// wl_surfaces. `xwm` is None until Xwayland reports Ready.
    pub xwayland_shell_state: XWaylandShellState,
    pub xwayland_enabled: bool,
    pub xwayland_source: Option<RegistrationToken>,
    pub xwayland_activation: Option<XWaylandActivation>,
    pub xwayland_activation_source: Option<RegistrationToken>,
    pub xwayland_source_dead: bool,
    pub xwayland_start_pending: bool,
    pub xwayland_start_after: Option<std::time::Instant>,
    pub xwm: Option<X11Wm>,
    /// X11 surfaces known to the window-manager side. Once an X11 surface
    /// gets a backing wl_surface it is mirrored into `desktop` as a Smithay
    /// `Window`; this list remains for XWM events and parent/selection policy.
    pub x11_surfaces: Vec<X11Surface>,
    /// X display number Xwayland is listening on. None until the
    /// XWayland Ready event arrives.
    pub xdisplay: Option<u32>,

    pub render: crate::render::LazyRenderState,
    /// The single output object tracked today. Its `wl_output` global exists
    /// with a nonzero mode for the whole life of the compositor; multi-output
    /// support is left to a later phase (see `notes/multi-activity.md`).
    pub output: smithay::output::Output,
    /// Host whose dimensions currently back the single advertised output.
    /// None while the mode is still the provisional startup metrics.
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
        output_physical_size: (i32, i32),
        xwayland_enabled: bool,
        gtk3_broken_menus_workaround_enabled: bool,
        render: crate::render::LazyRenderState,
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
        let ext_data_control_state = ExtDataControlState::new::<Self, _>(&dh, None, |_| true);
        let wlr_data_control_state = WlrDataControlState::new::<Self, _>(&dh, None, |_| true);
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
        // Advertise only input devices TAWC actually has. Android touch
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

        // wp_cursor_shape_v1 turns the common cursor request into an enum
        // instead of a bitmap upload, which is exactly what maps onto an
        // Android PointerIcon. Its device dispatch needs TabletSeatHandler;
        // TAWC has no tablet seat, so the default impl is enough. The
        // returned state has no Drop impl — the global lives for the
        // lifetime of the Display.
        CursorShapeManagerState::new::<Self>(&dh);

        dh.create_global::<Self, AndroidWlegl, ()>(2, ());
        dh.create_global::<Self, ZwpTextInputManagerV3, ()>(1, ());
        // gfxstream-bridge custom Vulkan WSI. Disabled builds do not
        // advertise the protocol or link the gfxstream backend.
        #[cfg(feature = "gfxstream")]
        dh.create_global::<Self, TawcGfxstream, ()>(1, ());

        let xwayland_shell_state = XWaylandShellState::new::<Self>(&dh);
        // CLOCK_MONOTONIC is both what the feedback timestamps below are
        // taken with and what clients compare them against.
        let presentation = PresentationState::new::<Self>(&dh, libc::CLOCK_MONOTONIC as u32);

        let mut state = Self {
            display_handle: dh,
            presentation,
            loop_handle: None,
            compositor_state,
            shm_state,
            xdg_shell_state,
            output_manager_state,
            xdg_decoration_state,
            kde_decoration_state,
            fractional_scale_manager_state,
            data_device_state,
            ext_data_control_state,
            wlr_data_control_state,
            seat_state,
            seat,
            desktop: crate::desktop::DesktopRegistry::new(),
            popup_manager: PopupManager::default(),
            active_popup_grab: None,
            mouse_attached: false,
            cursor: crate::cursor::State::new(),
            pointer_location: Point::from((0.0, 0.0)),
            pointer_focus: None,
            gtk3_broken_menus_workaround: crate::gtk3_menus_workaround::State::new(
                gtk3_broken_menus_workaround_enabled,
            ),
            output_scale,
            output_logical_size: (0, 0),
            output_refresh_mhz: DEFAULT_OUTPUT_REFRESH_MHZ,
            output_physical_size: (0, 0),
            text_input_state: TextInputState::new(),
            clipboard_pull: None,
            last_announced_android_clip_ts: None,
            selection_mirrored: false,
            surface_count: 0,
            idle_since: None,
            hardware_keys_down: HashSet::new(),
            client_count: Arc::new(AtomicU32::new(0)),
            client_ids: Arc::new(Mutex::new(Vec::new())),
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
            xwayland_enabled,
            xwayland_source: None,
            xwayland_activation: None,
            xwayland_activation_source: None,
            xwayland_source_dead: false,
            xwayland_start_pending: false,
            xwayland_start_after: None,
            xwm: None,
            x11_surfaces: Vec::new(),
            xdisplay: None,
            render,
            output,
            advertised_output_host: None,
            start_time: std::time::Instant::now(),
            frame_count: 0,
            needs_render: true,
            last_rendered_toplevels: 0,
        };

        // The pointer capability has exactly one owner; at construction the
        // only reason that can be set is the GTK3 workaround.
        state.sync_pointer_capability();

        // The output global lives for the whole life of the compositor. A
        // registry with no wl_output is a state very little client code
        // handles (SDL's video init fails outright, Xwayland has no screen
        // geometry), so clients get a display from the moment they connect.
        // The mode here is provisional — the Android panel size, not the
        // eventual Activity surface — and the first host registration
        // corrects it. Toplevels are still never configured from it; that
        // waits for a real host size (`configure_toplevel_for_host`).
        state.set_output_mode(output_physical_size);
        state.output.create_global::<Self>(&state.display_handle);

        state
    }

    /// Sole owner of the seat's `wl_pointer` capability.
    ///
    /// Two independent reasons want a pointer: attached mouse hardware
    /// ([`TawcState::mouse_attached`]) and the GTK3 broken menubar
    /// workaround. Neither may touch the seat directly: smithay's
    /// `Seat::add_pointer` on a seat that already has a pointer *replaces*
    /// the `PointerHandle`, dropping focus and any live grab. Call this
    /// after changing either reason; it acts only on the 0↔1 transition.
    pub fn sync_pointer_capability(&mut self) {
        let wanted = self.mouse_attached || self.gtk3_broken_menus_workaround.enabled;
        let present = self.seat.get_pointer().is_some();
        if wanted == present {
            return;
        }
        if wanted {
            self.seat.add_pointer();
        } else {
            self.seat.remove_pointer();
            self.pointer_focus = None;
            crate::cursor::reset(self);
        }
        info!("wl_pointer capability: {}", wanted);
    }

    /// Sole owner of the advertised output's mode. Takes physical pixels,
    /// derives the logical size from the current scale, and pushes both to
    /// the `wl_output`.
    pub fn set_output_mode(&mut self, physical_px: (i32, i32)) {
        let (mut w, mut h) = physical_px;
        if w <= 0 || h <= 0 {
            error!("Invalid output size {}x{}, clamping to 1x1", w, h);
            (w, h) = (1, 1);
        }
        self.output_physical_size = (w, h);
        self.output_logical_size = self.output_scale.logical_size(w, h);
        let previous = self.output.current_mode();
        let mode = smithay::output::Mode {
            size: (w, h).into(),
            refresh: self.output_refresh_mhz as i32,
        };
        self.output.change_current_state(
            Some(mode),
            Some(smithay::utils::Transform::Normal),
            Some(self.output_scale.smithay_scale()),
            Some((0, 0).into()),
        );
        self.output.set_preferred(mode);
        // Keep exactly one mode. `change_current_state` only appends, so
        // without this the provisional startup mode (and every size a
        // rotation or split-screen resize passed through) would linger in
        // the list forever and be offered to clients that enumerate modes.
        if let Some(previous) = previous.filter(|previous| *previous != mode) {
            self.output.delete_mode(previous);
        }
    }

    /// Adopt a new advertised refresh rate and re-publish the mode.
    ///
    /// Implausible values are ignored rather than clamped: a wrong rate is
    /// worse than the previous one for the clients that trust it, and the
    /// caller (`CompositorService`) has already picked the value out of the
    /// panel's own mode list.
    pub fn set_output_refresh_mhz(&mut self, mhz: u32) {
        if !(MIN_OUTPUT_REFRESH_MHZ..=MAX_OUTPUT_REFRESH_MHZ).contains(&mhz) {
            warn!("Ignoring implausible output refresh rate: {} mHz", mhz);
            return;
        }
        if self.output_refresh_mhz == mhz {
            return;
        }
        self.output_refresh_mhz = mhz;
        // Re-publish so an existing client sees the new rate: the same
        // "only ever one mode" path a size change takes.
        self.set_output_mode(self.output_physical_size);
        info!("output refresh rate: {} mHz", mhz);
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
    /// The calloop handle (cheap Rc clone). Only callable once the event
    /// loop exists, i.e. from code running inside `event_loop::run`.
    pub fn loop_handle(&self) -> LoopHandle<'static, TawcState> {
        self.loop_handle
            .clone()
            .expect("loop_handle is set before the event loop runs")
    }

    pub fn set_input_focus(&mut self, target: Option<&WlSurface>) {
        let target_owned = target.cloned();
        let mut focus_moved = false;
        if let Some(keyboard) = self.seat.get_keyboard() {
            focus_moved = keyboard.current_focus().as_ref() != target;
            let serial = smithay::utils::SERIAL_COUNTER.next_serial();
            keyboard.set_focus(self, target_owned, serial);
        }
        let target_client = target.and_then(|surface| surface.client());
        set_data_device_focus(&self.display_handle, &self.seat, target_client);
        // Age out clipboard offers, after the handover so the one smithay
        // just gave the newly focused client is superseded too — it gets
        // re-offered under the new serial, everyone else's goes stale.
        // Only on a real move: a redundant call would otherwise be able
        // to invalidate the focused client's offer mid-paste.
        if focus_moved {
            crate::clipboard::refresh_android_selection(self);
        }
        let target_activity_id = target.and_then(|surface| self.desktop.host_for_surface(surface));
        self.text_input_state.update_focus(target, target_activity_id.as_ref());
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
        self.set_output_mode((size.w, size.h));
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
        let ids = self.client_ids.lock().unwrap().clone();
        let disconnected = ids.len();
        for id in ids {
            self.display_handle
                .backend_handle()
                .kill_client(id, DisconnectReason::ConnectionClosed);
        }

        for toplevel in self.xdg_shell_state.toplevel_surfaces() {
            if toplevel.alive() {
                toplevel.send_close();
            }
        }

        let x11_surfaces = self.x11_surfaces.clone();
        for surface in x11_surfaces {
            if let Err(e) = surface.close() {
                warn!("xwayland: failed to close window {}: {}", surface.window_id(), e);
            }
        }
        disconnected
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

    pub fn first_toplevel_for_host(&self, host_id: &ActivityId) -> Option<WlSurface> {
        self.first_wayland_toplevel_for_host(host_id)
            .or_else(|| self.desktop.first_surface_for_host(host_id))
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
        self.surface_count += 1;
        smithay::wayland::compositor::add_destruction_hook(surface, |state: &mut TawcState, _| {
            state.surface_count = state.surface_count.saturating_sub(1);
        });
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

        crate::cursor::after_commit(self, surface);
        crate::gtk3_menus_workaround::after_commit(self, surface);
    }
}

impl XdgShellHandler for TawcState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        // Run the desktop registry's assignment policy.
        // The result tells us whether to spawn a new Activity (phase 5+).
        let assignment = self.assign_toplevel_to_host(&surface);

        surface.with_pending_state(|state| {
            state.states.set(
                wayland_protocols::xdg::shell::server::xdg_toplevel::State::Activated,
            );
        });
        set_toplevel_fullscreen_state(&surface, self.host_fullscreen(&assignment.host), None);
        if self.configure_toplevel_for_host(&surface, &assignment.host).is_some() {
            surface.send_configure();
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
        crate::gtk3_menus_workaround::toplevel_destroyed(self, surface.wl_surface());
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
        image: smithay::input::pointer::CursorImageStatus,
    ) {
        crate::cursor::set_status(self, image);
    }
}

// TAWC has no zwp_tablet_v2 seat; this exists only because
// wp_cursor_shape_v1's device dispatch is shared with tablet tools.
impl TabletSeatHandler for TawcState {}

// ---------------------------------------------------------------------------
// Client state + delegate macros
// ---------------------------------------------------------------------------

pub struct ClientState {
    pub compositor_state: CompositorClientState,
    pub client_count: Arc<AtomicU32>,
    pub client_ids: Arc<Mutex<Vec<ClientId>>>,
}

impl ClientState {
    pub fn new(client_count: Arc<AtomicU32>, client_ids: Arc<Mutex<Vec<ClientId>>>) -> Self {
        Self {
            compositor_state: CompositorClientState::default(),
            client_count,
            client_ids,
        }
    }
}

impl ClientData for ClientState {
    fn initialized(&self, client_id: ClientId) {
        self.client_ids.lock().unwrap().push(client_id);
        self.client_count.fetch_add(1, Ordering::Relaxed);
    }

    fn disconnected(&self, client_id: ClientId, _reason: DisconnectReason) {
        self.client_ids.lock().unwrap().retain(|id| id != &client_id);
        self.client_count.fetch_sub(1, Ordering::Relaxed);
    }
}

impl DataDeviceHandler for TawcState {
    fn data_device_state(&mut self) -> &mut DataDeviceState {
        &mut self.data_device_state
    }
}

impl ExtDataControlHandler for TawcState {
    fn data_control_state(&mut self) -> &mut ExtDataControlState {
        &mut self.ext_data_control_state
    }
}

impl WlrDataControlHandler for TawcState {
    fn data_control_state(&mut self) -> &mut WlrDataControlState {
        &mut self.wlr_data_control_state
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
        if ty == SelectionTarget::Clipboard {
            self.selection_mirrored = false;
        }
        if let Some(xwm) = self.xwm.as_mut() {
            if let Err(e) = xwm.new_selection(ty, mime_types.clone()) {
                log::warn!("clipboard: failed to notify XWayland of Wayland selection: {:?}", e);
            }
        }

        if ty == SelectionTarget::Clipboard {
            if let Some(mime_types) = mime_types {
                crate::clipboard::queue_selection_pull(
                    crate::clipboard::PullSource::Wayland,
                    &mime_types,
                );
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
            crate::clipboard::SelectionUserData::Android(serial) => {
                if ty != SelectionTarget::Clipboard {
                    return;
                }
                // Reject an offer the client held past the focus change
                // that superseded it (see `refresh_android_selection`).
                // Dropping the fd is the existing refusal path: the
                // client sees EOF, i.e. an empty paste.
                if crate::clipboard::current_android_selection_serial(self) != Some(*serial) {
                    return;
                }
                crate::clipboard::write_android_clipboard_to_fd(fd);
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

    fn allow_data_control_read(&mut self, _ty: SelectionTarget, client: &Client) -> bool {
        crate::clipboard::allow_data_control_read(self, client)
    }
}

delegate_dispatch2!(TawcState);
