//! Android-specific EGL setup and native surface implementation.

use std::ffi::c_void;
use std::sync::Arc;

use smithay::backend::egl::display::EGLDisplayHandle;
use smithay::backend::egl::ffi as egl_ffi;
use smithay::backend::egl::native::EGLNativeSurface;
use smithay::backend::egl::EGLError;

use log::info;

/// Create a raw EGL display, config, and context suitable for GLES 2.0 rendering on Android.
///
/// Returns (EGLDisplay, EGLConfig, EGLContext) as raw pointers.
pub unsafe fn create_raw_egl_context() -> Result<(*const c_void, *const c_void, *const c_void), Box<dyn std::error::Error>> {
    // Load EGL function pointers (Smithay's internal mechanism)
    egl_ffi::make_sure_egl_is_loaded()
        .map_err(|e| format!("Failed to load EGL: {:?}", e))?;

    // Get default display
    let display = egl_ffi::egl::GetDisplay(egl_ffi::egl::DEFAULT_DISPLAY);
    if display == egl_ffi::egl::NO_DISPLAY {
        return Err("eglGetDisplay failed".into());
    }

    // Initialize
    let mut major = 0i32;
    let mut minor = 0i32;
    if egl_ffi::egl::Initialize(display, &mut major, &mut minor) == egl_ffi::egl::FALSE {
        return Err("eglInitialize failed".into());
    }
    info!("EGL initialized: {}.{}", major, minor);

    // Bind OpenGL ES API
    if egl_ffi::egl::BindAPI(egl_ffi::egl::OPENGL_ES_API) == egl_ffi::egl::FALSE {
        return Err("eglBindAPI(OPENGL_ES_API) failed".into());
    }

    // Choose config
    let config_attribs: &[i32] = &[
        egl_ffi::egl::SURFACE_TYPE as i32, egl_ffi::egl::WINDOW_BIT as i32,
        egl_ffi::egl::RENDERABLE_TYPE as i32, egl_ffi::egl::OPENGL_ES2_BIT as i32,
        egl_ffi::egl::RED_SIZE as i32, 8,
        egl_ffi::egl::GREEN_SIZE as i32, 8,
        egl_ffi::egl::BLUE_SIZE as i32, 8,
        egl_ffi::egl::ALPHA_SIZE as i32, 8,
        egl_ffi::egl::NONE as i32,
    ];
    let mut config: egl_ffi::egl::types::EGLConfig = std::ptr::null();
    let mut num_configs = 0i32;
    if egl_ffi::egl::ChooseConfig(display, config_attribs.as_ptr(), &mut config, 1, &mut num_configs) == egl_ffi::egl::FALSE || num_configs == 0 {
        return Err("eglChooseConfig failed".into());
    }
    info!("EGL config chosen: {:?} ({} configs)", config, num_configs);

    // Create context
    let context_attribs: &[i32] = &[
        egl_ffi::egl::CONTEXT_CLIENT_VERSION as i32, 2,
        egl_ffi::egl::NONE as i32,
    ];
    let context = egl_ffi::egl::CreateContext(
        display,
        config,
        egl_ffi::egl::NO_CONTEXT,
        context_attribs.as_ptr(),
    );
    if context == egl_ffi::egl::NO_CONTEXT {
        return Err("eglCreateContext failed".into());
    }
    info!("EGL context created: {:?}", context);

    // Make current with no surface (surfaceless) to allow GlesRenderer init
    if egl_ffi::egl::MakeCurrent(display, egl_ffi::egl::NO_SURFACE, egl_ffi::egl::NO_SURFACE, context) == egl_ffi::egl::FALSE {
        // Some drivers don't support surfaceless. That's OK -- we'll make current with a real surface later.
        info!("Surfaceless MakeCurrent failed (may be OK on some drivers)");
    }

    Ok((display, config, context))
}

/// Wraps a raw ANativeWindow pointer as an EGLNativeSurface for Smithay.
pub struct AndroidNativeSurface {
    window: *mut c_void,
}

unsafe impl Send for AndroidNativeSurface {}

impl AndroidNativeSurface {
    pub fn new(window: *mut c_void) -> Self {
        Self { window }
    }
}

unsafe impl EGLNativeSurface for AndroidNativeSurface {
    unsafe fn create(
        &self,
        display: &Arc<EGLDisplayHandle>,
        config_id: egl_ffi::egl::types::EGLConfig,
    ) -> Result<*const c_void, EGLError> {
        let attribs: &[i32] = &[egl_ffi::egl::NONE as i32];
        let surface = egl_ffi::egl::CreateWindowSurface(
            display.handle,
            config_id,
            self.window as egl_ffi::egl::types::EGLNativeWindowType,
            attribs.as_ptr(),
        );
        if surface == egl_ffi::egl::NO_SURFACE {
            Err(EGLError::BadNativeWindow)
        } else {
            Ok(surface)
        }
    }

    fn identifier(&self) -> Option<String> {
        Some("Android/ANativeWindow".to_string())
    }
}
