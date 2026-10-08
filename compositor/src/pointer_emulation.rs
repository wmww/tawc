//! Pointer emulation: what touchscreen input does to `wl_pointer`.
//! See notes/input.md ("Pointer emulation").
//!
//! - `None`: touch is `wl_touch` only.
//! - `Hover`: also moves the pointer (no buttons) with the first finger.
//!   Some GTK3 code polls the client pointer instead of reading touch
//!   events (Nemo's rubberband), and the pointer entering before the first
//!   tap fixes GTK3's cold menubar path.
//! - `Full`: no `wl_touch` at all. A tap clicks, a drag holds the left
//!   button, a long press without moving right-clicks.
//!
//! The global setting applies unless the window's launch carried an
//! override (the launcher editor's "Override pointer emulation"). Overrides
//! attach to the launched program's session id, so every window of its
//! process tree gets one, and to its matched launch host, which covers
//! handoffs to an already-running instance and X11 windows.

use std::collections::HashMap;

use log::info;
use smithay::reexports::calloop::RegistrationToken;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Logical, Point};

use crate::compositor::TawcState;
use crate::host::ActivityId;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    None,
    Hover,
    Full,
}

impl Mode {
    /// The JNI encoding, shared with Kotlin's `PointerEmulation.ordinal`.
    pub fn from_index(i: i32) -> Option<Mode> {
        match i {
            0 => Some(Mode::None),
            1 => Some(Mode::Hover),
            2 => Some(Mode::Full),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Mode::None => "none",
            Mode::Hover => "hover",
            Mode::Full => "full",
        }
    }
}

/// What one touch slot is doing, fixed at down.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Gesture {
    /// Plain `wl_touch`; `hover` when it also drives the pointer.
    Touch { hover: bool },
    /// Drives the pointer as a mouse ([`State::full`]).
    Full,
    /// A second finger on a `Full` window: dropped.
    Ignored,
}

/// The one live `Full` gesture.
pub struct FullGesture {
    pub slot: i32,
    pub host: ActivityId,
    /// Down point, screen (host logical) coordinates.
    pub down: Point<f64, Logical>,
    /// Left button is held (the finger moved past the slop).
    pub pressed: bool,
    /// The long-press right click already fired; the lift does nothing.
    pub long_pressed: bool,
    pub timer: Option<RegistrationToken>,
}

pub struct State {
    pub global: Mode,
    /// Launch overrides by the launched program's session id.
    sessions: HashMap<i32, Mode>,
    /// Launch overrides by matched launch host.
    hosts: HashMap<ActivityId, Mode>,
    pub gestures: HashMap<i32, Gesture>,
    pub full: Option<FullGesture>,
    /// Mode the last touch on a window resolved to, and how many there
    /// were, for `query-state`.
    pub last_touch: Option<Mode>,
    pub touch_downs: u64,
}

impl State {
    pub fn new(global: Mode) -> Self {
        Self {
            global,
            sessions: HashMap::new(),
            hosts: HashMap::new(),
            gestures: HashMap::new(),
            full: None,
            last_touch: None,
            touch_downs: 0,
        }
    }

    /// Whether any window may want the pointer.
    pub fn wants_pointer(&self) -> bool {
        self.global != Mode::None
            || self.sessions.values().any(|m| *m != Mode::None)
            || self.hosts.values().any(|m| *m != Mode::None)
    }

    /// Slot the hover pointer follows, if any.
    pub fn hover_slot(&self) -> Option<i32> {
        self.gestures
            .iter()
            .find(|(_, g)| **g == Gesture::Touch { hover: true })
            .map(|(slot, _)| *slot)
    }
}

impl TawcState {
    pub fn set_pointer_emulation(&mut self, mode: Mode) {
        if self.pointer_emulation.global == mode {
            return;
        }
        self.pointer_emulation.global = mode;
        self.sync_pointer_capability();
        info!("pointer emulation: {}", mode.name());
    }

    /// A launch's session is known: its windows get `mode`, or the global
    /// setting when `None`. Overwrites, so a reused session id doesn't
    /// inherit a dead program's override.
    pub fn set_session_pointer_emulation(&mut self, sid: i32, mode: Option<Mode>) {
        match mode {
            Some(mode) => self.pointer_emulation.sessions.insert(sid, mode),
            None => self.pointer_emulation.sessions.remove(&sid),
        };
        self.sync_pointer_capability();
    }

    pub fn set_host_pointer_emulation(&mut self, host: &ActivityId, mode: Option<Mode>) {
        match mode {
            Some(mode) => self.pointer_emulation.hosts.insert(host.clone(), mode),
            None => self.pointer_emulation.hosts.remove(host),
        };
        self.sync_pointer_capability();
    }

    /// Mode for a touch landing on `surface`, shown on `host`.
    pub fn pointer_emulation_for(&self, surface: &WlSurface, host: &ActivityId) -> Mode {
        let state = &self.pointer_emulation;
        if !state.sessions.is_empty() {
            let sid = surface
                .client()
                .and_then(|c| c.get_credentials(&self.display_handle).ok())
                .and_then(|cred| crate::launch::session_of(cred.pid));
            if let Some(mode) = sid.and_then(|sid| state.sessions.get(&sid)) {
                return *mode;
            }
        }
        state.hosts.get(host).copied().unwrap_or(state.global)
    }
}
