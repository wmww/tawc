//! GL/EGL extension loading and texture import.
//!
//! Primary purpose: import AHardwareBuffers as GlesTextures via
//! eglGetNativeClientBufferANDROID + eglCreateImageKHR + glEGLImageTargetTexture2DOES.

use std::ffi::{c_void, CString};
use log::{info, error};

use smithay::backend::egl::ffi as egl_ffi;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::gles::GlesTexture;
use smithay::utils::Size;

// EGL constants for AHB import (not in Smithay's generated bindings)
const EGL_NATIVE_BUFFER_ANDROID: u32 = 0x3140;
const EGL_IMAGE_PRESERVED_KHR: i32 = 0x30D2;

// GL constants
const GL_TEXTURE_EXTERNAL_OES: u32 = 0x8D65;
const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
const GL_LINEAR: i32 = 0x2601;

// Function pointer types
type FnEglGetNativeClientBufferANDROID = unsafe extern "C" fn(*const c_void) -> *const c_void;
type FnGlGenTextures = unsafe extern "C" fn(i32, *mut u32);
type FnGlBindTexture = unsafe extern "C" fn(u32, u32);
type FnGlTexParameteri = unsafe extern "C" fn(u32, u32, i32);
type FnGlEGLImageTargetTexture2DOES = unsafe extern "C" fn(u32, *const c_void);
type FnGlGetError = unsafe extern "C" fn() -> u32;

/// Loaded EGL/GL extension function pointers for texture import and creation.
pub struct AhbTextureImporter {
    egl_get_native_client_buffer: FnEglGetNativeClientBufferANDROID,
    gl_gen_textures: FnGlGenTextures,
    gl_bind_texture: FnGlBindTexture,
    gl_tex_parameteri: FnGlTexParameteri,
    gl_egl_image_target_texture_2d_oes: FnGlEGLImageTargetTexture2DOES,
    gl_get_error: FnGlGetError,
}

impl AhbTextureImporter {
    /// Load all required EGL/GL extension function pointers.
    /// Must be called after EGL is initialized.
    pub fn new() -> Result<Self, String> {
        unsafe {
            // Load GL core functions from libGLESv2.so (eglGetProcAddress may
            // return stubs for core functions on some Android drivers)
            let gles_lib = libloading::Library::new("libGLESv2.so")
                .map_err(|e| format!("Failed to load libGLESv2.so: {}", e))?;

            let load_gl = |lib: &libloading::Library, name: &[u8]| -> Result<*const c_void, String> {
                let sym: libloading::Symbol<*const c_void> = lib.get(name)
                    .map_err(|e| format!("{}: {}", String::from_utf8_lossy(&name[..name.len()-1]), e))?;
                let ptr = *sym;
                if ptr.is_null() {
                    Err(format!("{} is null", String::from_utf8_lossy(&name[..name.len()-1])))
                } else {
                    Ok(ptr)
                }
            };

            let load_egl = |name: &str| -> Result<*const c_void, String> {
                let cname = CString::new(name).unwrap();
                let ptr = egl_ffi::egl::GetProcAddress(cname.as_ptr()) as *const c_void;
                if ptr.is_null() {
                    Err(format!("{} not available", name))
                } else {
                    Ok(ptr)
                }
            };

            let importer = Self {
                egl_get_native_client_buffer: std::mem::transmute(
                    load_egl("eglGetNativeClientBufferANDROID")?
                ),
                gl_gen_textures: std::mem::transmute(load_gl(&gles_lib, b"glGenTextures\0")?),
                gl_bind_texture: std::mem::transmute(load_gl(&gles_lib, b"glBindTexture\0")?),
                gl_tex_parameteri: std::mem::transmute(load_gl(&gles_lib, b"glTexParameteri\0")?),
                gl_egl_image_target_texture_2d_oes: std::mem::transmute(
                    load_egl("glEGLImageTargetTexture2DOES")?
                ),
                gl_get_error: std::mem::transmute(load_gl(&gles_lib, b"glGetError\0")?),
            };

            // Leak the library handle to keep it loaded (it's already loaded by the process anyway)
            std::mem::forget(gles_lib);

            Ok(importer)
        }
    }

    /// Import an AHardwareBuffer as a GlesTexture.
    ///
    /// The EGL context must be current on the calling thread.
    /// `raw_display` is the raw EGLDisplay pointer.
    pub fn import_ahb(
        &self,
        renderer: &GlesRenderer,
        raw_display: *const c_void,
        ahb: *mut ndk_sys::AHardwareBuffer,
        width: i32,
        height: i32,
    ) -> Result<GlesTexture, String> {
        unsafe {
            let client_buffer = (self.egl_get_native_client_buffer)(ahb as *const c_void);
            if client_buffer.is_null() {
                return Err("eglGetNativeClientBufferANDROID returned null".into());
            }
            self.import_client_buffer(renderer, raw_display, client_buffer, width, height)
        }
    }

    /// Import an EGLClientBuffer (typically an `ANativeWindowBuffer*`) as a
    /// GlesTexture via `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ...)`.
    ///
    /// The EGL context must be current on the calling thread.
    pub unsafe fn import_client_buffer(
        &self,
        renderer: &GlesRenderer,
        raw_display: *const c_void,
        client_buffer: *const c_void,
        width: i32,
        height: i32,
    ) -> Result<GlesTexture, String> {
        // Clear any prior GL errors
        while (self.gl_get_error)() != 0 {}

        let attribs: [i32; 3] = [
            EGL_IMAGE_PRESERVED_KHR, egl_ffi::egl::TRUE as i32,
            egl_ffi::egl::NONE as i32,
        ];
        let egl_image = egl_ffi::egl::CreateImageKHR(
            raw_display,
            egl_ffi::egl::NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID,
            client_buffer,
            attribs.as_ptr(),
        );
        if egl_image.is_null() {
            let err = egl_ffi::egl::GetError();
            return Err(format!("eglCreateImageKHR failed, EGL error: 0x{:X}", err));
        }

        let mut tex: u32 = 0;
        (self.gl_gen_textures)(1, &mut tex);
        if tex == 0 {
            egl_ffi::egl::DestroyImageKHR(raw_display, egl_image);
            return Err("glGenTextures failed".into());
        }

        (self.gl_bind_texture)(GL_TEXTURE_EXTERNAL_OES, tex);
        (self.gl_egl_image_target_texture_2d_oes)(GL_TEXTURE_EXTERNAL_OES, egl_image);

        let gl_err = (self.gl_get_error)();
        if gl_err != 0 {
            egl_ffi::egl::DestroyImageKHR(raw_display, egl_image);
            error!("GL error after EGLImageTargetTexture2DOES: 0x{:X}", gl_err);
            return Err(format!("glEGLImageTargetTexture2DOES failed: GL error 0x{:X}", gl_err));
        }

        // The GL texture holds its own reference to the underlying buffer.
        // Destroy the intermediate EGLImage to avoid leaking one per import.
        egl_ffi::egl::DestroyImageKHR(raw_display, egl_image);

        (self.gl_tex_parameteri)(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        (self.gl_tex_parameteri)(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        (self.gl_bind_texture)(GL_TEXTURE_EXTERNAL_OES, 0);

        info!("GL texture {} created from EGLImage (EXTERNAL_OES) {}x{}", tex, width, height);

        let size = Size::from((width, height));
        let texture = GlesTexture::from_raw_with_flags(
            renderer, None, false, true, false, tex, size,
        );

        Ok(texture)
    }
}
