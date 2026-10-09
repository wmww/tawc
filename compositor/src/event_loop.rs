//! Calloop-based event loop for the compositor.
//!
//! Integrates the Wayland display, client listener, vsync frame clock and
//! per-Activity surface lifecycle into a single calloop event loop.
//! All `OutputHost` mutation happens here on the compositor thread —
//! JNI threads send events through channels.

use std::ffi::c_void;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use log::{error, info, warn};
use smithay::reexports::wayland_server::Resource;

use smithay::backend::input::{Axis, AxisSource, ButtonState, KeyState, TouchSlot};
use smithay::desktop::{PopupManager, Window, WindowSurfaceType};
use smithay::desktop::PopupUngrabStrategy;
use smithay::input::keyboard::{FilterResult, Keycode};
use smithay::input::pointer::{
    AxisFrame, ButtonEvent, MotionEvent as PointerMotionEvent,
};
use smithay::input::touch::{DownEvent, MotionEvent, UpEvent};
use smithay::reexports::calloop::channel::{Channel, Event as ChannelEvent};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{EventLoop, Interest, LoopHandle, LoopSignal, Mode, PostAction};
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::utils::{Logical, Point, SERIAL_COUNTER};
use smithay::wayland::compositor::{
    get_parent, get_role, SUBSURFACE_ROLE,
};
use smithay::wayland::seat::WaylandFocus;
use smithay::wayland::shell::xdg::XDG_POPUP_ROLE;
use wayland_server::Display;

use crate::host::{ActivityId, OutputHost, SurfaceEvent};
use crate::input::{PointerAxisSource, PointerEvent, TouchEvent};
use crate::scale::OutputScale;
use crate::text_input::TextInputEvent;
use crate::vsync::VsyncEvent;
use crate::clipboard::ClipboardEvent;

use crate::compositor::{ClientState, TawcState};
use crate::pointer_emulation::{FullGesture, Gesture, Mode as Emulation};
use crate::placement::Role;
use crate::render;

enum KeyboardFocusAction {
    Set(WlSurface),
    Keep,
    Clear,
}

struct TouchResolution {
    hit: Option<Hit>,
    keyboard_focus: KeyboardFocusAction,
}

/// A surface under a screen point. `origin` and `local` are in `window`'s
/// frame (root surface origin at `(0,0)`), which is what smithay's seat code
/// gets: it subtracts the origin before sending surface-local coordinates.
struct Hit {
    surface: WlSurface,
    origin: Point<f64, Logical>,
    window: Window,
    local: Point<f64, Logical>,
}

impl Hit {
    fn focus(&self) -> (WlSurface, Point<f64, Logical>) {
        (self.surface.clone(), self.origin)
    }
}

/// Hit-test the visible host's window stack at a screen (host logical)
/// point, through each window's placement. Shared by touch and pointer: both
/// honour the visible-host guard and `WindowSurfaceType::ALL`, which
/// respects `wl_surface.set_input_region` (Firefox/WebRender attaches
/// render-only children with an empty region). The scrim under a dialog
/// swallows points that miss it.
fn surface_at(
    data: &TawcState,
    activity_id: &ActivityId,
    screen: Point<f64, Logical>,
) -> Option<Hit> {
    if data.desktop_visible_host_id().as_ref() != Some(activity_id) {
        return None;
    }
    let layout = data.host_layout(activity_id)?;
    for (i, entry) in layout.entries.iter().enumerate().rev() {
        let local = entry.placement.to_window(screen);
        if let Some((surface, origin)) = entry.window.surface_under(local, WindowSurfaceType::ALL) {
            return Some(Hit {
                surface,
                origin: origin.to_f64(),
                window: entry.window.clone(),
                local,
            });
        }
        if layout.scrim_below == Some(i) {
            return None;
        }
    }
    None
}

/// `screen` in the frame of `window`, or unchanged when it has no placement
/// (gone, or never hit anything).
fn to_window_frame(
    data: &TawcState,
    window: Option<&Window>,
    screen: Point<f64, Logical>,
) -> Point<f64, Logical> {
    window
        .and_then(|window| data.window_placement(window))
        .map_or(screen, |placement| placement.to_window(screen))
}

fn is_in_xdg_popup_tree(surface: &WlSurface) -> bool {
    let mut current = Some(surface.clone());
    while let Some(surface) = current {
        if get_role(&surface) == Some(XDG_POPUP_ROLE) {
            return true;
        }
        current = get_parent(&surface);
    }
    false
}

fn main_surface_for_subsurface_tree(surface: &WlSurface) -> WlSurface {
    let mut current = surface.clone();
    while get_role(&current) == Some(SUBSURFACE_ROLE) {
        let Some(parent) = get_parent(&current) else {
            break;
        };
        current = parent;
    }
    current
}

fn resolve_touch_down(
    data: &TawcState,
    activity_id: &ActivityId,
    screen: Point<f64, Logical>,
) -> TouchResolution {
    let hit = surface_at(data, activity_id, screen);
    let keyboard_focus = match hit.as_ref().map(|hit| &hit.surface) {
        Some(surface) if is_in_xdg_popup_tree(surface) => KeyboardFocusAction::Keep,
        Some(surface) => KeyboardFocusAction::Set(main_surface_for_subsurface_tree(surface)),
        None => KeyboardFocusAction::Clear,
    };

    TouchResolution { hit, keyboard_focus }
}

fn apply_keyboard_focus_action(data: &mut TawcState, action: KeyboardFocusAction) {
    match action {
        KeyboardFocusAction::Set(surface) => data.set_input_focus(Some(&surface)),
        KeyboardFocusAction::Keep => {}
        KeyboardFocusAction::Clear => data.set_input_focus(None),
    }
}

/// Send `wl_pointer.leave` and forget the pointer's focus.
///
/// Used where the pointer's surface stops being something the user can point
/// at: Activity focus loss, surface destroy, host switch. Deliberately *not*
/// wired to Android's `ACTION_HOVER_EXIT` — Android synthesizes one before
/// every mouse `ACTION_DOWN`, and wrapping each click in leave/enter closes
/// GTK menus and breaks drags.
fn clear_pointer_focus(data: &mut TawcState) {
    if data.pointer_focus.is_none() {
        return;
    }
    data.pointer_focus = None;
    data.pointer_frame = None;
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };
    let location = data.pointer_location;
    let time = data.start_time.elapsed().as_millis() as u32;
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

/// Real-mouse (and `Full` emulation) motion to `screen` on `activity_id`.
fn pointer_motion_to(
    data: &mut TawcState,
    activity_id: &ActivityId,
    screen: Point<f64, Logical>,
    time: u32,
) {
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };
    // Hover is not activation: motion never moves keyboard focus.
    let hit = surface_at(data, activity_id, screen);
    // A grab (held button, popup) keeps delivering in the frame it started
    // in; a hit on another window is outside it.
    if !pointer.is_grabbed() {
        data.pointer_frame = hit.as_ref().map(|hit| hit.window.clone());
    }
    let location = to_window_frame(data, data.pointer_frame.as_ref(), screen);
    let focus = hit
        .filter(|hit| data.pointer_frame.as_ref() == Some(&hit.window))
        .map(|hit| hit.focus());
    data.pointer_screen_location = screen;
    data.pointer_location = location;
    data.pointer_focus = focus.clone();
    let serial = SERIAL_COUNTER.next_serial();
    pointer.motion(data, focus, &PointerMotionEvent { location, serial, time });
    pointer.frame(data);
}

