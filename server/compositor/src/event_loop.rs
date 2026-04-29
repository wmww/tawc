//! Calloop-based event loop for the compositor.
//!
//! Integrates the Wayland display, client listener, frame timer and
//! per-Activity surface lifecycle into a single calloop event loop.
//! All `OutputHost` mutation happens here on the compositor thread —
//! JNI threads send events through channels.

use std::ffi::c_void;
use std::os::fd::{AsFd, OwnedFd};
use std::sync::Arc;
use std::time::{Duration, Instant};

use log::{error, info};
use smithay::reexports::wayland_server::Resource;

use smithay::backend::input::TouchSlot;
use smithay::input::touch::{DownEvent, MotionEvent, UpEvent};
use smithay::backend::input::KeyState;
use smithay::input::keyboard::{FilterResult, Keycode};
use smithay::reexports::calloop::channel::{Channel, Event as ChannelEvent};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{EventLoop, Interest, Mode, PostAction};
use smithay::reexports::rustix;
use smithay::utils::{Point, SERIAL_COUNTER};
use wayland_server::{Display, ListeningSocket};

use crate::host::{ActivityId, OutputHost, SurfaceEvent};
use crate::input::TouchEvent;
use crate::text_input::TextInputEvent;

use crate::compositor::{ClientState, TawcState};
use crate::render::{self, RenderState};

/// All state for the event loop. Calloop passes a single `&mut LoopData`
/// to every callback, so everything must be reachable from here.
pub struct LoopData {
    pub state: TawcState,
    pub render: RenderState,
    pub display: Display<TawcState>,
    /// The single `wl_output` global advertised today. Mode / scale are
    /// updated when the primary host's geometry changes; multi-output
    /// support is left to a later phase (see `notes/multi-activity.md`).
    pub output: smithay::output::Output,
    pub scale: i32,
    pub start_time: Instant,
    pub frame_count: u64,
    /// Set when buffer contents change; cleared after rendering.
    /// Skips GPU work when the screen hasn't changed.
    pub needs_render: bool,
    /// Number of toplevels visible in the last rendered frame.
    /// Used by the state query to verify the screen actually reflects cleanup.
    pub last_rendered_toplevels: usize,
}

