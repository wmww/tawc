//! Rendering: buffer import, frame drawing, and frame callbacks.
//!
//! This module owns all GPU/EGL state and the buffer import logic.
//! TawcState (compositor.rs) owns Wayland protocol state; this module
//! turns that protocol state into pixels.

use std::ffi::c_void;

use log::{error, info};

use smithay::backend::egl::display::PixelFormat;
use smithay::backend::egl::EGLDisplay;
use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::{
    GlesFrame, GlesRenderer, GlesTexProgram, GlesTexture,
    Uniform, UniformName, UniformType, UniformValue,
};
use smithay::backend::renderer::{Bind, Color32F, Frame, ImportMemWl, Renderer};
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Point, Rectangle, Size, Transform};
use smithay::desktop::PopupManager;
use smithay::wayland::compositor::{
    with_surface_tree_downward, SubsurfaceCachedState, SurfaceAttributes, TraversalAction,
};

use crate::compositor::TawcState;
use crate::egl_android::AndroidNativeSurface;
use crate::gl_import::AhbTextureImporter;
use crate::host::OutputHost;
use crate::wlegl::WleglBufferData;
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;

/// Material3 dark surface (#141218) — matches the home/install/distro-info
/// activities so the compositor's empty space looks like the rest of the app.
const BACKGROUND_COLOR: Color32F = Color32F::new(0.0784, 0.0706, 0.0941, 1.0);

/// GPU-side rendering state, separate from Wayland protocol state.
///
/// Lives in LoopData alongside TawcState. Anything that touches the
/// GlesRenderer, EGL, or GL textures belongs here — not in TawcState.
///
/// `egl_surface` no longer lives here: each `OutputHost` owns its own
/// EGLSurface bound to that Activity's `ANativeWindow`. Use
/// `RenderState::create_egl_surface_for_window` to make a new one when an
/// Activity registers its surface.
pub struct RenderState {
    pub renderer: GlesRenderer,
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
    pub raw_egl_display: *const c_void,
    pub raw_egl_context: *const c_void,
    /// EGL config + display handles needed to create per-host EGLSurfaces
    /// at any time after the renderer is initialized. `EGLDisplay` is a
    /// cheap clone of the renderer's internal handle (Arc internally);
    /// pixel_format and config_id are immutable for the life of the context.
    pub egl_display: EGLDisplay,
    pub egl_pixel_format: PixelFormat,
    pub egl_config_id: *const c_void,
}

impl RenderState {
    /// Create an `EGLSurface` bound to a freshly-acquired ANativeWindow.
    /// The host takes ownership of the surface; on drop it calls
    /// `eglDestroySurface`.
    pub fn create_egl_surface_for_window(
        &self,
        native_window: *mut c_void,
    ) -> Result<EGLSurface, smithay::backend::egl::EGLError> {
        let native_surface = AndroidNativeSurface::new(native_window);
        unsafe {
            EGLSurface::new(
                &self.egl_display,
                self.egl_pixel_format,
                self.egl_config_id,
                native_surface,
            )
        }
    }

    /// Bind a freshly-created EGLSurface onto the given host. Errors are
    /// logged and the host's `egl_surface` is left as `None`.
    pub fn attach_host_surface(&self, host: &mut OutputHost) {
        match self.create_egl_surface_for_window(host.native_window()) {
            Ok(surf) => {
                host.egl_surface = Some(surf);
                info!(
                    "Bound EGLSurface for host {} ({}x{})",
                    host.activity_id, host.physical_size.w, host.physical_size.h
                );
            }
            Err(e) => {
                error!(
                    "Failed to create EGLSurface for host {}: {:?}",
                    host.activity_id, e
                );
            }
        }
    }
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

    // Catch up on any X11 surface ↔ host associations that the commit
    // hook missed because smithay's `WL_SURFACE_SERIAL` handler hadn't
    // yet bound the X11Surface to its wl_surface when the commit fired.
    // No-op if every x11_surface is already in `x11_to_host`.
    crate::xwayland::associate_pending_x11_surfaces(state);

    let toplevel_surfaces: Vec<_> = state
        .toplevels
        .iter()
        .map(|t| t.wl_surface().clone())
        .collect();

