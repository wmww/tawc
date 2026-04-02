//! Custom zwp_text_input_v3 implementation.
//!
//! Bypasses Smithay's built-in TextInputManagerState (which requires a Wayland
//! input-method client) and implements the protocol directly. Our IME is
//! Android's Gboard communicating via InputConnection/JNI, not a Wayland client.
//!
//! Text input events from Android arrive via a calloop channel (same pattern as
//! touch in input.rs). Client requests (enable/disable) call back to Android
//! directly via reverse JNI.

use std::collections::HashMap;
use std::sync::Mutex;

use log::info;
use smithay::reexports::calloop::channel;
use smithay::reexports::wayland_server::backend::ObjectId;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};
use wayland_protocols::wp::text_input::zv3::server::{
    zwp_text_input_manager_v3::{self, ZwpTextInputManagerV3},
    zwp_text_input_v3::{self, ZwpTextInputV3},
};

use crate::compositor::TawcState;

// ---------------------------------------------------------------------------
// Android → Compositor text input events (via calloop channel)
// ---------------------------------------------------------------------------

/// Text input event from Android IME, expressed in text-input-v3 concepts.
/// The mapping from Android keycodes happens at the JNI boundary in lib.rs.
pub enum TextInputEvent {
    /// Insert finalized text (from commitText or enter/tab key).
    CommitString { text: String },
    /// Set preedit/composing text (from setComposingText).
    SetPreeditString { text: String },
    /// Clear preedit (from finishComposingText).
    ClearPreedit,
    /// Delete surrounding text (from deleteSurroundingText or backspace/forward-delete).
    DeleteSurroundingText { before: u32, after: u32 },
}

/// Global sender for text input events from JNI. Replaced on compositor restart.
static TEXT_INPUT_SENDER: Mutex<Option<channel::Sender<TextInputEvent>>> = Mutex::new(None);

/// Create the calloop channel pair. Returns the receiver for the event loop.
pub fn create_text_input_channel() -> channel::Channel<TextInputEvent> {
    let (sender, ch) = channel::channel();
    *TEXT_INPUT_SENDER.lock().unwrap() = Some(sender);
    ch
}

/// Send a text input event from JNI. No-op if the channel isn't set up yet.
pub fn send_text_input_event(event: TextInputEvent) {
    if let Some(sender) = TEXT_INPUT_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(event);
    }
}

// ---------------------------------------------------------------------------
// Compositor → Android (direct JNI calls)
// ---------------------------------------------------------------------------

fn show_keyboard() {
    info!("text_input: requesting keyboard show");
    crate::call_native_bridge_void("onShowKeyboard", "()V", &[]);
}

fn hide_keyboard() {
    info!("text_input: requesting keyboard hide");
    crate::call_native_bridge_void("onHideKeyboard", "()V", &[]);
}

// ---------------------------------------------------------------------------
// Per-instance and global state
// ---------------------------------------------------------------------------

/// Marker data stored on the ZwpTextInputV3 wayland-server resource.
/// Must be Send + Sync. Mutable per-instance state lives in TextInputState.
pub struct TextInputData;

/// Mutable per-instance state, stored in TextInputState keyed by ObjectId.
#[derive(Default)]
struct InstanceState {
    /// Number of commit requests received. Used as the serial in done() events.
    commit_count: u32,
    /// Pending enable/disable accumulated before commit.
    pending_enable: bool,
    pending_disable: bool,
}

/// State for all text input instances, stored in TawcState.
pub struct TextInputState {
    /// All active text input instances.
    instances: Vec<ZwpTextInputV3>,
    /// Per-instance mutable state, keyed by resource ObjectId.
    instance_state: HashMap<ObjectId, InstanceState>,
    /// The surface that currently has text input focus.
    pub focused_surface: Option<WlSurface>,
    /// Whether text input is currently enabled by the focused client.
    enabled: bool,
}

impl TextInputState {
    pub fn new() -> Self {
        Self {
            instances: Vec::new(),
            instance_state: HashMap::new(),
            focused_surface: None,
            enabled: false,
        }
    }

    /// Register a new text input instance.
    pub fn add_instance(&mut self, ti: ZwpTextInputV3) {
        // Send enter if we already have a focused surface for this client
        if let Some(ref surface) = self.focused_surface {
            if ti.id().same_client_as(&surface.id()) {
                ti.enter(surface);
            }
        }
        self.instance_state.insert(ti.id(), InstanceState::default());
        self.instances.push(ti);
    }

    /// Remove a text input instance.
    pub fn remove_instance(&mut self, id: &ObjectId) {
        self.instances.retain(|ti| ti.id() != *id);
        self.instance_state.remove(id);
    }

