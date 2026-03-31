use std::ffi::c_void;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::io::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::atomic::{AtomicBool, Ordering};

use jni::JNIEnv;
use jni::objects::JClass;
use jni::sys::jobject;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::egl::EGLSurface;
use smithay::backend::egl::ffi as egl_ffi;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::{Bind, Color32F, Frame, Renderer};
use smithay::utils::{Point, Rectangle, Size, Transform};

mod egl_android;
mod ahb;
mod gl_import;

use egl_android::AndroidNativeSurface;
use ahb::AhbBuffer;
use gl_import::AhbTextureImporter;

/// Global state shared between JNI calls.
static RUNNING: AtomicBool = AtomicBool::new(false);

/// AHB test buffer dimensions.
const AHB_TEST_WIDTH: u32 = 256;
const AHB_TEST_HEIGHT: u32 = 256;

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceCreated(
    env: JNIEnv,
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

    // External mode: compositor listens on a Unix socket for a cross-process
    // AHB client (e.g. glibc chroot). Triggered by a flag file in the app's data dir.
    let sock_path = "/data/data/me.phie.tawc/ahb-test.sock";
    let flag_path = "/data/data/me.phie.tawc/external-mode";
    let external_mode = std::path::Path::new(flag_path).exists();
    let sock_path_owned = sock_path.to_string();
    info!("external_mode: {}", external_mode);

    // Spawn render thread
    let window_addr = window_ptr as usize;
    std::thread::spawn(move || {
        let window_ptr = window_addr as *mut c_void;
        info!("Render thread started (external_mode={})", external_mode);

        if external_mode {
            // Client-allocates-AHB flow:
            // 1. Listen for client connection
            // 2. Client connects and sends its AHB (raw handle)
            // 3. Compositor receives and reconstructs the AHB
            // 4. Display the client-rendered AHB
            let sock_path = &sock_path_owned;
            let _ = std::fs::remove_file(sock_path);

            info!("Waiting for external client on {}", sock_path);
            let listener = match UnixListener::bind(sock_path) {
                Ok(l) => l,
                Err(e) => { log::error!("Failed to bind {}: {}", sock_path, e); return; }
            };
            let _ = std::fs::set_permissions(sock_path,
                std::fs::Permissions::from_mode(0o777));
            let (stream, _) = listener.accept().expect("Failed to accept");
            info!("External client connected");

            // Receive AHB from client via standard AHardwareBuffer socket protocol
            let ahb = match AhbBuffer::recv_from_socket(stream.as_raw_fd()) {
                Ok(a) => a,
                Err(e) => { log::error!("Failed to receive AHB: {}", e); return; }
            };
            info!("Received client-allocated AHB: {}x{}", ahb.width(), ahb.height());

            // Display the AHB
            if let Err(e) = render_loop_external(window_ptr, width, height, ahb) {
                log::error!("Render loop failed: {}", e);
            }
        } else {
            let (sock_sender, sock_receiver) = UnixStream::pair().unwrap();
            std::thread::spawn(move || {
                if let Err(e) = test_client_loop(sock_sender) {
                    log::error!("Test client failed: {}", e);
                }
            });

            if let Err(e) = render_loop(window_ptr, width, height, sock_receiver) {
                log::error!("Render loop failed: {}", e);
            }
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
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceDestroyed(
    _env: JNIEnv,
    _class: JClass,
) {
    info!("nativeOnSurfaceDestroyed");
    RUNNING.store(false, Ordering::SeqCst);
}

/// Test client thread: allocates AHB, fills with test pattern, sends over socket.
fn test_client_loop(sock: UnixStream) -> Result<(), String> {
    // Allocate AHB
    let ahb = AhbBuffer::allocate(AHB_TEST_WIDTH, AHB_TEST_HEIGHT)?;

    // Fill with checkerboard pattern
    ahb.fill_test_pattern()?;

    // Send AHB over socket
    ahb.send_to_socket(sock.as_raw_fd())?;

    info!("Test client: AHB sent, parking thread to keep buffer alive");

    // Keep the AHB alive - the compositor needs it. Park until app exits.
    while RUNNING.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_secs(1));
    }

    info!("Test client: exiting");
    // AHB is dropped here (released)
    Ok(())
}

/// Render loop for externally-rendered AHB (compositor already has the buffer).
fn render_loop_external(
    window_ptr: *mut c_void,
    width: i32,
    height: i32,
    ahb: AhbBuffer,
) -> Result<(), Box<dyn std::error::Error>> {
    let (raw_display, raw_config, raw_context) = unsafe { egl_android::create_raw_egl_context()? };
    let egl_context = unsafe { EGLContext::from_raw(raw_display, raw_config, raw_context)? };
    let mut renderer = unsafe { GlesRenderer::new(egl_context)? };

    let native_surface = AndroidNativeSurface::new(window_ptr);
    let display = renderer.egl_context().display();
    let pixel_format = renderer.egl_context().pixel_format()
        .ok_or("No pixel format")?;
    let config_id = renderer.egl_context().config_id();
    let mut egl_surface = unsafe {
        EGLSurface::new(display, pixel_format, config_id, native_surface)?
    };

    let importer = AhbTextureImporter::new()
        .map_err(|e| format!("Failed to load AHB importer: {}", e))?;

    // Import AHB as texture
    unsafe {
        egl_ffi::egl::MakeCurrent(raw_display, egl_ffi::egl::NO_SURFACE,
            egl_ffi::egl::NO_SURFACE, raw_context);
    }
    let ahb_texture = importer.import_ahb(
        &renderer, raw_display, ahb.as_raw(),
        ahb.width() as i32, ahb.height() as i32,
    ).map_err(|e| format!("Failed to import AHB: {}", e))?;
    info!("AHB imported as texture");

    let output_size = Size::from((width, height));
    let texture_size = Size::<i32, smithay::utils::Buffer>::from((
        ahb.width() as i32, ahb.height() as i32,
    ));
    let tex_x = (width - ahb.width() as i32) / 2;
    let tex_y = (height - ahb.height() as i32) / 2;
    let mut frame_count: u64 = 0;

    while RUNNING.load(Ordering::SeqCst) {
        let t = (frame_count as f32) / 240.0;
        let gray = (t.sin() * 0.15 + 0.2).clamp(0.0, 1.0);
        let bg_color = Color32F::new(gray, gray, gray, 1.0);

        let mut target = renderer.bind(&mut egl_surface)?;
        let mut frame = renderer.render(&mut target, output_size, Transform::Normal)?;
        frame.clear(bg_color, &[Rectangle::from_size(output_size)])?;

        let src_rect = Rectangle::from_size(texture_size.to_f64());
        let dst_rect = Rectangle::new(
            Point::from((tex_x, tex_y)), Size::from((ahb.width() as i32, ahb.height() as i32)),
        );
        let damage_rect = Rectangle::from_size(Size::from((ahb.width() as i32, ahb.height() as i32)));
        Frame::render_texture_from_to(
            &mut frame, &ahb_texture, src_rect, dst_rect,
            &[damage_rect], &[], Transform::Normal, 1.0,
        )?;

        let _ = frame.finish()?;
        drop(target);
        egl_surface.swap_buffers(None)?;

        frame_count += 1;
        if frame_count % 300 == 0 {
            info!("External render: {} frames", frame_count);
        }
        std::thread::sleep(std::time::Duration::from_millis(16));
    }
    Ok(())
}

fn render_loop(
    window_ptr: *mut c_void,
    width: i32,
    height: i32,
    sock: UnixStream,
) -> Result<(), Box<dyn std::error::Error>> {
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

    // Step 5: Load AHB texture importer
    let importer = AhbTextureImporter::new()
        .map_err(|e| format!("Failed to load AHB importer: {}", e))?;
    info!("AHB texture importer loaded");

    // Step 6: Receive AHB from test client (blocking)
    info!("Waiting for AHB from test client...");
    let received_ahb = AhbBuffer::recv_from_socket(sock.as_raw_fd())
        .map_err(|e| format!("Failed to receive AHB: {}", e))?;
    info!("AHB received: {}x{}", received_ahb.width(), received_ahb.height());

    // Step 7: Import AHB as GlesTexture (GL context must be current)
    // Smithay's bind doesn't persist MakeCurrent, so we do it manually
    unsafe {
        egl_ffi::egl::MakeCurrent(
            raw_display,
            egl_ffi::egl::NO_SURFACE,
            egl_ffi::egl::NO_SURFACE,
            raw_context,
        );
    }
    let ahb_texture = importer.import_ahb(
        &renderer,
        raw_display,
        received_ahb.as_raw(),
        received_ahb.width() as i32,
        received_ahb.height() as i32,
    ).map_err(|e| format!("Failed to import AHB as texture: {}", e))?;
    info!("AHB imported as GlesTexture");

    // Step 8: Render loop - display the AHB texture
    let output_size = Size::from((width, height));
    let texture_size = Size::<i32, smithay::utils::Buffer>::from((
        received_ahb.width() as i32,
        received_ahb.height() as i32,
    ));
    let mut frame_count: u64 = 0;

    // Center the texture on screen
    let tex_x = (width - received_ahb.width() as i32) / 2;
    let tex_y = (height - received_ahb.height() as i32) / 2;

    while RUNNING.load(Ordering::SeqCst) {
        // Animate background color
        let t = (frame_count as f32) / 240.0;
        let gray = (t.sin() * 0.15 + 0.2).clamp(0.0, 1.0);
        let bg_color = Color32F::new(gray, gray, gray, 1.0);

        let mut target = renderer.bind(&mut egl_surface)?;
        let mut frame = renderer.render(&mut target, output_size, Transform::Normal)?;

        // Clear background
        frame.clear(bg_color, &[Rectangle::from_size(output_size)])?;

        // Render the AHB texture centered on screen
        let tex_w = received_ahb.width() as i32;
        let tex_h = received_ahb.height() as i32;
        let src_rect = Rectangle::from_size(texture_size.to_f64());
        let dst_rect = Rectangle::new(
            Point::from((tex_x, tex_y)),
            Size::from((tex_w, tex_h)),
        );
        // Damage rects are relative to the dest rect origin
        let damage_rect = Rectangle::from_size(Size::from((tex_w, tex_h)));
        Frame::render_texture_from_to(
            &mut frame,
            &ahb_texture,
            src_rect,
            dst_rect,
            &[damage_rect],
            &[],
            Transform::Normal,
            1.0,
        )?;

        let _ = frame.finish()?;
        drop(target);

        egl_surface.swap_buffers(None)?;

        frame_count += 1;
        if frame_count % 300 == 0 {
            info!("Rendered {} frames with AHB texture", frame_count);
        }

        // ~60fps
        std::thread::sleep(std::time::Duration::from_millis(16));
    }

    info!("Render loop finished after {} frames", frame_count);
    Ok(())
}
