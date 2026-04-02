//! Touch input delivery from Android to Wayland clients.
//!
//! Android touch events arrive on the JNI thread via nativeOnTouchEvent.
//! They're sent through a calloop channel to the compositor thread, which
//! delivers them as wl_touch events via Smithay's TouchHandle.

use std::sync::Mutex;

use smithay::reexports::calloop::channel;

/// A touch event from Android, in physical pixel coordinates.
pub enum TouchEvent {
    Down { id: i32, x: f32, y: f32, time: u32 },
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

/// Send a touch event from JNI. No-op if the channel isn't set up yet.
pub fn send_touch_event(event: TouchEvent) {
    if let Some(sender) = TOUCH_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(event);
    }
}