/// Set up and run the calloop event loop. Returns when `running` becomes false.
pub fn run(
    display: Display<TawcState>,
    state: TawcState,
    render: RenderState,
    listener: ListeningSocket,
    output: smithay::output::Output,
    scale: i32,
    touch_channel: Channel<TouchEvent>,
    text_input_channel: Channel<TextInputEvent>,
    state_query_channel: Channel<()>,
    surface_event_channel: Channel<SurfaceEvent>,
    running: &std::sync::atomic::AtomicBool,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut event_loop: EventLoop<LoopData> = EventLoop::try_new()?;
    let loop_handle = event_loop.handle();

    // --- Source 1: Wayland display fd ---
    // When clients send protocol messages, this fd becomes readable.
    // We dispatch (process requests) and immediately flush (send responses
    // like configure events back). The immediate flush is important:
    // clients like GTK3 won't render until they receive their configure.
    let display_fd_dup: OwnedFd = rustix::io::dup(display.as_fd())?;
    let display_source = Generic::new(display_fd_dup, Interest::READ, Mode::Level);
    loop_handle.insert_source(display_source, |_, _, data: &mut LoopData| {
        match data.display.dispatch_clients(&mut data.state) {
            Ok(_) => {}
            Err(e) => error!("dispatch_clients error: {}", e),
        }
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error: {}", e);
        }
        Ok(PostAction::Continue)
    })?;

    // --- Source 2: Listener socket ---
    // Accept new client connections as they arrive.
    let listener_source = Generic::new(listener, Interest::READ, Mode::Level);
    loop_handle.insert_source(listener_source, |_, source, data: &mut LoopData| {
        while let Some(stream) = source.accept().map_err(|e| std::io::Error::other(e))? {
            info!("New Wayland client connected");
            let client_state = ClientState::new(data.state.client_count.clone());
            if let Err(e) = data
                .display
                .handle()
                .insert_client(stream, Arc::new(client_state))
            {
                error!("Failed to insert client: {}", e);
            }
        }
        Ok(PostAction::Continue)
    })?;

    // --- Source 3: Touch input channel ---
    // Receives touch events from the Android UI thread via JNI, tagged
    // with the activity_id of the SurfaceView that produced them.
    // Coordinates arrive in physical pixels; we convert to logical
    // (/ scale).
    //
    // Focus picks the first alive toplevel assigned to the touch's host —
    // each Android task only has its own toplevels in the recents card,
    // so this matches what the user sees.
    loop_handle.insert_source(touch_channel, |event, _, data: &mut LoopData| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        let touch = match data.state.seat.get_touch() {
            Some(t) => t,
            None => return,
        };

        // Identify the touch's host and its first alive assigned toplevel.
        let activity_id = match &evt {
            TouchEvent::Down { activity_id, .. }
            | TouchEvent::Motion { activity_id, .. }
            | TouchEvent::Up { activity_id, .. } => activity_id.clone(),
        };
        let focus = first_alive_toplevel_of_host(&data.state, &activity_id)
            .map(|wl| (wl, Point::from((0.0, 0.0))));

        let touch_scale = data.scale as f64;
        let serial = SERIAL_COUNTER.next_serial();

        match evt {
            TouchEvent::Down { id, x, y, time, .. } => {
                let location: Point<f64, smithay::utils::Logical> =
                    (x as f64 / touch_scale, y as f64 / touch_scale).into();
                // Finalize any active preedit *before* the touch reaches the
                // client. Wayland text-input-v3 has no way to insert text at
                // an "old cursor" — preedit is purely a cursor-relative
                // overlay — so once the touch moves the cursor we'd have to
                // either let the preedit visually follow the cursor (the
                // Android `setComposingRegion` "active word follows" bug)
                // or silently drop the typed text. Native clients (GTK, Qt,
                // Firefox) reset their IM on cursor-move and the IM commits
                // its pending text first; we are the IM, so we do that here.
                //
                // Same calloop callback, same compositor thread, same client
                // socket — the commit_string + done is on the wire before
                // touch.down's events, so the client commits at the old
                // cursor and only afterwards processes the touch. No-op
                // when there's no active preedit.
                data.state.text_input_state.handle_android_event(
                    crate::text_input::TextInputEvent::FinishComposingText,
                );
                // Set keyboard focus on touch to the target surface
                if let (Some((ref surface, _)), Some(keyboard)) = (&focus, data.state.seat.get_keyboard()) {
                    keyboard.set_focus(&mut data.state, Some(surface.clone()), serial);
                }
                touch.down(
                    &mut data.state,
                    focus,
                    &DownEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        location,
                        serial,
                        time,
                    },
                );
                touch.frame(&mut data.state);
            }
            TouchEvent::Motion { id, x, y, time, .. } => {
                let location: Point<f64, smithay::utils::Logical> =
                    (x as f64 / touch_scale, y as f64 / touch_scale).into();
                touch.motion(
                    &mut data.state,
                    focus,
                    &MotionEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        location,
                        time,
                    },
                );
                touch.frame(&mut data.state);
            }
            TouchEvent::Up { id, time, .. } => {
                touch.up(
                    &mut data.state,
                    &UpEvent {
                        slot: TouchSlot::from(Some(id as u32)),
                        serial,
                        time,
                    },
                );
                touch.frame(&mut data.state);
            }
        }

        // Flush immediately so clients see events without waiting for frame timer
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error after touch: {}", e);
        }
    })?;

    // --- Source 4: Text input channel ---
    // Receives text input events from Android IME via JNI.
    loop_handle.insert_source(text_input_channel, move |event, _, data: &mut LoopData| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        match evt {
            TextInputEvent::KeyPress { keycode } => {
                // Send as a real wl_keyboard key event (press + release)
                if let Some(keyboard) = data.state.seat.get_keyboard() {
                    let serial = SERIAL_COUNTER.next_serial();
                    let time = data.start_time.elapsed().as_millis() as u32;
                    let keycode = Keycode::from(keycode + 8); // evdev → XKB offset
                    keyboard.input::<(), _>(
                        &mut data.state, keycode, KeyState::Pressed, serial, time,
                        |_, _, _| FilterResult::Forward,
                    );
                    let serial = SERIAL_COUNTER.next_serial();
                    keyboard.input::<(), _>(
                        &mut data.state, keycode, KeyState::Released, serial, time + 1,
                        |_, _, _| FilterResult::Forward,
                    );
                }
            }
            _ => {
                data.state.text_input_state.handle_android_event(evt);
            }
        }

        // Flush so clients see text input events immediately
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error after text input: {}", e);
        }
    })?;

    // --- Source 5: State query channel ---
    // Receives requests to log compositor state (from QUERY_STATE broadcast).
    loop_handle.insert_source(state_query_channel, move |event, _, data: &mut LoopData| {
        if let ChannelEvent::Msg(()) = event {
            let clients = data.state.client_count.load(std::sync::atomic::Ordering::Relaxed);
            let bound_hosts = data
                .state
                .hosts
                .values()
                .filter(|h| h.egl_surface.is_some())
                .count();
            info!(
                "COMPOSITOR_STATE: clients={} toplevels={} surfaces_wlegl={} surfaces_shm={} frames={} rendered_toplevels={} hosts={} bound_hosts={}",
                clients,
                data.state.toplevels.len(),
                data.state.surface_wlegl.len(),
                data.state.surface_shm.len(),
                data.frame_count,
                data.last_rendered_toplevels,
                data.state.hosts.len(),
                bound_hosts,
            );
        }
    })?;

    // --- Source 6: Surface lifecycle events from Activities ---
    loop_handle.insert_source(surface_event_channel, |event, _, data: &mut LoopData| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };
        handle_surface_event(data, evt);
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error after surface event: {}", e);
        }
    })?;

    // --- Source 7: Frame timer (~60 fps) ---
    // This drives the render loop. Each tick:
    //   1. Dispatch pending client messages (see note below)
    //   2. Import new buffers (AHB and SHM) as GL textures
    //   3. Render the frame for each bound host
    //   4. Send frame-done callbacks
    //   5. Flush outgoing events to clients
    //   6. Clean up dead toplevels
    //
    // NOTE: We call dispatch_clients here IN ADDITION to the fd source above.
    // The fd source handles the fast path (wake immediately when data arrives),
    // but we also need to catch messages that arrived between the last fd wake
    // and this timer tick. Without this, some client messages would be delayed
    // by up to one frame. Do not remove this "duplicate" dispatch.
    let frame_timer = Timer::from_duration(Duration::from_millis(16));
    loop_handle.insert_source(frame_timer, |_, _, data: &mut LoopData| {
        // 1. Dispatch
        match data.display.dispatch_clients(&mut data.state) {
            Ok(_) => {}
            Err(e) => error!("dispatch_clients error: {}", e),
        }

        // New toplevels or dead toplevels need a repaint and focus update.
        // Consume the flag here so cleanup (step 5) can set it again for the
        // next frame. Both render and focus update use the local variable.
        let toplevels_changed = data.state.toplevels_changed;
        if toplevels_changed {
            data.state.toplevels_changed = false;
            data.needs_render = true;
        }

        // 2. Import buffers (marks dirty if new content arrived)
        if render::import_wlegl_buffers(&mut data.state, &mut data.render) {
            data.needs_render = true;
        }
        // Re-attaches of an already-imported wl_buffer (the hot path once
        // libhybris's buffer pool has been fully mapped) don't re-import, so
        // the commit handler signals via buffer_commit_pending instead.
        if data.state.buffer_commit_pending {
            data.state.buffer_commit_pending = false;
            data.needs_render = true;
        }
        if render::import_shm_buffers(&mut data.state, &mut data.render.renderer) {
            data.needs_render = true;
        }

        // 3. Render — once per bound host. Hosts without an EGLSurface
        // (Activity backgrounded / surfaceDestroyed) are skipped.
        if data.needs_render {
            data.needs_render = false;
            let mut rendered_any = false;
            // Snapshot keys so we don't hold a borrow on data.state.hosts
            // across the render call. We only render *foreground* hosts
            // (Phase 7) — backgrounded hosts have already been told to
            // suspend via the configure event in `set_host_foreground`,
            // so painting them would just burn cycles on pixels nobody
            // sees.
            let host_ids: Vec<ActivityId> = data
                .state
                .hosts
                .iter()
                .filter(|(_, h)| h.egl_surface.is_some() && h.foreground)
                .map(|(k, _)| k.clone())
                .collect();
            for id in host_ids {
                // Take the host out of the map so render_frame can hold
                // a `&mut OutputHost` while still passing `&TawcState`.
                if let Some(mut host) = data.state.hosts.remove(&id) {
                    if let Err(e) = render::render_frame(&data.state, &mut data.render, &mut host) {
                        error!("Render error on host {}: {}", id, e);
                    } else {
                        rendered_any = true;
                    }
                    data.state.hosts.insert(id, host);
                }
            }
            if rendered_any {
                data.frame_count += 1;
                data.last_rendered_toplevels = data.state.toplevels.len();
            }
        }

        // 4. Frame callbacks (always sent so clients can
        // submit new buffers even when we skipped rendering).
        let time = data.start_time.elapsed().as_millis() as u32;
        render::send_frame_callbacks(&data.state, time);

        // Flush so frame callbacks reach the client even on idle ticks. The
        // fd-source dispatcher only flushes on incoming requests; without an
        // explicit flush here clients wait forever for a callback that's
        // already been written to their socket-side queue but not posted.
        // Smithay's merge_into-driven wl_buffer.release events also flow out
        // here (they're queued during dispatch_clients above).
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error in frame timer: {}", e);
        }

        // 5. Cleanup — collect dead toplevels first, then remove their state.
        // (Two-pass avoids borrowing toplevels and surface maps simultaneously.)
        // Also remember each dead toplevel's host so we can finish the
        // Android Activity when its last toplevel goes away (multi-window:
        // the recents card disappears with the window).
        let dead: Vec<(_, Option<crate::host::ActivityId>)> = data.state.toplevels.iter()
            .filter(|t| !t.alive())
            .map(|t| {
                let surf = t.wl_surface().clone();
                let host = data.state.toplevel_to_host.get(&surf).cloned();
                (surf, host)
            })
            .collect();
        if !dead.is_empty() {
            data.state.toplevels_changed = true;
        }
        for (wl, _) in &dead {
            info!("Removing dead toplevel");
            data.state.surface_shm.remove(wl);
            data.state.surface_wlegl.remove(wl);
            data.state.toplevel_to_host.remove(wl);
        }
        data.state.toplevels.retain(|t| t.alive());

        // Cap the rendered-toplevels counter at the live count: once a host
        // has been torn down, no further frames render here, and otherwise
        // last_rendered_toplevels would stay frozen at its peak value (so
        // waiters for "compositor went idle" — assert_compositor_clean,
        // wait_for_rendered_toplevels(0) — never see it return to 0).
        if data.last_rendered_toplevels > data.state.toplevels.len() {
            data.last_rendered_toplevels = data.state.toplevels.len();
        }

        // Clean up wlegl/SHM entries for surfaces whose client disconnected.
        // The toplevel cleanup above only removes entries keyed by the toplevel's
        // wl_surface, but subsurfaces (e.g. Firefox WebRender) live separately.
        data.state.surface_wlegl.retain(|surface, _| surface.is_alive());
        data.state.surface_shm.retain(|surface, _| surface.is_alive());
        data.state.toplevel_to_host.retain(|surface, _| surface.is_alive());

        // For each host that just lost a toplevel: if no toplevels remain
        // assigned to it, ask Kotlin to finishAndRemoveTask the matching
        // Activity so its recents card disappears. There is no special
        // launcher / bootstrap Activity to exempt — every
        // CompositorActivity exists for exactly one Wayland window.
        let mut already_finished: std::collections::HashSet<crate::host::ActivityId> =
            std::collections::HashSet::new();
        for (_, host_id) in dead {
            let Some(host_id) = host_id else { continue };
            if already_finished.contains(&host_id) {
                continue;
            }
            let still_used = data.state.toplevel_to_host.values().any(|h| h == &host_id);
            if !still_used {
                already_finished.insert(host_id.clone());
                crate::finish_activity_from_native(&host_id);
            }
        }

        data.state.popup_manager.cleanup();
        data.state.text_input_state.cleanup();

        // Update keyboard and text input focus only when toplevels changed
        if toplevels_changed {
            let new_focus = data.state.toplevels.iter()
                .find(|t| t.alive())
                .map(|t| t.wl_surface().clone());
            data.state.text_input_state.update_focus(new_focus.as_ref());
        }

        // 6. Flush (after focus updates so enter/leave events are sent immediately)
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error: {}", e);
        }

        // Periodic heartbeat — every 300 actual frames. Guard on
        // frame_count > 0 so the log doesn't fire every tick when the
        // compositor is idle (no foreground host means rendering is
        // skipped entirely and frame_count stays at 0 forever).
        if data.frame_count > 0 && data.frame_count % 300 == 0 {
            info!(
                "Compositor: {} frames, {} toplevels, {} wlegl, {} shm, {} hosts",
                data.frame_count,
                data.state.toplevels.len(),
                data.state.surface_wlegl.len(),
                data.state.surface_shm.len(),
                data.state.hosts.len(),
            );
        }

        TimeoutAction::ToDuration(Duration::from_millis(16))
    })?;

    let mut loop_data = LoopData {
        state,
        render,
        display,
        output,
        scale,
        start_time: Instant::now(),
        frame_count: 0,
        needs_render: true, // render background on first frame
        last_rendered_toplevels: 0,
    };

    // Spawn Xwayland (best-effort: failure logs and continues — the
    // Wayland-only subset of the compositor still works without it).
    crate::xwayland::start_xwayland(&loop_handle, &loop_data.state);

    info!("Entering calloop event loop");

    while running.load(std::sync::atomic::Ordering::SeqCst) {
        event_loop.dispatch(Some(Duration::from_millis(16)), &mut loop_data)?;
    }

    info!("Event loop exited after {} frames", loop_data.frame_count);
    Ok(())
}

