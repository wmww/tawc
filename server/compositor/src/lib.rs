use std::ffi::c_void;
use std::os::unix::fs::PermissionsExt;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

use jni::JNIEnv;
use jni::objects::{GlobalRef, JClass, JObject, JValue};
use jni::sys::jobject;
use jni::JavaVM;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::egl::EGLSurface;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::utils::{Size, Transform};
use wayland_server::{Display, ListeningSocket};

mod egl_android;
mod gl_import;
mod protocol;
mod wlegl;
mod compositor;
mod render;
mod background;
mod event_loop;
mod input;
mod text_input;

use egl_android::AndroidNativeSurface;
use gl_import::AhbTextureImporter;
use compositor::TawcState;
use render::RenderState;

/// Global flag shared between JNI calls to signal shutdown.
static RUNNING: AtomicBool = AtomicBool::new(false);

/// Cached JavaVM for reverse JNI calls from the compositor thread.
static JAVA_VM: OnceLock<JavaVM> = OnceLock::new();

/// Cached global ref to NativeBridge class for reverse JNI callbacks.
static NATIVE_BRIDGE_CLASS: OnceLock<GlobalRef> = OnceLock::new();

/// Wayland socket path accessible from chroot.
const WAYLAND_SOCKET_PATH: &str = "/data/data/me.phie.tawc/wayland-0";

/// Global sender for state query requests. Replaced each time the compositor restarts.
static STATE_QUERY_SENDER: Mutex<Option<smithay::reexports::calloop::channel::Sender<()>>> = Mutex::new(None);

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnSurfaceCreated(
    mut env: JNIEnv,
    _class: JClass,
    surface: jobject,
) {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag("tawc-native"),
    );

    info!("nativeOnSurfaceCreated called");

    // Cache JavaVM for reverse JNI calls from compositor thread
    if JAVA_VM.get().is_none() {
        match env.get_java_vm() {
            Ok(vm) => { let _ = JAVA_VM.set(vm); }
            Err(e) => log::error!("Failed to get JavaVM: {}", e),
        }
    }

    // Cache NativeBridge class ref for static method callbacks
    if NATIVE_BRIDGE_CLASS.get().is_none() {
        if let Ok(class) = env.find_class("me/phie/tawc/NativeBridge") {
            let obj = JObject::from(class);
            if let Ok(global) = env.new_global_ref(&obj) {
                let _ = NATIVE_BRIDGE_CLASS.set(global);
            }
        }
    }

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

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeOnTouchEvent(
    _env: JNIEnv,
    _class: JClass,
    action: i32,
    pointer_id: i32,
    x: f32,
    y: f32,
    event_time: i64,
) {
    // Android MotionEvent actions
    const ACTION_DOWN: i32 = 0;
    const ACTION_UP: i32 = 1;
    const ACTION_MOVE: i32 = 2;
    const ACTION_POINTER_DOWN: i32 = 5;
    const ACTION_POINTER_UP: i32 = 6;

    let time = event_time as u32;
    let event = match action {
        ACTION_DOWN | ACTION_POINTER_DOWN => input::TouchEvent::Down { id: pointer_id, x, y, time },
        ACTION_MOVE => input::TouchEvent::Motion { id: pointer_id, x, y, time },
        ACTION_UP | ACTION_POINTER_UP => input::TouchEvent::Up { id: pointer_id, time },
        _ => return,
    };
    input::send_touch_event(event);
}

