//! Calloop-based event loop for the compositor.
//!
//! Integrates the Wayland display, client listener, frame timer and
//! per-Activity surface lifecycle into a single calloop event loop.
//! All `OutputHost` mutation happens here on the compositor thread —
//! JNI threads send events through channels.

use std::ffi::c_void;
use std::os::unix::fs::PermissionsExt;
use std::sync::Arc;
use std::time::Duration;

use log::{error, info};
use smithay::reexports::wayland_server::Resource;

use smithay::backend::input::TouchSlot;
use smithay::backend::renderer::{buffer_type, BufferType};
use smithay::backend::input::KeyState;
use smithay::desktop::PopupManager;
use smithay::desktop::PopupUngrabStrategy;
use smithay::input::keyboard::{FilterResult, Keycode};
use smithay::input::touch::{DownEvent, MotionEvent, UpEvent};
use smithay::reexports::calloop::channel::{Channel, Event as ChannelEvent};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{EventLoop, Interest, Mode, PostAction};
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::utils::{Logical, Point, SERIAL_COUNTER};
use smithay::wayland::compositor::{
    get_parent, get_role, with_states, with_surface_tree_downward, BufferAssignment,
    SubsurfaceCachedState, SurfaceAttributes, SurfaceData, TraversalAction, SUBSURFACE_ROLE,
};
use smithay::wayland::shell::xdg::{SurfaceCachedState, XDG_POPUP_ROLE};
use wayland_server::{Display, ListeningSocket};

use crate::host::{ActivityId, OutputHost, SurfaceEvent};
use crate::input::TouchEvent;
use crate::scale::OutputScale;
use crate::text_input::TextInputEvent;
use crate::clipboard::ClipboardEvent;

use crate::compositor::{ClientState, TawcState};
use crate::render;

struct HitSurface {
    surface: WlSurface,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}

enum KeyboardFocusAction {
    Set(WlSurface),
    Keep,
    Clear,
}

struct TouchResolution {
    touch_focus: Option<(WlSurface, Point<f64, Logical>)>,
    popup_focus: Option<WlSurface>,
    keyboard_focus: KeyboardFocusAction,
}

fn xdg_window_geometry_loc(surface: &WlSurface) -> Point<i32, Logical> {
    with_states(surface, |states| {
        states
            .cached_state
            .get::<SurfaceCachedState>()
            .current()
            .geometry
            .map(|geometry| geometry.loc)
            .unwrap_or_default()
    })
}

fn surface_logical_size_from_states(
    data: &TawcState,
    surface: &WlSurface,
    states: &SurfaceData,
) -> Option<(i32, i32)> {
    if let Some(ws) = data.surface_wlegl.get(surface) {
        return Some(render::logical_size(
            ws.committed_width,
            ws.committed_height,
            ws.buffer_scale,
            ws.viewport_dst,
        ));
    }

    if let Some(ss) = data.surface_shm.get(surface) {
        return Some(render::logical_size(
            ss.committed_width,
            ss.committed_height,
            ss.buffer_scale,
            ss.viewport_dst,
        ));
    }

    let mut guard = states.cached_state.get::<SurfaceAttributes>();
    let attrs = guard.current();
    let buffer_scale = attrs.buffer_scale.max(1);
    let buffer = match &attrs.buffer {
        Some(BufferAssignment::NewBuffer(buf))
            if matches!(buffer_type(buf), Some(BufferType::Shm)) =>
        {
            Some(buf.clone())
        }
        _ => None,
    };
    drop(guard);

    let mut vp_guard = states
        .cached_state
        .get::<smithay::wayland::viewporter::ViewportCachedState>();
    let viewport_dst = vp_guard.current().dst.map(|s| (s.w, s.h));
    let buffer = buffer?;
    let dims = smithay::wayland::shm::with_buffer_contents(&buffer, |_, _, data| {
        (data.width, data.height)
    });
    dims.ok()
        .map(|(w, h)| render::logical_size(w, h, buffer_scale, viewport_dst))
}

