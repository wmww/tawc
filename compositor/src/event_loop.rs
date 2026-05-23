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
use smithay::backend::input::KeyState;
use smithay::desktop::{PopupManager, WindowSurfaceType};
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
    get_parent, get_role, SUBSURFACE_ROLE,
};
use smithay::wayland::shell::xdg::XDG_POPUP_ROLE;
use wayland_server::{Display, ListeningSocket};

use crate::host::{ActivityId, OutputHost, SurfaceEvent};
use crate::input::TouchEvent;
use crate::scale::OutputScale;
use crate::text_input::TextInputEvent;
use crate::clipboard::ClipboardEvent;

use crate::compositor::{ClientState, TawcState};
use crate::render;

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

fn touch_focus_at(
    data: &TawcState,
    activity_id: &ActivityId,
    location: Point<f64, Logical>,
) -> Option<(WlSurface, Point<f64, Logical>)> {
    if data.desktop_visible_host_id().as_ref() != Some(activity_id) {
        return None;
    }

    let Some(visible_space) = data.desktop.visible_space(&data.hosts) else {
        return None;
    };
    if let Some((window, window_location)) = visible_space.element_under(location) {
        let window_point = location - window_location.to_f64();
        if let Some((surface, origin)) = window.surface_under(window_point, WindowSurfaceType::ALL) {
            return Some((
                surface,
                Point::from((origin.x as f64, origin.y as f64)),
            ));
        }
    }
    None
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
            let (surfaces_wlegl, surfaces_shm) = data.attached_buffer_counts();
            info!(
                "COMPOSITOR_STATE: clients={} toplevels={} surfaces_wlegl={} surfaces_shm={} frames={} rendered_toplevels={} hosts={} bound_hosts={} output_scale={:.2} output_physical_w={} output_physical_h={} output_logical_w={} output_logical_h={} output_advertised={}",
                clients,
                toplevel_count(data),
                surfaces_wlegl,
                surfaces_shm,
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
    //   1. Update pending XWayland host associations
    //   2. Render one frame for the visible bound host
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

        // 1. Catch up on XWayland surface ↔ host associations that can land
        // after the first wl_surface commit.
        if crate::xwayland::associate_pending_x11_surfaces(data) {
            data.needs_render = true;
        }
        // Surface commits use Smithay's renderer state now. The actual texture
        // import happens when Smithay creates render elements; this flag only
        // wakes the render loop for new buffers, damage, viewport changes, or
        // re-attaches of an already-imported wl_buffer.
        if data.buffer_commit_pending {
            data.buffer_commit_pending = false;
            data.needs_render = true;
        }

        // 2. Render only the foreground bound host. Background hosts neither
        // render nor get frame callbacks; hidden commits can mark the
        // compositor dirty without triggering hidden texture imports.
        if data.needs_render {
            if render_visible_host(data) {
                data.needs_render = false;
            }
        }

        // 3. Frame callbacks for the visible host only. They are still sent
        // on idle ticks so visible clients can submit new buffers even when
        // we skipped rendering.
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

        // 4. Cleanup. Smithay owns xdg toplevel lifetime and calls our
        // `toplevel_destroyed` handler; this timer only prunes stale desktop
        // windows/assignments for surfaces that disappeared outside that path.
        data.desktop.retain_live_windows();
        data.sync_desktop_hosts();

        // Cap the rendered-toplevels counter at the live count: once a host
        // has been torn down, no further frames render here, and otherwise
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
        data.text_input_state.cleanup();

        // Update keyboard and text input focus only when toplevels changed.
        // Both focuses move together: a dead focused surface would otherwise
        // leave the keyboard pointed at it (events go nowhere) until the
        // next FocusChanged event arrives.
        if toplevels_changed {
            let new_focus = data
                .desktop_visible_host_id()
                .and_then(|host| data.first_wayland_toplevel_for_host(&host));
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
            let (surfaces_wlegl, surfaces_shm) = data.attached_buffer_counts();
            info!(
                "Compositor: {} frames, {} toplevels, {} wlegl, {} shm, {} hosts",
                data.frame_count,
                toplevel_count(data),
                surfaces_wlegl,
                surfaces_shm,
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
    data.hosts.insert(id, host);

    if rendered {
        data.frame_count += 1;
        data.last_rendered_toplevels = toplevel_count(data);
    }
    rendered
}

fn toplevel_count(data: &TawcState) -> usize {
    data.xdg_shell_state.toplevel_surfaces().len()
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
                let target = data.first_wayland_toplevel_for_host(&activity_id);
                data.set_input_focus(target.as_ref());
                data.needs_render = true;
            } else if data.desktop.foreground_host() == Some(&activity_id) {
                data.desktop.set_foreground_host(None);
                data.set_input_focus(None);
            }
            data.sync_desktop_hosts();
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
        SurfaceEvent::CloseAllClientsForTest { response } => {
            let closed = data.request_close_all_client_windows_for_test();
            if closed > 0 {
                info!("test-init requested close for {} client windows", closed);
            }
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
        let (pw, ph) = state.output_physical_size;
        state.output_logical_size = scale.logical_size(pw, ph);
        let mode_size = if pw > 0 && ph > 0 { (pw, ph) } else { (1, 1) };
        state.output.change_current_state(
            Some(smithay::output::Mode { size: mode_size.into(), refresh: 60_000 }),
            None,
            Some(scale.smithay_scale()),
            None,
        );
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
    use wayland_protocols::xdg::shell::server::xdg_toplevel::State as XdgState;

    let host_ready = state.host_logical_size(host_id).is_some();
    for t in state.wayland_toplevels_for_host(host_id) {
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
        if let Some((w, h)) = state.configure_toplevel_for_host(toplevel, host_id) {
            toplevel.send_pending_configure();
            crate::gtk3_menus_workaround::prime_toplevel(state, toplevel.wl_surface(), w, h);
        }
    }

    crate::xwayland::configure_x11_toplevels_for_hosts(state);
}
