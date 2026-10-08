//! Rendering: buffer import, frame drawing, and frame callbacks.
//!
//! This module owns all GPU/EGL state and the buffer import logic.
//! TawcState (compositor.rs) owns Wayland protocol state; this module
//! turns that protocol state into pixels.

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use log::{error, info};

use smithay::backend::egl::display::PixelFormat;
use smithay::backend::egl::{EGLContext, EGLDisplay};
use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::{
    GlesFrame, GlesRenderer, GlesTexProgram,
    Uniform, UniformName, UniformType, UniformValue,
};
use smithay::backend::renderer::element::{
    Element, Id as RenderElementId, Kind as RenderElementKind, RenderElement,
};
use smithay::backend::renderer::element::solid::SolidColorRenderElement;
use smithay::backend::renderer::element::surface::{
    WaylandSurfaceRenderElement, WaylandSurfaceTexture,
};
use smithay::backend::renderer::element::AsRenderElements;
use smithay::backend::renderer::{buffer_type, Bind, BufferType, Color32F, Frame, Renderer};
use smithay::backend::renderer::utils::{
    draw_render_elements, CommitCounter, DamageSet, OpaqueRegions,
};
use smithay::reexports::wayland_server::protocol::{wl_buffer::WlBuffer, wl_surface::WlSurface};
use smithay::desktop::utils::OutputPresentationFeedback;
use smithay::utils::user_data::UserDataMap;
use smithay::utils::Monotonic;
use smithay::wayland::presentation::Refresh;
use wayland_protocols::wp::presentation_time::server::wp_presentation_feedback;
use smithay::utils::{Buffer as BufferCoord, Physical, Rectangle, Scale, Size, Transform};

use crate::compositor::TawcState;
use crate::egl_android::AndroidNativeSurface;
use crate::gl_import::AhbTextureImporter;
use crate::host::OutputHost;
use crate::placement::Layout;
use crate::scale::OutputScale;
use crate::wlegl::{wlegl_buffer_data, BufferOrigin};

/// TAWC dark window surface (#1B1B22) — matches the home/install/distro-info
/// activities so the compositor's empty space looks like the rest of the app.
/// Mirror any change here in `app/src/main/res/values-night/colors.xml`'s
/// `tawc_window_bg`.
const BACKGROUND_COLOR: Color32F = Color32F::new(0.1059, 0.1059, 0.1333, 1.0);

/// Whether to wash each surface in a debug colour identifying its
/// buffer source (libhybris-AHB → lime, gfxstream-AHB → cyan,
/// SHM → magenta). Driven by the in-app "Tint buffers based on type"
/// checkbox via `nativeSetTintBuffersByType`. Read on every frame so a
/// toggle takes effect on the next paint without restarting any client.
/// Defaults to `true` so a fresh process matches the historical
/// behaviour before any setting has been pushed in.
pub static TINT_BUFFERS_BY_TYPE: AtomicBool = AtomicBool::new(true);

/// Width, as a fraction of the buffer, of the band along each buffer edge
/// over which the buffer-type tint fades from full strength at the
/// very edge to zero. Picked to be large enough to read the colour
/// at a glance but narrow enough that the centre of every window
/// renders untinted.
const TINT_EDGE_FADE: f32 = 0.10;

/// What kind of buffer is backing a surface for this frame. Drives the
/// renderer's per-surface choice of debug tint colour. SHM is its own variant
/// rather than a `BufferOrigin` because SHM buffers don't carry a
/// `WleglBufferData`.
#[derive(Clone, Copy, Debug)]
enum SurfaceKind {
    Shm,
    Wlegl(BufferOrigin),
}

impl SurfaceKind {
    /// Per-source debug tint colour applied when `TINT_BUFFERS_BY_TYPE`
    /// is on. Picked to be saturated and easy to tell apart at a glance
    /// even through arbitrary content.
    fn tint_color(self) -> [f32; 3] {
        match self {
            SurfaceKind::Shm => [1.0, 0.0, 1.0], // magenta — software fallback path
            SurfaceKind::Wlegl(BufferOrigin::Hybris) => [0.5, 1.0, 0.0], // lime — libhybris/AHB
            #[cfg(feature = "gfxstream")]
            SurfaceKind::Wlegl(BufferOrigin::Gfxstream) => [0.0, 1.0, 1.0], // cyan — gfxstream/AHB
        }
    }

}