/// Real-mouse (and `Full` emulation) button at the pointer's position.
fn pointer_button(data: &mut TawcState, activity_id: &ActivityId, code: u32, pressed: bool, time: u32) {
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };
    let serial = SERIAL_COUNTER.next_serial();
    if pressed {
        // A press takes the touch-down path: click outside a menu dismisses
        // it, click in a toplevel moves keyboard and text-input focus.
        // Smithay's default grab keeps pointer focus while the button is
        // held.
        let screen = data.pointer_screen_location;
        let resolution = resolve_touch_down(data, activity_id, screen);
        dismiss_host_popups_if_touch_is_outside_popup(
            data,
            activity_id,
            resolution.hit.as_ref().map(|hit| &hit.surface),
            serial,
            time,
        );
        apply_keyboard_focus_action(data, resolution.keyboard_focus);
    }
    let state = if pressed { ButtonState::Pressed } else { ButtonState::Released };
    pointer.button(data, &ButtonEvent { serial, time, button: code, state });
    pointer.frame(data);
}

const BTN_LEFT: u32 = 0x110;
const BTN_RIGHT: u32 = 0x111;
/// `Full` emulation: movement (logical px) that turns a press into a drag.
const FULL_TOUCH_SLOP: f64 = 8.0;
/// `Full` emulation: hold this long without moving to right-click.
const FULL_LONG_PRESS: Duration = Duration::from_millis(500);

fn touch_down(
    data: &mut TawcState,
    activity_id: &ActivityId,
    id: i32,
    screen: Point<f64, Logical>,
    time: u32,
) {
    let resolution = resolve_touch_down(data, activity_id, screen);
    let mode = resolution.hit.as_ref().map_or(Emulation::None, |hit| {
        data.pointer_emulation_for(&hit.surface, activity_id)
    });
    if resolution.hit.is_some() {
        data.pointer_emulation.last_touch = Some(mode);
        data.pointer_emulation.touch_downs += 1;
    }
    let pointer = data.seat.get_pointer().is_some();

    if mode == Emulation::Full && pointer {
        let gesture = if data.pointer_emulation.full.is_some() {
            Gesture::Ignored
        } else {
            full_down(data, activity_id, id, screen, time);
            Gesture::Full
        };
        data.pointer_emulation.gestures.insert(id, gesture);
        return;
    }

    let Some(touch) = data.seat.get_touch() else {
        return;
    };
    let serial = SERIAL_COUNTER.next_serial();
    dismiss_host_popups_if_touch_is_outside_popup(
        data,
        activity_id,
        resolution.hit.as_ref().map(|hit| &hit.surface),
        serial,
        time,
    );
    // Touch chooses the input target, but keyboard/text-input focus follows
    // Wayland role policy. In particular, wl_subsurface targets focus their
    // main surface, and non-grabbed xdg_popup touches leave keyboard focus
    // alone. Do not speculatively commit preedit here: a touch may scroll,
    // hit a button, or be ignored. If the client really moves the cursor,
    // its following set_surrounding_text(cause=other) drives preedit
    // cleanup.
    apply_keyboard_focus_action(data, resolution.keyboard_focus);
    // The slot's motion stays in the touched window's frame even after the
    // finger leaves it.
    let hit = resolution.hit;
    let location = hit.as_ref().map_or(screen, |hit| hit.local);
    match &hit {
        Some(hit) => data.touch_frames.insert(id, hit.window.clone()),
        None => data.touch_frames.remove(&id),
    };
    let hover = mode == Emulation::Hover
        && pointer
        && data.pointer_emulation.full.is_none()
        && data.pointer_emulation.hover_slot().is_none_or(|slot| slot == id);
    data.pointer_emulation.gestures.insert(id, Gesture::Touch { hover });
    if let (true, Some(hit)) = (hover, &hit) {
        // Before the touch, so the pointer is in place when GTK starts
        // acting on the emulated press.
        hover_down(data, hit, screen, time);
    }
    touch.down(
        data,
        hit.as_ref().map(Hit::focus),
        &DownEvent {
            slot: TouchSlot::from(Some(id as u32)),
            location,
            serial,
            time,
        },
    );
    touch.frame(data);
}

fn touch_motion(data: &mut TawcState, id: i32, screen: Point<f64, Logical>, time: u32) {
    match data.pointer_emulation.gestures.get(&id).copied() {
        Some(Gesture::Full) => return full_motion(data, screen, time),
        Some(Gesture::Ignored) => return,
        Some(Gesture::Touch { hover: true }) => hover_motion(data, screen, time),
        Some(Gesture::Touch { hover: false }) | None => {}
    }
    let Some(touch) = data.seat.get_touch() else {
        return;
    };
    let location = to_window_frame(data, data.touch_frames.get(&id), screen);
    // Smithay keeps the focus from down; this argument is unused.
    touch.motion(
        data,
        None,
        &MotionEvent {
            slot: TouchSlot::from(Some(id as u32)),
            location,
            time,
        },
    );
    touch.frame(data);
}

fn touch_up(data: &mut TawcState, id: i32, time: u32) {
    match data.pointer_emulation.gestures.remove(&id) {
        Some(Gesture::Full) => return full_up(data, time),
        Some(Gesture::Ignored) => return,
        Some(Gesture::Touch { .. }) | None => {}
    }
    data.touch_frames.remove(&id);
    let Some(touch) = data.seat.get_touch() else {
        return;
    };
    touch.up(
        data,
        &UpEvent {
            slot: TouchSlot::from(Some(id as u32)),
            serial: SERIAL_COUNTER.next_serial(),
            time,
        },
    );
    touch.frame(data);
}

/// `Hover` emulation: put the pointer (no buttons) on the touched surface.
/// It stays pinned to that surface for the gesture, like a mouse implicit
/// grab, and rests there after the lift — no leave, since crossing pairs
/// close GTK menus.
fn hover_down(data: &mut TawcState, hit: &Hit, screen: Point<f64, Logical>, time: u32) {
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };
    data.pointer_frame = Some(hit.window.clone());
    data.pointer_screen_location = screen;
    data.pointer_location = hit.local;
    data.pointer_focus = Some(hit.focus());
    let serial = SERIAL_COUNTER.next_serial();
    pointer.motion(
        data,
        Some(hit.focus()),
        &PointerMotionEvent { location: hit.local, serial, time },
    );
    pointer.frame(data);
}

fn hover_motion(data: &mut TawcState, screen: Point<f64, Logical>, time: u32) {
    let Some(pointer) = data.seat.get_pointer() else {
        return;
    };
    let location = to_window_frame(data, data.pointer_frame.as_ref(), screen);
    data.pointer_screen_location = screen;
    data.pointer_location = location;
    let focus = data.pointer_focus.clone();
    let serial = SERIAL_COUNTER.next_serial();
    pointer.motion(data, focus, &PointerMotionEvent { location, serial, time });
    pointer.frame(data);
}

/// `Full` emulation: move the pointer there and wait to see whether this is
/// a tap, a drag or a long press.
fn full_down(
    data: &mut TawcState,
    activity_id: &ActivityId,
    id: i32,
    screen: Point<f64, Logical>,
    time: u32,
) {
    pointer_motion_to(data, activity_id, screen, time);
    let timer = data
        .loop_handle()
        .insert_source(Timer::from_duration(FULL_LONG_PRESS), move |_, _, data| {
            full_long_press(data, id);
            TimeoutAction::Drop
        })
        .ok();
    data.pointer_emulation.full = Some(FullGesture {
        slot: id,
        host: activity_id.clone(),
        down: screen,
        pressed: false,
        long_pressed: false,
        timer,
    });
}

