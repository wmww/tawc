//! Per-Activity output host. Each `OutputHost` represents one Android
//! Activity's `SurfaceView` that the compositor renders into. For now there
//! is at most one host at a time (single-window mode); the multi-window
//! phases populate `TawcState::hosts` with more entries.
//!
//! See `notes/multi-activity.md` for the full design.

use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};

use log::info;
use smithay::backend::egl::EGLSurface;
use smithay::reexports::calloop::channel::{Channel, Sender, channel};
use smithay::utils::{Physical, Size};

use crate::scale::OutputScale;

/// Activity identifier — stable per Android task. UUID once we get to
/// per-document Activities; for now a hardcoded `"primary"` for the single
/// `CompositorActivity`. The string travels Rust→Kotlin in
/// `spawnActivity`/`finishActivity` reverse-JNI calls and Kotlin→Rust in
/// every per-activity surface/input JNI call.
pub type ActivityId = String;

/// State owned by one Android Activity's `SurfaceView`.
///
/// Created when the Activity registers its `Surface`
/// (`nativeRegisterActivitySurface`); the `EGLSurface` is bound to
/// `native_window`. Dropped on surface destroy / Activity destroy. The
/// `ANativeWindow` reference is owned and released in `Drop`.
pub struct OutputHost {
    pub activity_id: ActivityId,

    /// Owned ANativeWindow pointer. Released by `Drop`.
    /// `None` after the Surface has been destroyed but the host record
    /// is still alive (e.g. Activity backgrounded but not yet destroyed).
    native_window: *mut c_void,

    /// EGLSurface bound to `native_window`. `None` while waiting for
    /// the Surface to arrive (Activity spawned but `surfaceCreated`
    /// hasn't fired yet) or after `surfaceDestroyed`.
    pub egl_surface: Option<EGLSurface>,

    /// Physical dimensions of the SurfaceView in pixels.
    pub physical_size: Size<i32, Physical>,

    /// Logical dimensions sent to clients in `xdg_toplevel.configure`
    /// (= physical_size / output_scale, rounded to logical pixels).
    pub logical_size: (i32, i32),

    /// True iff Android currently shows this Activity in the foreground.
    /// Drives `Activated`/`Suspended` configure events on assigned
    /// toplevels and gates per-frame work (rendering, frame callbacks)
    /// so backgrounded clients stop drawing. Updated by
    /// `SurfaceEvent::FocusChanged`. Defaults to `false` — the FocusChanged
    /// event arrives within ms of `Register` and flips it on for hosts
    /// Android has actually given focus.
    pub foreground: bool,

    /// Whether this Android Activity is in immersive fullscreen. This is
    /// mirrored into xdg_toplevel state: false means Maximized, true means
    /// Fullscreen.
    pub fullscreen: bool,
}

// SAFETY: `native_window` is a raw `ANativeWindow*` (refcount is
// thread-safe per the NDK contract). `EGLSurface` and `EGLContext` are
// !Send by default but only ever touched from the compositor thread —
// calloop requires `TawcState: Send` even though it never moves data
// across threads. Mirrors `RenderState`'s `Send` impl.
unsafe impl Send for OutputHost {}

impl OutputHost {
    /// Take ownership of an `ANativeWindow` reference (caller must have
    /// done `ANativeWindow_acquire`-equivalent — `ANativeWindow_fromSurface`
    /// returns a +1 ref).
    pub fn new(activity_id: ActivityId, native_window: *mut c_void, w: i32, h: i32, scale: OutputScale) -> Self {
        Self {
            activity_id,
            native_window,
            egl_surface: None,
            physical_size: Size::from((w, h)),
            logical_size: scale.logical_size(w, h),
            foreground: false,
            fullscreen: false,
        }
    }

    pub fn native_window(&self) -> *mut c_void {
        self.native_window
    }

    /// Replace the Surface (e.g. after `surfaceDestroyed` →
    /// `surfaceCreated` round-trip). Drops the old EGLSurface and
    /// releases the old ANativeWindow before taking the new one.
    pub fn replace_native_window(&mut self, native_window: *mut c_void, w: i32, h: i32, scale: OutputScale) {
        self.egl_surface = None;
        if !self.native_window.is_null() {
            unsafe { ndk_sys::ANativeWindow_release(self.native_window as *mut _) };
        }
        self.native_window = native_window;
        self.physical_size = Size::from((w, h));
        self.logical_size = scale.logical_size(w, h);
    }

    /// Forget the Surface (Activity backgrounded / surface destroyed).
    /// Keeps the host record alive so the Activity coming back can
    /// rebind without losing its assigned toplevels.
    pub fn drop_surface(&mut self) {
        self.egl_surface = None;
        if !self.native_window.is_null() {
            unsafe { ndk_sys::ANativeWindow_release(self.native_window as *mut _) };
            self.native_window = std::ptr::null_mut();
        }
    }

    pub fn update_size(&mut self, w: i32, h: i32, scale: OutputScale) {
        self.physical_size = Size::from((w, h));
        self.logical_size = scale.logical_size(w, h);
    }

    pub fn update_scale(&mut self, scale: OutputScale) {
        self.logical_size = scale.logical_size(self.physical_size.w, self.physical_size.h);
    }
}