/// GPU-side rendering state, separate from Wayland protocol state.
///
/// Lives on TawcState (as the `render` field). Anything that touches the
/// GlesRenderer, EGL, or GL textures belongs here — the rest of TawcState
/// owns Wayland protocol state.
///
/// `egl_surface` no longer lives here: each `OutputHost` owns its own
/// EGLSurface bound to that Activity's `ANativeWindow`. Use
/// `RenderState::create_egl_surface_for_window` to make a new one when an
/// Activity registers its surface.
pub struct RenderState {
    pub renderer: GlesRenderer,
    pub importer: AhbTextureImporter,
    /// Texture shader with a `force_opaque` uniform but no tinting. Used for
    /// every surface when `TINT_BUFFERS_BY_TYPE` is off, so no-alpha buffer
    /// formats can be drawn opaque even with the custom shader.
    pub plain_shader: Option<GlesTexProgram>,
    /// Texture shader that washes the sampled colour toward a per-call
    /// `tint_color`. Used for every surface when `TINT_BUFFERS_BY_TYPE` is
    /// on; the caller picks the tint colour from [`SurfaceKind::tint_color`].
    pub tint_shader: Option<GlesTexProgram>,
    /// EGL config + display handles needed to create per-host EGLSurfaces
    /// at any time after the renderer is initialized. `EGLDisplay` is a
    /// cheap clone of the renderer's internal handle (Arc internally);
    /// pixel_format and config_id are immutable for the life of the context.
    pub egl_display: EGLDisplay,
    pub egl_pixel_format: PixelFormat,
    pub egl_config_id: *const c_void,
    /// Last so it drops after the renderer is done with the context.
    _raw_context: RawEglContext,
}

/// `EGLContext::from_raw` leaves the context externally managed, so
/// smithay never destroys it; this does.
struct RawEglContext {
    display: *const c_void,
    context: *const c_void,
}

impl Drop for RawEglContext {
    fn drop(&mut self) {
        unsafe { smithay::backend::egl::ffi::egl::DestroyContext(self.display, self.context) };
    }
}

impl RenderState {
    /// Create the EGL context (no surface yet — the first Activity provides
    /// one), the renderer, the AHB importer and our shaders. Leaves the
    /// context unbound so another thread can make it current.
    fn init() -> Result<Self, String> {
        let (raw_display, raw_config, raw_context) =
            unsafe { crate::egl_android::create_raw_egl_context() }.map_err(|e| e.to_string())?;
        let raw = RawEglContext { display: raw_display, context: raw_context };
        let egl_context = unsafe { EGLContext::from_raw(raw_display, raw_config, raw_context) }
            .map_err(|e| format!("EGLContext::from_raw: {}", e))?;

        let egl_display = egl_context.display().clone();
        let egl_pixel_format = egl_context.pixel_format().ok_or("No pixel format")?;
        let egl_config_id = egl_context.config_id();

        let mut renderer = unsafe { GlesRenderer::new(egl_context) }
            .map_err(|e| format!("GlesRenderer::new: {}", e))?;

        let importer = AhbTextureImporter::new()
            .map_err(|e| format!("Failed to load AHB importer: {}", e))?;
        info!("EGL + GlesRenderer + AHB importer ready");

        let plain_shader = compile_plain_shader(&mut renderer);
        let tint_shader = compile_tint_shader(&mut renderer);
        renderer
            .egl_context()
            .unbind()
            .map_err(|e| format!("EGL unbind: {}", e))?;

        Ok(RenderState {
            renderer,
            importer,
            plain_shader,
            tint_shader,
            egl_display,
            egl_pixel_format,
            egl_config_id,
            _raw_context: raw,
        })
    }

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

// SAFETY: RenderState is created on the compositor thread and only accessed
// from the calloop event loop on that same thread. Calloop requires Send for
// TawcState even though it never actually moves data across threads.
unsafe impl Send for RenderState {}

/// `RenderState` whose GL setup runs on a helper thread. Shader compiles
/// dominate compositor startup (~45 ms phone, ~75 ms emulator) and no
/// Wayland global depends on them, so the socket starts accepting while
/// they run. The first user (buffer creation, host surface bind, frame)
/// joins the thread.
pub struct LazyRenderState {
    ready: Option<RenderState>,
    pending: Option<std::thread::JoinHandle<Result<RenderState, String>>>,
}

impl LazyRenderState {
    pub fn spawn() -> Self {
        // Moving the `RenderState` across threads relies on `init` leaving
        // the EGL context unbound; nothing else in it is thread-affine.
        let pending = std::thread::Builder::new()
            .name("gl-init".into())
            .spawn(RenderState::init)
            .expect("spawn gl-init thread");
        Self { ready: None, pending: Some(pending) }
    }