fn full_motion(data: &mut TawcState, screen: Point<f64, Logical>, time: u32) {
    let Some(gesture) = data.pointer_emulation.full.as_ref() else {
        return;
    };
    let host = gesture.host.clone();
    let delta = screen - gesture.down;
    let start_drag = !gesture.pressed
        && !gesture.long_pressed
        && delta.x.hypot(delta.y) > FULL_TOUCH_SLOP;
    if !gesture.pressed && !gesture.long_pressed && !start_drag {
        return;
    }
    if start_drag {
        // Press where the finger went down, then follow it.
        full_cancel_timer(data);
        pointer_button(data, &host, BTN_LEFT, true, time);
        if let Some(gesture) = data.pointer_emulation.full.as_mut() {
            gesture.pressed = true;
        }
    }
    pointer_motion_to(data, &host, screen, time);
}

fn full_up(data: &mut TawcState, time: u32) {
    full_cancel_timer(data);
    let Some(gesture) = data.pointer_emulation.full.take() else {
        return;
    };
    if gesture.long_pressed {
        return;
    }
    if !gesture.pressed {
        pointer_button(data, &gesture.host, BTN_LEFT, true, time);
    }
    pointer_button(data, &gesture.host, BTN_LEFT, false, time);
}

fn full_long_press(data: &mut TawcState, id: i32) {
    let Some(gesture) = data.pointer_emulation.full.as_mut() else {
        return;
    };
    if gesture.slot != id || gesture.pressed || gesture.long_pressed {
        return;
    }
    gesture.timer = None;
    gesture.long_pressed = true;
    let host = gesture.host.clone();
    let time = data.start_time.elapsed().as_millis() as u32;
    pointer_button(data, &host, BTN_RIGHT, true, time);
    pointer_button(data, &host, BTN_RIGHT, false, time);
    if let Err(e) = data.display_handle.flush_clients() {
        error!("flush_clients error after long press: {}", e);
    }
}

fn full_cancel_timer(data: &mut TawcState) {
    if let Some(token) = data.pointer_emulation.full.as_mut().and_then(|g| g.timer.take()) {
        data.loop_handle().remove(token);
    }
}

/// [`clear_pointer_focus`], but only when the pointer is currently inside a
/// surface belonging to `activity_id`.
fn clear_pointer_focus_for_host(data: &mut TawcState, activity_id: &ActivityId) {
    let inside = data
        .pointer_focus
        .as_ref()
        .is_some_and(|(surface, _)| host_for_surface(data, surface).as_ref() == Some(activity_id));
    if inside {
        clear_pointer_focus(data);
    }
}

fn host_for_surface(data: &TawcState, surface: &WlSurface) -> Option<ActivityId> {
    if let Some(host) = data.desktop.host_for_surface(surface) {
        return Some(host);
    }

    let mut current = Some(surface.clone());
    while let Some(surface) = current {
        if let Some(host) = data.desktop.assigned_host(&surface) {
            return Some(host.clone());
        }
        current = get_parent(&surface);
    }
    None
}

fn send_keyboard_key_press(data: &mut TawcState, evdev_keycode: u32) {
    if let Some(keyboard) = data.seat.get_keyboard() {
        let serial = SERIAL_COUNTER.next_serial();
        let time = data.start_time.elapsed().as_millis() as u32;
        let keycode = Keycode::from(evdev_keycode + 8);
        keyboard.input::<(), _>(
            data, keycode, KeyState::Pressed, serial, time,
            |_, _, _| FilterResult::Forward,
        );
        let serial = SERIAL_COUNTER.next_serial();
        keyboard.input::<(), _>(
            data, keycode, KeyState::Released, serial, time + 1,
            |_, _, _| FilterResult::Forward,
        );
    }
}

fn send_keyboard_key_state(data: &mut TawcState, evdev_keycode: u32, pressed: bool) {
    if let Some(keyboard) = data.seat.get_keyboard() {
        let serial = SERIAL_COUNTER.next_serial();
        let time = data.start_time.elapsed().as_millis() as u32;
        let keycode = Keycode::from(evdev_keycode + 8);
        let state = if pressed {
            KeyState::Pressed
        } else {
            KeyState::Released
        };
        keyboard.input::<(), _>(
            data, keycode, state, serial, time,
            |_, _, _| FilterResult::Forward,
        );
    }
}

fn host_can_receive_hardware_key(data: &TawcState, activity_id: &ActivityId) -> bool {
    data.desktop.foreground_host() == Some(activity_id) && data.hosts.contains_key(activity_id)
}

fn handle_hardware_key(
    data: &mut TawcState,
    activity_id: &ActivityId,
    evdev_keycode: u32,
    pressed: bool,
    _repeat_count: u32,
) {
    let key = (activity_id.clone(), evdev_keycode);
    if pressed {
        if !host_can_receive_hardware_key(data, activity_id) {
            return;
        }
        if data.hardware_keys_down.insert(key) {
            send_keyboard_key_state(data, evdev_keycode, true);
        }
        return;
    }

    if data.hardware_keys_down.remove(&key) {
        send_keyboard_key_state(data, evdev_keycode, false);
    }
}

fn dismiss_topmost_grabbing_popup(data: &mut TawcState, activity_id: &ActivityId) -> bool {
    let Some(grab) = data.active_popup_grab.as_ref() else {
        return false;
    };
    if grab.has_ended() {
        return false;
    }
    let Some(surface) = grab.current_grab() else {
        return false;
    };
    if host_for_surface(data, &surface).as_ref() != Some(activity_id) {
        return false;
    }

    let serial = SERIAL_COUNTER.next_serial();
    let time = data.start_time.elapsed().as_millis() as u32;
    let ended = if let Some(grab) = data.active_popup_grab.as_mut() {
        let _ = grab.ungrab(PopupUngrabStrategy::Topmost);
        grab.has_ended()
    } else {
        false
    };
    if ended {
        data.active_popup_grab = None;
        if let Some(pointer) = data.seat.get_pointer() {
            pointer.unset_grab(data, serial, time);
        }
        if let Some(keyboard) = data.seat.get_keyboard() {
            if keyboard.is_grabbed() {
                keyboard.unset_grab(data);
            }
        }
    }
    data.needs_render = true;
    true
}

fn handle_back_pressed(data: &mut TawcState, activity_id: &ActivityId) {
    if data.desktop.foreground_host() != Some(activity_id) || !data.hosts.contains_key(activity_id) {
        return;
    }

    if dismiss_topmost_grabbing_popup(data, activity_id) {
        return;
    }

    if data.host_fullscreen(activity_id) {
        data.set_host_fullscreen(activity_id, false);
        crate::set_activity_fullscreen_from_native(activity_id, false);
        data.needs_render = true;
        return;
    }

    if close_topmost_dialog(data, activity_id) {
        return;
    }

    send_keyboard_key_press(data, crate::keymap::EVDEV_KEY_BACK);
}

/// Back on a dialog acts like its title-bar close button; dialogs have no
/// decorations here, and few handle the Back key.
fn close_topmost_dialog(data: &mut TawcState, activity_id: &ActivityId) -> bool {
    let Some(layout) = data.host_layout(activity_id) else {
        return false;
    };
    let Some(top) = layout.entries.iter().rev().find(|e| e.role != Role::OverrideRedirect) else {
        return false;
    };
    if top.role != Role::Child || top.window.geometry().is_empty() {
        return false;
    }
    if let Some(toplevel) = top.window.toplevel() {
        toplevel.send_close();
    } else if let Some(x11) = top.window.x11_surface() {
        if let Err(e) = x11.close() {
            warn!("xwayland: failed to close window {}: {}", x11.window_id(), e);
            return false;
        }
    }
    true
}

