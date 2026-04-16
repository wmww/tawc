//! Background gradient renderer.
//!
//! Draws a fullscreen gradient from black to dark turquoise in the upper-right
//! corner. Uses a custom Smithay texture shader that computes the gradient from
//! texture coordinates, applied to a 1x1 dummy texture rendered fullscreen.

use log::{error, info};

use smithay::backend::renderer::gles::{
    GlesFrame, GlesRenderer, GlesTexProgram, GlesTexture,
    Uniform, UniformName, UniformType, UniformValue,
};
use smithay::utils::{Point, Rectangle, Size, Transform};

/// Dark turquoise target color in the upper-right corner (R, G, B).
const TURQUOISE: (f32, f32, f32) = (0.1, 0.75, 0.95);

/// Compiled gradient shader and dummy texture for background rendering.
pub struct BackgroundRenderer {
    shader: GlesTexProgram,
    texture: GlesTexture,
}

impl BackgroundRenderer {
    /// Compile the gradient shader and create a 1x1 dummy texture.
    /// Must be called with EGL context current.
    pub fn new(renderer: &mut GlesRenderer) -> Option<Self> {
        let shader = compile_gradient_shader(renderer)?;
        let texture = create_dummy_texture(renderer)?;
        Some(BackgroundRenderer { shader, texture })
    }

    /// Draw the fullscreen background gradient.
    pub fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        width: i32,
        height: i32,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let src = Rectangle::from_size(Size::<i32, smithay::utils::Buffer>::from((1, 1)).to_f64());
        let dst = Rectangle::new(Point::from((0, 0)), Size::from((width, height)));
        let damage = Rectangle::from_size(Size::from((width, height)));

        frame.render_texture_from_to(
            &self.texture,
            src,
            dst,
            &[damage],
            &[],
            Transform::Normal,
            1.0,
            Some(&self.shader),
            &[
                Uniform::new("target_r", UniformValue::_1f(TURQUOISE.0)),
                Uniform::new("target_g", UniformValue::_1f(TURQUOISE.1)),
                Uniform::new("target_b", UniformValue::_1f(TURQUOISE.2)),
            ],
        )?;
        Ok(())
    }
}

/// Compile the gradient fragment shader.
fn compile_gradient_shader(renderer: &mut GlesRenderer) -> Option<GlesTexProgram> {
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

uniform float target_r;
uniform float target_g;
uniform float target_b;

#if defined(DEBUG_FLAGS)
uniform float tint;
#endif

void main() {
    // v_coords: (0,0) at top-left, (1,1) at bottom-right in screen space.
    // Gradient: black at top, turquoise at bottom.
    float t = 1.0 - v_coords.y;
    gl_FragColor = vec4(target_r * t, target_g * t, target_b * t, 1.0);
}
"#,
        &[
            UniformName::new("target_r", UniformType::_1f),
            UniformName::new("target_g", UniformType::_1f),
            UniformName::new("target_b", UniformType::_1f),
        ],
    ) {
        Ok(program) => {
            info!("Background gradient shader compiled");
            Some(program)
        }
        Err(e) => {
            error!("Failed to compile background gradient shader: {:?}", e);
            None
        }
    }
}

/// Create a 1x1 white GL texture wrapped as a GlesTexture.
fn create_dummy_texture(renderer: &GlesRenderer) -> Option<GlesTexture> {
    use std::ffi::c_void;

    let lib = match unsafe { libloading::Library::new("libGLESv2.so") } {
        Ok(l) => l,
        Err(e) => {
            error!("Background: failed to load libGLESv2.so: {}", e);
            return None;
        }
    };

    unsafe {
        type FnGenTextures = unsafe extern "C" fn(i32, *mut u32);
        type FnBindTexture = unsafe extern "C" fn(u32, u32);
        type FnTexImage2D = unsafe extern "C" fn(u32, i32, i32, i32, i32, i32, u32, u32, *const c_void);
        type FnTexParameteri = unsafe extern "C" fn(u32, u32, i32);

        macro_rules! load {
            ($name:expr, $ty:ty) => {{
                let sym: libloading::Symbol<*const c_void> = lib.get($name).ok()?;
                std::mem::transmute::<*const c_void, $ty>(*sym)
            }};
        }

        let gen_textures: FnGenTextures = load!(b"glGenTextures\0", FnGenTextures);
        let bind_texture: FnBindTexture = load!(b"glBindTexture\0", FnBindTexture);
        let tex_image_2d: FnTexImage2D = load!(b"glTexImage2D\0", FnTexImage2D);
        let tex_parameteri: FnTexParameteri = load!(b"glTexParameteri\0", FnTexParameteri);

        const GL_TEXTURE_2D: u32 = 0x0DE1;
        const GL_RGBA: u32 = 0x1908;
        const GL_UNSIGNED_BYTE: u32 = 0x1401;
        const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
        const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
        const GL_NEAREST: i32 = 0x2600;

        let mut tex = 0u32;
        gen_textures(1, &mut tex);
        bind_texture(GL_TEXTURE_2D, tex);
        let white: [u8; 4] = [255, 255, 255, 255];
        tex_image_2d(
            GL_TEXTURE_2D, 0, GL_RGBA as i32, 1, 1, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, white.as_ptr() as *const c_void,
        );
        tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        bind_texture(GL_TEXTURE_2D, 0);

        std::mem::forget(lib);

        let texture = GlesTexture::from_raw_with_flags(
            renderer, None, false, false, false, tex,
            Size::from((1, 1)),
        );

        info!("Background 1x1 dummy texture created (GL tex {})", tex);
        Some(texture)
    }
}
