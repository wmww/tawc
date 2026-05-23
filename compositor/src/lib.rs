use std::ffi::c_void;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{OnceLock, mpsc};
use std::time::Duration;

use jni::JNIEnv;
use jni::objects::{GlobalRef, JClass, JObject, JString, JValue};
use jni::sys::{jboolean, jint, jobject};
use jni::JavaVM;
use log::info;

use smithay::backend::egl::EGLContext;
use smithay::backend::renderer::gles::GlesRenderer;
use wayland_server::Display;

#[cfg(feature = "gfxstream")]
mod ahb_export;
mod app_paths;
#[cfg(feature = "gfxstream")]
mod bridge;
mod egl_android;
#[cfg(feature = "gfxstream")]
mod gfxstream_present;
mod gtk3_menus_workaround;
mod gl_import;
mod host;
mod protocol;
mod wlegl;
mod clipboard;
mod compositor;
mod desktop;
mod render;
mod scale;
mod event_loop;
mod input;
mod keymap;
mod launcher;
mod text_input;
mod xwayland;

use gl_import::AhbTextureImporter;
use compositor::TawcState;
use host::{ActivityId, SurfaceEvent};
use render::RenderState;
use scale::OutputScale;

/// Global flag shared between JNI calls to signal shutdown.
static RUNNING: AtomicBool = AtomicBool::new(false);

/// Cached JavaVM for reverse JNI calls from the compositor thread.
static JAVA_VM: OnceLock<JavaVM> = OnceLock::new();

/// Cached global ref to NativeBridge class for reverse JNI callbacks.
static NATIVE_BRIDGE_CLASS: OnceLock<GlobalRef> = OnceLock::new();

type StateQueryResponse = mpsc::Sender<String>;

