//! Rendering: buffer import, frame drawing, and frame callbacks.
//!
//! This module owns all GPU/EGL state and the buffer import logic.
//! TawcState (compositor.rs) owns Wayland protocol state; this module
//! turns that protocol state into pixels.

use std::os::unix::io::AsRawFd;

use log::{error, info};

use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::{
    GlesFrame, GlesRenderer, GlesTexProgram, GlesTexture,
    Uniform, UniformName, UniformType, UniformValue,
};
use smithay::backend::renderer::{Bind, Frame, ImportMemWl, Renderer};
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Point, Rectangle, Size, Transform};
use smithay::desktop::PopupManager;
use smithay::wayland::compositor::{
    with_surface_tree_downward, SubsurfaceCachedState, SurfaceAttributes, TraversalAction,
};

use crate::ahb::AhbBuffer;
use crate::background::BackgroundRenderer;
use crate::compositor::TawcState;
use crate::gl_import::AhbTextureImporter;

/// GPU-side rendering state, separate from Wayland protocol state.
///
/// Lives in LoopData alongside TawcState. Anything that touches the
/// GlesRenderer, EGL, or GL textures belongs here — not in TawcState.
pub struct RenderState {
    pub renderer: GlesRenderer,
    pub egl_surface: EGLSurface,
    pub importer: AhbTextureImporter,
    pub shm_tint_shader: Option<GlesTexProgram>,
    pub background: Option<BackgroundRenderer>,
    pub raw_egl_display: *const std::ffi::c_void,
    pub raw_egl_context: *const std::ffi::c_void,
}

// SAFETY: Contains raw EGL pointers (raw_egl_display, raw_egl_context) which are
// !Send. RenderState is created on the compositor thread and only accessed from the
// calloop event loop on that same thread. Calloop requires Send for LoopData even
// though it never actually moves data across threads.
unsafe impl Send for RenderState {}

/// Compile the magenta tint shader used for SHM buffer rendering.
/// Must be called after the EGL context is current.
pub fn compile_shm_tint_shader(renderer: &mut GlesRenderer) -> Option<GlesTexProgram> {
    match renderer.compile_custom_texture_shader(
        r#"
#version 100

//_DEFINES_

#if defined(EXTERNAL)
#extension GL_OES_EGL_image_external : require
#endif

precision mediump float;
#if defined(EXTERNAL)
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

uniform float alpha;
varying vec2 v_coords;
uniform float magenta_mix;

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

void main() {
    vec4 color = texture2D(tex, v_coords);

#if defined(NO_ALPHA)
    color = vec4(color.rgb, 1.0) * alpha;
#else
    color = color * alpha;
#endif

    // Apply magenta tint: boost red and blue, reduce green
    vec3 magenta = vec3(color.r * 1.0 + 0.3, color.g * 0.4, color.b * 1.0 + 0.3);
    color.rgb = mix(color.rgb, magenta, magenta_mix);

#if defined(DEBUG_FLAGS)
    if (tint == 1.0)
        color = vec4(0.0, 0.2, 0.0, 0.2) + color * 0.8;
#endif

    gl_FragColor = color;
}
"#,
        &[UniformName::new("magenta_mix", UniformType::_1f)],
    ) {
        Ok(program) => {
            info!("SHM magenta tint shader compiled");
            Some(program)
        }
        Err(e) => {
            error!("Failed to compile SHM tint shader: {:?}", e);
            None
        }
    }
}

// ---------------------------------------------------------------------------
// Buffer import
// ---------------------------------------------------------------------------

/// Import pending AHB buffers for all surfaces that have one queued.
pub fn import_pending_ahbs(state: &mut TawcState, render: &RenderState) {
    let surfaces: Vec<_> = state
        .surface_ahb
        .iter()
        .filter(|(_, s)| s.pending_width.is_some())
        .map(|(surf, _)| surf.clone())
        .collect();

    for surface in surfaces {
        import_one_ahb(state, render, &surface);
    }
}

/// Import a single pending AHB for a surface.
fn import_one_ahb(
    state: &mut TawcState,
    render: &RenderState,
    surface: &smithay::reexports::wayland_server::protocol::wl_surface::WlSurface,
) {
    let Some(ahb_state) = state.surface_ahb.get_mut(surface) else {
        return;
    };

    let (Some(width), Some(height)) = (
        ahb_state.pending_width.take(),
        ahb_state.pending_height.take(),
    ) else {
        return;
    };

    // Drain all pending AHBs from the side channel, keep only the latest.
    let recv_fd = ahb_state.recv_socket.as_raw_fd();
    ahb_state.recv_socket.set_nonblocking(true).ok();
    let mut latest_ahb = None;
    loop {
        match AhbBuffer::recv_from_socket(recv_fd) {
            Ok(ahb) => latest_ahb = Some(ahb),
            Err(_) => break,
        }
    }
    ahb_state.recv_socket.set_nonblocking(false).ok();

    let Some(ahb) = latest_ahb else {
        return; // Client sent attach but AHB hasn't arrived yet
    };

    info!(
        "Received AHB {}x{} for surface {:?}",
        ahb.width(),
        ahb.height(),
        surface.id()
    );

    // Make EGL context current for GL operations
    unsafe {
        smithay::backend::egl::ffi::egl::MakeCurrent(
            render.raw_egl_display,
            smithay::backend::egl::ffi::egl::NO_SURFACE,
            smithay::backend::egl::ffi::egl::NO_SURFACE,
            render.raw_egl_context,
        );
    }

    match render.importer.import_ahb(
        &render.renderer,
        render.raw_egl_display,
        ahb.as_raw(),
        width,
        height,
    ) {
        Ok(texture) => {
            info!("AHB imported as texture for surface {:?}", surface.id());
            ahb_state.texture = Some(texture);
            ahb_state.ahb = Some(ahb);
            ahb_state.committed_width = width;
            ahb_state.committed_height = height;
        }
        Err(e) => error!("Failed to import AHB: {}", e),
    }
}

