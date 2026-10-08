//! Touch and pointer input delivery from Android to Wayland clients.
//!
//! Android events arrive on the JNI thread via nativeOnTouchEvent /
//! nativeOnPointerEvent. They're sent through calloop channels to the
//! compositor thread, which delivers them as wl_touch / wl_pointer events
//! via Smithay's TouchHandle / PointerHandle.
//!
//! Touchscreen and stylus go to touch; mouse-source events go to pointer.
//! The split happens in `CompositorActivity` — see notes/input.md.

use std::sync::Mutex;

use smithay::reexports::calloop::channel;

use crate::host::ActivityId;

/// A touch event from Android, in physical pixel coordinates.
/// `activity_id` identifies which `CompositorActivity`'s SurfaceView
/// produced the touch — phase 6 uses this to route the touch to the
/// foreground toplevel of THAT host instead of the global first toplevel.
pub enum TouchEvent {
    Down { id: i32, x: f32, y: f32, time: u32, activity_id: ActivityId },
    /// Slots keep the host and window they went down on.
    Motion { id: i32, x: f32, y: f32, time: u32 },
    Up { id: i32, time: u32 },
}

/// Global sender. Replaced each time the compositor restarts.
static TOUCH_SENDER: Mutex<Option<channel::Sender<TouchEvent>>> = Mutex::new(None);

/// Create the calloop channel pair. Returns the receiver (for the event loop).
/// The sender is stored globally for JNI access.
pub fn create_touch_channel() -> channel::Channel<TouchEvent> {
    let (sender, channel) = channel::channel();
    *TOUCH_SENDER.lock().unwrap() = Some(sender);
    channel
}

pub fn clear_senders() {
    *TOUCH_SENDER.lock().unwrap() = None;
    *POINTER_SENDER.lock().unwrap() = None;
}

/// Send a touch event from JNI. No-op if the channel isn't set up yet.
pub fn send_touch_event(event: TouchEvent) {
    if let Some(sender) = TOUCH_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(event);
    }
}

// ---------------------------------------------------------------------------
// Pointer input (real mouse hardware)
// ---------------------------------------------------------------------------

/// Where a scroll frame came from. Mirrors the subset of
/// `wl_pointer.axis_source` TAWC can actually distinguish on Android.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PointerAxisSource {
    /// A mouse wheel with discrete detents.
    Wheel,
    /// A touchpad scroll gesture. Requires a terminating stop frame.
    Finger,
}

/// A pointer event from Android, already translated out of Android units.
///
/// Coordinates are physical pixels (converted to logical on the compositor
/// thread, like touch). Axis values are Wayland-facing: `dy` is positive
/// downward and `v120_*` is in 1/120ths of a detent.
pub enum PointerEvent {
    Motion { x: f32, y: f32, time: u32, activity_id: ActivityId },
    Button { code: u32, pressed: bool, time: u32, activity_id: ActivityId },
    Axis {
        dx: f64,
        dy: f64,
        v120_x: i32,
        v120_y: i32,
        source: PointerAxisSource,
        stop: bool,
        time: u32,
        activity_id: ActivityId,
    },
}

static POINTER_SENDER: Mutex<Option<channel::Sender<PointerEvent>>> = Mutex::new(None);

/// Create the pointer calloop channel pair. Same replace-on-restart shape as
/// [`create_touch_channel`].
pub fn create_pointer_channel() -> channel::Channel<PointerEvent> {
    let (sender, channel) = channel::channel();
    *POINTER_SENDER.lock().unwrap() = Some(sender);
    channel
}

/// Send a pointer event from JNI. No-op if the channel isn't set up yet.
pub fn send_pointer_event(event: PointerEvent) {
    if let Some(sender) = POINTER_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(event);
    }
}
