# Wayland Compositor for Android via Smithay

## Goal

A Rust/Kotlin Android app that runs a Wayland compositor inside an Android Activity.
Linux Wayland clients (running in Termux/proot/chroot) connect via a Unix domain socket
and get GPU-accelerated rendering onto Android Surfaces. All buffer handling stays on the
GPU — the compositor imports client dmabuf buffers as GL textures, composites with
GlesRenderer, and presents via eglSwapBuffers to Android's SurfaceFlinger.

**Non-goal: wl_shm-only support.** This compositor exists to provide hardware-accelerated
rendering for Wayland clients on Android. `wl_shm` (CPU shared memory buffers) may be
supported later as a fallback for simple clients, but is never the primary path and must
not be the first thing implemented. Every phase should target GPU buffers via
`zwp_linux_dmabuf_v1`.

---

## Prior Art: wlroots-android-bridge

[Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) achieves
this for wlroots/labwc on x86_64 Android (Intel GPUs + Mesa). Key design decisions we borrow:

- **One Android Activity per Wayland toplevel** — Android's own window manager handles
  task switching, recent apps, and window positioning.
- **ASurfaceTransaction for presentation** — submit rendered buffers directly to SurfaceFlinger.
- **Two-process design with Binder IPC** — compositor runs via `app_process` in Termux
  (because labwc/wlroots/Mesa are C libraries that need Termux's environment); the Android
  app provides Surfaces and forwards input over AIDL.
- **AHardwareBuffer as the allocator** — all buffers are GPU-resident Android buffers.

**Why it doesn't work on ARM:** It extracts DRM buffer attributes by casting AHardwareBuffer
handles to `cros_gralloc_handle` (minigbm header). This struct layout is specific to
Intel/ChromeOS gralloc — Qualcomm Adreno and ARM Mali use proprietary handle formats.
It also depends on Mesa for the GLES/Vulkan renderer and GBM, which don't exist for mobile GPUs.

**Why the compositor doesn't need to run in Termux:** The wlroots-android-bridge uses two
processes because labwc is a standalone C program with heavy native dependencies (Mesa,
wlroots, etc.) that must run inside Termux. It uses `app_process` from Termux to get a
hybrid process with both Android framework access and Termux's native library environment.
We're building a Rust library loaded via JNI — the compositor runs as a background thread
inside the Android app itself. This eliminates Binder IPC for surfaces and input, meaning
zero serialization overhead and direct access to ANativeWindow from the compositor thread.

**However**, we still need a lightweight `app_process` relay for Wayland socket sharing (see
below). This relay is only involved in connection bootstrapping — once a client is connected,
data flows directly between the client and compositor with no relay in the path.

**Critical constraint: Wayland socket sharing.** The wlroots-android-bridge avoids the
cross-app socket problem because its compositor runs *inside Termux* (via `app_process`),
so the Wayland socket lives in Termux's own filesystem. In our single-process design, the
compositor runs in the Android app (different UID/SELinux domain from Termux). On Android
9+, SELinux blocks Unix domain socket `connect()` between different `untrusted_app` domains
— both abstract and filesystem sockets. This means we **cannot** simply create a socket and
expect Termux clients to connect.

**Socket sharing solutions (in order of preference):**

1. **`app_process` relay (proven approach):** Ship a small Kotlin class (e.g.,
   `com.tawc.Relay`) in the APK. Termux users launch it via:
   ```
   /system/bin/app_process -Djava.class.path="$(pm path com.tawc | cut -d: -f2)" / com.tawc.Relay
   ```
   (or a wrapper shell script we provide). This works because `app_process` with the APK's
   classpath gives the relay process both Android framework access (Binder, can talk to the
   tawc app) AND Termux filesystem access (launched from Termux's shell).

   **Important: `app_process` does NOT provide an Android `Context`.** This means the relay
   **cannot** call `bindService()` or use other Context-dependent APIs. The wlroots-android-bridge
   and Termux:X11 solve this by reversing the Binder flow: the relay creates its own Binder
   objects and sends them *to* the app via a direct `IActivityManager.startActivity()` call
   (using reflection to bypass the need for a Context). Specifically:

   The relay:
   - Creates `$XDG_RUNTIME_DIR/wayland-0` listening socket in Termux's filesystem
   - Wraps the listening fd in a `ParcelFileDescriptor` inside a Binder-compatible object
   - Sends the Binder object to the tawc app by launching an Activity or sending a broadcast
     via direct `IActivityManager` Binder calls (TermuxAm pattern — reflection on hidden APIs)
   - The tawc app receives the fd from the relay's Binder object

   This is the **reverse** of a naive "relay calls bindService()" flow. The relay pushes to
   the app, not the other way around. Both wlroots-android-bridge and Termux:X11 use this
   pattern. Alternatively, the relay can set up a `Looper` and use
   `ActivityThread.systemMain()` (via reflection/`sun.misc.Unsafe`) to obtain a system
   Context, but this is fragile across Android versions.

   **Relay data path — listening socket handoff (preferred):**

   The relay creates the listening socket at `$XDG_RUNTIME_DIR/wayland-0`, then passes
   the *listening fd* to the compositor via the Binder mechanism above
   (`ParcelFileDescriptor`). The compositor calls `accept()` directly on the fd and uses
   `DisplayHandle::insert_client()` to add each new connection to the Wayland display.
   After the handoff, the relay can exit — it is only needed at startup.

   This works because SELinux checks apply to `connect()`/`bind()` syscalls, not to
   `read()`/`write()`/`accept()` on inherited file descriptors. Once the compositor holds
   the fd, the kernel treats it as the compositor's own.

   **Fallback — fd handoff per client:** If `accept()` on a handed-off listening socket
   hits SELinux issues on some devices, the relay can instead `accept()` each client
   connection itself, then pass the connected client fd to the compositor over Binder.
   The relay stays running but is not in the data path after handoff.

   **Last resort — byte proxy:** Relay sits in the middle, forwarding all Wayland protocol
   bytes. Simple but adds latency and CPU overhead. Only use if fd passing doesn't work.

   Termux:X11 uses `sharedUserId` instead, which only works for Termux plugins signed
   with the same key. wlroots-android-bridge avoids the problem entirely because its
   compositor runs in Termux. Our relay approach adds one lightweight process but no
   Binder IPC for rendering/input — only connection bootstrapping.

   **Android 12+ caveat: Phantom Process Killer.** Android 12 introduced
   `PhantomProcessKiller` which kills child processes of apps when more than 32 exist.
   An `app_process` launched from Termux counts as a phantom process. Mitigations:
   - Android 12-13: `adb shell device_config put activity_manager max_phantom_processes 2147483647`
   - Android 14+: Developer Options → "Disable child process restrictions"
   Since the relay can exit after fd handoff, it only needs to survive long enough for
   the handoff — but users should still be aware of this restriction for other Termux
   processes.

   **Note:** The Binder interaction (relay → app fd handoff) is the *only* use of Binder
   in the architecture — all rendering and input are Binder-free.

2. **`socketpair()` + fd passing:** The app creates `socketpair()` fds and passes one end
   to Termux via a ContentProvider or bound Service using `ParcelFileDescriptor`. Termux
   clients would need a wrapper that obtains the fd before starting the Wayland client.
   More complex, but fully in-process for established connections.

3. **Shared writable directory (fragile):** Place the socket in a world-accessible location
   (e.g., `/data/local/tmp/`). May work on some devices/Android versions but SELinux
   enforcement varies by OEM. Not reliable for production.

---

## Architecture

The compositor runs as a Rust native library inside the Android app, on a dedicated
background thread. Clients send GPU buffers (dmabuf fds) via `zwp_linux_dmabuf_v1`. The
compositor imports them as GL textures, composites with Smithay's GlesRenderer, and
presents to the Activity's ANativeWindow via `eglSwapBuffers`. One GPU composition pass,
no CPU buffer copies.

A lightweight `app_process` relay (started from Termux) handles Wayland socket creation
and hands off the listening fd to the compositor via a one-shot Binder call — after that,
the relay can exit.

```
┌────────────────────── Android App Process ──────────────────────┐
│                                                                  │
│  Kotlin / UI Thread                                              │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  MainActivity                                              │  │
│  │    └─ manages SurfaceViewActivity × N (one per window)     │  │
│  │         ├─ SurfaceView → ANativeWindow ──┐                 │  │
│  │         └─ onTouchEvent / onKeyEvent ────┐│                │  │
│  └──────────────────────────────────────────┼┼────────────────┘  │
│                                    JNI calls ││                   │
│  Compositor Thread (Rust)                    ││                   │
│  ┌───────────────────────────────────────────▼▼───────────────┐  │
│  │  smithay-android                                           │  │
│  │                                                            │  │
│  │  ┌─ GlesRenderer (stock Smithay, vendor GLES driver)       │  │
│  │  ├─ AndroidEglBackend (EGL on ANativeWindow)               │  │
│  │  ├─ AndroidInputBackend (events from Kotlin via channel)   │  │
│  │  ├─ dmabuf import (EGLImage or AHB→EGLImage → GL texture)  │  │
│  │  └─ Wayland state (xdg-shell, wl_seat, wl_output, etc)    │  │
│  └────────────────────────────────────────────────────────────┘  │
│         │                                                        │
│    Unix domain socket (listening fd received from relay)          │
│         │                                                        │
└─────────┼────────────────────────────────────────────────────────┘
          │
  app_process relay (separate process, Termux's UID)
    ├─ Creates socket at $XDG_RUNTIME_DIR/wayland-0
    ├─ Passes listening fd to app via Intent + Binder object
    └─ Exits after handoff
          │
  Termux / proot / chroot (separate process, same device)
    └─ Wayland clients (GTK, Qt, wlroots apps — GPU-accelerated via Mesa Turnip)
```

**Rendering pipeline:**
1. Client renders with GPU (Mesa Turnip on Qualcomm, via `/dev/kgsl-3d0` in chroot)
2. Client sends dmabuf fd to compositor via `zwp_linux_dmabuf_v1`
3. Compositor imports dmabuf as GL texture (via `EGL_EXT_image_dma_buf_import` if available
   on stock EGL, or via dmabuf→AHardwareBuffer wrapping + `EGL_ANDROID_image_native_buffer`)
4. GlesRenderer composites all surfaces (toplevel + popups + subsurfaces) onto the
   Activity's EGLSurface
5. `eglSwapBuffers()` → SurfaceFlinger presents

**dmabuf import — two paths (determined at runtime):**
- **Path A (preferred):** Stock Android EGL supports `EGL_EXT_image_dma_buf_import`. Smithay's
  `ImportDma` on `GlesRenderer` handles this directly — dmabuf fd → EGLImage → GL texture.
  No custom code needed beyond Smithay's existing implementation.
- **Path B (fallback):** Stock EGL does NOT support dmabuf import. Wrap the dmabuf fd as an
  `AHardwareBuffer` (via socketpair trick with `AHardwareBuffer_recvHandleFromUnixSocket`, or
  via Gralloc 4 IMapper JNI), then import via AHB → `eglGetNativeClientBufferANDROID` →
  EGLImage → GL texture. More complex but works on all Android EGL drivers.

Both paths are GPU-only. No CPU copies involved.

Communication between Kotlin UI thread and Rust compositor thread:
- **Kotlin → Rust:** JNI calls for surface lifecycle (`onSurfaceCreated(nativeWindow)`,
  `onSurfaceDestroyed()`) and input events (`onTouchEvent(...)`, `onKeyEvent(...)`).
  These post into a lock-free channel/ring buffer consumed by the compositor's event loop.
- **Rust → Kotlin:** JNI callbacks for window management (`requestNewActivity(appId, x, y, w, h)`,
  `requestCloseActivity(windowId)`). The compositor calls back to Kotlin when a Wayland
  client creates or destroys a toplevel, and Kotlin launches/finishes Activities accordingly.

---

## Components

### 1. Kotlin App Shell (`app/`)

Standard Android app. Receives the Wayland listening socket fd from the `app_process`
relay via a Binder object passed through an Intent. No Binder IPC for rendering or input.

- **`MainActivity`** — entry point. Loads `libsmithay_android.so` via `System.loadLibrary()`.
  Starts the compositor thread via JNI (`nativeStartCompositor(socketPath)`).
  Listens for callbacks from Rust to create/destroy window Activities.
- **`SurfaceViewActivity`** — one per Wayland toplevel. Uses `SurfaceView` (not TextureView)
  for a dedicated SurfaceFlinger layer. On surface ready: calls JNI
  `nativeOnSurfaceCreated(windowId, surface)` which gives the compositor direct access
  to the ANativeWindow. Forwards touch/key events to Rust via JNI
  `nativeOnTouchEvent(windowId, ...)` / `nativeOnKeyEvent(windowId, ...)`.

JNI interface (Kotlin → Rust):
- `nativeStartCompositor(socketPath: String)` — start compositor event loop on background thread
- `nativeStopCompositor()` — shut down
- `nativeOnSurfaceCreated(windowId: Long, surface: Surface)` — hand off ANativeWindow
- `nativeOnSurfaceChanged(windowId: Long, width: Int, height: Int)` — resize
- `nativeOnSurfaceDestroyed(windowId: Long)` — surface gone
- `nativeOnTouchEvent(windowId: Long, action: Int, x: Float, y: Float, pointerId: Int)`
- `nativeOnKeyEvent(windowId: Long, action: Int, keyCode: Int, scanCode: Int)`

JNI callbacks (Rust → Kotlin):
- `requestNewActivity(windowId: Long, appId: String, title: String, x: Int, y: Int, w: Int, h: Int)`
- `requestCloseActivity(windowId: Long)`
- `requestResizeActivity(windowId: Long, w: Int, h: Int)`

Bootstrapping: app starts → loads native lib → spawns compositor thread. Separately, user
launches `app_process` relay from Termux which creates `$XDG_RUNTIME_DIR/wayland-0` and
passes the listening socket fd to the app via a Binder object sent through an Intent
(TermuxAm pattern — direct `IActivityManager` calls, no Context needed). The app extracts
the fd and passes it to the compositor via JNI. The compositor calls `accept()` on the fd
directly. After handoff, the relay can exit. No Binder for rendering/input — only for this
one-shot socket bootstrap.

### 2. Android EGL Backend (`smithay-android/src/egl.rs`)

Initialize EGL from an Android Surface rather than a GBM device. This is the foundation
of the rendering pipeline — the compositor uses EGL/GLES for all rendering.

```rust
// Pseudocode
pub struct AndroidEglDisplay {
    display: EGLDisplay,  // from eglGetDisplay(EGL_DEFAULT_DISPLAY)
    // or eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, ...)
}

pub struct AndroidEglSurface {
    surface: EGLSurface,  // from eglCreateWindowSurface(display, config, ANativeWindow, ...)
}
```

Key considerations:
- Use `EGL_ANDROID_native_fence_sync` for fence-based synchronization with SurfaceFlinger.
- Query `EGL_ANDROID_image_native_buffer` to confirm AHardwareBuffer → EGLImage import is
  available. This is a **driver extension**, not API-level gated — must be queried at runtime
  via `eglQueryString` + `eglGetProcAddress`. Widely available on Android 8+ but not guaranteed.
- Query `EGL_EXT_image_dma_buf_import` to determine if direct dmabuf import is available.
  If yes, Smithay's `ImportDma` works directly. If no, use the AHB wrapping fallback.
- Smithay's `GlesRenderer::new()` takes a Smithay `EGLContext` (not raw EGL handles). We must
  use Smithay's EGL wrappers.

Smithay EGL integration (researched — no GBM assumption):
- `EGLDisplay::from_raw(display, config_id)` accepts pre-initialized raw `EGLDisplay` and
  `EGLConfig` pointers. This is the escape hatch. We create the EGL display ourselves via
  `eglGetDisplay(EGL_DEFAULT_DISPLAY)` + `eglInitialize`, then wrap it. `from_raw` skips
  `eglTerminate` on drop (we manage lifetime externally).
- Alternatively, implement the `EGLNativeDisplay` trait with `EGL_PLATFORM_ANDROID_KHR`
  (0x3141) to go through Smithay's platform display path.
- From the wrapped `EGLDisplay`, create an `EGLContext`, then pass to `GlesRenderer::new()`.
- No Smithay fork needed for this.

### 3. Buffer Import: dmabuf → GL Texture (`smithay-android/src/import.rs`)

The primary buffer path. Clients send dmabuf fds via `zwp_linux_dmabuf_v1`. The compositor
must import them as GL textures for compositing.

**Path A: Direct dmabuf import (preferred)**

If the stock Android EGL driver supports `EGL_EXT_image_dma_buf_import`, Smithay's
`GlesRenderer` already implements `ImportDma` which handles this:

```
dmabuf fd → eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...) → EGLImage → GL texture
```

No custom code needed. Just enable Smithay's `ImportDma` and test.

**Path B: AHardwareBuffer wrapping fallback**

If stock EGL lacks `EGL_EXT_image_dma_buf_import`, wrap the dmabuf fd as an AHardwareBuffer
first, then use Android's native EGL import:

```
dmabuf fd → AHardwareBuffer (via socketpair trick or Gralloc IMapper)
  → eglGetNativeClientBufferANDROID() → EGLClientBuffer
  → eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ...) → EGLImage → GL texture
```

The AHB wrapping step uses one of:
- **Socketpair trick:** Forge the `GraphicBuffer` serialization format (stable since Android 8),
  write to a socketpair with the dmabuf fd via `SCM_RIGHTS`, call
  `AHardwareBuffer_recvHandleFromUnixSocket()` on the other end. Uses only public NDK API.
- **Gralloc 4 IMapper:** Call `android.hardware.graphics.mapper@4.0` HIDL/AIDL interface via
  JNI to import the native handle. More robust, requires Android 11+.

Both AHB wrapping approaches are GPU-only — no CPU buffer copies.

### 4. Presentation (`smithay-android/src/output.rs`)

The compositor renders all surfaces for a given toplevel (the toplevel itself plus its
popups and subsurfaces) onto the Activity's EGLSurface via GlesRenderer, then calls
`eglSwapBuffers()`. SurfaceFlinger handles the final presentation.

One EGLSurface per Activity, backed by its ANativeWindow from the SurfaceView. The
compositor switches between Activities' EGLSurfaces via `eglMakeCurrent()`.

**Popups and subsurfaces** are composited by GlesRenderer into the same EGLSurface as
their parent toplevel. They do not get separate Android surfaces. This is the standard
Wayland compositor approach — the compositor reads client surface textures and draws them
at the correct positions relative to the toplevel.

**`wl_output` configuration:** Report Android display metrics to Wayland clients:
- Resolution: from `DisplayMetrics.widthPixels` / `heightPixels` (passed to Rust via JNI)
- Physical size: from `DisplayMetrics.xdpi` / `ydpi` (compute mm from pixels/dpi)
- Refresh rate: from `Display.getRefreshRate()` (or `Display.getSupportedModes()`)
- Scale factor: derive Wayland scale from `DisplayMetrics.density`. Android density 1.0 =
  160dpi ≈ Wayland scale 1. density 2.0 = 320dpi ≈ scale 2. Use `wp-fractional-scale-v1`
  for non-integer scales (common on Android: 2.625, 3.5, etc.)

### 5. Input Backend (`smithay-android/src/input.rs`)

Implements Smithay's `InputBackend` trait. Translates Android events to Smithay event types.

Smithay's `InputBackend` trait requires ~25 associated types (keyboard, pointer, touch,
gesture, tablet, switch events). For MVP, we implement the ones we need and use Smithay's
`UnusedEvent` (uninhabited type) for the rest:

**Implemented:**
- `KeyboardKeyEvent` — from Android `KeyEvent` (AKEYCODE_* → Linux KEY_* via mapping table)
- `PointerMotionAbsoluteEvent` — from Android `MotionEvent` (SOURCE_MOUSE)
- `PointerButtonEvent` — from Android `MotionEvent` button state changes
- `PointerAxisEvent` — from Android `MotionEvent` scroll events
- `TouchDownEvent`, `TouchUpEvent`, `TouchMotionEvent`, `TouchCancelEvent`, `TouchFrameEvent`

**Stubbed with `UnusedEvent`:**
- `GestureSwipeBeginEvent`, `GestureSwipeUpdateEvent`, `GestureSwipeEndEvent`
- `GesturePinchBeginEvent`, `GesturePinchUpdateEvent`, `GesturePinchEndEvent`
- `GestureHoldBeginEvent`, `GestureHoldEndEvent`
- `TabletToolAxisEvent`, `TabletToolProximityEvent`, `TabletToolTipEvent`, `TabletToolButtonEvent`
- `SwitchToggleEvent`, `PointerMotionEvent` (relative motion — not applicable to touchscreen)
- `SpecialEvent`

Also requires a `Device` type implementing `id()`, `name()`, `has_capability()`, `usb_id()`, `syspath()`.

Input flow:
1. `SurfaceViewActivity.onTouchEvent()` / `onKeyEvent()` fires on UI thread
2. Kotlin calls JNI `nativeOnTouchEvent(windowId, ...)` / `nativeOnKeyEvent(windowId, ...)`
3. JNI function posts event into a lock-free MPSC channel (crossbeam or similar)
4. Compositor thread drains channel each frame, converts to Smithay input events, feeds to `wl_seat`

Keyboard mapping is the hardest part:
- Android `AKEYCODE_*` → Linux `KEY_*` scancode (mostly 1:1, some exceptions)
- Need an XKB keymap that matches. Ship a default US layout, allow configuration.
- Software keyboard (IME): implement `zwp_text_input_v3` protocol to bridge Android IME
  into Wayland clients. This is a stretch goal — physical/external keyboards first.

### 6. Wayland Protocol Implementation (`smithay-android/src/compositor.rs`)

Use Smithay's protocol handling (this is where it shines — no custom work needed):

**Required (MVP):**
- `wl_compositor` + `wl_subcompositor` — surface management
- `zwp_linux_dmabuf_v1` — **the primary buffer path** (GPU buffer sharing from clients)
- `xdg_shell` (xdg_wm_base + xdg_surface + xdg_toplevel) — window management
- `wl_seat` (wl_pointer + wl_keyboard) — input
- `wl_output` — display information
- `wp-viewporter` — buffer scaling

**Later:**
- `wl_shm` — shared memory buffers (software rendering fallback, low priority)
- `xdg-decoration-unstable-v1` — server-side decorations
- `wp-fractional-scale-v1` — Android has high-DPI screens
- `xwayland` — X11 app support via Xwayland connecting to our compositor
- `zwp_text_input_v3` — Android IME bridge
- `wl_data_device_manager` — clipboard (bridge to Android clipboard)
- `wp-cursor-shape-v1` — cursor theming

---

## Crate Structure

```
smithay-android/
├── Cargo.toml
├── src/
│   ├── lib.rs              # JNI entry points, public API
│   ├── egl.rs              # Android EGL display/context/surface setup
│   ├── import.rs           # dmabuf → GL texture (EGLImage or AHB path)
│   ├── output.rs           # eglSwapBuffers presentation
│   ├── input.rs            # AndroidInputBackend (InputBackend impl)
│   ├── keymap.rs           # AKEYCODE → Linux scancode mapping
│   ├── compositor.rs       # Wayland state machine, protocol handling
│   └── window.rs           # Per-toplevel state (surface, geometry, etc.)
└── build.rs                # bindgen for NDK APIs
```

Dependencies:
- `smithay` (features: `renderer_gl`, `backend_egl`, `wayland_frontend`, `xdg_shell`, `desktop`)
  **Important:** use `default-features = false` to avoid pulling in libdrm, libgbm, libinput,
  libudev, libseat, and X11 — none of which exist on Android.
- `ndk` — AHardwareBuffer, ANativeWindow bindings
- `jni` — JNI interop with Kotlin app
- `crossbeam-channel` — lock-free MPSC for input events (UI thread → compositor thread)
- `xkbcommon` — keymap handling (smithay depends on this; requires cross-compiled libxkbcommon.so)

Android app:
```
app/
├── src/main/
│   ├── java/com/tawc/
│   │   ├── MainActivity.kt
│   │   ├── SurfaceViewActivity.kt
│   │   ├── NativeBridge.kt           # JNI extern declarations
│   │   ├── RelayReceiver.kt          # Receives fd from relay via Intent + Binder
│   │   └── Relay.kt                  # app_process entry point (runs in Termux)
│   └── res/
└── build.gradle.kts                   # invokes cargo-ndk, copies .so to jniLibs
```

No C++ glue. No AIDL — the relay passes the socket fd to the app via a Binder object
embedded in an Intent (TermuxAm pattern). The entire native layer is the Rust .so.

---

## Build System

- Rust library cross-compiled for `aarch64-linux-android` via `cargo-ndk`
- Outputs `libsmithay_android.so` loaded by the Kotlin app via `System.loadLibrary()`
- Gradle build invokes cargo-ndk as a custom task, copies .so into `jniLibs/arm64-v8a/`
- Target API level: 29 (Android 10) minimum for AHardwareBuffer import/export APIs.

**Native dependency: libxkbcommon.** Smithay depends on the `xkbcommon` crate which links
to `libxkbcommon.so` via FFI. This C library is **not in the Android NDK** and must be
cross-compiled for `aarch64-linux-android`. Options:
1. Cross-compile libxkbcommon from source using the NDK toolchain and bundle it in the .so
   (set `XKBCOMMON_LIB_DIR` / use pkg-config cross-compilation)
2. Use the pure-Rust `xkbcommon-rs` crate (by wysiwys, port of libxkbcommon 1.7.0, zero C
   deps) — but this is not yet integrated with Smithay, so would need a compatibility shim
   or Smithay feature flag

Option 1 is the pragmatic choice for now. Cross-compiling libxkbcommon is well-documented
and it has minimal dependencies (meson build, no X11 deps needed for just xkb).

**wayland-rs is pure Rust.** Smithay uses the `wayland-backend` crate which defaults to a
pure Rust Wayland protocol implementation — no `libwayland-server.so` needed. Do NOT enable
the `server_system` cargo feature (which would require the C library).

**Other native deps (provided by NDK):** EGL, GLESv2, libc (bionic), libandroid (for
AHardwareBuffer/ANativeWindow). These are all in the NDK sysroot.

---

## Implementation Order

The priority is getting a single GPU-accelerated Wayland client visible on screen as early
as possible. The rendering path must use GPU buffers (`zwp_linux_dmabuf_v1`) from the start
— **do not implement or test with `wl_shm` first.**

### Phase 1: Build Toolchain & EGL Proof
1. Set up Android app scaffold: single Activity with a SurfaceView
2. Cross-compile toolchain: cargo-ndk, NDK sysroot, cross-compile libxkbcommon for
   aarch64-linux-android
3. Rust library with JNI: receive ANativeWindow, create EGL context via Smithay's
   `EGLDisplay::from_raw()` or `EGLNativeDisplay` trait, wrap in `GlesRenderer`
4. Render a solid color to the EGLSurface via GlesRenderer + `eglSwapBuffers`
5. Query and log available EGL extensions — specifically check for
   `EGL_EXT_image_dma_buf_import` and `EGL_ANDROID_image_native_buffer`
6. **Milestone: GlesRenderer renders to Android Surface, EGL extension support known**

### Phase 2: dmabuf Import Proof
7. Determine dmabuf import path based on Phase 1 extension query:
   - If `EGL_EXT_image_dma_buf_import` available: use Smithay's `ImportDma` directly
   - If not: implement AHB wrapping (socketpair trick or Gralloc IMapper) +
     `EGL_ANDROID_image_native_buffer` import
8. Test: create a dmabuf (via AHardwareBuffer_allocate + export, or via a test fd),
   import it as a GL texture, render it onto the EGLSurface via GlesRenderer
9. **Milestone: dmabuf fd → GL texture → composited onto screen. GPU buffer import proven.**

### Phase 3: Wayland Server + Socket Relay
10. Initialize Smithay's Wayland state (Display, compositor, xdg_shell,
    `zwp_linux_dmabuf_v1`, wl_output)