fn dismiss_host_popups_if_touch_is_outside_popup(
    data: &mut TawcState,
    activity_id: &ActivityId,
    focus: Option<&WlSurface>,
    serial: smithay::utils::Serial,
    time: u32,
) {
    if focus.is_some_and(is_in_xdg_popup_tree) {
        return;
    }

    let mut dismissed_active_grab = false;
    let mut grab_ended = false;
    if let Some(grab) = data.active_popup_grab.as_mut() {
        let had_active_grab = !grab.has_ended();
        if had_active_grab {
            let _ = grab.ungrab(PopupUngrabStrategy::All);
            dismissed_active_grab = true;
        }
        grab_ended = grab.has_ended();
    }
    if grab_ended {
        data.active_popup_grab = None;
        if let Some(pointer) = data.seat.get_pointer() {
            pointer.unset_grab(data, serial, time);
        }
        if let Some(keyboard) = data.seat.get_keyboard() {
            if keyboard.is_grabbed() {
                keyboard.unset_grab(data);
            }
        }
    }
    if dismissed_active_grab {
        data.needs_render = true;
        return;
    }

    let roots: Vec<WlSurface> = data
        .wayland_toplevels_for_host(activity_id)
        .into_iter()
        .map(|t| t.wl_surface().clone())
        .collect();

    for root in roots {
        if let Some((popup, _)) = PopupManager::popups_for_surface(&root).next() {
            if PopupManager::dismiss_popup(&root, &popup).is_ok() {
                data.needs_render = true;
            }
        }
    }
}

