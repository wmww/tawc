use std::ffi::c_void;
use std::os::unix::fs::PermissionsExt;
use std::sync::atomic::{AtomicBool, Ordering};

use jni::JNIEnv;
use jni::objects::JClass;
use jni::sys::jobject;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::utils::{Size, Transform};
use wayland_server::{Display, ListeningSocket};

mod egl_android;
mod ahb;
mod gl_import;
mod protocol;
mod compositor;
mod render;
mod event_loop;

use egl_android::AndroidNativeSurface;
use gl_import::AhbTextureImporter;
use compositor::TawcState;
use render::RenderState;

/// Global flag shared between JNI calls to signal shutdown.
static RUNNING: AtomicBool = AtomicBool::new(false);

/// Wayland socket path accessible from chroot.
const WAYLAND_SOCKET_PATH: &str = "/data/data/me.phie.tawc/wayland-0";

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceCreated(
    env: JNIEnv,
    _class: JClass,
    surface: jobject,
) {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag("tawc-native"),
    );

    info!("nativeOnSurfaceCreated called");

    let window_ptr = unsafe {
        let ptr = ndk_sys::ANativeWindow_fromSurface(env.get_raw(), surface);
        if ptr.is_null() {
            log::error!("Failed to get ANativeWindow from Surface");
            return;
        }
        ptr as *mut c_void
    };

    let width = unsafe { ndk_sys::ANativeWindow_getWidth(window_ptr as *mut _) };
    let height = unsafe { ndk_sys::ANativeWindow_getHeight(window_ptr as *mut _) };
    info!("Native window: {}x{}", width, height);

    RUNNING.store(true, Ordering::SeqCst);

    // ANativeWindow_fromSurface acquires a ref. Acquire another for the render thread,
    // then release the one from fromSurface.
    unsafe { ndk_sys::ANativeWindow_acquire(window_ptr as *mut _) };
    unsafe { ndk_sys::ANativeWindow_release(window_ptr as *mut _) };

    let window_addr = window_ptr as usize;
    std::thread::spawn(move || {
        let window_ptr = window_addr as *mut c_void;
        info!("Compositor thread started");

        if let Err(e) = run_compositor(window_ptr, width, height) {
            log::error!("Compositor failed: {}", e);
        }

        unsafe { ndk_sys::ANativeWindow_release(window_ptr as *mut _) };
        info!("Compositor thread exited");
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceChanged(
    _env: JNIEnv,
    _class: JClass,
    _width: i32,
    _height: i32,
) {
    info!("nativeOnSurfaceChanged: {}x{}", _width, _height);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceDestroyed(
    _env: JNIEnv,
    _class: JClass,
) {
    info!("nativeOnSurfaceDestroyed");
    RUNNING.store(false, Ordering::SeqCst);
}

/// Set up EGL, Wayland display, and output, then hand off to the calloop event loop.
fn run_compositor(
    window_ptr: *mut c_void,
    width: i32,
    height: i32,
) -> Result<(), Box<dyn std::error::Error>> {
    // --- EGL + renderer setup ---
    let (raw_display, raw_config, raw_context) =
        unsafe { egl_android::create_raw_egl_context()? };
    let egl_context = unsafe { EGLContext::from_raw(raw_display, raw_config, raw_context)? };
    let mut renderer = unsafe { GlesRenderer::new(egl_context)? };

    let native_surface = AndroidNativeSurface::new(window_ptr);
    let display_ref = renderer.egl_context().display();
    let pixel_format = renderer.egl_context().pixel_format().ok_or("No pixel format")?;
    let config_id = renderer.egl_context().config_id();
    let egl_surface =
        unsafe { EGLSurface::new(display_ref, pixel_format, config_id, native_surface)? };

    let importer = AhbTextureImporter::new()
        .map_err(|e| format!("Failed to load AHB importer: {}", e))?;
    info!("EGL + GlesRenderer + AHB importer ready");

    let shm_tint_shader = render::compile_shm_tint_shader(&mut renderer);

    let render_state = RenderState {
        renderer,
        egl_surface,
        importer,
        shm_tint_shader,
        raw_egl_display: raw_display,
        raw_egl_context: raw_context,
    };

    // --- Wayland display + protocol state ---
    let mut wl_display: Display<TawcState> = Display::new()?;
    let state = TawcState::new(&mut wl_display);

    // --- Output ---
    let output = smithay::output::Output::new(
        "tawc-0".to_string(),
        smithay::output::PhysicalProperties {
            size: (68, 150).into(),
            subpixel: smithay::output::Subpixel::Unknown,
            make: "tawc".into(),
            model: "Android".into(),
        },
    );
    let mode = smithay::output::Mode {
        size: (width, height).into(),
        refresh: 60_000,
    };
    output.change_current_state(
        Some(mode),
        Some(Transform::Normal),
        Some(smithay::output::Scale::Integer(1)),
        Some((0, 0).into()),
    );
    output.set_preferred(mode);
    let _output_global = output.create_global::<TawcState>(&state.display_handle);
    info!("Wayland output: {}x{}", width, height);

    // --- Listening socket ---
    let _ = std::fs::remove_file(WAYLAND_SOCKET_PATH);
    if let Some(parent) = std::path::Path::new(WAYLAND_SOCKET_PATH).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let listener = ListeningSocket::bind_absolute(WAYLAND_SOCKET_PATH.into())?;
    let _ = std::fs::set_permissions(WAYLAND_SOCKET_PATH, std::fs::Permissions::from_mode(0o777));
    info!("Wayland socket: {}", WAYLAND_SOCKET_PATH);

    let output_size = Size::from((width, height));

    // --- Run ---
    event_loop::run(wl_display, state, render_state, listener, output_size, &RUNNING)
}