fn collect_tree_hits(
    data: &TawcState,
    root: &WlSurface,
    offset_x: i32,
    offset_y: i32,
) -> Vec<HitSurface> {
    let mut hits = Vec::new();
    with_surface_tree_downward(
        root,
        (offset_x, offset_y),
        |_surf, states, &(px, py)| {
            let loc = states
                .cached_state
                .get::<SubsurfaceCachedState>()
                .current()
                .location;
            TraversalAction::DoChildren((px + loc.x, py + loc.y))
        },
        |surf, states, &(base_x, base_y)| {
            if let Some((w, h)) = surface_logical_size_from_states(data, surf, states) {
                let loc = states
                    .cached_state
                    .get::<SubsurfaceCachedState>()
                    .current()
                    .location;
                hits.push(HitSurface {
                    surface: surf.clone(),
                    x: base_x + loc.x,
                    y: base_y + loc.y,
                    w,
                    h,
                });
            }
        },
        |_, _, _| true,
    );
    hits.reverse();
    hits
}

fn touch_focus_at(
    data: &TawcState,
    activity_id: &ActivityId,
    location: Point<f64, Logical>,
) -> Option<(WlSurface, Point<f64, Logical>)> {
    let mut hits = Vec::new();

    for toplevel in &data.toplevels {
        let root = toplevel.wl_surface();
        match data.toplevel_to_host.get(root) {
            Some(assigned) if assigned == activity_id => {}
            _ => continue,
        }

        hits.extend(collect_tree_hits(data, root, 0, 0));
        let parent_geometry_loc = xdg_window_geometry_loc(root);
        for (popup, location) in PopupManager::popups_for_surface(root) {
            let popup_geometry_loc = popup.geometry().loc;
            let popup_offset = parent_geometry_loc + location - popup_geometry_loc;
            hits.extend(collect_tree_hits(
                data,
                popup.wl_surface(),
                popup_offset.x,
                popup_offset.y,
            ));
        }
    }

    hits.into_iter().rev().find_map(|hit| {
        let within_x = location.x >= hit.x as f64 && location.x < (hit.x + hit.w) as f64;
        let within_y = location.y >= hit.y as f64 && location.y < (hit.y + hit.h) as f64;
        let origin = Point::from((hit.x as f64, hit.y as f64));
        if within_x && within_y && surface_accepts_touch_at(&hit.surface, location - origin) {
            Some((hit.surface, origin))
        } else {
            None
        }
    })
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
    location: Point<f64, Logical>,
) -> TouchResolution {
    let touch_focus = touch_focus_at(data, activity_id, location);
    let popup_focus = touch_focus.as_ref().map(|(surface, _)| surface.clone());
    let keyboard_focus = match touch_focus.as_ref().map(|(surface, _)| surface) {
        Some(surface) if is_in_xdg_popup_tree(surface) => KeyboardFocusAction::Keep,
        Some(surface) => KeyboardFocusAction::Set(main_surface_for_subsurface_tree(surface)),
        None => KeyboardFocusAction::Clear,
    };

    TouchResolution {
        touch_focus,
        popup_focus,
        keyboard_focus,
    }
}

fn apply_keyboard_focus_action(data: &mut TawcState, action: KeyboardFocusAction) {
    match action {
        KeyboardFocusAction::Set(surface) => data.set_input_focus(Some(&surface)),
        KeyboardFocusAction::Keep => {}
        KeyboardFocusAction::Clear => data.set_input_focus(None),
    }
}

