//! Custom zwp_text_input_v3 implementation.
//!
//! Bridges Android's InputConnection API and Wayland's text-input-v3 protocol.
//! Both are built around the same concepts (composing text, committing text,
//! deleting surrounding text, content type hints), but differ in details:
//!
//! - Android uses UTF-16 code units for character counts; Wayland uses UTF-8
//!   byte offsets. The compositor converts between them using the client's
//!   stored surrounding text.
//! - Android batches operations via beginBatchEdit/endBatchEdit; Wayland
//!   batches via done events.
//! - Android's IME queries editor state (getTextBeforeCursor etc.); Wayland
//!   clients push state (set_surrounding_text). The compositor mirrors the
//!   client's surrounding text up to Android's BaseInputConnection Editable
//!   so Gboard's queries return the truth.
//!
//! Bypasses Smithay's built-in TextInputManagerState which assumes a Wayland
//! input-method client. Our IME is Android's Gboard via InputConnection/JNI.

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

pub enum TextInputEvent {
    /// Insert finalized text (from commitText or tab key). Replaces any
    /// active preedit on the client side per the protocol's done ordering.
    /// If the client-side text under the soon-to-be-replaced composing
    /// region was committed text (Gboard's `setComposingRegion` flow,
    /// not a previous `setComposingText`), `delete_before`/`delete_after`
    /// are non-zero and identify the bytes the client should drop before
    /// the new commit — measured in UTF-16 code units around the cursor.
    CommitString { text: String, delete_before: u32, delete_after: u32 },
    /// Set preedit/composing text (from setComposingText). Replaces the
    /// previous preedit; cursor lives at the end of the preedit.
    /// `delete_before`/`delete_after` carry the same meaning as for
    /// `CommitString` — when Gboard marks already-committed text as
    /// composing (`setComposingRegion`) and then `setComposingText`
    /// replaces it, the original text must be deleted from the client's
    /// committed buffer before the new preedit is shown.
    SetPreeditString { text: String, delete_before: u32, delete_after: u32 },
    /// Finalize current preedit (from finishComposingText): commit it as
    /// final text and clear the preedit. Without this, finishComposingText
    /// would discard the composing text.
    FinishComposingText,
    /// Delete surrounding text (from Android IME's deleteSurroundingText).
    /// before/after are in Android's UTF-16 code units, NOT bytes or chars —
    /// the compositor converts using the client's stored surrounding text.
    DeleteSurroundingText { before: u32, after: u32 },
    /// Send an actual wl_keyboard key event (evdev keycode).
    /// Used for Enter, Backspace, Delete, etc. — keys that should be real
    /// key events rather than text-input-v3 operations.
    KeyPress { keycode: u32 },
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
// Compositor → Android (reverse JNI calls)
// ---------------------------------------------------------------------------

fn show_keyboard() {
    info!("text_input: requesting keyboard show");
    crate::call_native_bridge_void("onShowKeyboard", "()V", &[]);
}

fn hide_keyboard() {
    info!("text_input: requesting keyboard hide");
    crate::call_native_bridge_void("onHideKeyboard", "()V", &[]);
}

/// Notify Android IME (via InputMethodManager.updateSelection) that the
/// editor's selection/cursor has changed. Critical after non-IME cursor
/// movement so Gboard can reset its internal model.
fn update_selection(sel_start: i32, sel_end: i32) {
    crate::call_native_bridge_void(
        "onUpdateSelection",
        "(IIII)V",
        &[
            jni::objects::JValue::Int(sel_start),
            jni::objects::JValue::Int(sel_end),
            jni::objects::JValue::Int(-1), // no composing span
            jni::objects::JValue::Int(-1),
        ],
    );
}

/// Push the Wayland client's text + selection up to the Android side so
/// the active TawcInputConnection's Editable matches the editor's truth.
/// `sel_start`/`sel_end` are UTF-16 code-unit offsets within `text`.
fn update_editable_text(text: &str, sel_start: i32, sel_end: i32) {
    crate::update_editable_text(text, sel_start, sel_end);
}

// ---------------------------------------------------------------------------
// State types
// ---------------------------------------------------------------------------

/// Marker data stored on the ZwpTextInputV3 wayland-server resource.
pub struct TextInputData;

/// Surrounding text reported by a Wayland client via set_surrounding_text.
/// All offsets are UTF-8 byte positions within `text`.
#[derive(Clone)]
struct SurroundingText {
    text: String,
    cursor: usize,
    anchor: usize,
}

/// Why the surrounding text changed, per the protocol's change_cause enum.
#[derive(Clone, Copy, PartialEq, Default)]
enum ChangeCause {
    /// The input method (compositor/IME) caused the change.
    #[default]
    InputMethod,
    /// Something else (user click, arrow keys, etc.) caused the change.
    Other,
}

/// Per-instance state for a zwp_text_input_v3 object.
/// State is double-buffered: pending fields are applied atomically on commit.
#[derive(Default)]
struct InstanceState {
    commit_count: u32,