/// Walk all toplevel surface trees and import any new SHM buffers as textures.
/// Releases old buffers when replaced.
pub fn import_shm_buffers(state: &mut TawcState, renderer: &mut GlesRenderer) {
    use smithay::backend::renderer::buffer_type;
    use smithay::backend::renderer::BufferType;
    use smithay::wayland::compositor::BufferAssignment;
    use smithay::reexports::wayland_server::protocol::wl_buffer;

    let toplevel_surfaces: Vec<_> = state
        .toplevels
        .iter()
        .map(|t| t.wl_surface().clone())
        .collect();

    // Collect all root surfaces to walk: toplevels + their popup surfaces
    let mut all_roots: Vec<_> = toplevel_surfaces.clone();
    for toplevel_wl in &toplevel_surfaces {
        for (popup, _) in PopupManager::popups_for_surface(toplevel_wl) {
            all_roots.push(popup.wl_surface().clone());
        }
    }

    // Collect (surface, buffer) pairs that need importing.
    let mut to_import: Vec<(WlSurface, wl_buffer::WlBuffer)> = Vec::new();

    for root in &all_roots {
        with_surface_tree_downward(
            root,
            (),
            |_, _, &()| TraversalAction::DoChildren(()),
            |surf, surf_states, &()| {
                if state.surface_ahb.contains_key(surf) {
                    return; // AHB surfaces are handled separately
                }
                let mut guard = surf_states.cached_state.get::<SurfaceAttributes>();
                let attrs = guard.current();

                if let Some(BufferAssignment::NewBuffer(ref buf)) = attrs.buffer {
                    if matches!(buffer_type(buf), Some(BufferType::Shm)) {
                        // Skip if we already imported this exact buffer
                        if let Some(existing) = state.surface_shm.get(surf) {
                            if let Some(ref current_buf) = existing.current_buffer {
                                if current_buf.id() == buf.id() {
                                    return;
                                }
                            }
                        }
                        to_import.push((surf.clone(), buf.clone()));
                    }
                }
            },
            |_, _, &()| true,
        );
    }

    for (surface, buf) in to_import {
        let dims = smithay::wayland::shm::with_buffer_contents(&buf, |_, _, data| {
            (data.width, data.height)
        });
        let (width, height) = match dims {
            Ok(d) => d,
            Err(e) => {
                error!("Failed to get SHM buffer contents: {}", e);
                continue;
            }
        };
        let damage = [Rectangle::from_size(Size::from((width, height)))];
        match renderer.import_shm_buffer(&buf, None, &damage) {
            Ok(texture) => {
                let is_new = !state.surface_shm.contains_key(&surface);
                let shm_state = state
                    .surface_shm
                    .entry(surface.clone())
                    .or_insert(crate::compositor::SurfaceShmState {
                        texture: None,
                        current_buffer: None,
                        committed_width: 0,
                        committed_height: 0,
                    });
                if let Some(old_buf) = shm_state.current_buffer.take() {
                    old_buf.release();
                }
                shm_state.texture = Some(texture);
                shm_state.current_buffer = Some(buf);
                shm_state.committed_width = width;
                shm_state.committed_height = height;
                if is_new {
                    info!(
                        "SHM buffer imported: {}x{} for {:?}",
                        width, height, surface.id()
                    );
                }
            }
            Err(e) => error!("SHM import failed: {}", e),
        }
    }
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

/// Render one frame: bind EGL surface, clear, draw all surfaces, swap.
pub fn render_frame(
    state: &TawcState,
    render: &mut RenderState,
    output_size: Size<i32, smithay::utils::Physical>,
    _frame_count: u64,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut target = render.renderer.bind(&mut render.egl_surface)?;

    let mut frame = render.renderer.render(&mut target, output_size, Transform::Normal)?;

    // Background gradient (black to dark turquoise)
    if let Some(ref bg) = render.background {
        bg.draw(&mut frame, output_size.w, output_size.h)?;
    }

    let screen_h = output_size.h;

    // Draw AHB surfaces (fullscreen at origin — no coordinate transform needed)
    for ahb_state in state.surface_ahb.values() {
        let Some(ref texture) = ahb_state.texture else { continue };
        let w = ahb_state.committed_width;
        let h = ahb_state.committed_height;

        let src = Rectangle::from_size(Size::<i32, smithay::utils::Buffer>::from((w, h)).to_f64());
        let dst = Rectangle::new(
            Point::from((0, 0)),
            Size::from((w, h)),
        );
        let damage = Rectangle::from_size(Size::from((w, h)));

        Frame::render_texture_from_to(
            &mut frame, texture, src, dst, &[damage], &[], Transform::Normal, 1.0,
        )?;
    }

    // Draw SHM surfaces from toplevel surface trees with subsurface positioning
    draw_shm_surfaces(state, &mut frame, screen_h, &render.shm_tint_shader)?;

    let _ = frame.finish()?;
    drop(target);
    render.egl_surface.swap_buffers(None)?;
    Ok(())
}

/// Collect and draw all SHM-backed surfaces from toplevel and popup trees.
///
/// Coordinate transform: subsurface positions are in logical (surface-local)
/// coords, but smithay's GlesRenderer uses a GL projection where Y=0 is at
/// the screen bottom (not top). So we must both scale logical→physical AND
/// flip Y. The formula for each surface is:
///
///   physical_x = logical_x * scale
///   physical_y = screen_h - logical_y * scale - texture_h
///
/// AHB surfaces (drawn in render_frame) skip this because they're always
/// fullscreen at the origin — the transform is a no-op at (0, 0, full size).
fn draw_shm_surfaces(
    state: &TawcState,
    frame: &mut GlesFrame<'_, '_>,
    screen_h: i32,
    shm_shader: &Option<GlesTexProgram>,
) -> Result<(), Box<dyn std::error::Error>> {
    let scale = state.output_scale;
    let toplevel_surfaces: Vec<_> = state
        .toplevels
        .iter()
        .map(|t| t.wl_surface().clone())
        .collect();

    // Gather (texture, logical_x, logical_y, buf_w, buf_h) for every SHM surface.
    let mut to_render: Vec<(GlesTexture, i32, i32, i32, i32)> = Vec::new();

    // Walk a surface tree rooted at `root`, accumulating subsurface offsets
    // starting from (offset_x, offset_y) in logical coords.
    let collect_tree = |root: &WlSurface, offset_x: i32, offset_y: i32,
                        to_render: &mut Vec<(GlesTexture, i32, i32, i32, i32)>| {
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
            |surf, states, &(abs_x, abs_y)| {
                let loc = states
                    .cached_state
                    .get::<SubsurfaceCachedState>()
                    .current()
                    .location;
                let x = abs_x + loc.x;
                let y = abs_y + loc.y;

                if let Some(shm_state) = state.surface_shm.get(surf) {
                    if let Some(ref tex) = shm_state.texture {
                        to_render.push((
                            tex.clone(),
                            x,
                            y,
                            shm_state.committed_width,
                            shm_state.committed_height,
                        ));
                    }
                }
            },
            |_, _, _| true,
        );
    };

    for root in &toplevel_surfaces {
        collect_tree(root, 0, 0, &mut to_render);

        for (popup, location) in PopupManager::popups_for_surface(root) {
            collect_tree(popup.wl_surface(), location.x, location.y, &mut to_render);
        }
    }

    for (texture, logical_x, logical_y, tex_w, tex_h) in &to_render {
        let src = Rectangle::from_size(
            Size::<i32, smithay::utils::Buffer>::from((*tex_w, *tex_h)).to_f64(),
        );
        let dst = Rectangle::new(
            Point::from((
                logical_x * scale,
                screen_h - logical_y * scale - tex_h,
            )),
            Size::from((*tex_w, *tex_h)),
        );
        let damage = Rectangle::from_size(Size::from((*tex_w, *tex_h)));

        frame.render_texture_from_to(
            &texture,
            src,
            dst,
            &[damage],
            &[],
            Transform::Flipped180,
            1.0,
            shm_shader.as_ref(),
            &[Uniform::new("magenta_mix", UniformValue::_1f(1.0))],
        )?;
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// Frame callbacks
// ---------------------------------------------------------------------------

/// Send frame-done callbacks to all surfaces in all toplevel and popup surface trees.
pub fn send_frame_callbacks(state: &TawcState, time: u32) {
    let send_callbacks = |root: &smithay::reexports::wayland_server::protocol::wl_surface::WlSurface| {
        with_surface_tree_downward(
            root,
            (),
            |_, _, &()| TraversalAction::DoChildren(()),
            |_surf, states, &()| {
                for callback in states
                    .cached_state
                    .get::<SurfaceAttributes>()
                    .current()
                    .frame_callbacks
                    .drain(..)
                {
                    callback.done(time);
                }
            },
            |_, _, &()| true,
        );
    };

    for toplevel in &state.toplevels {
        let wl = toplevel.wl_surface();
        send_callbacks(wl);
        for (popup, _) in PopupManager::popups_for_surface(wl) {
            send_callbacks(popup.wl_surface());
        }
    }
}