11. Build `app_process` relay (TermuxAm pattern) for listening socket handoff
12. Test: Termux client connects to `$XDG_RUNTIME_DIR/wayland-0`, compositor receives
    connection via `DisplayHandle::insert_client()`
13. Handle dmabuf client commit: import dmabuf → GL texture → GlesRenderer composites
    onto EGLSurface → `eglSwapBuffers`
14. Test with a GPU-accelerated Wayland client from Termux (Mesa Turnip + a simple
    EGL/Vulkan client, or `weston-simple-dmabuf` if available)
15. **Milestone: GPU-accelerated Wayland client visible on screen via compositor**

### Phase 4: Input
16. Implement AndroidInputBackend (touch + pointer first, keyboard second)
17. Wire up Kotlin onTouchEvent/onKeyEvent → JNI → crossbeam channel → Smithay wl_seat
18. AKEYCODE → Linux scancode mapping + XKB keymap
19. **Milestone: can interact with a Wayland client**

### Phase 5: Multi-Window
20. JNI callback: compositor notifies Kotlin of new xdg_toplevels
21. MainActivity spawns SurfaceViewActivity per toplevel
22. Each Activity's SurfaceView gets its own EGLSurface; compositor switches via
    `eglMakeCurrent()` to render each window
23. Window lifecycle (map, unmap, close, resize)
24. **Milestone: multiple Wayland windows as separate Android Activities**