/// Set up and run the calloop event loop. Returns when `running` becomes false.
#[allow(clippy::too_many_arguments)]
pub fn run(
    display: Display<TawcState>,
    mut state: TawcState,
    touch_channel: Channel<TouchEvent>,
    pointer_channel: Channel<PointerEvent>,
    text_input_channel: Channel<TextInputEvent>,
    clipboard_channel: Channel<ClipboardEvent>,
    state_query_channel: Channel<mpsc::Sender<String>>,
    surface_event_channel: Channel<SurfaceEvent>,
    running: &std::sync::atomic::AtomicBool,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut event_loop: EventLoop<TawcState> = EventLoop::try_new()?;
    let loop_handle = event_loop.handle();
    // Callbacks reach the loop via state, never by capturing LoopHandle
    // clones — a captured handle sits inside the loop's own source list,
    // creating an Rc cycle that leaks every source (and the Wayland
    // listening socket's lock) past this function's return. See
    // TawcState::loop_handle.
    state.loop_handle = Some(loop_handle.clone());

    // --- Source 1: Wayland display fd ---
    // When clients send protocol messages, this fd becomes readable.
    // Generic owns the Display; the closure receives `&mut Generic<Display<...>>`
    // and `&mut TawcState` as separate borrows so we can call
    // dispatch_clients with the state. Mirrors anvil's pattern.
    loop_handle.insert_source(
        Generic::new(display, Interest::READ, Mode::Level),
        |_, display, data: &mut TawcState| {
            // Safety: we don't drop the display.
            unsafe {
                if let Err(e) = display.get_mut().dispatch_clients(data) {
                    error!("dispatch_clients error: {}", e);
                }
            }
            // Immediate flush is important: clients like GTK3 won't render
            // until they receive their configure.
            if let Err(e) = data.display_handle.flush_clients() {
                error!("flush_clients error: {}", e);
            }
            Ok(PostAction::Continue)
        },
    )?;

    // --- Source 2: client listener, inserted last (end of this function).

    // --- Source 3: Touch input channel ---
    // Receives touch events from the Android UI thread via JNI, tagged
    // with the activity_id of the SurfaceView that produced them.
    // Coordinates arrive in physical pixels; we convert to logical.
    //
    // Focus picks the first alive toplevel assigned to the touch's host —
    // each Android task only has its own toplevels in the recents card,
    // so this matches what the user sees.
    loop_handle.insert_source(touch_channel, |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };
        let scale = data.output_scale;
        let to_screen = |x: f32, y: f32| -> Point<f64, Logical> {
            (scale.logical_coord(x as f64), scale.logical_coord(y as f64)).into()
        };
        match evt {
            TouchEvent::Down { id, x, y, time, activity_id } => {
                touch_down(data, &activity_id, id, to_screen(x, y), time)
            }
            TouchEvent::Motion { id, x, y, time, .. } => touch_motion(data, id, to_screen(x, y), time),
            TouchEvent::Up { id, time, .. } => touch_up(data, id, time),
        }

        // Flush immediately so clients see events without waiting for a frame
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after touch: {}", e);
        }
    })?;

    // --- Source 4: Pointer input channel ---
    // Real mouse input. `CompositorActivity` splits mouse-source
    // MotionEvents off the touch path, so touchscreen and stylus never
    // reach here and a click never produces both a wl_touch.down and a
    // wl_pointer.button. See notes/input.md ("Pointer Input").
    loop_handle.insert_source(pointer_channel, |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        // No pointer capability means no mouse and no pointer emulation;
        // the events are stale Android input for a seat that can't carry
        // them.
        let pointer = match data.seat.get_pointer() {
            Some(p) => p,
            None => return,
        };

        let activity_id = match &evt {
            PointerEvent::Motion { activity_id, .. }
            | PointerEvent::Button { activity_id, .. }
            | PointerEvent::Axis { activity_id, .. } => activity_id.clone(),
        };
        // Host scoping, like touch: only the visible host drives the pointer.
        if data.desktop_visible_host_id().as_ref() != Some(&activity_id) {
            return;
        }

        let scale = data.output_scale;

        match evt {
            PointerEvent::Motion { x, y, time, .. } => {
                let screen: Point<f64, Logical> =
                    (scale.logical_coord(x as f64), scale.logical_coord(y as f64)).into();
                pointer_motion_to(data, &activity_id, screen, time);
            }
            PointerEvent::Button { code, pressed, time, .. } => {
                pointer_button(data, &activity_id, code, pressed, time);
            }
            PointerEvent::Axis { dx, dy, v120_x, v120_y, source, stop, time, .. } => {
                let mut frame = AxisFrame::new(time).source(match source {
                    PointerAxisSource::Wheel => AxisSource::Wheel,
                    PointerAxisSource::Finger => AxisSource::Finger,
                });
                if stop {
                    // Finger scrolling must end explicitly or GTK's kinetic
                    // scrolling never settles.
                    frame = frame.stop(Axis::Horizontal).stop(Axis::Vertical);
                } else {
                    if dx != 0.0 {
                        frame = frame.value(Axis::Horizontal, dx);
                    }
                    if dy != 0.0 {
                        frame = frame.value(Axis::Vertical, dy);
                    }
                    // Detents only exist on a wheel. Smithay sends
                    // axis_value120 to v8+ clients and accumulates
                    // axis_discrete for older ones by itself.
                    if source == PointerAxisSource::Wheel {
                        if v120_x != 0 {
                            frame = frame.v120(Axis::Horizontal, v120_x);
                        }
                        if v120_y != 0 {
                            frame = frame.v120(Axis::Vertical, v120_y);
                        }
                    }
                }
                pointer.axis(data, frame);
                pointer.frame(data);
            }
        }

        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after pointer: {}", e);
        }
    })?;

    // --- Source 5: Android clipboard channel ---
    //
    // Kotlin listens to Android's real ClipboardManager and forwards
    // content-free clip announces here (the content is fetched only when
    // a client pastes). Client-owned selections are pulled eagerly only after
    // Smithay has installed them in seat state; the selection handlers
    // queue PullSelection for this source to perform that deferred request.
    loop_handle.insert_source(clipboard_channel, move |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };
        let handle = &data.loop_handle();

        match evt {
            ClipboardEvent::AndroidClipAvailable { ts, own_write } => {
                // Skip echoes of our own Wayland→Android mirror and
                // re-announces of an already-announced clip (focus syncs)
                // — but only while some selection is live. With none
                // (e.g. the mirrored owner died), announce anyway so the
                // compositor takes ownership and paste keeps working.
                // ts == 0 (OEM builds that don't stamp clips) never
                // matches, so foreign-clip focus syncs re-announce on
                // every focus gain there. Mostly idempotent; the real
                // cost is a client selection whose mirror never
                // completed (non-text, over cap, timeout) getting
                // replaced — completed mirrors are own-label writes.
                let already_announced =
                    ts != 0 && data.last_announced_android_clip_ts == Some(ts);
                if (own_write || already_announced) && crate::clipboard::selection_exists(data) {
                    return;
                }
                // Android's clipboard is now the newest state; a pull of an
                // older client selection must not overwrite it later.
                crate::clipboard::cancel_pull(handle, data);
                data.last_announced_android_clip_ts = Some(ts);
                crate::clipboard::install_android_selection(data);
                if let Err(e) = data.display_handle.flush_clients() {
                    error!("flush_clients error after Android clipboard update: {}", e);
                }
            }
            ClipboardEvent::PullSelection { source, mime_type } => {
                let (read_fd, write_fd) = match crate::clipboard::pipe() {
                    Ok(fds) => fds,
                    Err(e) => {
                        log::warn!("clipboard: pipe failed for selection pull: {}", e);
                        return;
                    }
                };
                let requested = match source {
                    crate::clipboard::PullSource::Wayland => {
                        match smithay::wayland::selection::data_device::request_data_device_client_selection(
                            &data.seat,
                            mime_type,
                            write_fd,
                        ) {
                            Ok(()) => {
                                // Flush so the owner sees the send request
                                // without waiting for a frame tick.
                                if let Err(e) = data.display_handle.flush_clients() {
                                    error!("flush_clients error after clipboard request: {}", e);
                                }
                                true
                            }
                            Err(e) => {
                                log::warn!("clipboard: wayland selection request failed: {:?}", e);
                                false
                            }
                        }
                    }
                    crate::clipboard::PullSource::X11 => match data.xwm.as_mut() {
                        Some(xwm) => xwm
                            .send_selection(
                                smithay::wayland::selection::SelectionTarget::Clipboard,
                                mime_type,
                                write_fd,
                            )
                            .map_err(|e| log::warn!("clipboard: x11 selection request failed: {:?}", e))
                            .is_ok(),
                        None => false,
                    },
                };
                if requested {
                    crate::clipboard::start_pull(handle, data, read_fd, source);
                }
            }
        }
    })?;

    // --- Source 6: Text input channel ---
    // Receives text input events from Android IME via JNI.
    loop_handle.insert_source(text_input_channel, move |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        match evt {
            TextInputEvent::KeyPress { keycode } => {
                // Send as a real wl_keyboard key event (press + release)
                send_keyboard_key_press(data, keycode);
            }
            TextInputEvent::KeyState { keycode, pressed } => {
                send_keyboard_key_state(data, keycode, pressed);
            }
            _ => {
                data.text_input_state.handle_android_event(evt);
            }
        }

        // Flush so clients see text input events immediately
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after text input: {}", e);
        }
    })?;

    // --- Source 7: State query channel ---
    // Receives requests for a compositor-thread state snapshot.
    loop_handle.insert_source(state_query_channel, move |event, _, data: &mut TawcState| {
        if let ChannelEvent::Msg(response) = event {
            let clients = data.client_count.load(std::sync::atomic::Ordering::Relaxed);
            // Report smithay's live pointer focus, not TAWC's tracked copy:
            // a grab can hold focus somewhere other than the last resolved
            // hit test. The position is TAWC's screen copy; smithay's is in
            // a window frame.
            let pointer = data.seat.get_pointer();
            let bound_hosts = data
                .hosts
                .values()
                .filter(|h| h.egl_surface.is_some())
                .count();
            let (surfaces_wlegl, surfaces_shm) = data.attached_buffer_counts();
            let x11_surfaces_with_host = data
                .x11_surfaces
                .iter()
                .filter(|surface| data.x11_surface_host(surface).is_some())
                .count();
            let wlegl = crate::wlegl::debug_counters();
            let xwayland_pids = crate::xwayland::xwayland_pids()
                .iter()
                .map(|pid| pid.to_string())
                .collect::<Vec<_>>()
                .join(",");
            // `<host>:<windows>` per registered host, `-` for none.
            let mut host_windows = data
                .hosts
                .keys()
                .map(|id| format!("{}:{}", id, data.desktop.window_count_for_host(id)))
                .collect::<Vec<_>>();
            host_windows.sort();
            let host_windows = if host_windows.is_empty() {
                "-".to_string()
            } else {
                host_windows.join(",")
            };
            let (windows, scrim) = visible_layout_debug(data);
            let payload = format!(
                "clients={} toplevels={} surfaces_wlegl={} surfaces_shm={} frames={} rendered_toplevels={} hosts={} bound_hosts={} xwayland_running={} xwayland_pids={} x11_surfaces={} x11_surfaces_with_host={} wlegl_create_buffer_total={} wlegl_import_texture_total={} wlegl_buffer_destroy_total={} last_wlegl_width={} last_wlegl_height={} last_wlegl_format={} output_scale={:.2} output_physical_w={} output_physical_h={} output_logical_w={} output_logical_h={} pointer_present={} pointer_x={:.2} pointer_y={:.2} pointer_focus={} cursor_shape={} vsync_ticks={} last_vsync_ns={} vsync_period_ns={} tick_latency_max_ns={} output_refresh_mhz={} pending_launches={} host_windows={} windows={} scrim={} pointer_emulation={} last_touch_emulation={} touch_downs={}",
                clients,
                toplevel_count(data),
                surfaces_wlegl,
                surfaces_shm,
                data.frame_count,
                data.last_rendered_toplevels,
                data.hosts.len(),
                bound_hosts,
                data.xwm.is_some(),
                xwayland_pids,
                data.x11_surfaces.len(),
                x11_surfaces_with_host,
                wlegl.create_buffer_total,
                wlegl.import_texture_total,
                wlegl.buffer_destroy_total,
                wlegl.last_width,
                wlegl.last_height,
                wlegl.last_format,
                data.output_scale.fractional(),
                data.output_physical_size.0,
                data.output_physical_size.1,
                data.output_logical_size.0,
                data.output_logical_size.1,
                pointer.is_some(),
                if pointer.is_some() { data.pointer_screen_location.x } else { 0.0 },
                if pointer.is_some() { data.pointer_screen_location.y } else { 0.0 },
                if pointer.as_ref().is_some_and(|p| p.current_focus().is_some()) {
                    "yes"
                } else {
                    "no"
                },
                crate::cursor::debug_shape(data),
                data.frame_clock.ticks,
                data.frame_clock.last_vsync_ns,
                data.frame_clock.measured_period_ns,
                data.frame_clock.tick_latency_max_ns,
                data.output_refresh_mhz,
                data.pending_launches.len(),
                host_windows,
                windows,
                scrim,
                data.pointer_emulation.global.name(),
                data.pointer_emulation.last_touch.map_or("unset", |m| m.name()),
                data.pointer_emulation.touch_downs,
            );
            let _ = response.send(payload);
        }
    })?;

    // --- Source 8: Surface lifecycle events from Activities ---
    loop_handle.insert_source(surface_event_channel, move |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };
        handle_surface_event(&data.loop_handle(), data, evt);
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after surface event: {}", e);
        }
    })?;

    // --- Source 9: Vsync ticks ---
    // Armed on demand by `after_dispatch`; see vsync.rs. Each tick renders
    // the visible host and sends frame callbacks. Housekeeping lives in
    // `after_dispatch` and the slow timer below.
    let (vsync, vsync_channel) = crate::vsync::Vsync::spawn()?;
    loop_handle.insert_source(vsync_channel, |event, _, data: &mut TawcState| {
        if let ChannelEvent::Msg(VsyncEvent { time_ns }) = event {
            frame_tick(data, time_ns);
        }
    })?;

    // --- Source 10: Slow housekeeping timer ---
    // Idle auto-stop and Xwayland start retries are time-based, so they
    // can't wait for an event.
    loop_handle.insert_source(
        Timer::from_duration(HOUSEKEEPING_PERIOD),
        |_, _, data: &mut TawcState| {
            check_idle(data);
            TimeoutAction::ToDuration(HOUSEKEEPING_PERIOD)
        },
    )?;

    // Spawn Xwayland (best-effort: failure logs and continues — the
    // Wayland-only subset of the compositor still works without it).
    let initial_xwayland = state.xwayland_enabled;
    crate::xwayland::set_enabled(&loop_handle, &mut state, initial_xwayland);

    // Accept on a clone of the process-lifetime listener (activation.rs).
    // Clients that connected before now have been waiting in its backlog;
    // inserted last so they are served by a fully wired loop.
    let listener = crate::activation::wayland_listener()?;
    let listener_source = Generic::new(listener, Interest::READ, Mode::Level);
    let listener_token = loop_handle.insert_source(listener_source, |_, listener, data: &mut TawcState| {
        loop {
            let stream = match listener.accept() {
                Ok((stream, _)) => stream,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => {
                    error!("Wayland accept failed: {}", e);
                    break;
                }
            };
            let client_state = ClientState::new(data.client_count.clone(), data.client_ids.clone());
            if let Err(e) = data
                .display_handle
                .insert_client(stream, Arc::new(client_state))
            {
                error!("Failed to insert client: {}", e);
            }
        }
        Ok(PostAction::Continue)
    })?;

    *LOOP_SIGNAL.lock().unwrap() = Some(event_loop.get_signal());
    info!("Entering calloop event loop");

    let mut loop_result: Result<(), Box<dyn std::error::Error>> = Ok(());
    while running.load(std::sync::atomic::Ordering::SeqCst) {
        if let Err(e) = event_loop.dispatch(None, &mut state) {
            loop_result = Err(e.into());
            break;
        }
        after_dispatch(&mut state, &vsync);
    }
    *LOOP_SIGNAL.lock().unwrap() = None;
    drop(vsync);

    // Dropping `event_loop` does not reliably drop its sources: any
    // callback still holding a LoopHandle keeps the source list alive in
    // an Rc cycle. Remove our listener clone explicitly so a stopped
    // compositor can never steal a connection from the next one.
    loop_handle.remove(listener_token);
    crate::xwayland::release_activation_socket(&loop_handle, &mut state);

    info!("Event loop exited after {} frames", state.frame_count);
    crate::clear_senders();
    // State first: its teardown (client disconnects, X11Wm source removal)
    // still talks to the loop.
    drop(state);
    drop(event_loop);
    loop_result
}