    /// Record a pending enable request for the given instance.
    pub fn set_pending_enable(&mut self, id: &ObjectId) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_enable = true;
        }
    }

    /// Record a pending disable request for the given instance.
    pub fn set_pending_disable(&mut self, id: &ObjectId) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_disable = true;
        }
    }

    /// Process a commit from the given instance: apply pending state,
    /// increment commit count, trigger keyboard show/hide.
    pub fn commit(&mut self, id: &ObjectId) {
        let state = match self.instance_state.get_mut(id) {
            Some(s) => s,
            None => return,
        };

        info!("text_input: commit (enable={}, disable={})",
            state.pending_enable, state.pending_disable);

        // Per text-input-v3 spec, enable and disable are mutually exclusive
        // within a single commit. If both are set, disable wins.
        if state.pending_disable {
            self.enabled = false;
            hide_keyboard();
        } else if state.pending_enable {
            self.enabled = true;
            show_keyboard();
        }

        state.commit_count += 1;
        state.pending_enable = false;
        state.pending_disable = false;
    }

    /// Send enter event to all text input instances belonging to the given surface's client.
    fn enter(&mut self, surface: &WlSurface) {
        self.focused_surface = Some(surface.clone());
        for ti in &self.instances {
            if ti.id().same_client_as(&surface.id()) {
                ti.enter(surface);
            }
        }
    }

    /// Send leave event and reset enabled state.
    fn leave(&mut self, surface: &WlSurface) {
        for ti in &self.instances {
            if ti.id().same_client_as(&surface.id()) {
                ti.leave(surface);
            }
        }
        if self.focused_surface.as_ref() == Some(surface) {
            self.focused_surface = None;
            self.enabled = false;
        }
    }

    /// Update text input focus to track the given surface.
    /// Sends leave/enter only when focus actually changes.
    pub fn update_focus(&mut self, new_focus: Option<&WlSurface>) {
        if self.focused_surface.as_ref() == new_focus {
            return;
        }
        if let Some(old) = self.focused_surface.clone() {
            self.leave(&old);
        }
        if let Some(new) = new_focus {
            self.enter(new);
        }
    }

    /// Handle an incoming text input event from Android and send it to the focused client.
    pub fn handle_android_event(&mut self, event: TextInputEvent) {
        if !self.enabled {
            return;
        }

        let surface = match &self.focused_surface {
            Some(s) => s,
            None => return,
        };

        for ti in &self.instances {
            if !ti.id().same_client_as(&surface.id()) {
                continue;
            }
            let serial = self.instance_state.get(&ti.id())
                .map(|s| s.commit_count)
                .unwrap_or(0);

            match &event {
                TextInputEvent::CommitString { text } => {
                    ti.preedit_string(None, 0, 0);
                    ti.commit_string(Some(text.clone()));
                }
                TextInputEvent::SetPreeditString { text } => {
                    let len = text.len() as i32;
                    ti.preedit_string(Some(text.clone()), 0, len);
                }
                TextInputEvent::ClearPreedit => {
                    ti.preedit_string(None, 0, 0);
                }
                TextInputEvent::DeleteSurroundingText { before, after } => {
                    ti.delete_surrounding_text(*before, *after);
                }
            }
            ti.done(serial);
        }
    }

    /// Clean up instances for dead resources.
    pub fn cleanup(&mut self) {
        self.instances.retain(|ti| {
            if ti.is_alive() {
                true
            } else {
                self.instance_state.remove(&ti.id());
                false
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Protocol dispatch: zwp_text_input_manager_v3
// ---------------------------------------------------------------------------

impl GlobalDispatch<ZwpTextInputManagerV3, ()> for TawcState {
    fn bind(
        _state: &mut Self,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: New<ZwpTextInputManagerV3>,
        _global_data: &(),
        data_init: &mut DataInit<'_, Self>,
    ) {
        data_init.init(resource, ());
        info!("Client bound zwp_text_input_manager_v3");
    }
}

impl Dispatch<ZwpTextInputManagerV3, ()> for TawcState {
    fn request(
        state: &mut Self,
        _client: &Client,
        _resource: &ZwpTextInputManagerV3,
        request: zwp_text_input_manager_v3::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            zwp_text_input_manager_v3::Request::GetTextInput { id, seat: _ } => {
                let ti = data_init.init(id, TextInputData);
                info!("Created zwp_text_input_v3: {:?}", ti.id());
                state.text_input_state.add_instance(ti);
            }
            zwp_text_input_manager_v3::Request::Destroy => {}
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// Protocol dispatch: zwp_text_input_v3
// ---------------------------------------------------------------------------

impl Dispatch<ZwpTextInputV3, TextInputData> for TawcState {
    fn request(
        state: &mut Self,
        _client: &Client,
        resource: &ZwpTextInputV3,
        request: zwp_text_input_v3::Request,
        _data: &TextInputData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Self>,
    ) {
        let id = resource.id();
        match request {
            zwp_text_input_v3::Request::Enable => {
                state.text_input_state.set_pending_enable(&id);
            }
            zwp_text_input_v3::Request::Disable => {
                state.text_input_state.set_pending_disable(&id);
            }
            zwp_text_input_v3::Request::SetSurroundingText { .. } => {}
            zwp_text_input_v3::Request::SetTextChangeCause { .. } => {}
            zwp_text_input_v3::Request::SetContentType { .. } => {}
            zwp_text_input_v3::Request::SetCursorRectangle { .. } => {}
            zwp_text_input_v3::Request::Commit => {
                state.text_input_state.commit(&id);
            }
            zwp_text_input_v3::Request::Destroy => {
                state.text_input_state.remove_instance(&id);
            }
            _ => {}
        }
    }
}