    /// The render state, waiting for GL setup if it is still running.
    /// `None` if setup failed; that also stops the compositor.
    pub fn get(&mut self) -> Option<&mut RenderState> {
        if let Some(pending) = self.pending.take() {
            match pending.join() {
                Ok(Ok(render)) => self.ready = Some(render),
                Ok(Err(e)) => {
                    error!("GL init failed: {}", e);
                    crate::RUNNING.store(false, Ordering::SeqCst);
                }
                Err(_) => {
                    error!("GL init thread panicked");
                    crate::RUNNING.store(false, Ordering::SeqCst);
                }
            }
        }
        self.ready.as_mut()
    }
}

impl Drop for LazyRenderState {
    /// A compositor run that never touched GL (clipboard-only clients)
    /// still owns the helper thread's result; join so the EGL context is
    /// destroyed before the compositor thread reports itself gone.
    fn drop(&mut self) {
        if let Some(pending) = self.pending.take() {
            let _ = pending.join();
        }
    }
}

/// Compile the plain texture shader. One uniform: `force_opaque`
/// (`1.0` rewrites the texture's alpha channel to `1.0` before
/// blending; `0.0` leaves it as-is). Used when `TINT_BUFFERS_BY_TYPE`
/// is off, but kept distinct from smithay's stock EXTERNAL_OES shader so
/// the plain and tinting paths share one draw setup.
fn compile_plain_shader(renderer: &mut GlesRenderer) -> Option<GlesTexProgram> {
    match renderer.compile_custom_texture_shader(
        SHADER_SOURCE_PLAIN,
        &[UniformName::new("force_opaque", UniformType::_1f)],
    ) {
        Ok(program) => {
            info!("plain texture shader compiled");
            Some(program)
        }
        Err(e) => {
            error!("Failed to compile plain texture shader: {:?}", e);
            None
        }
    }
}

/// Compile the tinting texture shader. Uniforms:
///  - `force_opaque` (`1.0`/`0.0`): same as the plain shader.
///  - `tint_color` (`vec3`): hue to wash the sampled colour toward.
///    Channels at `1.0` get boosted, channels at `0.0` get suppressed.
///  - `edge_fade` (`float`): the tint is at full strength on the
///    very edge of the buffer and fades linearly to zero at this
///    normalized texture-coordinate distance. Set to `0` (or below) to
///    skip the fade and tint the whole frame.
///
/// The tint formula generalises the original SHM-only magenta wash:
/// each channel is multiplied by `mix(0.4, 1.0, tint_color[c])` and
/// then `tint_color * 0.3` is added. With `tint_color = (1, 0, 1)`
/// this collapses back to the previous `(r * 1.0 + 0.3, g * 0.4, b *
/// 1.0 + 0.3)` magenta formula.
fn compile_tint_shader(renderer: &mut GlesRenderer) -> Option<GlesTexProgram> {
    match renderer.compile_custom_texture_shader(
        SHADER_SOURCE_TINT,
        &[
            UniformName::new("force_opaque", UniformType::_1f),
            UniformName::new("tint_color", UniformType::_3f),
            UniformName::new("edge_fade", UniformType::_1f),
        ],
    ) {
        Ok(program) => {
            info!("tinting texture shader compiled");
            Some(program)
        }
        Err(e) => {
            error!("Failed to compile tinting texture shader: {:?}", e);
            None
        }
    }
}

const SHADER_SOURCE_PLAIN: &str = r#"
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
uniform float force_opaque;
varying vec2 v_coords;

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

void main() {
    vec4 color = texture2D(tex, v_coords);
    if (force_opaque == 1.0) {
        color = vec4(color.rgb, 1.0) * alpha;
    } else {
        color = color * alpha;
    }
#if defined(DEBUG_FLAGS)
    if (tint == 1.0)
        color = vec4(0.0, 0.2, 0.0, 0.2) + color * 0.8;
#endif
    gl_FragColor = color;
}
"#;

const SHADER_SOURCE_TINT: &str = r#"
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
uniform float force_opaque;
uniform vec3 tint_color;
uniform float edge_fade;
varying vec2 v_coords;

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

void main() {
    vec4 color = texture2D(tex, v_coords);
    if (force_opaque == 1.0) {
        color = vec4(color.rgb, 1.0) * alpha;
    } else {
        color = color * alpha;
    }

    // Distance, in texture coordinates, to the nearest drawn buffer edge.
    vec2 clamped_coords = clamp(v_coords, vec2(0.0), vec2(1.0));
    vec2 d2 = min(clamped_coords, vec2(1.0) - clamped_coords);
    float d = min(d2.x, d2.y);
    // 1.0 right at the edge → 0.0 once we're edge_fade in.
    float edge_strength = edge_fade > 0.0
        ? clamp(1.0 - d / edge_fade, 0.0, 1.0)
        : 1.0;

    vec3 tinted = color.rgb * mix(vec3(0.4), vec3(1.0), tint_color) + tint_color * 0.3;
    color.rgb = mix(color.rgb, tinted, edge_strength);

#if defined(DEBUG_FLAGS)
    if (tint == 1.0)
        color = vec4(0.0, 0.2, 0.0, 0.2) + color * 0.8;
#endif
    gl_FragColor = color;
}
"#;

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

struct TawcWaylandRenderElement<'a> {
    inner: WaylandSurfaceRenderElement<GlesRenderer>,
    shader: Option<&'a GlesTexProgram>,
    uniforms: Vec<Uniform<'static>>,
    /// The window's placement scale. Smithay sizes surface elements from
    /// the scale passed at draw time, so it is folded in here; locations
    /// were already computed at the combined scale.
    window_scale: f64,
}

