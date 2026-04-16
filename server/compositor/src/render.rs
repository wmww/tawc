//! Rendering: buffer import, frame drawing, and frame callbacks.
//!
//! This module owns all GPU/EGL state and the buffer import logic.
//! TawcState (compositor.rs) owns Wayland protocol state; this module
//! turns that protocol state into pixels.

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

use crate::background::BackgroundRenderer;
use crate::compositor::TawcState;
use crate::gl_import::AhbTextureImporter;
use crate::wlegl::WleglBufferData;
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;

/// GPU-side rendering state, separate from Wayland protocol state.
///
/// Lives in LoopData alongside TawcState. Anything that touches the
/// GlesRenderer, EGL, or GL textures belongs here — not in TawcState.
pub struct RenderState {
    pub renderer: GlesRenderer,
    pub egl_surface: EGLSurface,
    pub importer: AhbTextureImporter,
    pub shm_tint_shader: Option<GlesTexProgram>,
    /// Custom shader used for wlegl (EXTERNAL_OES AHB-backed) textures that
    /// forces alpha=1 regardless of the texture's alpha channel. Smithay's
    /// built-in EXTERNAL variant is compiled without NO_ALPHA and so
    /// multiplies by the texture alpha; GTK/libhybris render RGB content
    /// with alpha=0 (GTK assumes its surfaces are opaque and never writes
    /// alpha), which makes the output fully transparent. See
    /// `variant_for_format` in smithay/src/backend/renderer/gles/shaders/implicit/mod.rs.
    pub wlegl_opaque_shader: Option<GlesTexProgram>,
    pub background: Option<BackgroundRenderer>,
    pub raw_egl_display: *const std::ffi::c_void,
    pub raw_egl_context: *const std::ffi::c_void,
}

// SAFETY: Contains raw EGL pointers (raw_egl_display, raw_egl_context) which are
// !Send. RenderState is created on the compositor thread and only accessed from the
// calloop event loop on that same thread. Calloop requires Send for LoopData even
// though it never actually moves data across threads.
unsafe impl Send for RenderState {}

/// Compile a texture shader that always forces alpha=1.
///
/// smithay's built-in EXTERNAL shader variant samples the texture's alpha
/// channel (no NO_ALPHA define for external textures). GTK/libhybris render
/// into AHB-backed EGLImages without writing alpha, so texels end up
/// (R,G,B,0) and smithay outputs fully-transparent pixels. This shader
/// discards the texture's alpha and writes 1.0 instead, making the
/// client-provided RGB content actually visible.
pub fn compile_wlegl_opaque_shader(renderer: &mut GlesRenderer) -> Option<GlesTexProgram> {
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

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

void main() {
    vec4 color = texture2D(tex, v_coords);
    // Always ignore the texture's alpha channel — GTK on libhybris paints
    // RGB only and leaves alpha at 0, which would otherwise render the
    // surface fully transparent.
    color = vec4(color.rgb, 1.0) * alpha;
#if defined(DEBUG_FLAGS)
    if (tint == 1.0)
        color = vec4(0.0, 0.2, 0.0, 0.2) + color * 0.8;
#endif
    gl_FragColor = color;
}
"#,
        &[],
    ) {
        Ok(program) => {
            info!("wlegl opaque shader compiled");
            Some(program)
        }
        Err(e) => {
            error!("Failed to compile wlegl opaque shader: {:?}", e);
            None
        }
    }
}

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

/// Ensure GlesTextures are created for all wlegl buffers currently attached to
/// surfaces. The texture is cached on the wl_buffer's user-data so repeat
/// attaches don't re-import.
/// Returns true if any new texture was imported (caller uses for dirty tracking).
pub fn import_wlegl_buffers(state: &mut TawcState, render: &mut RenderState) -> bool {
    let buffers: Vec<WlBuffer> = state
        .surface_wlegl
        .values()
        .filter_map(|s| s.current_buffer.clone())
        .collect();

    let mut imported = false;
    for buf in buffers {
        let Some(data) = buf.data::<WleglBufferData>() else {
            continue;
        };
        {
            let tex_guard = data.texture.lock().unwrap();
            if tex_guard.is_some() {
                continue; // already imported
            }
        }
        if ensure_wlegl_texture(render, data) {
            imported = true;
        }
    }
    imported
}

