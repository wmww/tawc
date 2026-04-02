//! Calloop-based event loop for the compositor.
//!
//! Integrates the Wayland display, client listener, and frame timer into
//! a single calloop event loop. This is the standard smithay pattern —
//! all real smithay compositors use calloop, not raw poll().

use std::os::fd::{AsFd, OwnedFd};
use std::sync::Arc;
use std::time::{Duration, Instant};

use log::{error, info};

use smithay::backend::input::TouchSlot;
use smithay::input::touch::{DownEvent, MotionEvent, UpEvent};
use smithay::reexports::calloop::channel::{Channel, Event as ChannelEvent};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{EventLoop, Interest, Mode, PostAction};
use smithay::reexports::rustix;
use smithay::utils::{Point, Size, SERIAL_COUNTER};
use wayland_server::{Display, ListeningSocket};

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
    pub output_size: Size<i32, smithay::utils::Physical>,
    pub start_time: Instant,
    pub frame_count: u64,
}

/// Set up and run the calloop event loop. Returns when `running` becomes false.
pub fn run(
    display: Display<TawcState>,
    state: TawcState,
    render: RenderState,
    listener: ListeningSocket,
    output_size: Size<i32, smithay::utils::Physical>,
    scale: i32,
    touch_channel: Channel<TouchEvent>,
    text_input_channel: Channel<TextInputEvent>,
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
            if let Err(e) = data
                .display
                .handle()
                .insert_client(stream, Arc::new(ClientState::default()))
            {
                error!("Failed to insert client: {}", e);
            }
        }
        Ok(PostAction::Continue)
    })?;

    // --- Source 3: Touch input channel ---
    // Receives touch events from the Android UI thread via JNI.
    // Coordinates arrive in physical pixels; we convert to logical (/ touch_scale).
    let touch_scale = scale as f64;
    loop_handle.insert_source(touch_channel, move |event, _, data: &mut LoopData| {
        let evt = match event {
            ChannelEvent::Msg(e) => e,
            ChannelEvent::Closed => return,
        };

        let touch = match data.state.seat.get_touch() {
            Some(t) => t,
            None => return,
        };

        // Find the first alive toplevel's surface as the focus target.
        // All toplevels are maximized at (0,0), so surface-local coords = global coords.
        let focus = data.state.toplevels.iter()
            .find(|t| t.alive())
            .map(|t| (t.wl_surface().clone(), Point::from((0.0, 0.0))));

        let serial = SERIAL_COUNTER.next_serial();

        match evt {
            TouchEvent::Down { id, x, y, time } => {
                let location: Point<f64, smithay::utils::Logical> =
                    (x as f64 / touch_scale, y as f64 / touch_scale).into();
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
            TouchEvent::Motion { id, x, y, time } => {
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
            TouchEvent::Up { id, time } => {
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

        data.state.text_input_state.handle_android_event(evt);

        // Flush so clients see text input events immediately
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error after text input: {}", e);
        }
    })?;

    // --- Source 5: Frame timer (~60 fps) ---
    // This drives the render loop. Each tick:
    //   1. Dispatch pending client messages (see note below)
    //   2. Import new buffers (AHB and SHM) as GL textures
    //   3. Render the frame
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

        // 2. Import buffers
        render::import_pending_ahbs(&mut data.state, &data.render);
        render::import_shm_buffers(&mut data.state, &mut data.render.renderer);

        // 3. Render
        if let Err(e) = render::render_frame(
            &data.state,
            &mut data.render,
            data.output_size,
            data.frame_count,
        ) {
            error!("Render error: {}", e);
        }

        // 4. Frame callbacks
        let time = data.start_time.elapsed().as_millis() as u32;
        render::send_frame_callbacks(&data.state, time);

        // 5. Flush
        if let Err(e) = data.display.flush_clients() {
            error!("flush_clients error: {}", e);
        }

        // 6. Cleanup
        data.state.toplevels.retain(|t| {
            if t.alive() {
                true
            } else {
                data.state.surface_shm.remove(t.wl_surface());
                false
            }
        });
        data.state.popup_manager.cleanup();
        data.state.text_input_state.cleanup();

        // Update keyboard and text input focus if toplevels changed
        let new_focus = data.state.toplevels.iter()
            .find(|t| t.alive())
            .map(|t| t.wl_surface().clone());
        data.state.text_input_state.update_focus(new_focus.as_ref());

        data.frame_count += 1;
        if data.frame_count % 300 == 0 {
            info!(
                "Compositor: {} frames, {} toplevels, {} ahb, {} shm",
                data.frame_count,
                data.state.toplevels.len(),
                data.state.surface_ahb.len(),
                data.state.surface_shm.len(),
            );
        }

        TimeoutAction::ToDuration(Duration::from_millis(16))
    })?;

    let mut loop_data = LoopData {
        state,
        render,
        display,
        output_size,
        start_time: Instant::now(),
        frame_count: 0,
    };

    info!("Entering calloop event loop");

    while running.load(std::sync::atomic::Ordering::SeqCst) {
        event_loop.dispatch(Some(Duration::from_millis(16)), &mut loop_data)?;
    }

    info!("Event loop exited after {} frames", loop_data.frame_count);
    Ok(())
}