fn host_for_surface(data: &TawcState, surface: &WlSurface) -> Option<ActivityId> {
    let mut current = Some(surface.clone());
    while let Some(surface) = current {
        if let Some(host) = data.toplevel_to_host.get(&surface) {
            return Some(host.clone());
        }
        current = get_parent(&surface);
    }

    for (root, host) in &data.toplevel_to_host {
        for (popup, _) in PopupManager::popups_for_surface(root) {
            if popup.wl_surface() == surface {
                return Some(host.clone());
            }
        }
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
    if data.foreground_host.as_ref() != Some(activity_id) || !data.hosts.contains_key(activity_id) {
        info!("Ignoring BackPressed for non-foreground/unknown host {}", activity_id);
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

    send_keyboard_key_press(data, crate::keymap::EVDEV_KEY_ESC);
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
        .toplevels
        .iter()
        .map(|t| t.wl_surface())
        .filter(|root| {
            data.toplevel_to_host
                .get(*root)
                .is_some_and(|assigned| assigned == activity_id)
        })
        .cloned()
        .collect();

    for root in roots {
        if let Some((popup, _)) = PopupManager::popups_for_surface(&root).next() {
            if PopupManager::dismiss_popup(&root, &popup).is_ok() {
                data.needs_render = true;
            }
        }
    }
}

fn surface_accepts_touch_at(surface: &WlSurface, local: Point<f64, Logical>) -> bool {
    with_states(surface, |states| {
        let mut guard = states.cached_state.get::<SurfaceAttributes>();
        let attrs = guard.current();
        attrs
            .input_region
            .as_ref()
            .map(|region| region.contains(local.to_i32_round()))
            .unwrap_or(true)
    })
}

/// Set up and run the calloop event loop. Returns when `running` becomes false.
///
/// `socket_path` is bound and inserted as a calloop source as the last
/// step before entering the dispatch loop — see the comment at the
/// caller in `lib.rs::run_compositor` for why we don't bind earlier.
#[allow(clippy::too_many_arguments)]
pub fn run(
    display: Display<TawcState>,
    mut state: TawcState,
    socket_path: &str,
    touch_channel: Channel<TouchEvent>,
    text_input_channel: Channel<TextInputEvent>,
    clipboard_channel: Channel<ClipboardEvent>,
    state_query_channel: Channel<()>,
    surface_event_channel: Channel<SurfaceEvent>,
    running: &std::sync::atomic::AtomicBool,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut event_loop: EventLoop<TawcState> = EventLoop::try_new()?;
    let loop_handle = event_loop.handle();

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

    // --- Source 2: deferred ---
    // The listening socket is bound and inserted just before
    // `event_loop.dispatch` starts (see end of this function). Binding
    // earlier would let clients `connect()` while we're still wiring up
    // sources, leaving their initial roundtrip stuck in the listen
    // backlog and exceeding GTK4's connect timeout on slow hosts.

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

        let touch = match data.seat.get_touch() {
            Some(t) => t,
            None => return,
        };

        // Identify the touch's host and the surface under the event. Touch
        // focus stores the surface origin in compositor space; Smithay
        // subtracts it before sending surface-local wl_touch coordinates.
        let activity_id = match &evt {
            TouchEvent::Down { activity_id, .. }
            | TouchEvent::Motion { activity_id, .. }
            | TouchEvent::Up { activity_id, .. } => activity_id.clone(),
        };

        let touch_scale = data.output_scale;
        let serial = SERIAL_COUNTER.next_serial();

        match evt {
            TouchEvent::Down { id, x, y, time, .. } => {
                let location: Point<f64, smithay::utils::Logical> =
                    (touch_scale.logical_coord(x as f64), touch_scale.logical_coord(y as f64)).into();
                let touch_resolution = resolve_touch_down(data, &activity_id, location);
                dismiss_host_popups_if_touch_is_outside_popup(
                    data,
                    &activity_id,
                    touch_resolution.popup_focus.as_ref(),
                    serial,
                    time,
                );
                // Touch chooses the input target, but keyboard/text-input
                // focus follows Wayland role policy. In particular,
                // wl_subsurface targets focus their main surface, and
                // non-grabbed xdg_popup touches leave keyboard focus alone.
                // Do not speculatively commit preedit here: a touch may
                // scroll, hit a button, or be ignored. If the client really
                // moves the cursor, its following
                // set_surrounding_text(cause=other) drives preedit cleanup.
                apply_keyboard_focus_action(data, touch_resolution.keyboard_focus);
                touch.down(
                    data,
                    touch_resolution.touch_focus,
                    &DownEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        location,
                        serial,
                        time,
                    },
                );
                touch.frame(data);
            }
            TouchEvent::Motion { id, x, y, time, .. } => {
                let location: Point<f64, smithay::utils::Logical> =
                    (touch_scale.logical_coord(x as f64), touch_scale.logical_coord(y as f64)).into();
                let focus = touch_focus_at(data, &activity_id, location);
                touch.motion(
                    data,
                    focus,
                    &MotionEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        location,
                        time,
                    },
                );
                touch.frame(data);
            }
            TouchEvent::Up { id, time, .. } => {
                touch.up(
                    data,
                    &UpEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        serial,
                        time,
                    },
                );
                touch.frame(data);
            }
        }

        // Flush immediately so clients see events without waiting for frame timer
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after touch: {}", e);
        }
    })?;

    // --- Source 4: Android clipboard channel ---
    //
    // Kotlin listens to Android's real ClipboardManager and forwards text
    // changes here. Client-owned Wayland selections are pulled eagerly only
    // after Smithay has installed them in seat state; the SelectionHandler
    // queues PullWaylandSelection for this source to perform that deferred
    // request.
    loop_handle.insert_source(clipboard_channel, |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        match evt {
            ClipboardEvent::AndroidText(text) => {
                smithay::wayland::selection::data_device::set_data_device_selection(
                    &data.display_handle,
                    &data.seat,
                    crate::clipboard::text_mime_types(),
                    crate::clipboard::SelectionUserData::AndroidText(text),
                );
                if let Some(xwm) = data.xwm.as_mut() {
                    if let Err(e) = xwm.new_selection(
                        smithay::wayland::selection::SelectionTarget::Clipboard,
                        Some(crate::clipboard::text_mime_types()),
                    ) {
                        log::warn!("clipboard: failed to notify XWayland of Android selection: {:?}", e);
                    }
                }
                if let Err(e) = data.display_handle.flush_clients() {
                    error!("flush_clients error after Android clipboard update: {}", e);
                }
            }
            ClipboardEvent::PullWaylandSelection { mime_type } => {
                log::debug!("clipboard: requesting Wayland clipboard as {}", mime_type);
                let (read_fd, write_fd) = match crate::clipboard::pipe() {
                    Ok(fds) => fds,
                    Err(e) => {
                        log::warn!("clipboard: pipe failed for Wayland pull: {}", e);
                        return;
                    }
                };
                match smithay::wayland::selection::data_device::request_data_device_client_selection(
                    &data.seat,
                    mime_type,
                    write_fd,
                ) {
                    Ok(()) => {
                        if let Err(e) = data.display_handle.flush_clients() {
                            error!("flush_clients error after Wayland clipboard request: {}", e);
                        }
                        crate::clipboard::read_fd_for_android(read_fd, "wayland");
                    }
                    Err(e) => log::debug!("clipboard: Wayland pull skipped: {:?}", e),
                }
            }
        }
    })?;

    // --- Source 5: Text input channel ---
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
                if let Some(keyboard) = data.seat.get_keyboard() {
                    let serial = SERIAL_COUNTER.next_serial();
                    let time = data.start_time.elapsed().as_millis() as u32;
                    let keycode = Keycode::from(keycode + 8); // evdev → XKB offset
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
            _ => {
                data.text_input_state.handle_android_event(evt);
            }
        }

        // Flush so clients see text input events immediately
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after text input: {}", e);
        }
    })?;

    // --- Source 5: State query channel ---
    // Receives requests to log compositor state (from QUERY_STATE broadcast).
    loop_handle.insert_source(state_query_channel, move |event, _, data: &mut TawcState| {
        if let ChannelEvent::Msg(()) = event {
            let clients = data.client_count.load(std::sync::atomic::Ordering::Relaxed);
            let bound_hosts = data
                .hosts
                .values()
                .filter(|h| h.egl_surface.is_some())
                .count();
            info!(
                "COMPOSITOR_STATE: clients={} toplevels={} surfaces_wlegl={} surfaces_shm={} frames={} rendered_toplevels={} hosts={} bound_hosts={} output_scale={:.2} output_physical_w={} output_physical_h={} output_logical_w={} output_logical_h={} output_advertised={}",
                clients,
                data.toplevels.len(),
                data.surface_wlegl.len(),
                data.surface_shm.len(),
                data.frame_count,
                data.last_rendered_toplevels,
                data.hosts.len(),
                bound_hosts,
                data.output_scale.fractional(),
                data.output_physical_size.0,
                data.output_physical_size.1,
                data.output_logical_size.0,
                data.output_logical_size.1,
                data.output_advertised,
            );
        }
    })?;

    // --- Source 6: Surface lifecycle events from Activities ---
    loop_handle.insert_source(surface_event_channel, |event, _, data: &mut TawcState| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };
        handle_surface_event(data, evt);
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error after surface event: {}", e);
        }
    })?;

    // --- Source 7: Frame timer (~60 fps) ---
    // This drives the render loop. Each tick:
    //   1. Import new buffers (AHB and SHM) as GL textures
    //   2. Render the frame for each bound host
    //   3. Send frame-done callbacks
    //   4. Flush outgoing events to clients
    //   5. Clean up dead toplevels
    //
    // Note: incoming client requests are handled by the wayland fd source
    // (Source 1) — no separate dispatch_clients here. We do still flush at
    // the end of each tick so frame callbacks reach clients on idle ticks
    // (the fd-source dispatcher only flushes on incoming requests).
    let frame_timer = Timer::from_duration(Duration::from_millis(16));
    loop_handle.insert_source(frame_timer, |_, _, data: &mut TawcState| {
        // New toplevels or dead toplevels need a repaint and focus update.
        // Consume the flag here so cleanup (step 4) can set it again for the
        // next frame. Both render and focus update use the local variable.
        let toplevels_changed = data.toplevels_changed;
        if toplevels_changed {
            data.toplevels_changed = false;
            data.needs_render = true;
        }

        // 1. Import buffers (marks dirty if new content arrived)
        if render::import_wlegl_buffers(data) {
            data.needs_render = true;
        }
        // Re-attaches of an already-imported wl_buffer (the hot path once
        // libhybris's buffer pool has been fully mapped) don't re-import, so
        // the commit handler signals via buffer_commit_pending instead.
        if data.buffer_commit_pending {
            data.buffer_commit_pending = false;
            data.needs_render = true;
        }
        if render::import_shm_buffers(data) {
            data.needs_render = true;
        }

        // 2. Render — once per bound host. Hosts without an EGLSurface
        // (Activity backgrounded / surfaceDestroyed) are skipped.
        if data.needs_render {
            if render_bound_hosts(data) {
                data.needs_render = false;
            }
        }

        // 3. Frame callbacks (always sent so clients can
        // submit new buffers even when we skipped rendering).
        let time = data.start_time.elapsed().as_millis() as u32;
        render::send_frame_callbacks(data, time);

        // Flush so frame callbacks reach the client even on idle ticks. The
        // fd-source dispatcher only flushes on incoming requests; without an
        // explicit flush here clients wait forever for a callback that's
        // already been written to their socket-side queue but not posted.
        // Smithay's merge_into-driven wl_buffer.release events also flow out
        // here (they're queued during dispatch_clients in the fd source).
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error in frame timer: {}", e);
        }

        // 4. Cleanup — collect dead toplevels first, then remove their state.
        // (Two-pass avoids borrowing toplevels and surface maps simultaneously.)
        // Also remember each dead toplevel's host so we can finish the
        // Android Activity when its last toplevel goes away (multi-window:
        // the recents card disappears with the window).
        let dead: Vec<(_, Option<crate::host::ActivityId>)> = data.toplevels.iter()
            .filter(|t| !t.alive())
            .map(|t| {
                let surf = t.wl_surface().clone();
                let host = data.toplevel_to_host.get(&surf).cloned();
                (surf, host)
            })
            .collect();
        if !dead.is_empty() {
            data.toplevels_changed = true;
        }
        for (wl, _) in &dead {
            info!("Removing dead toplevel");
            data.surface_shm.remove(wl);
            data.surface_wlegl.remove(wl);
            data.toplevel_to_host.remove(wl);
        }
        data.toplevels.retain(|t| t.alive());

        // Cap the rendered-toplevels counter at the live count: once a host
        // has been torn down, no further frames render here, and otherwise
        // last_rendered_toplevels would stay frozen at its peak value (so
        // waiters for "compositor went idle" — assert_compositor_clean,
        // wait_for_rendered_toplevels(0) — never see it return to 0).
        if data.last_rendered_toplevels > data.toplevels.len() {
            data.last_rendered_toplevels = data.toplevels.len();
        }

        // Clean up wlegl/SHM entries for surfaces whose client disconnected.
        // The toplevel cleanup above only removes entries keyed by the toplevel's
        // wl_surface, but subsurfaces (e.g. Firefox WebRender) live separately.
        data.surface_wlegl.retain(|surface, _| surface.is_alive());
        data.surface_shm.retain(|surface, _| surface.is_alive());
        data.toplevel_to_host.retain(|surface, _| surface.is_alive());

        // For each host that just lost a window: if neither Wayland nor
        // Xwayland still has windows assigned to it, finish the matching
        // Activity so its recents card disappears.
        data.finish_hosts_if_unused(dead.into_iter().filter_map(|(_, host)| host));

        data.popup_manager.cleanup();
        if data
            .active_popup_grab
            .as_ref()
            .is_some_and(|grab| grab.has_ended())
        {
            data.active_popup_grab = None;
        }
        data.text_input_state.cleanup();

        // Update keyboard and text input focus only when toplevels changed.
        // Both focuses move together: a dead focused surface would otherwise
        // leave the keyboard pointed at it (events go nowhere) until the
        // next FocusChanged event arrives.
        if toplevels_changed {
            let new_focus = data.toplevels.iter()
                .find(|t| t.alive())
                .map(|t| t.wl_surface().clone());
            data.set_input_focus(new_focus.as_ref());
        }

        // 5. Flush (after focus updates so enter/leave events are sent immediately)
        if let Err(e) = data.display_handle.flush_clients() {
            error!("flush_clients error: {}", e);
        }

        // Periodic heartbeat — every 300 actual frames. Guard on
        // frame_count > 0 so the log doesn't fire every tick when the
        // compositor is idle (no foreground host means rendering is
        // skipped entirely and frame_count stays at 0 forever).
        if data.frame_count > 0 && data.frame_count.is_multiple_of(300) {
            info!(
                "Compositor: {} frames, {} toplevels, {} wlegl, {} shm, {} hosts",
                data.frame_count,
                data.toplevels.len(),
                data.surface_wlegl.len(),
                data.surface_shm.len(),
                data.hosts.len(),
            );
        }

        TimeoutAction::ToDuration(Duration::from_millis(16))
    })?;

    // Spawn Xwayland (best-effort: failure logs and continues — the
    // Wayland-only subset of the compositor still works without it).
    crate::xwayland::start_xwayland(&loop_handle, &state);

    // Bind the Wayland socket as the last setup step. From here on, any
    // new connection lands in a backlog the dispatch loop drains within
    // a single iteration. Setting the mode 0o777 lets clients running as
    // any uid (in-chroot bionic-libc with its own concept of "us") open
    // the socket.
    let listener = ListeningSocket::bind_absolute(socket_path.into())
        .map_err(Box::<dyn std::error::Error>::from)?;
    let _ = std::fs::set_permissions(
        socket_path,
        std::fs::Permissions::from_mode(0o777),
    );
    let listener_source = Generic::new(listener, Interest::READ, Mode::Level);
    loop_handle.insert_source(listener_source, |_, source, data: &mut TawcState| {
        while let Some(stream) = source.accept().map_err(std::io::Error::other)? {
            info!("New Wayland client connected");
            let client_state = ClientState::new(data.client_count.clone());
            if let Err(e) = data
                .display_handle
                .insert_client(stream, Arc::new(client_state))
            {
                error!("Failed to insert client: {}", e);
            }
        }
        Ok(PostAction::Continue)
    })?;
    info!("Wayland socket: {}", socket_path);

    info!("Entering calloop event loop");

    while running.load(std::sync::atomic::Ordering::SeqCst) {
        event_loop.dispatch(Some(Duration::from_millis(16)), &mut state)?;
    }

    info!("Event loop exited after {} frames", state.frame_count);
    Ok(())
}