impl TawcWaylandRenderElement<'_> {
    fn scale(&self, scale: Scale<f64>) -> Scale<f64> {
        Scale::from((scale.x * self.window_scale, scale.y * self.window_scale))
    }
}

impl Element for TawcWaylandRenderElement<'_> {
    fn id(&self) -> &RenderElementId {
        self.inner.id()
    }

    fn current_commit(&self) -> CommitCounter {
        self.inner.current_commit()
    }

    fn src(&self) -> Rectangle<f64, BufferCoord> {
        self.inner.src()
    }

    fn transform(&self) -> Transform {
        self.inner.transform()
    }

    fn geometry(&self, scale: Scale<f64>) -> Rectangle<i32, Physical> {
        self.inner.geometry(self.scale(scale))
    }

    fn damage_since(
        &self,
        scale: Scale<f64>,
        commit: Option<CommitCounter>,
    ) -> DamageSet<i32, Physical> {
        self.inner.damage_since(self.scale(scale), commit)
    }

    fn opaque_regions(&self, scale: Scale<f64>) -> OpaqueRegions<i32, Physical> {
        self.inner.opaque_regions(self.scale(scale))
    }

    fn alpha(&self) -> f32 {
        self.inner.alpha()
    }

    fn kind(&self) -> RenderElementKind {
        self.inner.kind()
    }
}

impl RenderElement<GlesRenderer> for TawcWaylandRenderElement<'_> {
    fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        src: Rectangle<f64, BufferCoord>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque_regions: &[Rectangle<i32, Physical>],
        _cache: Option<&UserDataMap>,
    ) -> Result<(), smithay::backend::renderer::gles::GlesError> {
        match self.inner.texture() {
            WaylandSurfaceTexture::Texture(texture) => frame.render_texture_from_to(
                texture,
                src,
                dst,
                damage,
                opaque_regions,
                self.inner.transform(),
                self.inner.alpha(),
                self.shader,
                &self.uniforms,
            ),
            WaylandSurfaceTexture::SolidColor(color) => {
                frame.draw_solid(dst, damage, *color * self.inner.alpha())
            }
        }
    }
}

fn surface_kind_for_buffer(buffer: &WlBuffer) -> (SurfaceKind, bool) {
    if let Some(data) = wlegl_buffer_data(buffer) {
        return (SurfaceKind::Wlegl(data.origin), !data.has_alpha);
    }
    match buffer_type(buffer) {
        Some(BufferType::Shm) => {
            (SurfaceKind::Shm, false)
        }
        _ => (SurfaceKind::Shm, false),
    }
}

