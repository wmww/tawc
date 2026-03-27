# Wayland Compositor for Android via Smithay

## Goal

A Rust/Kotlin Android app that runs a Wayland compositor inside an Android Activity.
Linux Wayland clients (running in Termux/proot/chroot) connect via a Unix domain socket
and get GPU-accelerated, zero-CPU-copy rendering onto Android Surfaces.

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
   tawc app) AND Termux filesystem access (launched from Termux's shell). The relay:
   - Creates `$XDG_RUNTIME_DIR/wayland-0` listening socket in Termux's filesystem
   - Connects to the tawc app's bound AIDL service via Binder
   - Hands off the listening socket fd to the compositor via `ParcelFileDescriptor`

   **Relay data path — listening socket handoff (preferred):**

   The relay creates the listening socket at `$XDG_RUNTIME_DIR/wayland-0`, then passes
   the *listening fd* to the compositor via Binder (`ParcelFileDescriptor`). The compositor
   calls `accept()` directly on the fd and uses `DisplayHandle::insert_client()` to add
   each new connection to the Wayland display. After the handoff, the relay can exit — it
   is only needed at startup.

   This works because SELinux checks apply to `connect()`/`bind()` syscalls, not to
   `read()`/`write()`/`accept()` on inherited file descriptors. Once the compositor holds
   the fd, the kernel treats it as the compositor's own.

   **Fallback — fd handoff per client:** If `accept()` on a handed-off listening socket
   hits SELinux issues on some devices, the relay can instead `accept()` each client
   connection itself, then pass the connected client fd to the compositor over Binder.
   The relay stays running but is not in the data path after handoff.

   **Last resort — byte proxy:** Relay sits in the middle, forwarding all Wayland protocol
   bytes. Simple but adds latency and CPU overhead. Only use if fd passing doesn't work.

   This is how wlroots-android-bridge works. (Termux:X11 uses `sharedUserId` instead,
   which only works for Termux plugins signed with the same key.) Adds one lightweight
   process but no Binder IPC for rendering/input — only connection bootstrapping.

   **Note:** The relay requires a small AIDL interface (or equivalent) for the initial fd
   handoff between the relay and the app. This is the *only* use of Binder in the
   architecture — all rendering and input are Binder-free.

2. **`socketpair()` + fd passing:** The app creates `socketpair()` fds and passes one end
   to Termux via a ContentProvider or bound Service using `ParcelFileDescriptor`. Termux
   clients would need a wrapper that obtains the fd before starting the Wayland client.
   More complex, but fully in-process for established connections.

3. **Shared writable directory (fragile):** Place the socket in a world-accessible location
   (e.g., `/data/local/tmp/`). May work on some devices/Android versions but SELinux
   enforcement varies by OEM. Not reliable for production.

## Our Approach: Smithay + Android EGL (works on all GPUs)

Instead of the dmabuf/minigbm trick, use the **standard Android EGL import path**:

```
AHardwareBuffer
  → eglGetNativeClientBufferANDROID()    → EGLClientBuffer
  → eglCreateImageKHR(NATIVE_BUFFER_ANDROID) → EGLImage
  → glEGLImageTargetTexture2DOES()       → GL texture
```

This goes through the vendor's own EGL/GLES driver and works on Qualcomm, Mali, PowerVR,
and Intel. No need for Mesa, GBM, or gralloc internals.

---

## Architecture

The compositor runs as a Rust native library inside the Android app, on a dedicated
background thread. No Binder IPC for surfaces or input. A lightweight `app_process` relay
(started from Termux) handles Wayland socket creation and hands off the listening fd to
the compositor via a one-shot Binder call — after that, the relay can exit.

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
│  │  ┌─ AndroidAllocator (AHardwareBuffer)                     │  │
│  │  ├─ GlesRenderer (stock Smithay, vendor GLES driver)       │  │
│  │  ├─ AndroidEglBackend (EGL on ANativeWindow)               │  │
│  │  ├─ AndroidInputBackend (events from Kotlin via channel)   │  │
│  │  ├─ AndroidOutputBackend (ASurfaceTransaction / eglSwap)   │  │
│  │  └─ Wayland state (xdg-shell, wl_seat, wl_output, etc)    │  │
│  └────────────────────────────────────────────────────────────┘  │
│         │                                                        │
│    Unix domain socket (see "Socket sharing" above)               │
│         │                                                        │
│  app_process relay (creates socket in Termux's XDG_RUNTIME_DIR)  │
│         │                                                        │
│  Termux / proot / chroot (separate process, same device)         │
│    └─ Wayland clients (GTK, Qt, wlroots apps, etc)               │
└──────────────────────────────────────────────────────────────────┘
```

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

Standard Android app. Includes a small AIDL service for the `app_process` relay to hand
off the Wayland listening socket fd. No Binder IPC for rendering or input.

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
passes the listening socket fd to the compositor via a bound AIDL service. The compositor
calls `accept()` on the fd directly. After handoff, the relay can exit. No AIDL for
rendering/input — only for this one-shot socket bootstrap.

### 2. Android EGL Backend (`smithay-android/src/egl.rs`)

Initialize EGL from an Android Surface rather than a GBM device.

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

### 3. Android Allocator (`smithay-android/src/allocator.rs`)

**Not needed for MVP.** During Phases 1-4 (eglSwapBuffers), the compositor renders directly
to EGLSurfaces backed by ANativeWindows — no separate buffer allocation needed. The
allocator is only required for Phase 5 (ASurfaceTransaction zero-copy path), where we need
to render to AHardwareBuffers and submit them directly to SurfaceFlinger.

Implements Smithay's `Allocator` trait using AHardwareBuffer.

```rust
pub struct AndroidAllocator;
pub struct AndroidBuffer {
    buffer: *mut AHardwareBuffer,
    size: Size<i32, BufferCoords>,
    format: Format,
}

impl Allocator for AndroidAllocator {
    type Buffer = AndroidBuffer;
    type Error = AndroidAllocatorError;

    fn create_buffer(
        &mut self,
        width: u32,
        height: u32,
        fourcc: Fourcc,
        modifiers: &[Modifier],
    ) -> Result<AndroidBuffer, Self::Error> {
        // Map DRM fourcc → AHardwareBuffer format (AHARDWAREBUFFER_FORMAT_*)
        // AHardwareBuffer_allocate() with GPU_FRAMEBUFFER | GPU_SAMPLED_IMAGE usage
        // Return wrapped buffer
    }
}

impl Buffer for AndroidBuffer {
    fn size(&self) -> Size<i32, BufferCoords> { self.size }
    fn format(&self) -> Format { self.format }
}
```

Format mapping (DRM fourcc → AHardwareBuffer format):

**IMPORTANT:** DRM fourcc names describe MSB-to-LSB channel order, which is the *reverse*
of memory byte order on little-endian. AHardwareBuffer format names describe memory byte order.
So `DRM_FORMAT_ARGB8888` = BGRA in memory ≠ `R8G8B8A8_UNORM` = RGBA in memory.

Correct mappings:
- `DRM_FORMAT_ABGR8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` (RGBA in memory)
- `DRM_FORMAT_XBGR8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM` (RGBX in memory)
- `DRM_FORMAT_RGB565`   → `AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM`
- `DRM_FORMAT_ARGB8888` → no direct AHB constant; use `HAL_PIXEL_FORMAT_BGRA_8888` (value 5)
  if vendor supports it, otherwise convert/avoid
- Others as needed; query supported formats at runtime

### 4. Buffer Import: AHardwareBuffer → GL Texture (`smithay-android/src/import.rs`)

For Wayland clients using `wl_shm`, Smithay's `ImportMem` / `ImportMemWl` handles it
(CPU memcpy → `glTexImage2D`). This works out of the box.

For GPU-accelerated clients and for the compositor's own render targets, two options:

**Option A: Dmabuf export (preferred — leverages existing Smithay code)**

On Android 10+, `AHardwareBuffer` can be exported to a dmabuf fd via NDK. Smithay's
`GlesRenderer` already implements `ImportDma`. This avoids writing custom GL import code:

```rust
// Export AHardwareBuffer → dmabuf fd (NDK function, Android 10+)
// Wrap fd as Smithay Dmabuf
// Use GlesRenderer::import_dmabuf() — already implemented
```

Needs testing to confirm Android dmabuf fds work with Smithay's desktop-oriented dmabuf
import path (same EGL extensions under the hood, but edge cases possible).

**Option B: Direct EGLImage import (fallback)**

```rust
impl ImportAndroidBuffer for GlesRenderer {
    fn import_ahardwarebuffer(
        &mut self,
        buffer: &AndroidBuffer,
    ) -> Result<GlesTexture, GlesError> {
        // 1. eglGetNativeClientBufferANDROID(buffer.raw())
        // 2. eglCreateImageKHR(display, EGL_NO_CONTEXT,
        //        EGL_NATIVE_BUFFER_ANDROID, client_buffer, attribs)
        // 3. glGenTextures + glBindTexture(GL_TEXTURE_2D)
        // 4. glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image)
        // 5. Wrap in GlesTexture (or custom texture type)
    }
}
```

Both are zero-copy: the AHardwareBuffer is GPU memory; importing it as a GL texture just
creates a GL view of the same memory.

### 5. Presentation Backend (`smithay-android/src/output.rs`)

Two options, in order of preference:

**Option A: ASurfaceTransaction (preferred, zero-copy)**
```rust
pub fn present(surface_control: *mut ASurfaceControl, buffer: &AndroidBuffer) {
    // ASurfaceTransaction_create()
    // ASurfaceTransaction_setBuffer(transaction, surface_control, buffer.raw())
    // ASurfaceTransaction_setVisibility(transaction, surface_control, VISIBLE)
    // ASurfaceTransaction_apply(transaction)
}
```
Submits the AHardwareBuffer directly to SurfaceFlinger. Supports hardware overlays
(direct scanout) if the device supports it. Available since API 29.

**Caveat:** `ASurfaceControl_createFromWindow` returns NULL for the root `ANativeWindow`
before API 35 (confirmed bug: https://issuetracker.google.com/issues/320706287). Must use
a child SurfaceView's `ANativeWindow`, not the Activity's root window. The wlroots-android-bridge
hit this exact issue.

**Option B: eglSwapBuffers (simpler, one extra copy)**
- Render onto the EGLSurface bound to the ANativeWindow
- Call `eglSwapBuffers()` — SurfaceFlinger gets the buffer via its own triple-buffering
- Simpler but involves the EGL buffer queue (producer-consumer), not true zero-copy
  from the allocator's buffer.

Start with Option B for bringup, migrate to Option A for production.

### 6. Input Backend (`smithay-android/src/input.rs`)

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

### 7. Wayland Protocol Implementation (`smithay-android/src/compositor.rs`)

Use Smithay's protocol handling (this is where it shines — no custom work needed):

**Required (MVP):**
- `wl_compositor` + `wl_subcompositor` — surface management
- `wl_shm` — shared memory buffers (software rendering fallback)
- `xdg_shell` (xdg_wm_base + xdg_surface + xdg_toplevel) — window management
- `wl_seat` (wl_pointer + wl_keyboard) — input
- `wl_output` — display information
- `xdg-decoration-unstable-v1` — server-side decorations (android manages window chrome)
- `wp-fractional-scale-v1` — Android has high-DPI screens
- `wp-viewporter` — buffer scaling

**Stretch goals:**
- `zwp_linux_dmabuf_v1` — GPU buffer sharing from clients (needs per-vendor testing)
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
│   ├── allocator.rs        # AndroidAllocator (AHardwareBuffer)
│   ├── import.rs           # AHardwareBuffer → EGLImage → GL texture
│   ├── output.rs           # ASurfaceTransaction or eglSwapBuffers presentation
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
- `ndk` — AHardwareBuffer, ANativeWindow, ASurfaceControl bindings
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
│   │   ├── RelayService.kt           # Bound AIDL service for socket fd handoff
│   │   └── Relay.kt                  # app_process entry point (runs in Termux)
│   ├── aidl/com/tawc/
│   │   └── IRelayService.aidl        # Single method: handoffListeningSocket(fd)
│   └── res/
└── build.gradle.kts                   # invokes cargo-ndk, copies .so to jniLibs
```

No C++ glue. AIDL is only for the one-shot relay socket handoff. The entire native layer
is the Rust .so.

---

## Build System

- Rust library cross-compiled for `aarch64-linux-android` via `cargo-ndk`
- Outputs `libsmithay_android.so` loaded by the Kotlin app via `System.loadLibrary()`
- Gradle build invokes cargo-ndk as a custom task, copies .so into `jniLibs/arm64-v8a/`
- Small AIDL interface for relay socket handoff (compiled by Gradle automatically)
- Target API level: 29 (Android 10) minimum for ASurfaceTransaction + AHardwareBuffer.
  API 35 recommended for `ASurfaceControl_createFromWindow` on root ANativeWindow.

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
AHardwareBuffer/ANativeWindow/ASurfaceControl). These are all in the NDK sysroot.

---

## Implementation Order

### Phase 1: Minimal Rendering (weeks 1-2)
1. Set up Android app scaffold with single SurfaceView
2. Cross-compile toolchain: cargo-ndk, NDK sysroot, cross-compile libxkbcommon for
   aarch64-linux-android (see Build System section)
3. Rust library with JNI: receive ANativeWindow, create EGL context via Smithay's
   `EGLDisplay::from_raw()` or `EGLNativeDisplay` trait, wrap in `GlesRenderer`
4. Verify: render a solid color to the EGLSurface via eglSwapBuffers
5. **Milestone: Rust code renders to Android Surface**

### Phase 2: Wayland Server (weeks 3-4)
6. Initialize Smithay's Wayland state (Display, compositor, xdg_shell)
7. Build `app_process` relay + AIDL service for listening socket handoff. Test that
   Termux clients can connect to `$XDG_RUNTIME_DIR/wayland-0` and the compositor
   receives the connections via `DisplayHandle::insert_client()`.
8. Handle wl_shm clients: ImportMemWl → GlesRenderer → eglSwapBuffers
9. Test with `weston-simple-shm` or `wlr-randr` from Termux
10. **Milestone: wl_shm Wayland client renders on screen**

### Phase 3: Input (weeks 5-6)
11. Implement AndroidInputBackend (keyboard + pointer first)
12. Wire up Kotlin onTouchEvent/onKeyEvent → JNI → crossbeam channel → Smithay wl_seat
13. AKEYCODE → Linux scancode mapping + XKB keymap
14. Pointer/touch from MotionEvent
15. **Milestone: can type and click in a Wayland client (e.g., `foot` terminal)**

### Phase 4: Multi-Window (weeks 7-8)
16. JNI callback: compositor notifies Kotlin of new xdg_toplevels
17. MainActivity spawns SurfaceViewActivity per window
18. Each window gets its own Surface → EGL context → render target
19. Window lifecycle (map, unmap, close, resize)
20. **Milestone: multiple Wayland windows as separate Android Activities**

### Phase 5: Zero-Copy Presentation (weeks 9-10)
21. Implement AndroidAllocator (AHardwareBuffer) — only needed now, for rendering to
    AHardwareBuffers instead of EGLSurfaces
22. Replace eglSwapBuffers with ASurfaceTransaction_setBuffer: compositor renders to
    AHardwareBuffer, submits directly to SurfaceFlinger
23. Fence synchronization via EGL_ANDROID_native_fence_sync
24. Benchmark: measure latency and throughput vs eglSwapBuffers path
25. **Milestone: zero-copy buffer path, no eglSwapBuffers overhead**

### Phase 6: Polish & Protocols (ongoing)
26. Server-side decorations (xdg-decoration)
27. Fractional scaling (wp-fractional-scale) for high-DPI Android screens
28. Clipboard bridge (wl_data_device ↔ Android ClipboardManager)
29. IME bridge (zwp_text_input_v3 ↔ Android InputMethodManager)
30. Xwayland support (stretch goal)

---

## Known Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| SELinux blocks cross-app Unix sockets | **Clients can't connect** | `app_process` relay in Termux (proven by wlroots-android-bridge and Termux:X11); or fd passing via ContentProvider |
| Smithay's EGL module assumes GBM | Blocks renderer init | Use `EGLDisplay::from_raw()` with pre-initialized Android EGL display — confirmed to exist in Smithay API. No fork needed. |
| AHardwareBuffer format support varies by vendor | Some fourcc formats unavailable | Query supported formats at runtime, fall back to ABGR8888 (RGBA). Note: DRM↔AHB format mapping has byte-order subtleties. |
| `eglGetNativeClientBufferANDROID` not available | Can't import AHB as texture | Runtime extension check; fall back to dmabuf export path or CPU readback. Widely available on Android 8+ but not guaranteed. |
| `ASurfaceControl_createFromWindow` returns NULL pre-API 35 | Zero-copy path broken | Use child SurfaceView's ANativeWindow (not root window); or fall back to eglSwapBuffers |
| JNI call overhead for input events | Input lag | Lock-free MPSC channel (crossbeam), batch drain per frame; JNI overhead is ~nanoseconds so unlikely to be an issue |
| Smithay GlesRenderer internals assume Linux desktop EGL | Texture import fails | Try dmabuf export path first (leverages existing ImportDma); fall back to custom EGLImage import |
| Android kills background Activities | Windows disappear | Use foreground service, SYSTEM_ALERT_WINDOW, or freeform multi-window mode |
| XKB keymap mismatch with Android keycodes | Wrong characters | Ship curated keymap, allow user override |
| EGL context thread affinity | Rendering fails from wrong thread | EGL context must be current on the compositor thread. Can render to multiple EGLSurfaces from one thread via `eglMakeCurrent` switching (has overhead per switch). |
| Activity launch latency (100-300ms per window) | Sluggish window creation | Suppress animations (`overridePendingTransition(0, 0)`), consider pre-creating a pool of Activities, or evaluate single-Activity multi-SurfaceView approach as alternative. |
| libxkbcommon not in Android NDK | Build fails | Cross-compile libxkbcommon from source with NDK toolchain; or shim the pure-Rust `xkbcommon-rs` crate. |
| Smithay has never been built for Android | Unknown build failures | Use `default-features = false`, only enable needed features. wayland-rs pure Rust backend avoids libwayland dependency. May need to patch platform-specific code paths. |

---

## References

- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) — prior art
- [Xtr126/labwc-android](https://github.com/Xtr126/labwc-android) — wlroots backend for Android
- [Smithay](https://github.com/Smithay/smithay) — Rust Wayland compositor library
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [ASurfaceTransaction NDK docs](https://developer.android.com/ndk/reference/group/native-activity#asurfacetransaction)
- [EGL_ANDROID_image_native_buffer](https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_image_native_buffer.txt)
- [ndk crate](https://crates.io/crates/ndk) — Rust bindings for Android NDK