/// Global sender for state query requests. Replaced each time the compositor restarts.
static STATE_QUERY_SENDER: Mutex<Option<smithay::reexports::calloop::channel::Sender<StateQueryResponse>>> = Mutex::new(None);

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
    output_scale: f32,
    xwayland: jboolean,
    gtk3_broken_menus_workaround: jboolean,
) {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Debug)
            .with_tag("tawc-native")
            // smithay's gles backend traces every frame at info level
            // via tracing → log bridge ("renderer_gles2_frame; …"),
            // which floods logcat at the framerate. Drop to warn so we
            // still see real renderer errors.
            .with_filter(
                android_logger::FilterBuilder::new()
                    .parse("debug,smithay::backend::renderer::gles=warn")
                    .build(),
            ),
    );
    // The default Rust panic handler writes to stderr, which Bionic
    // routes to /dev/null for app processes — so a panic in the
    // compositor thread vanishes silently and is misdiagnosed as a
    // hang. Route panics through `error!` (i.e. android_logger /
    // logcat) and then abort: the compositor is the only thing this
    // process is for, so a panic here is fatal and we'd rather show
    // up as a clean SIGABRT with a useful message than leave the JVM
    // running on a dead native worker.
    std::panic::set_hook(Box::new(|info| {
        let location = info.location()
            .map(|l| format!("{}:{}", l.file(), l.line()))
            .unwrap_or_else(|| "<unknown>".into());
        let msg = info.payload()
            .downcast_ref::<&'static str>().copied()
            .or_else(|| info.payload().downcast_ref::<String>().map(|s| s.as_str()))
            .unwrap_or("<non-string panic payload>");
        log::error!("compositor panic at {}: {}", location, msg);
        std::process::abort();
    }));
    cache_jni_globals(&mut env);
    app_paths::init_from_env();

    if COMPOSITOR_RUNNING.swap(true, Ordering::SeqCst) {
        info!("nativeStartCompositor: already running");
        return;
    }

    info!("nativeStartCompositor: spawning compositor thread");
    RUNNING.store(true, Ordering::SeqCst);

    // gfxstream-bridge kumquat listener runs as a sibling thread of
    // the calloop event loop. The patched rutabaga fork initializes
    // gfxstream itself only after the first client connects. See
    // notes/gfxstream-bridge.md.
    //
    // The AHB export hook must be installed BEFORE the kumquat
    // thread can serve a RESOURCE_CREATE_BLOB — the hook is what
    // dispatches an AHB into a dmabuf fd the chroot can mmap (for
    // host-visible memory) or ignore (for swapchain images, where
    // the protocol still wants an fd but the chroot doesn't read it).
    // Sequencing as (install hook, then spawn thread) makes the race
    // impossible.
    #[cfg(feature = "gfxstream")]
    {
        info!("nativeStartCompositor: spawning kumquat thread");
        ahb_export::install_hook();
        bridge::spawn();
    }
    #[cfg(not(feature = "gfxstream"))]
    {
        info!("nativeStartCompositor: gfxstream bridge disabled at build time");
    }

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
    let clipboard_channel = clipboard::create_clipboard_channel();
    let surface_event_channel = host::create_surface_event_channel();
    let (state_query_sender, state_query_channel) =
        smithay::reexports::calloop::channel::channel();
    *STATE_QUERY_SENDER.lock().unwrap() = Some(state_query_sender);

    let initial_scale = sanitize_output_scale(output_scale as f64).unwrap_or(DEFAULT_OUTPUT_SCALE);
    let initial_xwayland = xwayland != 0;
    let initial_gtk3_broken_menus_workaround = gtk3_broken_menus_workaround != 0;
    std::thread::spawn(move || {
        if let Err(e) = run_compositor(
            touch_channel,
            text_input_channel,
            clipboard_channel,
            surface_event_channel,
            state_query_channel,
            initial_scale,
            initial_xwayland,
            initial_gtk3_broken_menus_workaround,
        ) {
            log::error!("Compositor failed: {}", e);
        }
        *STATE_QUERY_SENDER.lock().unwrap() = None;
        clipboard::clear_clipboard_sender();
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

// JNI ABI requires non-unsafe `extern "system" fn`. The Surface jobject is
// already validated by Android before it reaches this entry point.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
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

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnActivityFullscreenChanged(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
    fullscreen: bool,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    info!("nativeOnActivityFullscreenChanged({}, {})", activity_id, fullscreen);
    host::send_surface_event(SurfaceEvent::FullscreenChanged { activity_id, fullscreen });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnBackPressed(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
) {
    let activity_id = jstring_to_id(&mut env, activity_id);
    host::send_surface_event(SurfaceEvent::BackPressed { activity_id });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnHardwareKeyEvent(
    mut env: JNIEnv,
    _class: JClass,
    activity_id: JString,
    keycode: i32,
    pressed: bool,
    repeat_count: i32,
) -> jboolean {
    let activity_id = jstring_to_id(&mut env, activity_id);
    let Some(evdev_keycode) = keymap::android_to_evdev(keycode) else { return 0 };

    host::send_surface_event(SurfaceEvent::HardwareKey {
        activity_id,
        evdev_keycode,
        pressed,
        repeat_count: repeat_count.max(0) as u32,
    });
    1
}

// ---------------------------------------------------------------------------
// JNI: Text input events from Android InputConnection
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeCommitText(
    mut env: JNIEnv,
    _class: JClass,
    text: jni::objects::JString,
    delete_before: jint,
    delete_after: jint,
) {
    let text: String = env.get_string(&text).map(|s| s.into()).unwrap_or_default();
    // Gboard sends Enter as commitText("\n") — route as a real key event
    if text == "\n" {
        text_input::send_text_input_event(text_input::TextInputEvent::KeyPress { keycode: keymap::EVDEV_KEY_ENTER });
    } else {
        text_input::send_text_input_event(text_input::TextInputEvent::CommitString {
            text,
            delete_before: delete_before.max(0) as u32,
            delete_after: delete_after.max(0) as u32,
        });
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetComposingText(
    mut env: JNIEnv,
    _class: JClass,
    text: jni::objects::JString,
    delete_before: jint,
    delete_after: jint,
) {
    let text: String = env.get_string(&text).map(|s| s.into()).unwrap_or_default();
    text_input::send_text_input_event(text_input::TextInputEvent::SetPreeditString {
        text,
        delete_before: delete_before.max(0) as u32,
        delete_after: delete_after.max(0) as u32,
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeFinishComposingText(
    _env: JNIEnv,
    _class: JClass,
) {
    text_input::send_text_input_event(text_input::TextInputEvent::FinishComposingText);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSendKeyEvent(
    _env: JNIEnv,
    _class: JClass,
    keycode: i32,
) {
    if let Some(evdev) = keymap::android_to_evdev(keycode) {
        text_input::send_text_input_event(
            text_input::TextInputEvent::KeyPress { keycode: evdev },
        );
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSendKeyState(
    _env: JNIEnv,
    _class: JClass,
    keycode: i32,
    pressed: bool,
) {
    if let Some(evdev) = keymap::android_to_evdev(keycode) {
        text_input::send_text_input_event(
            text_input::TextInputEvent::KeyState {
                keycode: evdev,
                pressed,
            },
        );
    }
}

// ---------------------------------------------------------------------------
// JNI: State query from Android
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeQueryState(
    env: JNIEnv,
    _class: JClass,
) -> jobject {
    let Some(sender) = STATE_QUERY_SENDER.lock().unwrap().as_ref().cloned() else {
        return std::ptr::null_mut();
    };
    let (tx, rx) = mpsc::channel();
    if sender.send(tx).is_err() {
        return std::ptr::null_mut();
    }
    let payload = match rx.recv_timeout(Duration::from_secs(1)) {
        Ok(payload) => payload,
        Err(_) => return std::ptr::null_mut(),
    };
    match env.new_string(payload) {
        Ok(s) => s.into_raw(),
        Err(e) => {
            log::error!("nativeQueryState: new_string failed: {}", e);
            std::ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetTintBuffersByType(
    _env: JNIEnv,
    _class: JClass,
    enabled: jboolean,
) {
    render::TINT_BUFFERS_BY_TYPE.store(enabled != 0, Ordering::Relaxed);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetOutputScale(
    _env: JNIEnv,
    _class: JClass,
    scale: f32,
) {
    match sanitize_output_scale(scale as f64) {
        Some(scale) => {
            host::send_surface_event(SurfaceEvent::OutputScaleChanged { scale });
        }
        None => log::error!("Ignoring invalid output scale: {}", scale),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetXwaylandEnabled(
    _env: JNIEnv,
    _class: JClass,
    enabled: jboolean,
) {
    host::send_surface_event(SurfaceEvent::XwaylandChanged {
        enabled: enabled != 0,
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeSetGtk3BrokenMenusWorkaround(
    _env: JNIEnv,
    _class: JClass,
    enabled: jboolean,
) {
    host::send_surface_event(SurfaceEvent::Gtk3BrokenMenusWorkaroundChanged {
        enabled: enabled != 0,
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeCloseAllClientsForTest(
    _env: JNIEnv,
    _class: JClass,
) -> jint {
    let (tx, rx) = mpsc::channel();
    if !host::send_surface_event(SurfaceEvent::CloseAllClientsForTest { response: tx }) {
        return -1;
    }
    match rx.recv_timeout(Duration::from_secs(1)) {
        Ok(closed) => closed.try_into().unwrap_or(jint::MAX),
        Err(_) => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeOnAndroidClipboardText(
    mut env: JNIEnv,
    _class: JClass,
    text: JString,
) {
    let text: String = match env.get_string(&text) {
        Ok(s) => s.into(),
        Err(e) => {
            log::error!("nativeOnAndroidClipboardText: bad text string: {}", e);
            return;
        }
    };
    clipboard::send_android_text(text);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeClipboardDebugState(
    env: JNIEnv,
    _class: JClass,
) -> jobject {
    match env.new_string(clipboard::debug_state()) {
        Ok(s) => s.into_raw(),
        Err(e) => {
            log::error!("nativeClipboardDebugState: new_string failed: {}", e);
            std::ptr::null_mut()
        }
    }
}

// ---------------------------------------------------------------------------
// JNI: Launcher (LauncherActivity)
// ---------------------------------------------------------------------------

/// Scan a rootfs for installed `.desktop` apps and return the result as a
/// JSON-encoded string. The launcher activity parses this on the Kotlin
/// side; keeping the wire format string-shaped means we don't have to
/// declare or look up Java classes from Rust.
///
/// JSON shape: array of `{id, name, comment, exec, terminal}`. Empty
/// array on any error / missing rootfs (the caller treats that as "no
/// apps").
#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeLauncherScan(
    mut env: JNIEnv,
    _class: JClass,
    rootfs: JString,
) -> jobject {
    let rootfs: String = match env.get_string(&rootfs) {
        Ok(s) => s.into(),
        Err(e) => {
            log::error!("nativeLauncherScan: bad rootfs string: {}", e);
            return std::ptr::null_mut();
        }
    };
    let json = launcher::scan_json(std::path::Path::new(&rootfs));
    match env.new_string(json) {
        Ok(s) => s.into_raw(),
        Err(e) => {
            log::error!("nativeLauncherScan: new_string failed: {}", e);
            std::ptr::null_mut()
        }
    }
}

// ---------------------------------------------------------------------------
// Reverse JNI: Compositor → Android
// ---------------------------------------------------------------------------

/// Call a static void method on NativeBridge from any thread.
/// Attaches to the JVM if needed.
pub fn call_native_bridge_void(method: &str, sig: &str, args: &[JValue]) {
    with_native_bridge(method, |env, class| {
        env.call_static_method(class, method, sig, args)?;
        Ok(())
    });
}

fn with_native_bridge(
    context: &str,
    f: impl FnOnce(&mut JNIEnv, JClass) -> jni::errors::Result<()>,
) {
    let vm = match JAVA_VM.get() {
        Some(vm) => vm,
        None => { log::error!("JavaVM not cached for {}", context); return; }
    };
    let class_ref = match NATIVE_BRIDGE_CLASS.get() {
        Some(r) => r,
        None => { log::error!("NativeBridge class not cached for {}", context); return; }
    };
    let mut env = match vm.attach_current_thread() {
        Ok(env) => env,
        Err(e) => { log::error!("attach_current_thread failed for {}: {}", context, e); return; }
    };

    let local_class = match env.new_local_ref(class_ref.as_obj()) {
        Ok(class) => class,
        Err(e) => {
            log::error!("new_local_ref(NativeBridge) failed for {}: {}", context, e);
            return;
        }
    };
    let class = unsafe { JClass::from_raw(local_class.as_raw()) };
    if let Err(e) = f(&mut env, class) {
        log::error!("Reverse JNI {} failed: {}", context, e);
    }
}

/// Reverse-JNI: push the Wayland client's authoritative surrounding text +
/// selection up to Android, replacing the TawcInputConnection attached to
/// `activity_id`. Called from the calloop thread whenever the focused client
/// commits a `set_surrounding_text`. `sel_start`/`sel_end` are UTF-16
/// code-unit offsets within `text` (Android's native editor measure).
pub fn update_editable_text(activity_id: &str, text: &str, sel_start: i32, sel_end: i32) {
    with_native_bridge("onUpdateEditableText", |env, class| {
        let activity_jstr = env.new_string(activity_id)?;
        let text_jstr = env.new_string(text)?;
        env.call_static_method(
            class,
            "onUpdateEditableText",
            "(Ljava/lang/String;Ljava/lang/String;II)V",
            &[
                (&activity_jstr).into(),
                (&text_jstr).into(),
                JValue::Int(sel_start),
                JValue::Int(sel_end),
            ],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: show the Android soft keyboard for one compositor Activity.
pub fn show_keyboard_from_native(activity_id: &str) {
    with_native_bridge("onShowKeyboard", |env, class| {
        let activity_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "onShowKeyboard",
            "(Ljava/lang/String;)V",
            &[(&activity_jstr).into()],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: hide the Android soft keyboard for one compositor Activity.
pub fn hide_keyboard_from_native(activity_id: &str) {
    with_native_bridge("onHideKeyboard", |env, class| {
        let activity_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "onHideKeyboard",
            "(Ljava/lang/String;)V",
            &[(&activity_jstr).into()],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: update the EditorInfo cached for one compositor Activity and
/// restart that Activity's input connection if it is live.
pub fn update_ime_content_type_from_native(activity_id: &str, input_type: i32, ime_flags: i32) {
    with_native_bridge("onContentTypeChanged", |env, class| {
        let activity_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "onContentTypeChanged",
            "(Ljava/lang/String;II)V",
            &[
                (&activity_jstr).into(),
                JValue::Int(input_type),
                JValue::Int(ime_flags),
            ],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: push compositor/Wayland-owned text into Android's real
/// ClipboardManager. Kotlin suppresses the resulting clipboard listener
/// bounce so the Wayland owner is not immediately replaced by our own
/// Android mirror.
pub fn set_android_clipboard_text(text: &str) {
    with_native_bridge("onSetAndroidClipboardText", |env, class| {
        let text_jstr = env.new_string(text)?;
        env.call_static_method(
            class,
            "onSetAndroidClipboardText",
            "(Ljava/lang/String;)V",
            &[(&text_jstr).into()],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: ask Kotlin to start a new `CompositorActivity` for the
/// given activity_id. The Activity will eventually call back via
/// `nativeRegisterActivitySurface` once its `SurfaceView` is laid out.
///
/// Phase 3 wires this up; phase 5 starts using it (single_activity_mode
/// is true through phase 4 so this path isn't taken yet).
pub fn spawn_activity_from_native(activity_id: &str) {
    with_native_bridge("spawnActivity", |env, class| {
        let id_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "spawnActivity",
            "(Ljava/lang/String;)V",
            &[(&id_jstr).into()],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: ask Kotlin to finish (and remove from recents) the
/// `CompositorActivity` for the given activity_id.
pub fn finish_activity_from_native(activity_id: &str) {
    with_native_bridge("finishActivity", |env, class| {
        let id_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "finishActivity",
            "(Ljava/lang/String;)V",
            &[(&id_jstr).into()],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: set the Android fullscreen/immersive-bars mode for one
/// compositor Activity.
pub fn set_activity_fullscreen_from_native(activity_id: &str, fullscreen: bool) {
    with_native_bridge("setActivityFullscreen", |env, class| {
        let id_jstr = env.new_string(activity_id)?;
        env.call_static_method(
            class,
            "setActivityFullscreen",
            "(Ljava/lang/String;Z)V",
            &[(&id_jstr).into(), JValue::Bool(if fullscreen { 1 } else { 0 })],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: update Android recents/task metadata for one compositor
/// Activity. Kotlin decodes the PNG path off the UI thread and falls
/// back to TAWC's app icon if no rootfs icon was found.
pub fn update_window_metadata_from_native(
    activity_id: &str,
    metadata: &compositor::WindowMetadata,
) {
    with_native_bridge("updateWindowMetadata", |env, class| {
        let id_jstr = env.new_string(activity_id)?;
        let title_jstr = env.new_string(&metadata.title)?;
        let app_id_jstr = env.new_string(&metadata.app_id)?;
        let desktop_id_jstr = env.new_string(&metadata.desktop_id)?;
        let desktop_name_jstr = env.new_string(&metadata.desktop_name)?;
        let icon_path_jstr = env.new_string(&metadata.icon_path)?;
        env.call_static_method(
            class,
            "updateWindowMetadata",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
            &[
                (&id_jstr).into(),
                (&title_jstr).into(),
                (&app_id_jstr).into(),
                (&desktop_id_jstr).into(),
                (&desktop_name_jstr).into(),
                (&icon_path_jstr).into(),
            ],
        )?;
        Ok(())
    });
}

/// Reverse-JNI: publish the current compositor toplevel/window count for
/// the persistent Android notification.
pub fn update_toplevel_count_from_native(count: usize) {
    with_native_bridge("onToplevelCountChanged", |env, class| {
        env.call_static_method(
            class,
            "onToplevelCountChanged",
            "(I)V",
            &[JValue::Int(count as jint)],
        )?;
        Ok(())
    });
}

/// Default output scale used while no Activity has registered its size.
/// Fractional by default so the normal dev path exercises the fractional
/// scale protocol and rendering math.
const DEFAULT_OUTPUT_SCALE: f64 = 2.0;
const MIN_OUTPUT_SCALE: f64 = 0.5;
const MAX_OUTPUT_SCALE: f64 = 4.0;

fn sanitize_output_scale(scale: f64) -> Option<f64> {
    if !scale.is_finite() {
        return None;
    }
    Some(scale.clamp(MIN_OUTPUT_SCALE, MAX_OUTPUT_SCALE))
}

/// Set up EGL context, renderer, Wayland display, and socket. Then hand off
/// to the calloop event loop. The first `OutputHost` is added asynchronously
/// when an Activity calls `nativeRegisterActivitySurface`.
fn run_compositor(
    touch_channel: smithay::reexports::calloop::channel::Channel<input::TouchEvent>,
    text_input_channel: smithay::reexports::calloop::channel::Channel<text_input::TextInputEvent>,
    clipboard_channel: smithay::reexports::calloop::channel::Channel<clipboard::ClipboardEvent>,
    surface_event_channel: smithay::reexports::calloop::channel::Channel<SurfaceEvent>,
    state_query_channel: smithay::reexports::calloop::channel::Channel<StateQueryResponse>,
    initial_scale: f64,
    initial_xwayland: bool,
    initial_gtk3_broken_menus_workaround: bool,
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

    let plain_shader = render::compile_plain_shader(&mut renderer);
    let tint_shader = render::compile_tint_shader(&mut renderer);

    let render_state = RenderState {
        renderer,
        importer,
        plain_shader,
        tint_shader,
        egl_display,
        egl_pixel_format,
        egl_config_id,
    };

    // --- Wayland display + protocol state ---
    // Output geometry is unknown until the assigned Android Activity
    // registers its SurfaceView. Toplevel configures are deferred until
    // that real size arrives, avoiding both configure(0,0) and
    // service-side display-size guesses.
    let mut wl_display: Display<TawcState> = Display::new()?;
    let scale = OutputScale::new(initial_scale);
    // --- Output (global advertised when first Activity surface arrives) ---
    let output = smithay::output::Output::new(
        "tawc-0".to_string(),
        smithay::output::PhysicalProperties {
            size: (68, 150).into(),
            subpixel: smithay::output::Subpixel::Unknown,
            make: "tawc".into(),
            model: "Android".into(),
            serial_number: String::new(),
        },
    );

    let state = TawcState::new(
        &mut wl_display,
        scale,
        (0, 0),
        (0, 0),
        initial_xwayland,
        initial_gtk3_broken_menus_workaround,
        render_state,
        output,
    );

    // --- Run ---
    // Note: the listening socket is bound inside `event_loop::run` as the
    // last step before entering the dispatch loop. Binding here would
    // create a window where clients can `connect()` and write requests
    // (the kernel queues them on the listening socket) but the
    // compositor isn't yet running `accept()` — on a slow emulator the
    // ~80ms of GLES/source setup between bind and dispatch is enough to
    // make a freshly-spawned GTK4 client time out its initial roundtrip.
    let wayland_socket_path = app_paths::get().wayland_socket_path.clone();
    let _ = std::fs::remove_file(&wayland_socket_path);
    if let Some(parent) = std::path::Path::new(&wayland_socket_path).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    event_loop::run(
        wl_display, state, &wayland_socket_path,
        touch_channel, text_input_channel, clipboard_channel, state_query_channel, surface_event_channel,
        &RUNNING,
    )
}