    // Pending state (applied on next commit)
    pending_enable: bool,
    pending_disable: bool,
    pending_surrounding: Option<SurroundingText>,
    pending_change_cause: ChangeCause,

    // Current (committed) state
    enabled: bool,
    surrounding: Option<SurroundingText>,

    /// Last preedit text we sent to this client. Needed so finishComposingText
    /// can commit the composing text rather than discarding it.
    current_preedit: Option<String>,
}

/// Global text input state, stored in TawcState.
pub struct TextInputState {
    /// All active text input instances.
    instances: Vec<ZwpTextInputV3>,
    /// Per-instance mutable state, keyed by resource ObjectId.
    instance_state: HashMap<ObjectId, InstanceState>,
    /// The surface that currently has text input focus.
    pub focused_surface: Option<WlSurface>,
    /// Whether the Android keyboard is currently shown.
    /// Tracked to avoid redundant show/hide calls and handle rapid toggling.
    keyboard_visible: bool,
}

impl TextInputState {
    pub fn new() -> Self {
        Self {
            instances: Vec::new(),
            instance_state: HashMap::new(),
            focused_surface: None,
            keyboard_visible: false,
        }
    }

    // --- Instance management ---

    pub fn add_instance(&mut self, ti: ZwpTextInputV3) {
        if let Some(ref surface) = self.focused_surface {
            if ti.id().same_client_as(&surface.id()) {
                ti.enter(surface);
            }
        }
        self.instance_state.insert(ti.id(), InstanceState::default());
        self.instances.push(ti);
    }

    pub fn remove_instance(&mut self, id: &ObjectId) {
        self.instances.retain(|ti| ti.id() != *id);
        self.instance_state.remove(id);
        self.sync_keyboard_visibility();
    }

    // --- Client request handlers (called from protocol dispatch) ---

