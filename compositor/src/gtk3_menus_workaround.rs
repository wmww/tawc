//! Contained workaround for GTK3 native Wayland menubars on touch-only tawc.
//!
//! GTK3 can open the leftmost menubar item on the first touchscreen tap when
//! legacy KDE server-side decorations put that item at window-local (0,0).
//! Briefly entering/leaving a wl_pointer at the center of each new toplevel
//! primes GTK3's pointer-crossing state and avoids the bad cold path.
//!
//! The `wl_pointer` seat capability is *not* owned here — see
//! `TawcState::sync_pointer_capability`; this module only flips its own
//! reason.
//!
//! This module is intentionally isolated compatibility glue. If the workaround
//! is removed, delete this module plus its small Settings/JNI and compositor
//! lifecycle hooks.

use std::collections::HashSet;

use log::info;
use smithay::backend::renderer::utils::with_renderer_surface_state;
use smithay::input::pointer::MotionEvent as PointerMotionEvent;
use smithay::reexports::wayland_server::backend::ObjectId;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Logical, Point, SERIAL_COUNTER};

use crate::compositor::TawcState;

pub(crate) struct State {
    pub(crate) enabled: bool,
    primed: HashSet<ObjectId>,
}

impl State {
    pub(crate) fn new(enabled: bool) -> Self {
        Self {
            enabled,
            primed: HashSet::new(),
        }
    }
}

pub(crate) fn set_enabled(data: &mut TawcState, enabled: bool) {
    if data.gtk3_broken_menus_workaround.enabled == enabled {
        return;
    }

    data.gtk3_broken_menus_workaround.enabled = enabled;
    if !enabled {
        data.gtk3_broken_menus_workaround.primed.clear();
    }
    // The seat capability has one owner; this is only one of its reasons.
    data.sync_pointer_capability();
    info!("GTK3 broken menus workaround changed: {}", enabled);
}

pub(crate) fn after_commit(data: &mut TawcState, committed: &WlSurface) {
    if !data.gtk3_broken_menus_workaround.enabled {
        return;
    }

    let committed_toplevel = data
        .xdg_shell_state
        .toplevel_surfaces()
        .iter()
        .find(|toplevel| toplevel.wl_surface() == committed)
        .cloned();
    let Some(toplevel) = committed_toplevel else {
        return;
    };

    let surface = toplevel.wl_surface();
    let id = surface.id();
    if data.gtk3_broken_menus_workaround.primed.contains(&id) {
        return;
    }
    let has_buffer = with_renderer_surface_state(surface, |renderer_state| {
        renderer_state.buffer().is_some()
    })
    .unwrap_or(false);
    if !has_buffer {
        return;
    }
    if data.desktop.assigned_host(surface).is_none() {
        return;
    }
    // The center of the window's own geometry, in its frame.
    let Some(geometry) = data.desktop.window(surface).map(|window| window.geometry()) else {
        return;
    };
    let center = geometry.loc.to_f64() + geometry.size.to_f64().downscale(2.0).to_point();

    prime_toplevel(data, surface, center);
    data.gtk3_broken_menus_workaround.primed.insert(id);
}

pub(crate) fn toplevel_destroyed(data: &mut TawcState, surface: &WlSurface) {
    data.gtk3_broken_menus_workaround
        .primed
        .remove(&surface.id());
}

fn prime_toplevel(data: &mut TawcState, surface: &WlSurface, location: Point<f64, Logical>) {
    if !data.gtk3_broken_menus_workaround.enabled {
        return;
    }
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };

    let (cx, cy) = (location.x, location.y);
    let time = data.start_time.elapsed().as_millis() as u32;

    info!(
        "GTK3 broken menus workaround: pointer enter/leave {:?} at {:.1},{:.1}",
        surface.id(),
        cx,
        cy,
    );
    pointer.motion(
        data,
        Some((surface.clone(), Point::from((0.0, 0.0)))),
        &PointerMotionEvent {
            location,
            serial: SERIAL_COUNTER.next_serial(),
            time,
        },
    );
    pointer.frame(data);
    // Finish by putting the pointer back where it was. An unconditional
    // leave here would yank a real mouse out of the surface it is hovering
    // every time a new toplevel commits its first buffer.
    let restore = data.pointer_focus.clone();
    let restore_location = if restore.is_some() {
        data.pointer_location
    } else {
        location
    };
    pointer.motion(
        data,
        restore,
        &PointerMotionEvent {
            location: restore_location,
            serial: SERIAL_COUNTER.next_serial(),
            time,
        },
    );
    pointer.frame(data);
}