fn render_bound_hosts(data: &mut TawcState) -> bool {
    let mut rendered_any = false;
    // Snapshot keys so we don't hold a borrow on data.hosts across the render
    // call. Render every bound EGL host: the SurfaceView can be registered
    // before Android delivers focus, and skipping that first paint leaves the
    // task black.
    let host_ids: Vec<ActivityId> = data
        .hosts
        .iter()
        .filter(|(_, h)| h.egl_surface.is_some())
        .map(|(k, _)| k.clone())
        .collect();
    for id in host_ids {
        // Take the host out of the map so render_frame can hold a
        // `&mut OutputHost` while still passing `&mut TawcState`.
        if let Some(mut host) = data.hosts.remove(&id) {
            if let Err(e) = render::render_frame(data, &mut host) {
                error!("Render error on host {}: {}", id, e);
            } else {
                rendered_any = true;
            }
            data.hosts.insert(id, host);
        }
    }
    if rendered_any {
        data.frame_count += 1;
        data.last_rendered_toplevels = data.toplevels.len();
    }
    rendered_any
}

// ---------------------------------------------------------------------------
// Surface event handling (per-Activity SurfaceView lifecycle)
// ---------------------------------------------------------------------------

fn handle_surface_event(data: &mut TawcState, evt: SurfaceEvent) {
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
                data.render.attach_host_surface(host);
            }
            let fullscreen = data.host_fullscreen(&activity_id);
            let foreground = data.foreground_host.as_ref() == Some(&activity_id);
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                host.fullscreen = fullscreen;
                host.foreground = foreground;
            }
            crate::set_activity_fullscreen_from_native(&activity_id, fullscreen);
            // Update primary-output mode + the cached logical_size that
            // configure events use. Phase 0/1 advertises only one
            // wl_output, so we just track the first/most recent host.
            data.output_physical_size = (width, height);
            data.output_logical_size = scale.logical_size(width, height);
            data.output.change_current_state(
                Some(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 }),
                Some(smithay::utils::Transform::Normal),
                Some(scale.smithay_scale()),
                Some((0, 0).into()),
            );
            data.output.set_preferred(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 });
            if !data.output_advertised {
                data.output.create_global::<TawcState>(&data.display_handle);
                data.output_advertised = true;
            }
            // Reconfigure existing toplevels with the new logical size.
            reconfigure_all_toplevels(data);
            data.needs_render = true;
            info!(
                "Host registered: {} ({}x{}) — bound={}, total hosts={}",
                activity_id, width, height,
                data.hosts.get(&activity_id).map(|h| h.egl_surface.is_some()).unwrap_or(false),
                data.hosts.len(),
            );
            if render_bound_hosts(data) {
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
            data.output_physical_size = (width, height);
            data.output_logical_size = scale.logical_size(width, height);
            data.output.change_current_state(
                Some(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 }),
                None, None, None,
            );
            reconfigure_all_toplevels(data);
            data.needs_render = true;
            if render_bound_hosts(data) {
                data.needs_render = false;
            }
        }
        SurfaceEvent::SurfaceDestroyed { activity_id } => {
            if let Some(host) = data.hosts.get_mut(&activity_id) {
                host.drop_surface();
                info!("Host {} surface dropped (record retained)", activity_id);
            }
        }
        SurfaceEvent::ActivityDestroyed { activity_id } => {
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
            if data.foreground_host.as_ref() == Some(&activity_id) {
                data.foreground_host = None;
            }
            data.toplevels_changed = true;
        }
        SurfaceEvent::FocusChanged { activity_id, has_focus } => {
            // Update host state + send Activated/Suspended configures.
            set_host_foreground(data, &activity_id, has_focus);
            // Refresh wider TawcState bookkeeping (foreground_host pointer,
            // keyboard / text input focus).
            if has_focus {
                data.foreground_host = Some(activity_id.clone());
                let target = first_alive_toplevel_of_host(data, &activity_id);
                data.set_input_focus(target.as_ref());
                data.needs_render = true;
            } else if data.foreground_host.as_ref() == Some(&activity_id) {
                data.foreground_host = None;
                data.set_input_focus(None);
            }
        }
        SurfaceEvent::OutputScaleChanged { scale } => {
            apply_output_scale(data, OutputScale::new(scale));
        }
        SurfaceEvent::Gtk3BrokenMenusWorkaroundChanged { enabled } => {
            crate::gtk3_menus_workaround::set_enabled(data, enabled);
        }
        SurfaceEvent::FullscreenChanged { activity_id, fullscreen } => {
            data.set_host_fullscreen(&activity_id, fullscreen);
            data.needs_render = true;
        }
        SurfaceEvent::BackPressed { activity_id } => {
            handle_back_pressed(data, &activity_id);
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

    let (pw, ph) = state.output_physical_size;
    state.output_logical_size = scale.logical_size(pw, ph);
    let mode_size = if pw > 0 && ph > 0 { (pw, ph) } else { (1, 1) };
    state.output.change_current_state(
        Some(smithay::output::Mode { size: mode_size.into(), refresh: 60_000 }),
        None,
        Some(scale.smithay_scale()),
        None,
    );

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
    for surface in state.surface_wlegl.keys().chain(state.surface_shm.keys()) {
        if surface.is_alive() && !surfaces.iter().any(|s: &WlSurface| s == surface) {
            surfaces.push(surface.clone());
        }
    }
    for toplevel in &state.toplevels {
        let surface = toplevel.wl_surface();
        if surface.is_alive() && !surfaces.iter().any(|s: &WlSurface| s == surface) {
            surfaces.push(surface.clone());
        }
    }
    for x11 in &state.x11_surfaces {
        if let Some(surface) = x11.wl_surface() {
            if surface.is_alive() && !surfaces.iter().any(|s: &WlSurface| s == &surface) {
                surfaces.push(surface.clone());
            }
        }
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
    use wayland_protocols::xdg::shell::server::xdg_toplevel::State as XdgState;

    let host_ready = state.host_logical_size(host_id).is_some();
    let toplevels: Vec<_> = state
        .toplevels
        .iter()
        .filter(|t| state.toplevel_to_host.get(t.wl_surface()) == Some(host_id))
        .cloned()
        .collect();
    for t in toplevels {
        t.with_pending_state(|s| {
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
        if host_ready {
            t.send_pending_configure();
        }
    }
    if let Some(host) = state.hosts.get_mut(host_id) {
        host.foreground = foreground;
    }
}

/// Find the first alive toplevel assigned to the given host. Used for
/// touch fallback (no per-coordinate hit-testing yet) and keyboard focus
/// when a host comes to the foreground.
fn first_alive_toplevel_of_host(
    state: &TawcState,
    host_id: &crate::host::ActivityId,
) -> Option<smithay::reexports::wayland_server::protocol::wl_surface::WlSurface> {
    state
        .toplevels
        .iter()
        .filter(|t| t.alive())
        .find(|t| state.toplevel_to_host.get(t.wl_surface()) == Some(host_id))
        .map(|t| t.wl_surface().clone())
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
    let toplevels = state.toplevels.clone();
    for toplevel in &toplevels {
        let Some(host_id) = state
            .toplevel_to_host
            .get(toplevel.wl_surface())
        else {
            continue;
        };
        if let Some((w, h)) = state.configure_toplevel_for_host(toplevel, host_id) {
            toplevel.send_pending_configure();
            crate::gtk3_menus_workaround::prime_toplevel(state, toplevel.wl_surface(), w, h);
        }
    }
}