    pub fn set_pending_enable(&mut self, id: &ObjectId) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_enable = true;
        }
    }

    pub fn set_pending_disable(&mut self, id: &ObjectId) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_disable = true;
        }
    }

    pub fn set_pending_surrounding(&mut self, id: &ObjectId, text: String, cursor: i32, anchor: i32) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_surrounding = Some(SurroundingText {
                text,
                cursor: cursor.max(0) as usize,
                anchor: anchor.max(0) as usize,
            });
        }
    }

    pub fn set_pending_change_cause(&mut self, id: &ObjectId, cause: u32) {
        if let Some(state) = self.instance_state.get_mut(id) {
            // Protocol enum: 0 = input_method, 1 = other
            state.pending_change_cause = if cause == 1 {
                ChangeCause::Other
            } else {
                ChangeCause::InputMethod
            };
        }
    }

    /// Process a commit from the client. Atomically applies all pending state.
    pub fn commit(&mut self, id: &ObjectId) {
        // Look up the resource up-front so we can emit events outside the
        // mutable borrow on `instance_state` below. Cloning a resource is
        // cheap (refcounted handle).
        let ti = self.instances.iter().find(|t| t.id() == *id).cloned();

        let state = match self.instance_state.get_mut(id) {
            Some(s) => s,
            None => return,
        };

        state.commit_count += 1;

        // Apply enable/disable. Per spec, if both are pending, disable wins.
        // enable resets all state from the previous enable/disable cycle.
        let mut just_enabled = false;
        if state.pending_disable {
            info!("text_input: commit disable");
            state.enabled = false;
            state.surrounding = None;
            state.current_preedit = None;
        } else if state.pending_enable {
            info!("text_input: commit enable");
            state.enabled = true;
            // Per spec: enable resets all associated state.
            state.surrounding = None;
            state.current_preedit = None;
            just_enabled = true;
        }

        // Apply pending surrounding text.
        let change_cause = state.pending_change_cause;
        let mut sync_to_android: Option<(String, i32, i32, bool)> = None;
        // Whether to send a preedit-clearing done event to the client
        // after we drop the borrow on `state`. Set when the client moved
        // the cursor outside IME control while we had an active preedit:
        // GTK keeps rendering preedit at the cursor, so without this,
        // the user's typed-but-not-committed word visually follows the
        // cursor. Clearing matches what every desktop text widget does
        // on click during composition (the in-progress word is dropped).
        let mut clear_client_preedit = false;
        if let Some(surrounding) = state.pending_surrounding.take() {
            let sel_start = byte_offset_to_utf16_count(&surrounding.text, surrounding.cursor) as i32;
            let sel_end = byte_offset_to_utf16_count(&surrounding.text, surrounding.anchor) as i32;
            let need_imm_update = change_cause == ChangeCause::Other && state.enabled;

            if change_cause == ChangeCause::Other {
                // Drop our compositor-side preedit tracking so a defensive
                // `finishComposingText` from the IME doesn't re-commit the
                // preedit at the new cursor (the "words reappear" bug).
                if state.current_preedit.take().is_some() {
                    // We had a preedit that the client may still be
                    // rendering; tell it to clear so it doesn't shadow
                    // the cursor at its new position.
                    clear_client_preedit = true;
                }
            }

            sync_to_android = Some((surrounding.text.clone(), sel_start, sel_end, need_imm_update));
            state.surrounding = Some(surrounding);
        } else if just_enabled {
            // Client enabled without first reporting surrounding text; treat
            // the editor as empty for Android-side mirroring purposes.
            sync_to_android = Some((String::new(), 0, 0, false));
        }

        // Reset pending state.
        state.pending_enable = false;
        state.pending_disable = false;
        state.pending_change_cause = ChangeCause::default();
        let enabled_now = state.enabled;
        let commit_count = state.commit_count;

        // End of borrow on `state`; do JNI calls outside the &mut.
        if enabled_now {
            if let Some((text, sel_start, sel_end, need_imm_update)) = sync_to_android {
                // Always mirror the Wayland client's text into the Android
                // Editable so Gboard's queries match the editor's truth.
                update_editable_text(&text, sel_start, sel_end);
                if need_imm_update {
                    // Cursor moved by user touch / arrow keys / etc. — also
                    // poke IMM so Gboard resets its internal composing model.
                    update_selection(sel_start, sel_end);
                }
            }
        }

        // Tell the client to drop its preedit if the cursor moved
        // out-of-band while we had one tracked. Done order means the
        // existing preedit is replaced by the cursor on the next done
        // (step 1) and nothing is inserted afterwards.
        if clear_client_preedit {
            if let Some(ti) = ti {
                ti.preedit_string(None, 0, 0);
                ti.done(commit_count);
            }
        }

        // Update keyboard visibility based on whether any instance is enabled.
        self.sync_keyboard_visibility();
    }

    /// Show/hide Android keyboard to match the current enabled state.
    /// Avoids redundant calls and handles rapid disable+enable gracefully.
    fn sync_keyboard_visibility(&mut self) {
        let any_enabled = self.instance_state.values().any(|s| s.enabled);
        if any_enabled != self.keyboard_visible {
            if any_enabled {
                show_keyboard();
            } else {
                hide_keyboard();
            }
            self.keyboard_visible = any_enabled;
        }
    }

    // --- Focus management ---

    fn enter(&mut self, surface: &WlSurface) {
        self.focused_surface = Some(surface.clone());
        for ti in &self.instances {
            if ti.id().same_client_as(&surface.id()) {
                ti.enter(surface);
            }
        }
    }

    fn leave(&mut self, surface: &WlSurface) {
        // Per spec, leave invalidates all per-instance state — including
        // any preedit we have outstanding. Without this finalize the user's
        // typed-but-not-committed word would silently vanish on focus
        // change. handle_android_event keys off `focused_surface`, which is
        // still the leaving surface at this point, so it targets the right
        // instances. No-op when no preedit is active.
        self.handle_android_event(TextInputEvent::FinishComposingText);

        for ti in &self.instances {
            if ti.id().same_client_as(&surface.id()) {
                ti.leave(surface);
            }
        }
        if self.focused_surface.as_ref() == Some(surface) {
            self.focused_surface = None;
        }
    }

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

    // --- Android IME event handling ---

    /// Handle an incoming text input event from Android and send protocol
    /// events to the focused Wayland client.
    pub fn handle_android_event(&mut self, event: TextInputEvent) {
        let surface = match &self.focused_surface {
            Some(s) => s,
            None => return,
        };

        for ti in &self.instances {
            if !ti.id().same_client_as(&surface.id()) {
                continue;
            }

            let inst = match self.instance_state.get_mut(&ti.id()) {
                Some(s) => s,
                None => continue,
            };

            if !inst.enabled {
                continue;
            }

            match &event {
                TextInputEvent::CommitString { text, delete_before, delete_after } => {
                    // If the client's current cursor sits inside committed
                    // text that Gboard has marked as composing (via
                    // setComposingRegion), the IME is asking us to *replace*
                    // that text with this commit. The replacement requires
                    // an explicit delete_surrounding_text — Wayland's preedit
                    // model doesn't carry "this committed text is composing".
                    let (before_bytes, after_bytes) = utf16_units_to_bytes(
                        inst.surrounding.as_ref(),
                        *delete_before,
                        *delete_after,
                    );
                    if before_bytes > 0 || after_bytes > 0 {
                        ti.delete_surrounding_text(before_bytes, after_bytes);
                    }
                    // Per done ordering: existing preedit is replaced by the
                    // cursor (step 1), surrounding text is deleted relative
                    // to that cursor (step 2), then the commit string is
                    // inserted (step 3). Sending preedit_string(None) is
                    // explicit and idempotent — clients that miss the
                    // automatic step-1 still get the right outcome.
                    ti.preedit_string(None, 0, 0);
                    ti.commit_string(Some(text.clone()));
                    inst.current_preedit = None;
                }
                TextInputEvent::SetPreeditString { text, delete_before, delete_after } => {
                    // Same delete-then-replace pattern as CommitString, but
                    // the new content lands as preedit, not committed text.
                    let (before_bytes, after_bytes) = utf16_units_to_bytes(
                        inst.surrounding.as_ref(),
                        *delete_before,
                        *delete_after,
                    );
                    if before_bytes > 0 || after_bytes > 0 {
                        ti.delete_surrounding_text(before_bytes, after_bytes);
                    }
                    if text.is_empty() {
                        // Empty composing text means "clear preedit" in the
                        // Android model. Map it to preedit_string(None).
                        ti.preedit_string(None, 0, 0);
                        inst.current_preedit = None;
                    } else {
                        // Caret at end of preedit (cursor_begin == cursor_end == byte_len).
                        let len = text.len() as i32;
                        ti.preedit_string(Some(text.clone()), len, len);
                        inst.current_preedit = Some(text.clone());
                    }
                }
                TextInputEvent::FinishComposingText => {
                    // Finalize current composing text: commit it as final,
                    // then clear the preedit. Done ordering means the client
                    // first removes the existing preedit, then inserts the
                    // commit string in its place — net effect: preedit becomes
                    // permanent text.
                    //
                    // current_preedit is cleared whenever the client
                    // independently finalizes its preedit (cursor moved
                    // by touch, etc.), so a stale finishComposingText after
                    // a click no-ops here instead of duplicating text.
                    if let Some(preedit) = inst.current_preedit.take() {
                        if !preedit.is_empty() {
                            ti.commit_string(Some(preedit));
                        }
                    }
                    ti.preedit_string(None, 0, 0);
                }
                TextInputEvent::DeleteSurroundingText { before, after } => {
                    // Android sends UTF-16 code unit counts. Wayland needs
                    // UTF-8 byte counts. Use the client's last reported
                    // surrounding text to convert exactly.
                    let (before_bytes, after_bytes) = utf16_units_to_bytes(
                        inst.surrounding.as_ref(),
                        *before,
                        *after,
                    );
                    if before_bytes == 0 && after_bytes == 0 {
                        // Nothing to delete; skip the round-trip entirely.
                        continue;
                    }
                    // Per done ordering: existing preedit is replaced by
                    // cursor (step 1), then surrounding text is deleted
                    // relative to that cursor (step 2). We don't touch the
                    // preedit here, so any active preedit is cleared as a
                    // side-effect. That matches Android semantics, where
                    // deleteSurroundingText typically applies after
                    // finishing or replacing the composing region.
                    ti.preedit_string(None, 0, 0);
                    ti.delete_surrounding_text(before_bytes, after_bytes);
                    inst.current_preedit = None;
                }
                TextInputEvent::KeyPress { .. } => {
                    unreachable!("KeyPress handled in event_loop.rs")
                }
            }

            ti.done(inst.commit_count);
        }
    }

    /// Clean up instances for dead resources.
    pub fn cleanup(&mut self) {
        let dead_ids: Vec<_> = self.instances.iter()
            .filter(|ti| !ti.is_alive())
            .map(|ti| ti.id())
            .collect();
        if dead_ids.is_empty() {
            return;
        }
        for id in &dead_ids {
            self.instance_state.remove(id);
        }
        self.instances.retain(|ti| ti.is_alive());
        self.sync_keyboard_visibility();
    }
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/// Convert a UTF-8 byte offset within `text` to a UTF-16 code-unit count
/// (the unit Android uses for cursor and selection offsets). Clamps to text
/// length if offset is out of bounds.
fn byte_offset_to_utf16_count(text: &str, byte_offset: usize) -> usize {
    let clamped = byte_offset.min(text.len());
    text[..clamped].chars().map(|c| c.len_utf16()).sum()
}