/// Wakes the running loop so it re-checks `running`. Set while
/// `run` is inside its dispatch loop.
static LOOP_SIGNAL: std::sync::Mutex<Option<LoopSignal>> = std::sync::Mutex::new(None);

/// Wake the event loop from another thread (compositor stop).
pub fn wake() {
    if let Some(signal) = LOOP_SIGNAL.lock().unwrap().as_ref() {
        signal.wakeup();
    }
}

/// Period of the slow timer for time-based housekeeping.
const HOUSEKEEPING_PERIOD: Duration = Duration::from_millis(250);

/// Event-driven housekeeping, run after every dispatch so code that sets
/// `needs_render` or `toplevels_changed` doesn't have to arm anything.
/// Ends by arming a vsync tick when there's a frame to draw.
fn after_dispatch(data: &mut TawcState, vsync: &crate::vsync::Vsync) {
    // Activities for hosts that kept their window through the dispatch;
    // the rest were dropped by `finish_host`.
    for host in std::mem::take(&mut data.pending_activity_spawns) {
        crate::spawn_activity_from_native(&host);
    }

    crate::xwayland::service_pending(&data.loop_handle(), data);

    // Catch up on XWayland surface ↔ host associations that can land
    // after the first wl_surface commit.
    if crate::xwayland::associate_pending_x11_surfaces(data) {
        data.needs_render = true;
    }

    // Smithay owns xdg toplevel lifetime and calls our `toplevel_destroyed`
    // handler; this only prunes stale desktop windows/assignments for
    // surfaces that disappeared outside that path.
    data.desktop.retain_live_windows();
    data.sync_desktop_hosts();

    // Cap the rendered-toplevels counter at the live count: once a host
    // has been torn down, no further frames render, and otherwise
    // last_rendered_toplevels would stay frozen at its peak value (so
    // waiters for "compositor went idle" — assert_compositor_clean,
    // wait_for_rendered_toplevels(0) — never see it return to 0).
    let live_toplevels = toplevel_count(data);
    if data.last_rendered_toplevels > live_toplevels {
        data.last_rendered_toplevels = live_toplevels;
    }

    data.desktop.retain_live_assignments();

    data.popup_manager.cleanup();
    if data
        .active_popup_grab
        .as_ref()
        .is_some_and(|grab| grab.has_ended())
    {
        data.active_popup_grab = None;
    }
    let focused_text_surface = data.text_input_state.focused_surface.clone();
    let focused_activity_id = focused_text_surface
        .as_ref()
        .and_then(|surface| data.desktop.host_for_surface(surface));
    data.text_input_state.cleanup(focused_activity_id.as_ref());

    // New or dead toplevels need a repaint and a focus update. Both
    // focuses move together: a dead focused surface would otherwise leave
    // the keyboard pointed at it until the next FocusChanged event.
    if std::mem::take(&mut data.toplevels_changed) {
        data.needs_render = true;
        crate::update_toplevel_count_from_native(client_toplevel_count(data));
        let new_focus = data
            .desktop_visible_host_id()
            .and_then(|host| data.first_toplevel_for_host(&host));
        data.set_input_focus(new_focus.as_ref());
    }

    // Focus changes above queue enter/leave events.
    if let Err(e) = data.display_handle.flush_clients() {
        error!("flush_clients error: {}", e);
    }

    if (data.needs_render || data.frame_callbacks_pending) && visible_host_renderable(data) {
        vsync.request();
    }
}

/// One vsync tick: send frame callbacks, then render the visible host if
/// dirty (which answers presentation feedback), all stamped with the vsync
/// time.
fn frame_tick(data: &mut TawcState, time_ns: i64) {
    data.frame_clock.tick(time_ns, data.output_refresh_period());
    let time = Duration::from_nanos(time_ns.max(0) as u64);

    // Frame callbacks first, flushed before rendering, so clients get the
    // whole period to draw the next frame instead of losing render+swap
    // time. Commits they send in reply are only dispatched after this
    // tick, so this frame's content can't change under the render. Sent
    // even when nothing renders, so visible clients that committed without
    // new content keep animating.
    data.frame_callbacks_pending = false;
    render::send_frame_callbacks(data, time);
    if let Err(e) = data.display_handle.flush_clients() {
        error!("flush_clients error in frame tick: {}", e);
    }

    // Only the foreground bound host renders. Background hosts neither
    // render nor get frame callbacks; hidden commits can mark the
    // compositor dirty without triggering hidden texture imports.
    if data.needs_render && render_visible_host(data) {
        data.needs_render = false;
        // Presentation feedback and wl_buffer.release from the render.
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error in frame tick: {}", e);
        }
    }
    data.frame_clock.tick_done(monotonic_now().as_nanos() as i64);
}