// ---------------------------------------------------------------------------
// JNI: Text input events from Android InputConnection
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeCommitText(
    mut env: JNIEnv,
    _class: JClass,
    text: jni::objects::JString,
) {
    let text: String = env.get_string(&text).map(|s| s.into()).unwrap_or_default();
    // Gboard sends Enter as commitText("\n") — route as a real key event
    if text == "\n" {
        text_input::send_text_input_event(text_input::TextInputEvent::KeyPress { keycode: text_input::EVDEV_KEY_ENTER }); // KEY_ENTER
    } else {
        text_input::send_text_input_event(text_input::TextInputEvent::CommitString { text });
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeSetComposingText(
    mut env: JNIEnv,
    _class: JClass,
    text: jni::objects::JString,
) {
    let text: String = env.get_string(&text).map(|s| s.into()).unwrap_or_default();
    text_input::send_text_input_event(text_input::TextInputEvent::SetPreeditString { text });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeFinishComposingText(
    _env: JNIEnv,
    _class: JClass,
) {
    text_input::send_text_input_event(text_input::TextInputEvent::FinishComposingText);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeDeleteSurroundingText(
    _env: JNIEnv,
    _class: JClass,
    before: i32,
    after: i32,
) {
    text_input::send_text_input_event(text_input::TextInputEvent::DeleteSurroundingText {
        before: before as u32,
        after: after as u32,
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeSendKeyEvent(
    _env: JNIEnv,
    _class: JClass,
    keycode: i32,
) {
    // Map Android keycodes to text-input-v3 concepts at the JNI boundary.
    const KEYCODE_DEL: i32 = 67;        // backspace
    const KEYCODE_FORWARD_DEL: i32 = 112;
    const KEYCODE_ENTER: i32 = 66;
    const KEYCODE_TAB: i32 = 61;

    const EVDEV_KEY_BACKSPACE: u32 = 14;
    const EVDEV_KEY_DELETE: u32 = 111;

    let event = match keycode {
        KEYCODE_DEL => text_input::TextInputEvent::KeyPress { keycode: EVDEV_KEY_BACKSPACE },
        KEYCODE_FORWARD_DEL => text_input::TextInputEvent::KeyPress { keycode: EVDEV_KEY_DELETE },
        KEYCODE_ENTER => text_input::TextInputEvent::KeyPress { keycode: text_input::EVDEV_KEY_ENTER },
        KEYCODE_TAB => text_input::TextInputEvent::CommitString { text: "\t".to_string() },
        _ => {
            info!("Unhandled key event: keycode={}", keycode);
            return;
        }
    };
    text_input::send_text_input_event(event);
}

// ---------------------------------------------------------------------------
// JNI: State query from Android
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_NativeBridge_nativeQueryState(
    _env: JNIEnv,
    _class: JClass,
) {
    if let Some(sender) = STATE_QUERY_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(());
    }
}

// ---------------------------------------------------------------------------
// Reverse JNI: Compositor → Android
// ---------------------------------------------------------------------------

/// Call a static void method on NativeBridge from any thread.
/// Attaches to the JVM if needed.
pub fn call_native_bridge_void(method: &str, sig: &str, args: &[JValue]) {
    let vm = match JAVA_VM.get() {
        Some(vm) => vm,
        None => { log::error!("JavaVM not cached"); return; }
    };
    let class_ref = match NATIVE_BRIDGE_CLASS.get() {
        Some(r) => r,
        None => { log::error!("NativeBridge class not cached"); return; }
    };

    // AttachCurrentThread is idempotent — safe to call if already attached
    let mut env = match vm.attach_current_thread() {
        Ok(env) => env,
        Err(e) => { log::error!("Failed to attach JNI thread: {}", e); return; }
    };

    // GlobalRef::as_obj() returns &JObject<'static>; we need a JClass.
    // The underlying jobject is a jclass, so this reinterpret is safe.
    let class = unsafe { JClass::from_raw(class_ref.as_obj().as_raw()) };
    if let Err(e) = env.call_static_method(class, method, sig, args) {
        log::error!("Reverse JNI call {}({}) failed: {}", method, sig, e);
    }
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
    let wlegl_opaque_shader = render::compile_wlegl_opaque_shader(&mut renderer);
    let background = background::BackgroundRenderer::new(&mut renderer);

    let render_state = RenderState {
        renderer,
        egl_surface,
        importer,
        shm_tint_shader,
        wlegl_opaque_shader,
        background,
        raw_egl_display: raw_display,
        raw_egl_context: raw_context,
    };

    // --- Wayland display + protocol state ---
    let mut wl_display: Display<TawcState> = Display::new()?;
    let scale = 2;
    let logical_size = (width / scale, height / scale);
    let state = TawcState::new(&mut wl_display, scale, logical_size);

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
        Some(smithay::output::Scale::Integer(2)),
        Some((0, 0).into()),
    );
    output.set_preferred(mode);
    // GlobalId is not RAII — dropping it does NOT remove the Wayland global.
    // The global lives as long as the Display. We just don't need the ID.
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

    // --- Touch input channel ---
    let touch_channel = input::create_touch_channel();

    // --- Text input channel ---
    let text_input_channel = text_input::create_text_input_channel();

    // --- State query channel ---
    let (state_query_sender, state_query_channel) = smithay::reexports::calloop::channel::channel();
    *STATE_QUERY_SENDER.lock().unwrap() = Some(state_query_sender);

    // --- Run ---
    event_loop::run(wl_display, state, render_state, listener, output_size, scale, touch_channel, text_input_channel, state_query_channel, &RUNNING)
}
