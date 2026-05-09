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

use log::{debug, info};
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
    /// `delete_before`/`delete_after` are non-zero when the IME is replacing
    /// a `setComposingRegion`-marked region (committed text on Wayland);
    /// the protocol's atomic done-ordering combines the delete and commit
    /// in one round-trip, which the client treats as IME-initiated rather
    /// than user-typing — important so the client doesn't fire
    /// `set_surrounding_text(cause=other)` between the delete and commit
    /// and trip our preedit-clearing logic.
    CommitString { text: String, delete_before: u32, delete_after: u32 },
    /// Set preedit/composing text (from setComposingText). Same atomic
    /// delete-then-replace contract as [CommitString].
    SetPreeditString { text: String, delete_before: u32, delete_after: u32 },
    /// Finalize current preedit (from finishComposingText): commit it as
    /// final text and clear the preedit. Without this, finishComposingText
    /// would discard the composing text.
    FinishComposingText,
    /// Send an actual wl_keyboard key event (evdev keycode).
    /// Used for Enter, Backspace, Delete, etc. — keys that should be real
    /// key events rather than text-input-v3 operations. Also covers the
    /// IC's translation of standalone `deleteSurroundingText` into
    /// Backspace presses.
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

/// Push a new `EditorInfo.inputType` (and a small set of `imeOptions`
/// flags the compositor wants OR'd in) to Android. The Android side
/// caches the values and calls `InputMethodManager.restartInput` so the
/// next `onCreateInputConnection` builds an `EditorInfo` carrying them.
fn push_input_type_to_android(input_type: i32, ime_flags: i32) {
    debug!(
        "text_input: pushing inputType=0x{:x} imeFlags=0x{:x}",
        input_type, ime_flags
    );
    crate::call_native_bridge_void(
        "onContentTypeChanged",
        "(II)V",
        &[
            jni::objects::JValue::Int(input_type),
            jni::objects::JValue::Int(ime_flags),
        ],
    );
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
    pending_content_hint: u32,
    pending_content_purpose: u32,

    // Current (committed) state
    enabled: bool,
    surrounding: Option<SurroundingText>,
    /// Most recently committed (hint, purpose). Defaults are (0, normal).
    /// Reset on enable per the protocol.
    content_hint: u32,
    content_purpose: u32,

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
    /// Last `(input_type, ime_flags)` pushed to Android via
    /// `onContentTypeChanged`. Used to dedupe — `restartInput` tears the
    /// IME's IC down and rebuilds it, so we only call it on real changes.
    last_pushed_input_type: Option<(i32, i32)>,
}