### Phase 6: Polish & Protocols (ongoing)
25. Frame callbacks (`wl_surface.frame`) for proper client frame pacing
26. Server-side decorations (xdg-decoration)
27. Fractional scaling (wp-fractional-scale) for high-DPI Android screens
28. `wl_shm` support (software rendering fallback for simple clients)
29. Clipboard bridge (wl_data_device ↔ Android ClipboardManager)
30. IME bridge (zwp_text_input_v3 ↔ Android InputMethodManager)
31. Xwayland support (stretch goal)

**Note on zero-copy presentation:** The current architecture uses a GPU composition pass
(GlesRenderer → eglSwapBuffers). A theoretically more efficient path would map each Wayland
surface to a separate `ASurfaceControl` child layer and submit `AHardwareBuffer`s directly
to SurfaceFlinger, eliminating the compositor's GPU pass entirely. This would require solving
the dmabuf→AHardwareBuffer wrapping problem (see notes.md §15) for every client buffer. It's
a possible future optimization but not a priority — the GPU composition pass is fast and the
architecture is simpler.

---

## Known Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| SELinux blocks cross-app Unix sockets | **Clients can't connect** | `app_process` relay in Termux (proven by wlroots-android-bridge and Termux:X11); or fd passing via ContentProvider |
| Smithay's EGL module assumes GBM | Blocks renderer init | Use `EGLDisplay::from_raw()` with pre-initialized Android EGL display — confirmed to exist in Smithay API. No fork needed. |
| Stock Android EGL lacks `EGL_EXT_image_dma_buf_import` | Can't import client dmabufs directly | Fallback: wrap dmabuf as AHardwareBuffer (socketpair trick or Gralloc IMapper), then import via `EGL_ANDROID_image_native_buffer`. Both paths are GPU-only. |
| `eglGetNativeClientBufferANDROID` not available | Can't import AHB as texture | Runtime extension check. Widely available on Android 8+ but not guaranteed. |
| Mesa Turnip dmabuf fds incompatible with stock EGL | Texture import fails | Both use KGSL (same kernel driver), so physical memory is compatible. Test early in Phase 2. |
| JNI call overhead for input events | Input lag | Lock-free MPSC channel (crossbeam), batch drain per frame; JNI overhead is ~nanoseconds so unlikely to be an issue |
| Smithay GlesRenderer internals assume Linux desktop EGL | Rendering or import fails | Test early. May need to patch Smithay for Android EGL quirks. |
| Android kills background Activities | Windows disappear | Use foreground service, SYSTEM_ALERT_WINDOW, or freeform multi-window mode |
| XKB keymap mismatch with Android keycodes | Wrong characters | Ship curated keymap, allow user override |
| EGL context thread affinity | Rendering fails from wrong thread | EGL context must be current on the compositor thread. Switch EGLSurfaces via `eglMakeCurrent` (has overhead per switch). |
| Activity launch latency (100-300ms per window) | Sluggish window creation | Suppress animations (`overridePendingTransition(0, 0)`), consider pre-creating a pool of Activities. |
| libxkbcommon not in Android NDK | Build fails | Cross-compile libxkbcommon from source with NDK toolchain; or shim the pure-Rust `xkbcommon-rs` crate. |
| Smithay has never been built for Android | Unknown build failures | Use `default-features = false`, only enable needed features. wayland-rs pure Rust backend avoids libwayland dependency. May need to patch platform-specific code paths. |
| Phantom Process Killer (Android 12+) | Relay process killed by OS | Relay exits after fd handoff (short-lived). Users may need Developer Options toggle (Android 14+). |
| `app_process` relay lacks Android Context | Can't call `bindService()` | Use TermuxAm pattern: relay creates Binder objects and sends them to app via direct `IActivityManager` calls (reflection). |
| Hidden API reflection breaks across Android versions | Relay stops working | The `IActivityManager` / `ActivityThread` reflection used by TermuxAm has worked through Android 15. Monitor for breakage; consider Shizuku as alternative mechanism. |
| Freeform windowing not universally available | Multi-window = fullscreen only on phones | On standard phones, each Activity is fullscreen (switch via recents). True freeform windows only on Samsung DeX, ChromeOS, Android 15+ desktop mode. |
| GPU-accelerated clients only work on Qualcomm | Limited device support | Mesa Turnip requires `/dev/kgsl-3d0` (Qualcomm Adreno). Mali/MediaTek/Exynos have no open-source drivers for stock Android kernels. Non-Qualcomm devices need `wl_shm` fallback (Phase 6). |

---

## References

- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) — prior art
- [Xtr126/labwc-android](https://github.com/Xtr126/labwc-android) — wlroots backend for Android
- [Smithay](https://github.com/Smithay/smithay) — Rust Wayland compositor library
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [EGL_ANDROID_image_native_buffer](https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_image_native_buffer.txt)
- [EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt)
- [ndk crate](https://crates.io/crates/ndk) — Rust bindings for Android NDK
- [Mesa Turnip (Qualcomm Vulkan)](https://docs.mesa3d.org/drivers/freedreno.html) — GPU driver for clients in chroot