    // Collect all root surfaces to walk: toplevels + their popup surfaces
    // + every X11 surface that has a backing wl_surface.
    let mut all_roots: Vec<_> = toplevel_surfaces.clone();
    for toplevel_wl in &toplevel_surfaces {
        for (popup, _) in PopupManager::popups_for_surface(toplevel_wl) {
            all_roots.push(popup.wl_surface().clone());
        }
    }
    for x11 in &state.x11_surfaces {
        if let Some(wl) = x11.wl_surface() {
            all_roots.push(wl);
        }
    }

    // Collect (surface, buffer, buffer_scale, viewport_dst) tuples that need
    // importing. We also pick up buffer_scale and viewport here so SHM
    // surfaces start with the right values; the commit handler keeps these
    // refreshed for state-only commits (set_buffer_scale, viewport changes).
    let mut to_import: Vec<(WlSurface, wl_buffer::WlBuffer, i32, Option<(i32, i32)>)> = Vec::new();

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
                let buffer_scale = attrs.buffer_scale.max(1);
                let new_shm_buf = match &attrs.buffer {
                    Some(BufferAssignment::NewBuffer(buf))
                        if matches!(buffer_type(buf), Some(BufferType::Shm)) =>
                    {
                        Some(buf.clone())
                    }
                    _ => None,
                };
                drop(guard);
                let mut vp_guard = surf_states
                    .cached_state
                    .get::<smithay::wayland::viewporter::ViewportCachedState>();
                let viewport_dst = vp_guard.current().dst.map(|s| (s.w, s.h));

                if let Some(buf) = new_shm_buf {
                    // Skip if we already imported this exact buffer
                    if let Some(existing) = state.surface_shm.get(surf) {
                        if let Some(ref current_buf) = existing.current_buffer {
                            if current_buf.id() == buf.id() {
                                return;
                            }
                        }
                    }
                    to_import.push((surf.clone(), buf, buffer_scale, viewport_dst));
                }
            },
            |_, _, &()| true,
        );
    }

    let mut imported = false;
    for (surface, buf, buffer_scale, viewport_dst) in to_import {
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
                        buffer_scale: 1,
                        viewport_dst: None,
                    });
                // Don't call old_buf.release() here — smithay's
                // SurfaceAttributes::merge_into has already released the
                // previous buffer during commit handling. A second release
                // breaks GTK4 cairo: gdkcairocontext-wayland.c caches the
                // first release for reuse, and a second release destroys
                // the cached surface, leaving a dangling pointer that
                // produces "target surface has been finished" warnings on
                // the next frame.
                shm_state.texture = Some(texture);
                shm_state.current_buffer = Some(buf);
                shm_state.committed_width = width;
                shm_state.committed_height = height;
                shm_state.buffer_scale = buffer_scale;
                shm_state.viewport_dst = viewport_dst;
                imported = true;
                if is_new {
                    info!(
                        "SHM buffer imported: {}x{} scale={} viewport={:?} for {:?}",
                        width, height, buffer_scale, viewport_dst, surface.id()
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

/// A surface ready to draw.
///
/// `buf_w`/`buf_h` are the buffer's pixel dimensions (used as the source
/// rectangle). `logical_w`/`logical_h` is the surface's logical size (from
/// `wp_viewport.set_destination` if set, else `buffer / buffer_scale`).
/// `logical_x`/`logical_y` is the surface's absolute position in logical
/// (surface-local) coordinates.
struct SurfaceDraw {
    texture: GlesTexture,
    logical_x: i32,
    logical_y: i32,
    logical_w: i32,
    logical_h: i32,
    buf_w: i32,
    buf_h: i32,
}

/// Compute a surface's logical (on-screen, surface-local) size:
/// `wp_viewport.set_destination` overrides if set, otherwise we fall back to
/// `buffer_size / buffer_scale` per the wl_surface spec.
///
/// TODO: `wp_viewport.set_source` (sub-rect crop) is not honoured here —
/// `draw_surfaces` always samples the full buffer. No current client
/// (Firefox / GTK / vkcube / weston-simple-egl) uses `set_source`, so this
/// is fine for now, but a client that does will see an uncropped result.
fn logical_size(
    buf_w: i32,
    buf_h: i32,
    buffer_scale: i32,
    viewport_dst: Option<(i32, i32)>,
) -> (i32, i32) {
    if let Some((w, h)) = viewport_dst {
        (w, h)
    } else {
        (buf_w / buffer_scale, buf_h / buffer_scale)
    }
}

/// Walk a single surface tree, collecting drawable surfaces at their absolute
/// logical positions. `offset_x`/`offset_y` is the root's position (0,0 for
/// toplevels, popup geometry origin for popups).
fn collect_tree_draws(
    root: &WlSurface,
    offset_x: i32,
    offset_y: i32,
    surface_fn: &impl Fn(&WlSurface) -> Option<(GlesTexture, i32, i32, i32, i32)>,
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
            if let Some((tex, buf_w, buf_h, lw, lh)) = surface_fn(surf) {
                out.push(SurfaceDraw {
                    texture: tex,
                    logical_x: abs_x,
                    logical_y: abs_y,
                    logical_w: lw,
                    logical_h: lh,
                    buf_w,
                    buf_h,
                });
            }
        },
        |_, _, _| true,
    );
}

