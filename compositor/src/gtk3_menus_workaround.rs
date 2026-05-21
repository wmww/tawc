//! Contained workaround for GTK3 native Wayland menubars on touch-only tawc.
//!
//! GTK3 can open the leftmost menubar item on the first touchscreen tap when
//! legacy KDE server-side decorations put that item at window-local (0,0).
//! Briefly entering/leaving a wl_pointer at the center of each new toplevel
//! primes GTK3's pointer-crossing state and avoids the bad cold path.
//!
//! This module is intentionally isolated compatibility glue. If the workaround
//! is removed, delete this module, its Settings/JNI toggle, and the single
//! new-toplevel call site.

use log::info;
use smithay::input::pointer::MotionEvent as PointerMotionEvent;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Logical, Point, SERIAL_COUNTER};

use crate::compositor::TawcState;

pub(crate) fn set_enabled(data: &mut TawcState, enabled: bool) {
    if data.gtk3_broken_menus_workaround_enabled == enabled {
        return;
    }

    data.gtk3_broken_menus_workaround_enabled = enabled;
    if enabled {
        data.seat.add_pointer();
    } else {
        data.seat.remove_pointer();
    }
    info!("GTK3 broken menus workaround changed: {}", enabled);
}

pub(crate) fn prime_toplevel(
    data: &mut TawcState,
    surface: &WlSurface,
    width: i32,
    height: i32,
) {
    if !data.gtk3_broken_menus_workaround_enabled {
        return;
    }
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };

    let cx = (width.max(1) as f64) / 2.0;
    let cy = (height.max(1) as f64) / 2.0;
    let location: Point<f64, Logical> = (cx, cy).into();
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
    pointer.motion(
        data,
        None,
        &PointerMotionEvent {
            location,
            serial: SERIAL_COUNTER.next_serial(),
            time,
        },
    );
    pointer.frame(data);
}