// ---------------------------------------------------------------------------
// Surface event handling (per-Activity SurfaceView lifecycle)
// ---------------------------------------------------------------------------

fn handle_surface_event(data: &mut LoopData, evt: SurfaceEvent) {
    match evt {
        SurfaceEvent::Register { activity_id, native_window, width, height } => {
            let nw = native_window as *mut c_void;
            let scale = data.scale;
            // If this Activity already has a host, replace its native_window
            // (Activity recreated, e.g. after rotation). Otherwise create a
            // fresh host record.
            match data.state.hosts.get_mut(&activity_id) {
                Some(host) => host.replace_native_window(nw, width, height, scale),
                None => {
                    let host = OutputHost::new(activity_id.clone(), nw, width, height, scale);
                    data.state.hosts.insert(activity_id.clone(), host);
                }
            }
            // Bind the EGLSurface (separate step: needs &RenderState).
            if let Some(host) = data.state.hosts.get_mut(&activity_id) {
                data.render.attach_host_surface(host);
            }
            // Update primary-output mode + the cached logical_size that
            // configure events use. Phase 0/1 advertises only one
            // wl_output, so we just track the first/most recent host.
            data.state.output_logical_size = (width / scale, height / scale);
            data.output.change_current_state(
                Some(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 }),
                Some(smithay::utils::Transform::Normal),
                Some(smithay::output::Scale::Integer(scale)),
                Some((0, 0).into()),
            );
            data.output.set_preferred(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 });
            // Reconfigure existing toplevels with the new logical size.
            reconfigure_all_toplevels(&mut data.state);
            data.needs_render = true;
            info!(
                "Host registered: {} ({}x{}) — bound={}, total hosts={}",
                activity_id, width, height,
                data.state.hosts.get(&activity_id).map(|h| h.egl_surface.is_some()).unwrap_or(false),
                data.state.hosts.len(),
            );
        }
        SurfaceEvent::SurfaceChanged { activity_id, width, height } => {
            let scale = data.scale;
            if let Some(host) = data.state.hosts.get_mut(&activity_id) {
                host.update_size(width, height, scale);
            } else {
                info!("SurfaceChanged for unknown host {}", activity_id);
                return;
            }
            data.state.output_logical_size = (width / scale, height / scale);
            data.output.change_current_state(
                Some(smithay::output::Mode { size: (width, height).into(), refresh: 60_000 }),
                None, None, None,
            );
            reconfigure_all_toplevels(&mut data.state);
            data.needs_render = true;
        }
        SurfaceEvent::SurfaceDestroyed { activity_id } => {
            if let Some(host) = data.state.hosts.get_mut(&activity_id) {
                host.drop_surface();
                info!("Host {} surface dropped (record retained)", activity_id);
            }
        }
        SurfaceEvent::ActivityDestroyed { activity_id } => {
            // Send xdg_toplevel.close to every toplevel assigned to this
            // host. Well-behaved clients then destroy the toplevel; we
            // clean up via the dead-toplevel pass on the next frame.
            // (Phase 7 polish: handle clients that refuse to close.)
            let assigned: Vec<_> = data
                .state
                .toplevels
                .iter()
                .filter(|t| {
                    data.state.toplevel_to_host.get(t.wl_surface()) == Some(&activity_id)
                })
                .cloned()
                .collect();
            for t in &assigned {
                t.send_close();
            }
            if data.state.hosts.remove(&activity_id).is_some() {
                info!("Host {} removed (closed {} toplevels)", activity_id, assigned.len());
            }
            if data.state.foreground_host.as_ref() == Some(&activity_id) {
                data.state.foreground_host = None;
            }
            data.state.toplevels_changed = true;
        }
        SurfaceEvent::FocusChanged { activity_id, has_focus } => {
            // Update host state + send Activated/Suspended configures.
            set_host_foreground(&mut data.state, &activity_id, has_focus);
            // Refresh wider TawcState bookkeeping (foreground_host pointer,
            // keyboard / text input focus).
            if has_focus {
                data.state.foreground_host = Some(activity_id.clone());
                let target = first_alive_toplevel_of_host(&data.state, &activity_id);
                if let Some(keyboard) = data.state.seat.get_keyboard() {
                    let serial = SERIAL_COUNTER.next_serial();
                    keyboard.set_focus(&mut data.state, target.clone(), serial);
                }
                data.state.text_input_state.update_focus(target.as_ref());
                data.needs_render = true;
            } else if data.state.foreground_host.as_ref() == Some(&activity_id) {
                data.state.foreground_host = None;
                if let Some(keyboard) = data.state.seat.get_keyboard() {
                    let serial = SERIAL_COUNTER.next_serial();
                    keyboard.set_focus(&mut data.state, None, serial);
                }
                data.state.text_input_state.update_focus(None);
            }
        }
    }
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
        t.send_pending_configure();
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
    // Each toplevel uses ITS OWN host's logical_size if known, else the
    // primary-output cached size as a fallback. Going through
    // `send_pending_configure` keeps us from re-sending an identical
    // configure when nothing changed (e.g. Register and SurfaceChanged
    // arrive back-to-back with the same dimensions): vkcube's Vulkan WSI
    // wedges if it sees a duplicate configure between its first and
    // second commit.
    let primary = state.output_logical_size;
    for toplevel in &state.toplevels {
        let (w, h) = state
            .toplevel_to_host
            .get(toplevel.wl_surface())
            .and_then(|id| state.hosts.get(id))
            .map(|host| host.logical_size)
            .unwrap_or(primary);
        toplevel.with_pending_state(|s| {
            s.size = Some((w, h).into());
        });
        toplevel.send_pending_configure();
    }
}