/// Walk all toplevel and popup surface trees assigned to the given host,
/// collecting drawable surfaces.
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
    host_id: &crate::host::ActivityId,
    surface_fn: impl Fn(&WlSurface) -> Option<(GlesTexture, i32, i32, i32, i32)>,
) -> Vec<SurfaceDraw> {
    let mut draws = Vec::new();

    for toplevel in &state.toplevels {
        let root = toplevel.wl_surface();
        // Skip toplevels not assigned to this host (phase 2 onward).
        // For phase 0/1 with a single host this never filters anything;
        // it's the correct behaviour once multi-window lands.
        match state.toplevel_to_host.get(root) {
            Some(assigned) if assigned == host_id => {}
            _ => continue,
        }

        let start = draws.len();
        collect_tree_draws(root, 0, 0, &surface_fn, &mut draws);
        draws[start..].reverse();

        for (popup, location) in PopupManager::popups_for_surface(root) {
            let start = draws.len();
            collect_tree_draws(popup.wl_surface(), location.x, location.y, &surface_fn, &mut draws);
            draws[start..].reverse();
        }
    }

    // X11 toplevels (XWayland). They live in a parallel list — the
    // X11Surface holds a backing wl_surface that the standard
    // commit/buffer path already populates; we just walk it here.
    for x11 in &state.x11_surfaces {
        let root = match x11.wl_surface() {
            Some(s) => s,
            None => continue,
        };
        match state.x11_to_host.get(&root) {
            Some(assigned) if assigned == host_id => {}
            _ => continue,
        }
        let start = draws.len();
        collect_tree_draws(&root, 0, 0, &surface_fn, &mut draws);
        draws[start..].reverse();
    }

    draws
}

/// Draw collected surfaces with the logical→physical coordinate transform.
///
/// Per the wl_surface spec, the surface's logical (surface-local) size is
/// `buffer_size / buffer_scale`, optionally overridden by
/// `wp_viewport.set_destination`. The on-screen physical size is that
/// logical size times the output scale. So:
///
///   dst_w = logical_w * output_scale
///   dst_h = logical_h * output_scale
///   dst_x = logical_x * output_scale
///   dst_y = screen_h - logical_y * output_scale - dst_h
///
/// The Y flip is because smithay's GlesRenderer projection has Y=0 at the
/// screen bottom; `Transform::Flipped180` then flips the texture's Y inside
/// the destination rect (Wayland buffers are Y-down, GL is Y-up).
fn draw_surfaces(
    draws: &[SurfaceDraw],
    frame: &mut GlesFrame<'_, '_>,
    scale: i32,
    screen_h: i32,
    shader: Option<&GlesTexProgram>,
    uniforms: &[Uniform],
) -> Result<(), Box<dyn std::error::Error>> {
    for draw in draws {
        let dst_w = draw.logical_w * scale;
        let dst_h = draw.logical_h * scale;
        let src = Rectangle::from_size(
            Size::<i32, smithay::utils::Buffer>::from((draw.buf_w, draw.buf_h)).to_f64(),
        );
        let dst = Rectangle::new(
            Point::from((
                draw.logical_x * scale,
                screen_h - draw.logical_y * scale - dst_h,
            )),
            Size::from((dst_w, dst_h)),
        );
        let damage = Rectangle::from_size(Size::from((dst_w, dst_h)));

        frame.render_texture_from_to(
            &draw.texture, src, dst, &[damage], &[], Transform::Flipped180, 1.0,
            shader, uniforms,
        )?;
    }
    Ok(())
}

