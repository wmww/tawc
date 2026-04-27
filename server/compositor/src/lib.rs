use std::ffi::c_void;
use std::os::unix::fs::PermissionsExt;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

use jni::JNIEnv;
use jni::objects::{GlobalRef, JClass, JObject, JString, JValue};
use jni::sys::{jint, jobject};
use jni::JavaVM;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::utils::Transform;
use wayland_server::{Display, ListeningSocket};

mod egl_android;
mod gl_import;
mod host;
mod protocol;
mod wlegl;
mod compositor;
mod render;
mod background;
mod event_loop;
mod input;
mod text_input;

use gl_import::AhbTextureImporter;
use compositor::TawcState;
use host::{ActivityId, SurfaceEvent};
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

/// Tracks whether the compositor thread is currently running. Set true
/// in `nativeStartCompositor`; cleared by the thread on exit. Used to
/// make `nativeStartCompositor` idempotent (Service can call it on every
/// `onCreate` after a process restart).
static COMPOSITOR_RUNNING: AtomicBool = AtomicBool::new(false);

/// Cache the JavaVM and NativeBridge class on the first JNI call so the
/// compositor thread can do reverse-JNI from any thread later.
fn cache_jni_globals(env: &mut JNIEnv) {
    if JAVA_VM.get().is_none() {
        match env.get_java_vm() {
            Ok(vm) => { let _ = JAVA_VM.set(vm); }
            Err(e) => log::error!("Failed to get JavaVM: {}", e),
        }
    }
    if NATIVE_BRIDGE_CLASS.get().is_none() {
        if let Ok(class) = env.find_class("me/phie/tawc/compositor/NativeBridge") {
            let obj = JObject::from(class);
            if let Ok(global) = env.new_global_ref(&obj) {
                let _ = NATIVE_BRIDGE_CLASS.set(global);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// JNI: compositor lifecycle (CompositorService)
// ---------------------------------------------------------------------------

/// Start the compositor thread. Called from `CompositorService.onCreate`.
/// Idempotent: if a compositor thread is already running, this is a no-op.
///
/// The compositor sets up its EGL context, GlesRenderer, Wayland display,
/// and listening socket up front, then enters its event loop with no
/// `OutputHost`s. `nativeRegisterActivitySurface` adds hosts later.
#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeStartCompositor(
    mut env: JNIEnv,
    _class: JClass,
    initial_width: jint,
    initial_height: jint,
) {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag("tawc-native"),
    );
    cache_jni_globals(&mut env);

    if COMPOSITOR_RUNNING.swap(true, Ordering::SeqCst) {
        info!("nativeStartCompositor: already running");
        return;
    }

    info!("nativeStartCompositor: spawning compositor thread");
    RUNNING.store(true, Ordering::SeqCst);

    // Create the channels here, BEFORE the compositor thread starts,
    // so JNI calls (especially `nativeRegisterActivitySurface`) can
    // immediately enqueue events. The calloop channel is durable —
    // events queue until the compositor thread plugs the receiver
    // into its loop. Without this, the very first `surfaceCreated`
    // (which fires within milliseconds of `bindService`) would race
    // the compositor thread's `create_*_channel` calls and silently
    // drop.
    let touch_channel = input::create_touch_channel();
    let text_input_channel = text_input::create_text_input_channel();
    let surface_event_channel = host::create_surface_event_channel();
    let (state_query_sender, state_query_channel) =
        smithay::reexports::calloop::channel::channel();
    *STATE_QUERY_SENDER.lock().unwrap() = Some(state_query_sender);

    let initial_size = (initial_width.max(0), initial_height.max(0));
    std::thread::spawn(move || {
        if let Err(e) = run_compositor(
            touch_channel,
            text_input_channel,
            surface_event_channel,
            state_query_channel,
            initial_size,
        ) {
            log::error!("Compositor failed: {}", e);
        }
        *STATE_QUERY_SENDER.lock().unwrap() = None;
        host::clear_surface_event_sender();
        COMPOSITOR_RUNNING.store(false, Ordering::SeqCst);
        info!("Compositor thread exited");
    });
}

/// Stop the compositor thread. Called from `CompositorService.onDestroy`.
#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeStopCompositor(
    _env: JNIEnv,
    _class: JClass,
) {
    info!("nativeStopCompositor");
    RUNNING.store(false, Ordering::SeqCst);
}

// ---------------------------------------------------------------------------
// JNI: per-Activity surface lifecycle (CompositorActivity)
// ---------------------------------------------------------------------------

fn jstring_to_id(env: &mut JNIEnv, s: JString) -> ActivityId {
    env.get_string(&s).map(|s| s.into()).unwrap_or_else(|_| "primary".to_string())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeRegisterActivitySurface(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
    surface: jobject,
    width: i32,
    height: i32,
) {
    cache_jni_globals(&mut env);
    let activity_id = jstring_to_id(&mut env, activity_id);

    let window_ptr = unsafe {
        let ptr = ndk_sys::ANativeWindow_fromSurface(env.get_raw(), surface);
        if ptr.is_null() {
            log::error!("Failed to get ANativeWindow from Surface for {}", activity_id);
            return;
        }
        ptr as *mut c_void
    };

    let w = if width > 0 { width } else { unsafe { ndk_sys::ANativeWindow_getWidth(window_ptr as *mut _) } };
    let h = if height > 0 { height } else { unsafe { ndk_sys::ANativeWindow_getHeight(window_ptr as *mut _) } };
    info!("nativeRegisterActivitySurface({}): {}x{}", activity_id, w, h);

    host::send_surface_event(SurfaceEvent::Register {
        activity_id,
        native_window: window_ptr as usize,
        width: w,
        height: h,
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnActivitySurfaceChanged(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
    width: i32,
    height: i32,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    info!("nativeOnActivitySurfaceChanged({}): {}x{}", activity_id, width, height);
    host::send_surface_event(SurfaceEvent::SurfaceChanged { activity_id, width, height });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnActivitySurfaceDestroyed(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    info!("nativeOnActivitySurfaceDestroyed({})", activity_id);
    host::send_surface_event(SurfaceEvent::SurfaceDestroyed { activity_id });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnActivityDestroyed(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    info!("nativeOnActivityDestroyed({})", activity_id);
    host::send_surface_event(SurfaceEvent::ActivityDestroyed { activity_id });
}

// ---------------------------------------------------------------------------
// JNI: input (touch). Per-Activity tagging arrives in phase 6.
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnTouchEvent(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
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

    let activity_id = jstring_to_id(&mut env, activity_id);
    let time = event_time as u32;
    let event = match action {
        ACTION_DOWN | ACTION_POINTER_DOWN => input::TouchEvent::Down { id: pointer_id, x, y, time, activity_id },
        ACTION_MOVE => input::TouchEvent::Motion { id: pointer_id, x, y, time, activity_id },
        ACTION_UP | ACTION_POINTER_UP => input::TouchEvent::Up { id: pointer_id, time, activity_id },
        _ => return,
    };
    input::send_touch_event(event);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnActivityFocusChanged(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
    has_focus: bool,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    info!("nativeOnActivityFocusChanged({}, {})", activity_id, has_focus);
    host::send_surface_event(SurfaceEvent::FocusChanged { activity_id, has_focus });
}

// ---------------------------------------------------------------------------
// JNI: Text input events from Android InputConnection
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeCommitText(
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
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetComposingText(
    mut env: JNIEnv,
    _class: JClass,
    text: jni::objects::JString,
) {
    let text: String = env.get_string(&text).map(|s| s.into()).unwrap_or_default();
    text_input::send_text_input_event(text_input::TextInputEvent::SetPreeditString { text });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeFinishComposingText(
    _env: JNIEnv,
    _class: JClass,
) {
    text_input::send_text_input_event(text_input::TextInputEvent::FinishComposingText);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeDeleteSurroundingText(
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
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSendKeyEvent(
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
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeQueryState(
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

/// Reverse-JNI: ask Kotlin to start a new `CompositorActivity` for the
/// given activity_id. The Activity will eventually call back via
/// `nativeRegisterActivitySurface` once its `SurfaceView` is laid out.
///
/// Phase 3 wires this up; phase 5 starts using it (single_activity_mode
/// is true through phase 4 so this path isn't taken yet).
pub fn spawn_activity_from_native(activity_id: &str) {
    let vm = match JAVA_VM.get() {
        Some(vm) => vm,
        None => { log::error!("JavaVM not cached for spawnActivity"); return; }
    };
    let class_ref = match NATIVE_BRIDGE_CLASS.get() {
        Some(r) => r,
        None => { log::error!("NativeBridge class not cached for spawnActivity"); return; }
    };
    let mut env = match vm.attach_current_thread() {
        Ok(env) => env,
        Err(e) => { log::error!("attach_current_thread failed: {}", e); return; }
    };
    let id_jstr = match env.new_string(activity_id) {
        Ok(s) => s,
        Err(e) => { log::error!("new_string({}) failed: {}", activity_id, e); return; }
    };
    let class = unsafe { JClass::from_raw(class_ref.as_obj().as_raw()) };
    if let Err(e) = env.call_static_method(
        class,
        "spawnActivity",
        "(Ljava/lang/String;)V",
        &[(&id_jstr).into()],
    ) {
        log::error!("Reverse JNI spawnActivity({}) failed: {}", activity_id, e);
    }
}

/// Reverse-JNI: ask Kotlin to finish (and remove from recents) the
/// `CompositorActivity` for the given activity_id.
pub fn finish_activity_from_native(activity_id: &str) {
    let vm = match JAVA_VM.get() {
        Some(vm) => vm,
        None => { log::error!("JavaVM not cached for finishActivity"); return; }
    };
    let class_ref = match NATIVE_BRIDGE_CLASS.get() {
        Some(r) => r,
        None => { log::error!("NativeBridge class not cached for finishActivity"); return; }
    };
    let mut env = match vm.attach_current_thread() {
        Ok(env) => env,
        Err(e) => { log::error!("attach_current_thread failed: {}", e); return; }
    };
    let id_jstr = match env.new_string(activity_id) {
        Ok(s) => s,
        Err(e) => { log::error!("new_string({}) failed: {}", activity_id, e); return; }
    };
    let class = unsafe { JClass::from_raw(class_ref.as_obj().as_raw()) };
    if let Err(e) = env.call_static_method(
        class,
        "finishActivity",
        "(Ljava/lang/String;)V",
        &[(&id_jstr).into()],
    ) {
        log::error!("Reverse JNI finishActivity({}) failed: {}", activity_id, e);
    }
}

/// Default output scale used while no Activity has registered its size.
/// Matches the historical hardcoded value.
const DEFAULT_OUTPUT_SCALE: i32 = 2;

/// Set up EGL context, renderer, Wayland display, and socket. Then hand off
/// to the calloop event loop. The first `OutputHost` is added asynchronously
/// when an Activity calls `nativeRegisterActivitySurface`.
fn run_compositor(
    touch_channel: smithay::reexports::calloop::channel::Channel<input::TouchEvent>,
    text_input_channel: smithay::reexports::calloop::channel::Channel<text_input::TextInputEvent>,
    surface_event_channel: smithay::reexports::calloop::channel::Channel<SurfaceEvent>,
    state_query_channel: smithay::reexports::calloop::channel::Channel<()>,
    initial_physical_size: (i32, i32),
) -> Result<(), Box<dyn std::error::Error>> {
    // --- EGL context (no surface yet — first Activity provides one) ---
    let (raw_display, raw_config, raw_context) =
        unsafe { egl_android::create_raw_egl_context()? };
    let egl_context = unsafe { EGLContext::from_raw(raw_display, raw_config, raw_context)? };

    let egl_display = egl_context.display().clone();
    let egl_pixel_format = egl_context.pixel_format().ok_or("No pixel format")?;
    let egl_config_id = egl_context.config_id();

    let mut renderer = unsafe { GlesRenderer::new(egl_context)? };

    let importer = AhbTextureImporter::new()
        .map_err(|e| format!("Failed to load AHB importer: {}", e))?;
    info!("EGL + GlesRenderer + AHB importer ready");

    let shm_tint_shader = render::compile_shm_tint_shader(&mut renderer);
    let wlegl_opaque_shader = render::compile_wlegl_opaque_shader(&mut renderer);
    let background = background::BackgroundRenderer::new(&mut renderer, &importer);

    let render_state = RenderState {
        renderer,
        importer,
        shm_tint_shader,
        wlegl_opaque_shader,
        background,
        raw_egl_display: raw_display,
        raw_egl_context: raw_context,
        egl_display,
        egl_pixel_format,
        egl_config_id,
    };

    // --- Wayland display + protocol state ---
    // Seed `output_logical_size` from the display size that
    // CompositorService passed in. A client connecting before the first
    // CompositorActivity has registered (test harness drives this — vkcube
    // launches faster than Android can spawn the Activity) would otherwise
    // see configure(0,0) on its xdg_toplevel, allocate a default-sized
    // swapchain, then receive a real size mid-flight when the Activity
    // finally registers — Vulkan WSI doesn't recover from that and the
    // cube hangs after committing two buffers.
    let mut wl_display: Display<TawcState> = Display::new()?;
    let scale = DEFAULT_OUTPUT_SCALE;
    let (init_pw, init_ph) = initial_physical_size;
    let initial_logical = if init_pw > 0 && init_ph > 0 {
        (init_pw / scale, init_ph / scale)
    } else {
        (0, 0)
    };
    let state = TawcState::new(&mut wl_display, scale, initial_logical);

    // --- Output (geometry updated when first Activity surface arrives) ---
    let output = smithay::output::Output::new(
        "tawc-0".to_string(),
        smithay::output::PhysicalProperties {
            size: (68, 150).into(),
            subpixel: smithay::output::Subpixel::Unknown,
            make: "tawc".into(),
            model: "Android".into(),
        },
    );
    let initial_mode_size: smithay::utils::Size<i32, smithay::utils::Physical> =
        if init_pw > 0 && init_ph > 0 { (init_pw, init_ph).into() } else { (1, 1).into() };
    output.change_current_state(
        Some(smithay::output::Mode { size: initial_mode_size, refresh: 60_000 }),
        Some(Transform::Normal),
        Some(smithay::output::Scale::Integer(scale)),
        Some((0, 0).into()),
    );
    // GlobalId is not RAII — the global lives as long as the Display.
    let _output_global = output.create_global::<TawcState>(&state.display_handle);

    // --- Listening socket ---
    let _ = std::fs::remove_file(WAYLAND_SOCKET_PATH);
    if let Some(parent) = std::path::Path::new(WAYLAND_SOCKET_PATH).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let listener = ListeningSocket::bind_absolute(WAYLAND_SOCKET_PATH.into())?;
    let _ = std::fs::set_permissions(WAYLAND_SOCKET_PATH, std::fs::Permissions::from_mode(0o777));
    info!("Wayland socket: {}", WAYLAND_SOCKET_PATH);

    // --- Run ---
    event_loop::run(
        wl_display, state, render_state, listener, output, scale,
        touch_channel, text_input_channel, state_query_channel, surface_event_channel,
        &RUNNING,
    )
}