/// Convert Android UTF-16 code-unit counts (before/after cursor) to UTF-8
/// byte counts, using the client's stored surrounding text. Falls back to
/// 1:1 mapping (ASCII assumption) if no surrounding text is available.
///
/// Critical for non-BMP characters (emoji, etc.): a single emoji is 1 Rust
/// `char` but 2 UTF-16 code units and 4 UTF-8 bytes. Android's
/// `deleteSurroundingText(2, 0)` for an emoji must become `(4, 0)`.
fn utf16_units_to_bytes(
    surrounding: Option<&SurroundingText>,
    before_units: u32,
    after_units: u32,
) -> (u32, u32) {
    let st = match surrounding {
        Some(st) => st,
        None => {
            // No surrounding text from client; assume 1 byte == 1 UTF-16 unit
            // (true for ASCII). Wrong for non-ASCII but we have nothing to
            // measure against — Android IMEs typically only send delete after
            // we've reported surrounding text anyway.
            info!("utf16_units_to_bytes: no surrounding text, assuming ASCII");
            return (before_units, after_units);
        }
    };
    let cursor = st.cursor.min(st.text.len());

    let mut before_remaining = before_units as usize;
    let mut before_bytes: usize = 0;
    for c in st.text[..cursor].chars().rev() {
        let units = c.len_utf16();
        if before_remaining < units {
            break;
        }
        before_remaining -= units;
        before_bytes += c.len_utf8();
        if before_remaining == 0 {
            break;
        }
    }

    let mut after_remaining = after_units as usize;
    let mut after_bytes: usize = 0;
    for c in st.text[cursor..].chars() {
        let units = c.len_utf16();
        if after_remaining < units {
            break;
        }
        after_remaining -= units;
        after_bytes += c.len_utf8();
        if after_remaining == 0 {
            break;
        }
    }

    (before_bytes as u32, after_bytes as u32)
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
            zwp_text_input_v3::Request::SetSurroundingText { text, cursor, anchor } => {
                state.text_input_state.set_pending_surrounding(&id, text, cursor, anchor);
            }
            zwp_text_input_v3::Request::SetTextChangeCause { cause } => {
                state.text_input_state.set_pending_change_cause(&id, cause.into());
            }
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