fn wrap_wayland_render_element<'a>(
    inner: WaylandSurfaceRenderElement<GlesRenderer>,
    window_scale: f64,
    plain_shader: Option<&'a GlesTexProgram>,
    tint_shader: Option<&'a GlesTexProgram>,
    tint_enabled: bool,
) -> TawcWaylandRenderElement<'a> {
    let (kind, implicit_opaque) = surface_kind_for_buffer(inner.buffer());
    let force_opaque = if implicit_opaque { 1.0 } else { 0.0 };
    let (shader, uniforms): (Option<&GlesTexProgram>, Vec<Uniform>) = if tint_enabled {
        let [r, g, b] = kind.tint_color();
        (
            tint_shader,
            vec![
                Uniform::new("force_opaque", UniformValue::_1f(force_opaque)),
                Uniform::new("tint_color", UniformValue::_3f(r, g, b)),
                Uniform::new("edge_fade", UniformValue::_1f(TINT_EDGE_FADE)),
            ],
        )
    } else {
        (
            plain_shader,
            vec![Uniform::new("force_opaque", UniformValue::_1f(force_opaque))],
        )
    };

    TawcWaylandRenderElement {
        inner,
        shader,
        uniforms: uniforms.into_iter().map(|u| u.into_owned()).collect(),
        window_scale,
    }
}

/// Dim behind a dialog, like Android's dialog scrim.
const SCRIM_COLOR: Color32F = Color32F::new(0.0, 0.0, 0.0, 0.5);

enum TawcRenderElement<'a> {
    Wayland(TawcWaylandRenderElement<'a>),
    Solid(SolidColorRenderElement),
}

impl Element for TawcRenderElement<'_> {
    fn id(&self) -> &RenderElementId {
        match self {
            Self::Wayland(e) => e.id(),
            Self::Solid(e) => e.id(),
        }
    }

    fn current_commit(&self) -> CommitCounter {
        match self {
            Self::Wayland(e) => e.current_commit(),
            Self::Solid(e) => e.current_commit(),
        }
    }

    fn src(&self) -> Rectangle<f64, BufferCoord> {
        match self {
            Self::Wayland(e) => e.src(),
            Self::Solid(e) => e.src(),
        }
    }

    fn transform(&self) -> Transform {
        match self {
            Self::Wayland(e) => e.transform(),
            Self::Solid(e) => e.transform(),
        }
    }

    fn geometry(&self, scale: Scale<f64>) -> Rectangle<i32, Physical> {
        match self {
            Self::Wayland(e) => e.geometry(scale),
            Self::Solid(e) => e.geometry(scale),
        }
    }

    fn damage_since(
        &self,
        scale: Scale<f64>,
        commit: Option<CommitCounter>,
    ) -> DamageSet<i32, Physical> {
        match self {
            Self::Wayland(e) => e.damage_since(scale, commit),
            Self::Solid(e) => e.damage_since(scale, commit),
        }
    }

    fn opaque_regions(&self, scale: Scale<f64>) -> OpaqueRegions<i32, Physical> {
        match self {
            Self::Wayland(e) => e.opaque_regions(scale),
            Self::Solid(e) => e.opaque_regions(scale),
        }
    }

    fn alpha(&self) -> f32 {
        match self {
            Self::Wayland(e) => e.alpha(),
            Self::Solid(e) => e.alpha(),
        }
    }

    fn kind(&self) -> RenderElementKind {
        match self {
            Self::Wayland(e) => e.kind(),
            Self::Solid(e) => e.kind(),
        }
    }
}

impl RenderElement<GlesRenderer> for TawcRenderElement<'_> {
    fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        src: Rectangle<f64, BufferCoord>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque_regions: &[Rectangle<i32, Physical>],
        cache: Option<&UserDataMap>,
    ) -> Result<(), smithay::backend::renderer::gles::GlesError> {
        match self {
            Self::Wayland(e) => e.draw(frame, src, dst, damage, opaque_regions, cache),
            Self::Solid(e) => RenderElement::<GlesRenderer>::draw(
                e, frame, src, dst, damage, opaque_regions, cache,
            ),
        }
    }
}

