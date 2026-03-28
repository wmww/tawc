use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};

use jni::JNIEnv;
use jni::objects::JClass;
use jni::sys::jobject;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::{Bind, Color32F, Frame, Renderer};
use smithay::utils::{Rectangle, Size, Transform};

mod egl_android;
use egl_android::AndroidNativeSurface;

/// Global state shared between JNI calls.
static RUNNING: AtomicBool = AtomicBool::new(false);

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceCreated(
    mut env: JNIEnv,
    _class: JClass,
    surface: jobject,
) {
    // Initialize Android logger
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag("tawc-native"),
    );

    info!("nativeOnSurfaceCreated called");

    // Get ANativeWindow from the Java Surface
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

    // ANativeWindow_fromSurface already acquires a ref. Acquire another for the render thread.
    unsafe { ndk_sys::ANativeWindow_acquire(window_ptr as *mut _) };
    // Release the ref from fromSurface (render thread owns it now)
    unsafe { ndk_sys::ANativeWindow_release(window_ptr as *mut _) };

    let window_addr = window_ptr as usize;
    std::thread::spawn(move || {
        let window_ptr = window_addr as *mut c_void;
        info!("Render thread started");
        if let Err(e) = render_loop(window_ptr, width, height) {
            log::error!("Render loop failed: {}", e);
        }
        unsafe { ndk_sys::ANativeWindow_release(window_ptr as *mut _) };
        info!("Render thread exited");
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
    // TODO: handle resize
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceDestroyed(
    _env: JNIEnv,
    _class: JClass,
) {
    info!("nativeOnSurfaceDestroyed");
    RUNNING.store(false, Ordering::SeqCst);
}

fn render_loop(window_ptr: *mut c_void, width: i32, height: i32) -> Result<(), Box<dyn std::error::Error>> {
    // Step 1: Create raw EGL display, config, and context
    let (raw_display, raw_config, raw_context) = unsafe { egl_android::create_raw_egl_context()? };
    info!("Raw EGL context created: display={:?}, config={:?}, context={:?}",
          raw_display, raw_config, raw_context);

    // Step 2: Wrap in Smithay types
    let egl_context = unsafe {
        EGLContext::from_raw(raw_display, raw_config, raw_context)?
    };
    info!("Smithay EGLContext created");

    // Step 3: Create GlesRenderer
    let mut renderer = unsafe { GlesRenderer::new(egl_context)? };
    info!("GlesRenderer created");

    // Step 4: Create EGLSurface from ANativeWindow
    let native_surface = AndroidNativeSurface::new(window_ptr);
    let display = renderer.egl_context().display();
    let pixel_format = renderer.egl_context().pixel_format()
        .ok_or("No pixel format on EGL context")?;
    let config_id = renderer.egl_context().config_id();

    let mut egl_surface = unsafe {
        EGLSurface::new(display, pixel_format, config_id, native_surface)?
    };
    info!("EGLSurface created");

    // Step 5: Render loop
    let output_size = Size::from((width, height));
    let mut frame_count: u64 = 0;

    while RUNNING.load(Ordering::SeqCst) {
        // Animate color: cycle hue over time
        let t = (frame_count as f32) / 120.0;
        let r = (t.sin() * 0.5 + 0.5).clamp(0.0, 1.0);
        let g = ((t + 2.094).sin() * 0.5 + 0.5).clamp(0.0, 1.0);
        let b = ((t + 4.189).sin() * 0.5 + 0.5).clamp(0.0, 1.0);
        let color = Color32F::new(r, g, b, 1.0);

        // Bind surface and render
        let mut target = renderer.bind(&mut egl_surface)?;
        let mut frame = renderer.render(&mut target, output_size, Transform::Normal)?;
        frame.clear(color, &[Rectangle::from_size(output_size)])?;
        frame.finish()?;
        drop(target);

        egl_surface.swap_buffers(None)?;

        frame_count += 1;

        // ~60fps
        std::thread::sleep(std::time::Duration::from_millis(16));
    }

    info!("Render loop finished after {} frames", frame_count);
    Ok(())
}