fn monotonic_now() -> Duration {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: clock_gettime only writes the timespec.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32)
}

fn visible_host_renderable(data: &TawcState) -> bool {
    data.desktop_visible_host_id()
        .and_then(|id| data.hosts.get(&id))
        .is_some_and(|host| host.egl_surface.is_some())
}

/// How long nothing may be connected before the compositor stops. Not
/// zero: app startup often has short-lived helper connections before the
/// real one, and scripts call `wl-copy`/`wl-paste` in bursts.
const IDLE_GRACE: Duration = Duration::from_secs(1);

/// Auto-stop: with no Wayland client and no Xwayland (which exits by
/// itself 5 s after its last X client) for [`IDLE_GRACE`], leave the
/// loop. The activation holder restarts us on the next connection.
fn check_idle(data: &mut TawcState) {
    let xwayland_running = data.xwayland_source.is_some() || data.xwm.is_some();
    let clients = data.client_count.load(std::sync::atomic::Ordering::Relaxed);

    // A client that only serves a selection (wl-copy's daemon) never
    // leaves by itself. Once its text is in Android's clipboard, take the
    // selection over: it gets `cancelled` and exits, and pastes are served
    // from Android. An unmirrored selection (non-text, over the cap) keeps
    // its owner, and with it the compositor — it is the only copy.
    if clients > 0 && !xwayland_running && data.surface_count == 0 && data.selection_mirrored {
        info!("clipboard: taking over mirrored selection from surfaceless client");
        crate::clipboard::install_android_selection(data);
    }

    // A reserved launch host waits for a client that may not have
    // connected yet; its splash Activity releases it when closed.
    if clients > 0 || xwayland_running || !data.pending_launches.is_empty() {
        data.idle_since = None;
        return;
    }
    let since = *data.idle_since.get_or_insert_with(std::time::Instant::now);
    if since.elapsed() >= IDLE_GRACE {
        if crate::stop_if_unpinned() {
            info!("No clients for {:?}; stopping compositor", IDLE_GRACE);
        }
        data.idle_since = None;
    }
}

fn render_visible_host(data: &mut TawcState) -> bool {
    let Some(id) = data.desktop_visible_host_id() else {
        return false;
    };
    let Some(host) = data.hosts.get(&id) else {
        return false;
    };
    if host.egl_surface.is_none() {
        return false;
    }

    // Take the host out of the map so render_frame can hold a
    // `&mut OutputHost` while still passing `&mut TawcState`.
    let Some(mut host) = data.hosts.remove(&id) else {
        return false;
    };
    let rendered = match render::render_frame(data, &mut host) {
        Ok(()) => true,
        Err(e) => {
            error!("Render error on host {}: {}", id, e);
            false
        }
    };
    data.hosts.insert(id.clone(), host);

    if rendered {
        data.frame_count += 1;
        data.last_rendered_toplevels = toplevel_count(data);
        // Stamped with the last vsync: direct renders (host register and
        // resize) happen between ticks, and their feedback must not wait
        // for the next render.
        let time = match data.frame_clock.last_vsync_ns {
            0 => monotonic_now(),
            ns => Duration::from_nanos(ns as u64),
        };
        render::report_presentation_feedback(data, time);
        if data.launch_first_frame.contains(&id) && data.launch_host_has_content(&id) {
            data.launch_first_frame.remove(&id);
            crate::launch_shown_from_native(&id);
        }
    }
    rendered
}

/// The visible host's placements for `query-state`, back to front:
/// `<surface id>:<role>:<gx>,<gy>,<gw>,<gh>:<offset x>,<offset y>:<scale>`
/// joined by `;` (`-` for none), and whether a scrim is drawn.
fn visible_layout_debug(data: &TawcState) -> (String, &'static str) {
    let Some(layout) = data
        .desktop_visible_host_id()
        .and_then(|host| data.host_layout(&host))
    else {
        return ("-".to_string(), "no");
    };
    let windows = layout
        .entries
        .iter()
        .map(|entry| {
            let id = entry
                .window
                .wl_surface()
                .map_or(0, |surface| surface.id().protocol_id());
            let g = entry.window.geometry();
            let p = entry.placement;
            format!(
                "{}:{}:{},{},{},{}:{:.2},{:.2}:{:.4}",
                id,
                entry.role.name(),
                g.loc.x,
                g.loc.y,
                g.size.w,
                g.size.h,
                p.offset.x,
                p.offset.y,
                p.scale,
            )
        })
        .collect::<Vec<_>>();
    let windows = if windows.is_empty() { "-".to_string() } else { windows.join(";") };
    (windows, if layout.scrim_below.is_some() { "yes" } else { "no" })
}

fn toplevel_count(data: &TawcState) -> usize {
    data.xdg_shell_state.toplevel_surfaces().len()
}

fn client_toplevel_count(data: &TawcState) -> usize {
    toplevel_count(data) + data.x11_surfaces.len()
}

// ---------------------------------------------------------------------------
// Surface event handling (per-Activity SurfaceView lifecycle)
// ---------------------------------------------------------------------------