fn draw_elements(
    elements: &[TawcRenderElement<'_>],
    frame: &mut GlesFrame<'_, '_>,
    scale: OutputScale,
    screen_w: i32,
    screen_h: i32,
) -> Result<(), Box<dyn std::error::Error>> {
    let output_damage = Rectangle::from_size(Size::from((screen_w, screen_h)));
    draw_render_elements::<GlesRenderer, _, _>(
        frame,
        Scale::from(scale.fractional()),
        &elements,
        &[output_damage],
    )?;
    Ok(())
}

/// Render one frame for the active desktop host: bind that host's EGL surface,
/// clear, draw each window at its placement (with a scrim under the topmost
/// dialog), and swap. Caller skips hosts whose `egl_surface` is `None`.
pub fn render_frame(
    state: &mut TawcState,
    host: &mut OutputHost,
) -> Result<(), Box<dyn std::error::Error>> {
    let egl_surface = host
        .egl_surface
        .as_mut()
        .expect("render_frame called for host without EGLSurface");

    let output_size = host.physical_size;
    let scale = state.output_scale;
    let screen_w = output_size.w;
    let screen_h = output_size.h;

    let render = state.render.get().ok_or("renderer unavailable")?;
    let plain_shader = render.plain_shader.as_ref();
    let tint_shader = render.tint_shader.as_ref();
    let tint_enabled = TINT_BUFFERS_BY_TYPE.load(Ordering::Relaxed);
    // Front to back, as `draw_render_elements` expects.
    let mut elements: Vec<TawcRenderElement> = Vec::new();
    if let Some(space) = state.desktop.host_space(&host.activity_id) {
        let layout = Layout::new(space, Size::from(host.logical_size));
        for (i, entry) in layout.entries.iter().enumerate().rev() {
            let placement = entry.placement;
            let location = placement.offset.to_physical(scale.fractional()).to_i32_round();
            let surfaces: Vec<WaylandSurfaceRenderElement<GlesRenderer>> = entry
                .window
                .render_elements(
                    &mut render.renderer,
                    location,
                    Scale::from(scale.fractional() * placement.scale),
                    1.0,
                );
            elements.extend(surfaces.into_iter().map(|surface| {
                TawcRenderElement::Wayland(wrap_wayland_render_element(
                    surface,
                    placement.scale,
                    plain_shader,
                    tint_shader,
                    tint_enabled,
                ))
            }));
            if layout.scrim_below == Some(i) {
                elements.push(TawcRenderElement::Solid(SolidColorRenderElement::new(
                    RenderElementId::new(),
                    Rectangle::from_size(output_size),
                    CommitCounter::default(),
                    SCRIM_COLOR,
                    RenderElementKind::Unspecified,
                )));
            }
        }
    }

    let mut target = render.renderer.bind(egl_surface)?;
    let mut frame = render
        .renderer
        .render(&mut target, output_size, Transform::Flipped180)?;

    // Flat background matching the Material3 dark surface used by the rest
    // of the app (home / install / distro-info screens).
    frame.clear(BACKGROUND_COLOR, &[Rectangle::from_size(output_size)])?;

    draw_elements(&elements, &mut frame, scale, screen_w, screen_h)?;

    let _ = frame.finish()?;
    drop(target);
    egl_surface.swap_buffers(None)?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Frame callbacks
// ---------------------------------------------------------------------------

/// Send frame-done callbacks for windows Smithay currently considers mapped
/// on TAWC's desktop output. Android host foreground/background transitions
/// update the visible desktop projection; Smithay owns the popup/subsurface traversal.
/// `time` is the vsync timestamp (`CLOCK_MONOTONIC`).
pub fn send_frame_callbacks(state: &TawcState, time: Duration) {
    let output = &state.output;

    let Some(visible_space) = state.desktop.visible_space(&state.hosts) else {
        return;
    };

    for window in visible_space.elements() {
        window.send_frame(
            output,
            time,
            None,
            |_: &WlSurface, _: &smithay::wayland::compositor::SurfaceData| {
                Some(output.clone())
            },
        );
    }
}

/// Answer `wp_presentation` feedback for the frame just swapped on the
/// visible host. `time` is the vsync the frame was rendered on, an
/// approximation of when it reaches the panel.
pub fn report_presentation_feedback(state: &TawcState, time: Duration) {
    let output = &state.output;
    let Some(visible_space) = state.desktop.visible_space(&state.hosts) else {
        return;
    };
    let mut feedback = OutputPresentationFeedback::new(output);
    for window in visible_space.elements() {
        window.take_presentation_feedback(
            &mut feedback,
            |_, _| Some(output.clone()),
            |_, _| wp_presentation_feedback::Kind::empty(),
        );
    }
    feedback.presented::<_, Monotonic>(
        time,
        Refresh::fixed(state.output_refresh_period()),
        state.frame_clock.msc,
        wp_presentation_feedback::Kind::Vsync,
    );
}