fn ensure_wlegl_texture(render: &RenderState, data: &WleglBufferData) -> bool {
    // Make EGL context current for GL operations
    let ok = unsafe {
        smithay::backend::egl::ffi::egl::MakeCurrent(
            render.raw_egl_display,
            smithay::backend::egl::ffi::egl::NO_SURFACE,
            smithay::backend::egl::ffi::egl::NO_SURFACE,
            render.raw_egl_context,
        )
    };
    if ok == smithay::backend::egl::ffi::egl::FALSE {
        error!("eglMakeCurrent failed in ensure_wlegl_texture");
        return false;
    }
    let res = render.importer.import_ahb(
        &render.renderer,
        render.raw_egl_display,
        data.ahb_raw(),
        data.width,
        data.height,
    );
    match res {
        Ok(tex) => {
            *data.texture.lock().unwrap() = Some(tex);
            info!("wlegl: imported ANativeWindowBuffer as texture {}x{}", data.width, data.height);
            true
        }
        Err(e) => {
            error!("wlegl: texture import failed: {}", e);
            false
        }
    }
}

/// Walk all toplevel surface trees and import any new SHM buffers as textures.
/// Releases old buffers when replaced. Returns true if any buffer was imported.
pub fn import_shm_buffers(state: &mut TawcState, renderer: &mut GlesRenderer) -> bool {
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
                if state.surface_wlegl.contains_key(surf) {
                    return; // GPU (AHB) surfaces are handled separately
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

    let mut imported = false;
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
                imported = true;
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
    imported
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

/// A surface ready to draw: texture, logical position, and buffer dimensions.
struct SurfaceDraw {
    texture: GlesTexture,
    logical_x: i32,
    logical_y: i32,
    buf_w: i32,
    buf_h: i32,
}

/// Walk a single surface tree, collecting drawable surfaces at their absolute
/// logical positions. `offset_x`/`offset_y` is the root's position (0,0 for
/// toplevels, popup geometry origin for popups).
fn collect_tree_draws(
    root: &WlSurface,
    offset_x: i32,
    offset_y: i32,
    surface_fn: &impl Fn(&WlSurface) -> Option<(GlesTexture, i32, i32)>,
    out: &mut Vec<SurfaceDraw>,
) {
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
        |surf, _states, &(abs_x, abs_y)| {
            if let Some((tex, w, h)) = surface_fn(surf) {
                out.push(SurfaceDraw {
                    texture: tex,
                    logical_x: abs_x,
                    logical_y: abs_y,
                    buf_w: w,
                    buf_h: h,
                });
            }
        },
        |_, _, _| true,
    );
}

/// Walk all toplevel and popup surface trees, collecting drawable surfaces.
///
/// Each tree is independently reversed for correct z-order:
/// `with_surface_tree_downward` processes children before parents (post-order),
/// but Wayland stacking puts subsurfaces above their parent, so we need
/// parents drawn first (behind) and children drawn last (on top).
/// Firefox/WebRender relies on this — its toplevel wl_surface holds a
/// placeholder buffer and WebRender draws the real UI into a subsurface.
///
/// Popup trees are appended after their parent toplevel, so they draw on top.
fn collect_surface_draws(
    state: &TawcState,
    surface_fn: impl Fn(&WlSurface) -> Option<(GlesTexture, i32, i32)>,
) -> Vec<SurfaceDraw> {
    let mut draws = Vec::new();

    for toplevel in &state.toplevels {
        let root = toplevel.wl_surface();

        let start = draws.len();
        collect_tree_draws(root, 0, 0, &surface_fn, &mut draws);
        draws[start..].reverse();

        for (popup, location) in PopupManager::popups_for_surface(root) {
            let start = draws.len();
            collect_tree_draws(popup.wl_surface(), location.x, location.y, &surface_fn, &mut draws);
            draws[start..].reverse();
        }
    }

    draws
}

/// Draw collected surfaces with the logical→physical coordinate transform.
///
/// Subsurface positions are in logical (surface-local) coords, but smithay's
/// GlesRenderer uses a GL projection where Y=0 is at the screen bottom (not
/// top). So we both scale logical→physical AND flip Y:
///
///   physical_x = logical_x * scale
///   physical_y = screen_h - logical_y * scale - buf_h
fn draw_surfaces(
    draws: &[SurfaceDraw],
    frame: &mut GlesFrame<'_, '_>,
    scale: i32,
    screen_h: i32,
    shader: Option<&GlesTexProgram>,
    uniforms: &[Uniform],
) -> Result<(), Box<dyn std::error::Error>> {
    for draw in draws {
        let src = Rectangle::from_size(
            Size::<i32, smithay::utils::Buffer>::from((draw.buf_w, draw.buf_h)).to_f64(),
        );
        let dst = Rectangle::new(
            Point::from((
                draw.logical_x * scale,
                screen_h - draw.logical_y * scale - draw.buf_h,
            )),
            Size::from((draw.buf_w, draw.buf_h)),
        );
        let damage = Rectangle::from_size(Size::from((draw.buf_w, draw.buf_h)));

        frame.render_texture_from_to(
            &draw.texture, src, dst, &[damage], &[], Transform::Flipped180, 1.0,
            shader, uniforms,
        )?;
    }
    Ok(())
}

/// Render one frame: bind EGL surface, clear, draw all surfaces, swap.
pub fn render_frame(
    state: &TawcState,
    render: &mut RenderState,
    output_size: Size<i32, smithay::utils::Physical>,
) -> Result<(), Box<dyn std::error::Error>> {
    // Clone shader refs up front — `frame` below takes an exclusive borrow on
    // `render.renderer` so we can't re-borrow render fields through it.
    let wlegl_shader = render.wlegl_opaque_shader.clone();
    let shm_shader = render.shm_tint_shader.clone();
    let mut target = render.renderer.bind(&mut render.egl_surface)?;

    let mut frame = render.renderer.render(&mut target, output_size, Transform::Normal)?;

    // Background gradient (black to dark turquoise)
    if let Some(ref bg) = render.background {
        bg.draw(&mut frame, output_size.w, output_size.h)?;
    }

    let scale = state.output_scale;
    let screen_h = output_size.h;

    // Draw wlegl (AHB-backed GPU) surfaces. Buffers are drawn 1:1 at their
    // pixel dimensions; see SurfaceWleglState for why we don't honour
    // wl_surface.set_buffer_scale here.
    let wlegl_draws = collect_surface_draws(state, |surf| {
        let ws = state.surface_wlegl.get(surf)?;
        let buf = ws.current_buffer.as_ref()?;
        let data = buf.data::<WleglBufferData>()?;
        let tex = data.texture.lock().unwrap().clone()?;
        Some((tex, ws.committed_width, ws.committed_height))
    });
    draw_surfaces(&wlegl_draws, &mut frame, scale, screen_h, wlegl_shader.as_ref(), &[])?;

    // Draw SHM fallback surfaces (magenta-tinted to make the fallback obvious)
    let shm_draws = collect_surface_draws(state, |surf| {
        if state.surface_wlegl.contains_key(surf) { return None; }
        let ss = state.surface_shm.get(surf)?;
        let tex = ss.texture.as_ref()?.clone();
        Some((tex, ss.committed_width, ss.committed_height))
    });
    draw_surfaces(&shm_draws, &mut frame, scale, screen_h, shm_shader.as_ref(),
        &[Uniform::new("magenta_mix", UniformValue::_1f(1.0))])?;

    let _ = frame.finish()?;
    drop(target);
    render.egl_surface.swap_buffers(None)?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Frame callbacks
// ---------------------------------------------------------------------------

/// Send wl_buffer.release for every wlegl surface whose current buffer has
/// been used by the just-completed render pass.
///
/// libhybris's wayland-egl platform (`hybris/platforms/wayland/wayland_window_common.cpp`)
/// allocates a fixed pool of wl_buffers (default 3, see `setBufferCount(3)` in
/// the WaylandNativeWindow ctor) and blocks in `dequeueBuffer` until one is
/// released back. Without this call libhybris hangs after the first frame —
/// the buffer is attached, libhybris waits for `wl_buffer.release` before
/// dequeuing the next, the release never comes, no second commit ever
/// happens. Smithay's own auto-release fires only at the *next* commit
/// (handlers.rs:125 `merge_into`), which by definition can't help here.
///
/// To prevent double-release (which trips libhybris's `assert(it !=
/// fronted.end())` in `releaseBuffer`), a pre-commit hook registered in
/// `CompositorHandler::new_surface` clears Smithay's cached buffer assignment
/// before `merge_into` runs. See compositor.rs for that hook.
pub fn release_consumed_wlegl_buffers(state: &mut TawcState) {
    let surfaces: Vec<WlSurface> = state.surface_wlegl.keys().cloned().collect();
    if surfaces.is_empty() { return; }
    for surface in surfaces {
        let Some(wlegl_state) = state.surface_wlegl.get_mut(&surface) else { continue };
        if wlegl_state.released { continue; }
        let Some(buf) = wlegl_state.current_buffer.clone() else { continue };
        let Some(data) = buf.data::<WleglBufferData>() else { continue };
        let consumed = data.texture.lock().unwrap().is_some();
        if !consumed { continue; }
        buf.release();
        wlegl_state.released = true;
    }
}

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