fn handle_surface_event(
    loop_handle: &LoopHandle<'static, TawcState>,
    data: &mut TawcState,
    evt: SurfaceEvent,
) {
    match evt {
        SurfaceEvent::Register { activity_id, native_window, width, height } => {
            let nw = native_window as *mut c_void;
            let scale = data.output_scale;
            // If this Activity already has a host, replace its native_window
            // (Activity recreated, e.g. after rotation). Otherwise create a
            // fresh host record.
            match data.hosts.get_mut(&activity_id) {
                Some(host) => host.replace_native_window(nw, width, height, scale),
                None => {
                    let host = OutputHost::new(activity_id.clone(), nw, width, height, scale);
                    data.hosts.insert(activity_id.clone(), host);
                }
            }
            // Bind the EGLSurface (separate step: needs &RenderState).
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                if let Some(render) = data.render.get() {
                    render.attach_host_surface(host);
                }
            }
            let fullscreen = data.host_fullscreen(&activity_id);
            let foreground = data.desktop.foreground_host() == Some(&activity_id);
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                host.fullscreen = fullscreen;
                host.foreground = foreground;
            }
            crate::set_activity_fullscreen_from_native(&activity_id, fullscreen);
            data.sync_advertised_output_to_host_if_visible(&activity_id);
            data.sync_desktop_hosts();
            // Reconfigure existing toplevels with the new logical size.
            reconfigure_all_toplevels(data);
            data.needs_render = true;
            info!(
                "Host registered: {} ({}x{}) — bound={}, total hosts={}",
                activity_id, width, height,
                data.hosts.get(&activity_id).map(|h| h.egl_surface.is_some()).unwrap_or(false),
                data.hosts.len(),
            );
            if render_visible_host(data) {
                data.needs_render = false;
            }
        }
        SurfaceEvent::SurfaceChanged { activity_id, width, height } => {
            let scale = data.output_scale;
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                host.update_size(width, height, scale);
            } else {
                info!("SurfaceChanged for unknown host {}", activity_id);
                return;
            }
            data.sync_advertised_output_to_host_if_visible(&activity_id);
            data.sync_desktop_hosts();
            reconfigure_all_toplevels(data);
            data.needs_render = true;
            if render_visible_host(data) {
                data.needs_render = false;
            }
        }
        SurfaceEvent::SurfaceDestroyed { activity_id } => {
            clear_pointer_focus_for_host(data, &activity_id);
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                host.drop_surface();
                info!("Host {} surface dropped (record retained)", activity_id);
            }
        }
        SurfaceEvent::ActivityDestroyed { activity_id } => {
            clear_pointer_focus_for_host(data, &activity_id);
            data.hardware_keys_down
                .retain(|(host, _)| host != &activity_id);
            // Ask every window assigned to this host to close. Well-behaved
            // clients then destroy/unmap their surfaces; the cleanup paths
            // remove the remaining assignments on later events.
            // (Phase 7 polish: handle clients that refuse to close.)
            let closed = data.request_close_windows_for_host(&activity_id);
            if data.hosts.remove(&activity_id).is_some() {
                info!("Host {} removed (closed {} windows)", activity_id, closed);
            }
            data.host_fullscreen.remove(&activity_id);
            data.window_metadata.remove(&activity_id);
            data.launch_first_frame.remove(&activity_id);
            data.set_host_pointer_emulation(&activity_id, None);
            data.desktop.clear_foreground_host_if(&activity_id);
            if data.advertised_output_host.as_ref() == Some(&activity_id) {
                data.advertised_output_host = None;
            }
            data.sync_desktop_hosts();
            data.toplevels_changed = true;
        }
        SurfaceEvent::FocusChanged { activity_id, has_focus } => {
            // Update host state + send Activated/Suspended configures.
            set_host_foreground(data, &activity_id, has_focus);
            // Refresh wider TawcState bookkeeping (foreground_host pointer,
            // keyboard / text input focus).
            if has_focus {
                data.desktop.set_foreground_host(Some(activity_id.clone()));
                data.sync_advertised_output_to_host_if_visible(&activity_id);
                let target = data.first_toplevel_for_host(&activity_id);
                data.set_input_focus(target.as_ref());
                data.needs_render = true;
            } else if data.desktop.foreground_host() == Some(&activity_id) {
                data.hardware_keys_down
                    .retain(|(host, _)| host != &activity_id);
                clear_pointer_focus(data);
                data.desktop.set_foreground_host(None);
                data.set_input_focus(None);
            }
            data.sync_desktop_hosts();
        }
        SurfaceEvent::OutputScaleChanged { scale } => {
            apply_output_scale(data, OutputScale::new(scale));
        }
        SurfaceEvent::OutputRefreshChanged { mhz } => {
            data.set_output_refresh_mhz(mhz);
        }
        SurfaceEvent::XwaylandChanged { enabled } => {
            crate::xwayland::set_enabled(loop_handle, data, enabled);
        }
        SurfaceEvent::PointerEmulationChanged { mode } => {
            data.set_pointer_emulation(mode);
        }
        SurfaceEvent::MouseAttachedChanged { attached } => {
            if data.mouse_attached != attached {
                data.mouse_attached = attached;
                if !attached {
                    clear_pointer_focus(data);
                }
                data.sync_pointer_capability();
            }
        }
        SurfaceEvent::FullscreenChanged { activity_id, fullscreen } => {
            data.set_host_fullscreen(&activity_id, fullscreen);
            data.needs_render = true;
        }
        SurfaceEvent::BackPressed { activity_id } => {
            handle_back_pressed(data, &activity_id);
        }
        SurfaceEvent::HardwareKey { activity_id, evdev_keycode, pressed, repeat_count } => {
            handle_hardware_key(data, &activity_id, evdev_keycode, pressed, repeat_count);
        }
        SurfaceEvent::ReserveLaunch { launch_id, desktop_id, pointer_emulation, response } => {
            let _ = response.send(data.reserve_launch(launch_id, desktop_id, pointer_emulation));
        }
        SurfaceEvent::UpdateLaunch { launch_id, sid, exited } => {
            data.update_launch(&launch_id, sid, exited);
        }
        SurfaceEvent::ReleaseLaunch { launch_id } => {
            data.release_launch(&launch_id);
        }
        SurfaceEvent::CloseAllClientsForTest { response } => {
            let closed = data.request_close_all_client_windows_for_test();
            let _ = response.send(closed);
        }
    }
}

fn apply_output_scale(state: &mut TawcState, scale: OutputScale) {
    if state.output_scale == scale {
        return;
    }

    state.output_scale = scale;
    for host in state.hosts.values_mut() {
        host.update_scale(scale);
    }

    if let Some(host_id) = state
        .desktop
        .foreground_host()
        .cloned()
        .or_else(|| state.advertised_output_host.clone())
    {
        state.sync_advertised_output_to_host_if_visible(&host_id);
    } else {
        // No host yet: keep the provisional startup mode, re-derived at
        // the new scale.
        state.set_output_mode(state.output_physical_size);
    }
    state.sync_desktop_hosts();

    for surface in live_surfaces(state) {
        state.send_surface_scale(&surface);
    }
    reconfigure_all_toplevels(state);
    state.needs_render = true;
    info!(
        "Output scale changed: {:.2} logical={}x{}",
        scale.fractional(),
        state.output_logical_size.0,
        state.output_logical_size.1,
    );
}

fn live_surfaces(state: &TawcState) -> Vec<WlSurface> {
    let mut surfaces = Vec::new();
    for window in state.desktop.windows() {
        window.with_surfaces(|surface, _| {
            if surface.is_alive() && !surfaces.iter().any(|s: &WlSurface| s == surface) {
                surfaces.push(surface.clone());
            }
        });
    }
    surfaces
}

/// Flip a host's foreground state and notify assigned toplevels via
/// `Activated`/`Suspended` configure events. Only sends a configure when
/// the pending state actually changed — Vulkan WSI clients (vkcube) hang
/// after recreating their swapchain on a redundant Activated configure
/// that arrives mid-frame, so we go through `send_pending_configure`
/// rather than the unconditional `send_configure` and skip the no-op
/// case where the state already matched.
fn set_host_foreground(state: &mut TawcState, host_id: &crate::host::ActivityId, foreground: bool) {
    let host_ready = state.host_logical_size(host_id).is_some();
    for t in state.wayland_toplevels_for_host(host_id) {
        set_toplevel_activated(&t, foreground);
        if host_ready {
            t.send_pending_configure();
        }
    }
    if let Some(host) = state.hosts.get_mut(host_id) {
        host.foreground = foreground;
    }
}

/// Pending `Activated`/`Suspended` state for a toplevel on a foreground
/// or background host.
pub fn set_toplevel_activated(toplevel: &smithay::wayland::shell::xdg::ToplevelSurface, foreground: bool) {
    use wayland_protocols::xdg::shell::server::xdg_toplevel::State as XdgState;

    toplevel.with_pending_state(|s| {
        if foreground {
            s.states.set(XdgState::Activated);
            s.states.unset(XdgState::Suspended);
        } else {
            s.states.unset(XdgState::Activated);
            // xdg-shell v6 introduced `Suspended`. Smithay only emits
            // it to clients on protocol version >= 6; for older
            // clients the unset Activated is the signal.
            s.states.set(XdgState::Suspended);
        }
    });
}

fn reconfigure_all_toplevels(state: &mut TawcState) {
    // Each toplevel uses its own host's real SurfaceView size. If the
    // Activity has not registered yet, leave the configure pending rather
    // than sending a service-side display-size guess or configure(0,0).
    //
    // Going through
    // `send_pending_configure` keeps us from re-sending an identical
    // configure when nothing changed (e.g. Register and SurfaceChanged
    // arrive back-to-back with the same dimensions): vkcube's Vulkan WSI
    // wedges if it sees a duplicate configure between its first and
    // second commit.
    let toplevels = state.xdg_shell_state.toplevel_surfaces().to_vec();
    for toplevel in &toplevels {
        let Some(host_id) = state
            .desktop
            .assigned_host(toplevel.wl_surface())
        else {
            continue;
        };
        if state.configure_toplevel_for_host(toplevel, host_id).is_some() {
            toplevel.send_pending_configure();
        }
    }

    crate::xwayland::configure_x11_toplevels_for_hosts(state);
}