/// Render one frame for a single host: bind that host's EGL surface, clear,
/// draw all surfaces assigned to it, swap. Caller skips hosts whose
/// `egl_surface` is `None`.
pub fn render_frame(
    state: &TawcState,
    render: &mut RenderState,
    host: &mut OutputHost,
) -> Result<(), Box<dyn std::error::Error>> {
    let egl_surface = host
        .egl_surface
        .as_mut()
        .expect("render_frame called for host without EGLSurface");

    // Clone shader refs up front — `frame` below takes an exclusive borrow on
    // `render.renderer` so we can't re-borrow render fields through it.
    let wlegl_shader = render.wlegl_opaque_shader.clone();
    let shm_shader = render.shm_tint_shader.clone();
    let mut target = render.renderer.bind(egl_surface)?;

    let output_size = host.physical_size;
    let mut frame = render.renderer.render(&mut target, output_size, Transform::Normal)?;

    // Flat background matching the Material3 dark surface used by the rest
    // of the app (home / install / distro-info screens).
    frame.clear(BACKGROUND_COLOR, &[Rectangle::from_size(output_size)])?;

    let scale = state.output_scale;
    let screen_h = output_size.h;

    // Draw wlegl (AHB-backed GPU) surfaces.
    let host_id = host.activity_id.clone();
    let wlegl_draws = collect_surface_draws(state, &host_id, |surf| {
        let ws = state.surface_wlegl.get(surf)?;
        let buf = ws.current_buffer.as_ref()?;
        let data = buf.data::<WleglBufferData>()?;
        let tex = data.texture.lock().unwrap().clone()?;
        let (lw, lh) = logical_size(
            ws.committed_width, ws.committed_height,
            ws.buffer_scale, ws.viewport_dst,
        );
        Some((tex, ws.committed_width, ws.committed_height, lw, lh))
    });
    draw_surfaces(&wlegl_draws, &mut frame, scale, screen_h, wlegl_shader.as_ref(), &[])?;

    // Draw SHM fallback surfaces (magenta-tinted to make the fallback obvious)
    let shm_draws = collect_surface_draws(state, &host_id, |surf| {
        if state.surface_wlegl.contains_key(surf) { return None; }
        let ss = state.surface_shm.get(surf)?;
        let tex = ss.texture.as_ref()?.clone();
        let (lw, lh) = logical_size(
            ss.committed_width, ss.committed_height,
            ss.buffer_scale, ss.viewport_dst,
        );
        Some((tex, ss.committed_width, ss.committed_height, lw, lh))
    });
    draw_surfaces(&shm_draws, &mut frame, scale, screen_h, shm_shader.as_ref(),
        &[Uniform::new("magenta_mix", UniformValue::_1f(1.0))])?;

    let _ = frame.finish()?;
    drop(target);
    egl_surface.swap_buffers(None)?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Frame callbacks
// ---------------------------------------------------------------------------

/// Send frame-done callbacks to all surfaces in foreground hosts'
/// toplevel and popup trees. Backgrounded hosts (Phase 7) don't get
/// callbacks — their clients sit on `Suspended` and shouldn't be
/// asking for new frames. Without this gate a well-behaved client
/// keeps drawing offscreen indefinitely on every Activity switch.
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
        // Look up the host this toplevel paints into; skip if it isn't
        // currently foreground. An orphaned toplevel (host removed
        // because Activity was destroyed) is also skipped — we'll send
        // close to it via the cleanup path.
        let host_is_fg = state
            .toplevel_to_host
            .get(wl)
            .and_then(|id| state.hosts.get(id))
            .map(|h| h.foreground)
            .unwrap_or(false);
        if !host_is_fg {
            continue;
        }
        send_callbacks(wl);
        for (popup, _) in PopupManager::popups_for_surface(wl) {
            send_callbacks(popup.wl_surface());
        }
    }

    // X11 surfaces (XWayland clients) — same foreground gate as
    // xdg toplevels. Without callbacks, animating X clients (xclock,
    // glxgears, …) push only their first frame and then sit silent
    // waiting for the compositor to acknowledge.
    for x11 in &state.x11_surfaces {
        let wl = match x11.wl_surface() {
            Some(s) => s,
            None => continue,
        };
        let host_is_fg = state
            .x11_to_host
            .get(&wl)
            .and_then(|id| state.hosts.get(id))
            .map(|h| h.foreground)
            .unwrap_or(false);
        if !host_is_fg {
            continue;
        }
        send_callbacks(&wl);
    }
}