impl TextInputState {
    pub fn new() -> Self {
        Self {
            instances: Vec::new(),
            instance_state: HashMap::new(),
            focused_surface: None,
            keyboard_visible: false,
            last_pushed_input_type: None,
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

    pub fn set_pending_content_type(&mut self, id: &ObjectId, hint: u32, purpose: u32) {
        if let Some(state) = self.instance_state.get_mut(id) {
            state.pending_content_hint = hint;
            state.pending_content_purpose = purpose;
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
            state.pending_content_hint = 0;
            state.pending_content_purpose = 0;
        } else if state.pending_enable {
            info!("text_input: commit enable");
            state.enabled = true;
            // Per spec: enable resets all associated state.
            state.surrounding = None;
            state.current_preedit = None;
            state.content_hint = 0;
            state.content_purpose = 0;
            just_enabled = true;
        }

        // Apply pending content type (defaults to (0, normal) if the
        // client didn't set it this cycle — exactly what enable's reset
        // above produced).
        state.content_hint = state.pending_content_hint;
        state.content_purpose = state.pending_content_purpose;

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
        state.pending_content_hint = 0;
        state.pending_content_purpose = 0;
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

        // Push EditorInfo to Android (deduped). Drives the IME's keyboard
        // layout — URL bars get the URL keyboard, etc.
        self.push_focused_input_type_if_changed();

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

    /// Compute the Android EditorInfo for whichever instance is focused
    /// and enabled, and push it (deduped). No-op when no focused instance
    /// is enabled — we leave the last value cached so the next time the
    /// IME comes up it sees something sensible.
    fn push_focused_input_type_if_changed(&mut self) {
        let focused_client = match &self.focused_surface {
            Some(s) => s.id(),
            None => return,
        };
        let resolved = self.instances.iter().find_map(|ti| {
            if !ti.id().same_client_as(&focused_client) {
                return None;
            }
            let inst = self.instance_state.get(&ti.id())?;
            if !inst.enabled {
                return None;
            }
            Some(wayland_content_to_android_input_type(inst.content_hint, inst.content_purpose))
        });
        if let Some(t) = resolved {
            if self.last_pushed_input_type != Some(t) {
                push_input_type_to_android(t.0, t.1);
                self.last_pushed_input_type = Some(t);
            }
        }
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
        // No content-type push here: the protocol requires the client to
        // re-enable + re-set state after enter, so any push at this point
        // would race the client's still-pending requests. The next commit
        // on the freshly-enabled instance does the push.
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
            if !ti.id().same_client_as(&surface.id()) {
                continue;
            }
            ti.leave(surface);
            // Spec: leave invalidates per-instance state. Mirror that
            // server-side so a re-enter doesn't see stale `enabled` /
            // surrounding / content_type from before the focus change.
            // Preserve `commit_count` — it's the protocol's monotonic
            // `done(serial)` source, not per-cycle state.
            if let Some(inst) = self.instance_state.get_mut(&ti.id()) {
                inst.enabled = false;
                inst.surrounding = None;
                inst.content_hint = 0;
                inst.content_purpose = 0;
                inst.current_preedit = None;
                inst.pending_enable = false;
                inst.pending_disable = false;
                inst.pending_surrounding = None;
                inst.pending_change_cause = ChangeCause::default();
                inst.pending_content_hint = 0;
                inst.pending_content_purpose = 0;
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
                    // If the IME is replacing a setComposingRegion span (still
                    // committed text on Wayland), we get non-zero deltas.
                    // Apply them as `delete_surrounding_text` in the SAME
                    // done() cycle as the commit_string — the protocol's
                    // atomic delete-then-insert is what keeps the client from
                    // firing a `set_surrounding_text(cause=other)` between
                    // the delete and the commit (which would land in our
                    // preedit-clearing logic and undo the IME's commit).
                    //
                    // Standalone deleteSurroundingText (Gboard's plain
                    // backspace) does NOT come through here — the IC
                    // translates it to wl_keyboard Backspaces at the source,
                    // see TawcInputConnection.deleteSurroundingText.
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
                    // Same atomic delete-then-replace as CommitString, but
                    // the new content lands as preedit overlay rather than
                    // committed text.
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

/// Map a text-input-v3 `(content_hint, content_purpose)` pair to an
/// Android `(EditorInfo.inputType, imeOptions-flags)` pair.
///
/// `inputType` follows Android's class+variation+flags layout (see
/// `android.text.InputType` constants). `ime_flags` is just the subset of
/// `imeOptions` flags the compositor wants forced on; the activity OR's
/// them with its base options (`IME_FLAG_NO_FULLSCREEN | IME_ACTION_NONE`).
fn wayland_content_to_android_input_type(hint: u32, purpose: u32) -> (i32, i32) {
    use android_input_type::*;
    use wayland_content::*;

    let mut input_type = match purpose {
        P_DIGITS => TYPE_CLASS_NUMBER,
        P_NUMBER => TYPE_CLASS_NUMBER | TYPE_NUMBER_FLAG_SIGNED | TYPE_NUMBER_FLAG_DECIMAL,
        P_PIN    => TYPE_CLASS_NUMBER | TYPE_NUMBER_VARIATION_PASSWORD,
        P_PHONE  => TYPE_CLASS_PHONE,
        P_DATE   => TYPE_CLASS_DATETIME | TYPE_DATETIME_VARIATION_DATE,
        P_TIME   => TYPE_CLASS_DATETIME | TYPE_DATETIME_VARIATION_TIME,
        P_DATETIME => TYPE_CLASS_DATETIME,
        P_URL    => TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_URI,
        P_EMAIL  => TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_EMAIL_ADDRESS,
        P_NAME   => TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_PERSON_NAME,
        // Spec says "combine with sensitive_data hint" but most clients
        // won't bother; mask unconditionally — leaking the chars onscreen
        // is the worse default. Use VISIBLE_PASSWORD only for non-password
        // purposes that explicitly request hidden display below.
        P_PASSWORD => TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_PASSWORD,
        // Terminal: kill suggestions/autocorrect; commands aren't English.
        P_TERMINAL => TYPE_CLASS_TEXT | TYPE_TEXT_FLAG_NO_SUGGESTIONS,
        // normal, alpha (Android doesn't have an alpha-only text class —
        // best we can do is plain text and let the IME's language layout
        // sort it out), and anything we don't recognise.
        _ => TYPE_CLASS_TEXT,
    };

    let hidden = (hint & HINT_HIDDEN_TEXT) != 0;

    // Hint-derived flags only meaningful for TEXT-class fields.
    if (input_type & TYPE_MASK_CLASS) == TYPE_CLASS_TEXT {
        if (hint & HINT_AUTO_CAPITALIZATION) != 0 {
            input_type |= TYPE_TEXT_FLAG_CAP_SENTENCES;
        }
        if (hint & HINT_UPPERCASE) != 0 {
            input_type |= TYPE_TEXT_FLAG_CAP_CHARACTERS;
        }
        if (hint & HINT_TITLECASE) != 0 {
            input_type |= TYPE_TEXT_FLAG_CAP_WORDS;
        }
        if (hint & HINT_SPELLCHECK) != 0 {
            input_type |= TYPE_TEXT_FLAG_AUTO_CORRECT;
        }
        if (hint & HINT_COMPLETION) != 0 {
            input_type |= TYPE_TEXT_FLAG_AUTO_COMPLETE;
        }
        if (hint & HINT_MULTILINE) != 0 {
            input_type |= TYPE_TEXT_FLAG_MULTI_LINE;
        }
        // hidden_text on a non-password field still wants suggestions off
        // (the IME would otherwise leak the hidden chars to its dictionary).
        if hidden && purpose != P_PASSWORD && purpose != P_PIN {
            input_type |= TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        }
    }

    let mut ime_flags = 0;
    // Stop the IME from learning typed text into its dictionary for any
    // password/PIN field, even when the client forgot the spec-mandated
    // `sensitive_data` companion hint.
    if (hint & HINT_SENSITIVE_DATA) != 0 || purpose == P_PASSWORD || purpose == P_PIN {
        ime_flags |= IME_FLAG_NO_PERSONALIZED_LEARNING;
    }

    (input_type, ime_flags)
}

/// Android `android.text.InputType` + `EditorInfo` constants used by the
/// content-type mapping. Names match the Java symbols verbatim so a grep
/// for the Android constant lands here.
#[allow(dead_code, non_upper_case_globals)]
mod android_input_type {
    pub const TYPE_MASK_CLASS: i32                  = 0x0000_000f;
    pub const TYPE_CLASS_TEXT: i32                  = 0x0000_0001;
    pub const TYPE_CLASS_NUMBER: i32                = 0x0000_0002;
    pub const TYPE_CLASS_PHONE: i32                 = 0x0000_0003;
    pub const TYPE_CLASS_DATETIME: i32              = 0x0000_0004;

    pub const TYPE_TEXT_VARIATION_URI: i32          = 0x0000_0010;
    pub const TYPE_TEXT_VARIATION_EMAIL_ADDRESS: i32 = 0x0000_0020;
    pub const TYPE_TEXT_VARIATION_PERSON_NAME: i32  = 0x0000_0060;
    pub const TYPE_TEXT_VARIATION_PASSWORD: i32     = 0x0000_0080;

    pub const TYPE_TEXT_FLAG_CAP_CHARACTERS: i32    = 0x0000_1000;
    pub const TYPE_TEXT_FLAG_CAP_WORDS: i32         = 0x0000_2000;
    pub const TYPE_TEXT_FLAG_CAP_SENTENCES: i32     = 0x0000_4000;
    pub const TYPE_TEXT_FLAG_AUTO_CORRECT: i32      = 0x0000_8000;
    pub const TYPE_TEXT_FLAG_AUTO_COMPLETE: i32     = 0x0001_0000;
    pub const TYPE_TEXT_FLAG_MULTI_LINE: i32        = 0x0002_0000;
    pub const TYPE_TEXT_FLAG_NO_SUGGESTIONS: i32    = 0x0008_0000;

    pub const TYPE_NUMBER_VARIATION_PASSWORD: i32   = 0x0000_0010;
    pub const TYPE_NUMBER_FLAG_SIGNED: i32          = 0x0000_1000;
    pub const TYPE_NUMBER_FLAG_DECIMAL: i32         = 0x0000_2000;

    pub const TYPE_DATETIME_VARIATION_DATE: i32     = 0x0000_0010;
    pub const TYPE_DATETIME_VARIATION_TIME: i32     = 0x0000_0020;

    pub const IME_FLAG_NO_PERSONALIZED_LEARNING: i32 = 0x0100_0000;
}

/// `text-input-v3` `content_hint` bitfield + `content_purpose` enum.
#[allow(dead_code)]
mod wayland_content {
    pub const HINT_COMPLETION: u32          = 0x001;
    pub const HINT_SPELLCHECK: u32          = 0x002;
    pub const HINT_AUTO_CAPITALIZATION: u32 = 0x004;
    pub const HINT_UPPERCASE: u32           = 0x010;
    pub const HINT_TITLECASE: u32           = 0x020;
    pub const HINT_HIDDEN_TEXT: u32         = 0x040;
    pub const HINT_SENSITIVE_DATA: u32      = 0x080;
    pub const HINT_MULTILINE: u32           = 0x200;

    pub const P_DIGITS: u32   = 2;
    pub const P_NUMBER: u32   = 3;
    pub const P_PHONE: u32    = 4;
    pub const P_URL: u32      = 5;
    pub const P_EMAIL: u32    = 6;
    pub const P_NAME: u32     = 7;
    pub const P_PASSWORD: u32 = 8;
    pub const P_PIN: u32      = 9;
    pub const P_DATE: u32     = 10;
    pub const P_TIME: u32     = 11;
    pub const P_DATETIME: u32 = 12;
    pub const P_TERMINAL: u32 = 13;
}

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
/// Used only by the combined commit-or-preedit-with-delete path
/// (Gboard's `setComposingRegion` + `commitText`/`setComposingText`
/// "tap to retype" replace). Standalone `deleteSurroundingText` is
/// translated to wl_keyboard Backspace key events at the IC layer
/// before reaching the channel — see `TawcInputConnection`.
///
/// Critical for non-BMP characters (emoji, etc.): a single emoji is 1 Rust
/// `char` but 2 UTF-16 code units and 4 UTF-8 bytes. Android's
/// `setComposingText("X", 2, 0)` to replace an emoji must become a
/// `delete_surrounding_text(4, 0)` on the wire.
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
            // measure against.
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
            zwp_text_input_v3::Request::SetContentType { hint, purpose } => {
                state.text_input_state.set_pending_content_type(&id, hint.into(), purpose.into());
            }
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