impl Drop for OutputHost {
    fn drop(&mut self) {
        // Drop EGLSurface first (eglDestroySurface) so the underlying
        // ANativeWindow refcount is back to 1 before we release.
        self.egl_surface = None;
        if !self.native_window.is_null() {
            unsafe { ndk_sys::ANativeWindow_release(self.native_window as *mut _) };
            self.native_window = std::ptr::null_mut();
        }
        info!("OutputHost dropped: {}", self.activity_id);
    }
}

// ---------------------------------------------------------------------------
// Surface event channel (Android JNI → compositor thread)
// ---------------------------------------------------------------------------

/// Lifecycle events sent from the JNI thread to the compositor thread.
/// All Activity-side surface management goes through this channel so the
/// compositor thread is the only owner of `OutputHost` records.
pub enum SurfaceEvent {
    /// `surfaceCreated` (or `surfaceChanged` when a host already exists with
    /// no surface). Carries an owned `ANativeWindow*` (refcount +1 from
    /// `ANativeWindow_fromSurface`); the compositor takes ownership.
    Register {
        activity_id: ActivityId,
        native_window: usize,    // *mut c_void as usize for Send-safety
        width: i32,
        height: i32,
    },
    /// `surfaceChanged` for an already-bound host: dimensions changed.
    /// `EGLSurface` does not need to be recreated — `eglSwapBuffers` picks up
    /// new geometry from `ANativeWindow_setBuffersGeometry` automatically.
    SurfaceChanged {
        activity_id: ActivityId,
        width: i32,
        height: i32,
    },
    /// `surfaceDestroyed`. Drop the EGLSurface and ANativeWindow but keep
    /// the host record alive (Activity still exists, may rebind).
    SurfaceDestroyed { activity_id: ActivityId },
    /// Activity `onDestroy`. Remove the host record entirely. Toplevels
    /// assigned to this host become orphaned (handled by the policy layer
    /// in later phases — for phase 0/1 this is single-Activity so the
    /// compositor effectively goes idle).
    ActivityDestroyed { activity_id: ActivityId },
    /// `onWindowFocusChanged`. The Activity's window has gained or lost
    /// the system focus. The compositor uses this to track which host is
    /// `foreground_host` for input routing (touch fallback, keyboard
    /// focus). Phase 7 extends this to drive `Activated`/`Suspended`
    /// configures.
    FocusChanged { activity_id: ActivityId, has_focus: bool },
    /// Runtime output scale change from Settings / test broker.
    OutputScaleChanged { scale: f64 },
    /// Runtime toggle for the contained GTK3 broken menubar workaround.
    Gtk3BrokenMenusWorkaroundChanged { enabled: bool },
    /// Android-side fullscreen state changed outside an xdg request.
    FullscreenChanged { activity_id: ActivityId, fullscreen: bool },
}

// SAFETY: `native_window` is stored as `usize` to keep `SurfaceEvent: Send`;
// `ANativeWindow` itself is reference-counted and the refcount ops are
// thread-safe per the NDK contract.

static SURFACE_EVENT_SENDER: OnceLock<Mutex<Option<Sender<SurfaceEvent>>>> = OnceLock::new();

/// Replace the sender used by `send_surface_event`. Called once per
/// compositor startup from `event_loop::run`.
pub fn set_surface_event_sender(sender: Sender<SurfaceEvent>) {
    let cell = SURFACE_EVENT_SENDER.get_or_init(|| Mutex::new(None));
    *cell.lock().unwrap() = Some(sender);
}

/// Drop the sender. Called when the event loop exits.
pub fn clear_surface_event_sender() {
    if let Some(cell) = SURFACE_EVENT_SENDER.get() {
        *cell.lock().unwrap() = None;
    }
}

/// Send a surface event from the JNI thread. Drops the event silently if
/// no compositor is currently running (e.g. between `nativeStopCompositor`
/// and `nativeStartCompositor`).
pub fn send_surface_event(event: SurfaceEvent) {
    if let Some(cell) = SURFACE_EVENT_SENDER.get() {
        if let Some(sender) = cell.lock().unwrap().as_ref() {
            let _ = sender.send(event);
        }
    }
}

/// Build a fresh `(Sender, Channel)` for surface events. The sender is
/// installed via `set_surface_event_sender`; the channel is plugged into
/// the calloop loop.
pub fn create_surface_event_channel() -> Channel<SurfaceEvent> {
    let (sender, channel) = channel();
    set_surface_event_sender(sender);
    channel
}

/// Generate a fresh ActivityId for a new host. Phase 5+ uses this to mint
/// a UUID that travels through `spawnActivity` → Intent.data →
/// `nativeRegisterActivitySurface`. We avoid pulling in the `uuid` crate;
/// the value just needs to be globally unique within a compositor run.
pub fn new_activity_id() -> ActivityId {
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    // Mix in epoch nanos so ids stay unique across compositor restarts
    // (Activities surviving in the recents list from a previous run
    // would otherwise collide with new ids).
    let epoch_nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    format!("a-{:x}-{:x}", epoch_nanos, n)
}
