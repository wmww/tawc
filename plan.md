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

**Why we don't need two processes:** The wlroots-android-bridge uses two processes because
labwc is a standalone C program with heavy native dependencies (Mesa, wlroots, etc.) that
must run inside Termux. We're building a Rust library loaded via JNI — the compositor runs
as a background thread inside the Android app itself. This eliminates all Binder IPC for
surfaces and input, meaning zero serialization overhead and direct access to ANativeWindow
and AInputQueue from the compositor thread.

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

Single-process: the compositor runs as a Rust native library inside the Android app,
on a dedicated background thread. No Binder IPC for surfaces or input.

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
│    Unix domain socket ($PREFIX/tmp/wayland-0)                    │
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

Standard Android app. No AIDL, no Binder, no separate process.

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

Bootstrapping: app starts → loads native lib → spawns compositor thread → creates Wayland
socket → clients connect from Termux. No `app_process`, no TermuxAm.

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
- Query `EGL_ANDROID_image_native_buffer` to confirm AHardwareBuffer → EGLImage import is available.
- Smithay's `GlesRenderer::new()` takes an `EGLContext` — we create one from our Android EGLDisplay
  and pass it in. The renderer itself doesn't need modification.

Risk: Smithay's EGL module may assume GBM-backed initialization internally. If so, we either
patch Smithay's `EGLDisplay` to accept an Android native display, or bypass Smithay's EGL
wrapper and create raw EGL objects ourselves, then construct `GlesRenderer` from the raw context.

### 3. Android Allocator (`smithay-android/src/allocator.rs`)

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
- `ARGB8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM`
- `XRGB8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM`
- `RGB565` → `AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM`
- Others as needed

### 4. Buffer Import: AHardwareBuffer → GL Texture (`smithay-android/src/import.rs`)

For Wayland clients using `wl_shm`, Smithay's `ImportMem` / `ImportMemWl` handles it
(CPU memcpy → `glTexImage2D`). This works out of the box.

For GPU-accelerated clients and for the compositor's own render targets, implement
AHardwareBuffer → EGLImage → GL texture import:

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

This is the zero-copy path. The AHardwareBuffer is GPU memory; importing it as a GL
texture does not copy — it just creates a GL view of the same memory.

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
(direct scanout) if the device supports it. Requires API 29+ (basic) / 34+ (setBuffer with AHB).

**Option B: eglSwapBuffers (simpler, one extra copy)**
- Render onto the EGLSurface bound to the ANativeWindow
- Call `eglSwapBuffers()` — SurfaceFlinger gets the buffer via its own triple-buffering
- Simpler but involves the EGL buffer queue (producer-consumer), not true zero-copy
  from the allocator's buffer.

Start with Option B for bringup, migrate to Option A for production.

### 6. Input Backend (`smithay-android/src/input.rs`)

Implements Smithay's `InputBackend` trait. Translates Android events to Smithay event types.

Required associated type implementations:
- `KeyboardKeyEvent` — from Android `KeyEvent` (AKEYCODE_* → Linux KEY_* via mapping table)
- `PointerMotionAbsoluteEvent` — from Android `MotionEvent` (SOURCE_MOUSE)
- `PointerButtonEvent` — from Android `MotionEvent` button state changes
- `PointerAxisEvent` — from Android `MotionEvent` scroll events
- `TouchDownEvent`, `TouchUpEvent`, `TouchMotionEvent` — from Android `MotionEvent` (SOURCE_TOUCHSCREEN)

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
- `ndk` — AHardwareBuffer, ANativeWindow, ASurfaceControl bindings
- `jni` — JNI interop with Kotlin app
- `crossbeam-channel` — lock-free MPSC for input events (UI thread → compositor thread)
- `xkbcommon` — keymap handling (smithay already depends on this)

Android app:
```
app/
├── src/main/
│   ├── java/com/project/
│   │   ├── MainActivity.kt
│   │   ├── SurfaceViewActivity.kt
│   │   └── NativeBridge.kt           # JNI extern declarations
│   └── res/
└── build.gradle.kts                   # invokes cargo-ndk, copies .so to jniLibs
```

No AIDL, no C++ glue, no separate process. The entire native layer is the Rust .so.

---

## Build System

- Rust library cross-compiled for `aarch64-linux-android` via `cargo-ndk`
- Outputs `libsmithay_android.so` loaded by the Kotlin app via `System.loadLibrary()`
- Gradle build invokes cargo-ndk as a custom task, copies .so into `jniLibs/arm64-v8a/`
- No C++ code, no AIDL compilation — everything is Rust + Kotlin
- Target API level: 34 (Android 14) minimum for `ASurfaceTransaction_setBuffer` with AHardwareBuffer

---

## Implementation Order

### Phase 1: Minimal Rendering (weeks 1-2)
1. Set up Android app scaffold with single SurfaceView
2. Rust library with JNI: receive ANativeWindow, create EGL context
3. AndroidAllocator: allocate AHardwareBuffer, import as GL texture
4. Verify: render a solid color to screen via eglSwapBuffers
5. **Milestone: Rust code renders to Android Surface**

### Phase 2: Wayland Server (weeks 3-4)
6. Initialize Smithay's Wayland state (Display, compositor, xdg_shell)
7. Create Unix socket, expose as WAYLAND_DISPLAY
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
21. Replace eglSwapBuffers with ASurfaceTransaction_setBuffer
22. Compositor renders to AHardwareBuffer, submits directly to SurfaceFlinger
23. Fence synchronization via EGL_ANDROID_native_fence_sync
24. Benchmark: measure latency and throughput vs Phase 2
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
| Smithay's EGL module assumes GBM | Blocks renderer init | Bypass Smithay's EGL wrapper, use raw EGL, construct GlesRenderer from raw GL context |
| AHardwareBuffer format support varies by vendor | Some fourcc formats unavailable | Query supported formats at runtime, fall back to RGBA8888 |
| ASurfaceTransaction_setBuffer requires API 34+ | Limits device support | Fall back to eglSwapBuffers on older APIs |
| JNI call overhead for input events | Input lag | Lock-free MPSC channel (crossbeam), batch drain per frame; JNI overhead is ~nanoseconds so unlikely to be an issue |
| Smithay GlesRenderer internals assume Linux desktop EGL | Texture import fails | May need to fork/patch GlesRenderer for AHardwareBuffer import path |
| Android kills background Activities | Windows disappear | Use foreground service, SYSTEM_ALERT_WINDOW, or freeform multi-window mode |
| XKB keymap mismatch with Android keycodes | Wrong characters | Ship curated keymap, allow user override |

---

## References

- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) — prior art
- [Xtr126/labwc-android](https://github.com/Xtr126/labwc-android) — wlroots backend for Android
- [Smithay](https://github.com/Smithay/smithay) — Rust Wayland compositor library
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [ASurfaceTransaction NDK docs](https://developer.android.com/ndk/reference/group/native-activity#asurfacetransaction)
- [EGL_ANDROID_image_native_buffer](https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_image_native_buffer.txt)
- [ndk crate](https://crates.io/crates/ndk) — Rust bindings for Android NDK
