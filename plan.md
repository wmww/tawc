# Wayland Compositor for Android via Smithay

## Goal

A Rust/Kotlin Android app that runs a Wayland compositor inside an Android Activity.
Linux Wayland clients (running in Termux/proot/chroot) connect via a Unix domain socket
and get GPU-accelerated rendering onto Android Surfaces. All buffer handling stays on the
GPU -- zero-copy between client and compositor because both sides use the same stock
Android GPU driver.

**Key insight:** Clients use **libhybris** to load the stock Android GPU driver into
their glibc environment. A custom **Wayland WSI layer** (EGL
wrapper library, with Vulkan implicit layer as stretch goal) bridges the gap between the
stock driver and Wayland protocol. The compositor is a normal Android app using the stock
driver natively. Same driver on both sides = buffer sharing via **AHardwareBuffer**
(Android's cross-process GPU buffer primitive).

---

## Prior Art

### wlroots-android-bridge
[Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) --
wlroots/labwc compositor on Android. Key design decisions we borrow:
- **One Android Activity per Wayland toplevel** -- Android's window manager handles
  task switching, recents, window positioning
- **ASurfaceTransaction for presentation** -- submit rendered buffers to SurfaceFlinger

Why it doesn't solve our problem: depends on Mesa + minigbm, only works on Intel/x86.
Different approach to GPU drivers entirely.

### Termux:X11
Termux:X11 is the current state of the art for graphical Linux apps on Android. It works
but **all paths involve CPU readback** -- no zero-copy GPU buffer sharing between Mesa
Turnip and the stock Android driver. See notes.md for detailed analysis.

### libhybris
[libhybris/libhybris](https://github.com/libhybris/libhybris) -- compatibility layer
allowing glibc programs to load bionic-linked Android shared libraries. Used by Sailfish
OS and Ubuntu Touch to run stock Android GPU drivers on glibc-based Linux. **Actively
maintained** -- Android 16 support merged March 2026. This is what enables our
architecture.

### ARM vulkan-wsi-layer
[ArmSoM/vulkan-wsi-layer](https://github.com/ArmSoM/vulkan-wsi-layer) -- open-source
Vulkan layer implementing Wayland/X11 WSI independently of the GPU driver. **Not directly
usable for our architecture** -- it requires `VK_EXT_external_memory_dma_buf` which stock
Android drivers don't support. But useful as structural reference for how to write a
Vulkan implicit layer.

### libhybris Vulkan WSI
libhybris itself has a built-in Vulkan WSI that swaps `VK_KHR_android_surface` for
`VK_KHR_wayland_surface` and presents via the `android_wlegl` protocol (Sailfish OS
ecosystem). Prior art for the client-side WSI approach. Uses `WaylandNativeWindow`
(inherits `ANativeWindow`) + gralloc for buffer allocation. See notes.md for deep dive.

---

## Architecture

### The GPU Driver Strategy

The fundamental problem in running a Wayland compositor on Android is GPU buffer sharing.
Wayland clients and the compositor must share GPU buffers zero-copy. On desktop Linux
this is trivial (both use Mesa, dmabufs). On Android there are two different GPU driver
stacks (Mesa in the chroot, stock proprietary driver in the Android app) that can't share
buffers.

**Our solution: eliminate the driver mismatch.** Both sides use the stock Android GPU
driver:

- **Compositor** (Android app): Uses the stock driver natively. Nothing special.
- **Clients** (glibc programs in Termux chroot): Use **libhybris** to load the stock
  Android GPU driver's bionic `.so` files into the glibc process. A custom **WSI layer**
  implements Wayland surface/swapchain support on top of the stock driver's Vulkan/EGL.

Same driver on both sides means AHardwareBuffers are natively compatible across
processes. No cross-driver import hacks, no CPU readback. Buffer sharing uses
`AHardwareBuffer_sendHandleToUnixSocket` / `recvHandleFromUnixSocket` (NDK API 26+).

### How libhybris Fits In

libhybris reimplements Android's bionic linker so glibc processes can load bionic `.so`
files. It hooks bionic libc calls (pthread, malloc, etc.) and redirects to glibc
equivalents. The loading chain in a client:

```
App (glibc-linked)
  -> dlopen("libEGL.so")  -- finds OUR wrapper (glibc-linked, in LD_LIBRARY_PATH)
    -> our wrapper calls libhybris
      -> libhybris loads /vendor/lib64/egl/libEGL_<vendor>.so (bionic-linked)
      -> libhybris loads /vendor/lib64/egl/libGLESv2_<vendor>.so (bionic-linked)
```
(e.g. `libEGL_adreno.so` on Qualcomm, `libEGL_mali.so` on ARM Mali, etc.)

For Vulkan, the implicit layer loads the vendor's `libvulkan_<vendor>.so` via libhybris
similarly.

The stock driver's dependencies (vendor libs, typically under `/vendor/lib64/`) and
kernel GPU device node (e.g. `/dev/kgsl-3d0` on Qualcomm, `/dev/mali0` on ARM Mali)
are accessible from the chroot. The process UID and SELinux context are unchanged, so
GPU access and Binder calls to gralloc work as they would from any Android app.

### Buffer Sharing via AHardwareBuffer

On desktop Linux, Wayland buffer sharing uses dmabufs (`zwp_linux_dmabuf_v1`). Stock
Android GPU drivers don't support the dmabuf Vulkan/EGL extensions
(`VK_EXT_external_memory_dma_buf`, `EGL_EXT_image_dma_buf_import`). Android's native
cross-process GPU buffer primitive is **AHardwareBuffer**.

Buffer sharing path:
1. Client allocates `AHardwareBuffer` with GPU usage flags (via NDK API through libhybris)
2. Client creates `EGLImage` from AHB, attaches to FBO, renders with GLES
3. Client sends AHB to compositor via `AHardwareBuffer_sendHandleToUnixSocket()` on a
   **side-channel socket** (AHB serialization uses its own wire format, not Wayland's)
4. Compositor receives via `AHardwareBuffer_recvHandleFromUnixSocket()`
5. Compositor imports: `eglGetNativeClientBufferANDROID(ahb)` -> `eglCreateImageKHR`
   with `EGL_NATIVE_BUFFER_ANDROID` -> GL texture
6. Compositor composites and presents

A custom Wayland protocol extension (`tawc_buffer_v1`) coordinates buffer lifecycle
(attach, commit, release) while the actual AHB data flows over the side channel. Prior
art: Sailfish OS's `android_wlegl` protocol does exactly this kind of Android-specific
buffer passing over Wayland.

### The WSI Layer

Standard Linux apps expect `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND, ...)` (EGL) or
`VK_KHR_wayland_surface` (Vulkan). The stock Android driver doesn't have these -- it has
`EGL_PLATFORM_ANDROID` / `VK_KHR_android_surface`. Our WSI layer bridges this gap.

**EGL/GLES: Wrapper `libEGL.so`** (primary path -- covers GTK3, Qt, SDL, most Linux apps)
- Named `libEGL.so`, placed first in `LD_LIBRARY_PATH`
- Uses libhybris to load the real stock EGL/GLES driver
- Intercepts `eglGetPlatformDisplay(WAYLAND)` -- initializes stock driver in surfaceless
  mode, stores `wl_display` for protocol work, opens side-channel socket for AHB transfer
- Intercepts `eglCreateWindowSurface` -- allocates AHardwareBuffer pool (double/triple
  buffered), creates EGLImages from AHBs, wraps in FBOs for rendering
- Intercepts `eglSwapBuffers` -- sends current AHB to compositor via side channel +
  Wayland buffer commit, rotates to next buffer in pool
- Passes through everything else (all GL calls, `eglMakeCurrent`, etc.) to stock driver
- Apps load it without modification

**Vulkan: Implicit layer** (stretch goal -- libhybris Vulkan compatibility varies by GPU vendor)
- Standard Khronos layer mechanism -- zero app changes needed
- Advertises `VK_KHR_wayland_surface` + `VK_KHR_swapchain`
- Allocates swapchain images backed by AHardwareBuffers via
  `VK_ANDROID_external_memory_android_hardware_buffer`
- Sends AHBs to compositor via same side-channel mechanism as EGL path
- Passes through all rendering calls untouched
- **Risk:** libhybris Vulkan compatibility varies by GPU vendor, with unmerged fixes
  (PRs #604, #607). EGL/GLES path should be proven first.

### System Diagram

```
+------------------------ Android App Process -------------------------+
|                                                                       |
|  Kotlin / UI Thread                                                   |
|  +------------------------------------------------------------------+ |
|  |  MainActivity                                                     | |
|  |    +-- manages SurfaceViewActivity x N (one per window)           | |
|  |         +-- SurfaceView -> ANativeWindow --+                      | |
|  |         +-- onTouchEvent / onKeyEvent ----+|                      | |
|  +------------------------------------------+|----------------------+ |
|                                    JNI calls ||                       |
|  Compositor Thread (Rust)                    ||                       |
|  +-------------------------------------------vv---------------------+ |
|  |  smithay-android                                                  | |
|  |                                                                   | |
|  |  +-- GlesRenderer (stock Smithay, stock GLES driver)              | |
|  |  +-- AndroidEglBackend (EGL on ANativeWindow)                     | |
|  |  +-- AndroidInputBackend (events from Kotlin via channel)         | |
|  |  +-- AHB import (recv AHB -> EGLImage -> GL texture)              | |
|  |  +-- Wayland state (xdg-shell, wl_seat, wl_output, etc)          | |
|  +-------------------------------------------------------------------+ |
|         |                                                             |
|    Unix domain socket (listening fd received from relay)               |
|         |                                                             |
+---------|-------------------------------------------------------------+
          |
  app_process relay (Termux UID, exits after handoff)
    +-- Creates socket at $XDG_RUNTIME_DIR/wayland-0
    +-- Passes listening fd to app via Intent + Binder
    +-- Exits
          |
  Termux / chroot
    +-- Wayland clients (GTK, Qt, SDL, etc.)
    +-- libhybris loads stock GPU driver (bionic .so in glibc process)
    +-- Our WSI layer (EGL wrapper; Vulkan layer as stretch goal)
    +-- AHardwareBuffer shared via side-channel socket
    +-- GPU access via vendor kernel device (stock driver, same as compositor)
```

### Buffer Sharing Flow

1. Client allocates `AHardwareBuffer` via NDK API (through libhybris)
2. Client creates `EGLImage` from AHB, attaches to FBO, renders with GLES
3. WSI layer sends AHB via `AHardwareBuffer_sendHandleToUnixSocket()` on side channel
4. WSI layer sends buffer-commit message via `tawc_buffer_v1` on Wayland socket
5. Compositor receives AHB via `AHardwareBuffer_recvHandleFromUnixSocket()`
6. Compositor imports as EGLImage via `eglGetNativeClientBufferANDROID` + `eglCreateImageKHR`
7. Compositor composites as GL texture via GlesRenderer
8. `eglSwapBuffers()` -> SurfaceFlinger presents

Zero-copy from client to compositor. One GPU composition pass for display.

### Wayland Socket Sharing

Cross-app Unix sockets are blocked by SELinux on Android 9+. Solution: `app_process`
relay launched from Termux.

The relay creates `$XDG_RUNTIME_DIR/wayland-0`, passes the **listening fd** to the
compositor app via Binder (Intent + ParcelFileDescriptor, using the TermuxAm pattern
of direct `IActivityManager` calls). The compositor calls `accept()` on the fd. After
handoff, the relay exits.

This works because SELinux checks apply to `connect()`/`bind()`, not to
`read()`/`write()`/`accept()` on inherited fds. Proven pattern -- used by
wlroots-android-bridge and Termux:X11.

---

## Components

### 1. Kotlin App Shell (`app/`)

Standard Android app. Receives the Wayland listening socket fd from the `app_process`
relay via a Binder object passed through an Intent.

- **`MainActivity`** -- entry point. Loads `libsmithay_android.so` via
  `System.loadLibrary()`. Starts compositor thread via JNI. Listens for callbacks from
  Rust to create/destroy window Activities.
- **`SurfaceViewActivity`** -- one per Wayland toplevel. Uses `SurfaceView` for a
  dedicated SurfaceFlinger layer. On surface ready: calls JNI to give compositor direct
  access to ANativeWindow. Forwards touch/key events to Rust via JNI.
- **`Relay.kt`** -- `app_process` entry point (runs in Termux). Creates Wayland socket,
  passes fd to app, exits.

JNI interface (Kotlin -> Rust):
- `nativeStartCompositor(socketFd: Int)` -- start compositor event loop
- `nativeStopCompositor()`
- `nativeOnSurfaceCreated(windowId: Long, surface: Surface)`
- `nativeOnSurfaceChanged(windowId: Long, width: Int, height: Int)`
- `nativeOnSurfaceDestroyed(windowId: Long)`
- `nativeOnTouchEvent(windowId: Long, action: Int, x: Float, y: Float, pointerId: Int)`
- `nativeOnKeyEvent(windowId: Long, action: Int, keyCode: Int, scanCode: Int)`

JNI callbacks (Rust -> Kotlin):
- `requestNewActivity(windowId: Long, appId: String, title: String)`
- `requestCloseActivity(windowId: Long)`
- `requestResizeActivity(windowId: Long, w: Int, h: Int)`

### 2. Compositor Rust Library (`smithay-android/`)

The core compositor, running as a background thread in the Android app process.

**EGL backend** (`egl.rs`): Initialize EGL from Android Surface. Use
`EGLDisplay::from_raw()` to wrap an Android-created EGL display in Smithay's types,
or implement `EGLNativeDisplay` with `EGL_PLATFORM_ANDROID_KHR`. Create one EGLSurface
per Activity's ANativeWindow.

**Buffer import** (`import.rs`): Imports client AHardwareBuffers as GL textures.
Smithay has no built-in AHB support, so this is custom code:
1. `AHardwareBuffer_recvHandleFromUnixSocket()` on side-channel socket
2. `eglGetNativeClientBufferANDROID(ahb)` -> `EGLClientBuffer`
3. `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ...)` -> `EGLImage`
4. `glEGLImageTargetTexture2DOES(image)` -> GL texture for compositing
Requires `EGL_ANDROID_get_native_client_buffer` extension (widely available, must
runtime-check). Smithay's `GlesRenderer` can composite the resulting texture.

**Presentation** (`output.rs`): GlesRenderer composites all surfaces for a toplevel
onto the Activity's EGLSurface, then `eglSwapBuffers()`. One EGLSurface per Activity.

**Input** (`input.rs`): Implements Smithay's `InputBackend`. Android touch/key events
arrive via JNI -> crossbeam channel -> Smithay `wl_seat`. AKEYCODE -> Linux KEY_*
mapping + XKB keymap.

**Wayland protocols** (`compositor.rs`): Standard Smithay protocol handling:
- MVP: `wl_compositor`, `xdg_shell`, `wl_seat`, `wl_output`, `wp-viewporter`,
  `tawc_buffer_v1` (custom AHB buffer sharing protocol)
- Later: `wl_shm` (software rendering fallback), `xdg-decoration`,
  `wp-fractional-scale-v1`, `zwp_text_input_v3`, `wl_data_device_manager`,
  `xdg_popup` handling (composited onto parent surface, NOT separate Activities),
  Xwayland

### 3. Client WSI Layer (`tawc-wsi/`)

Installed in the Termux chroot. EGL wrapper is the primary component; Vulkan layer is
a stretch goal.

**EGL wrapper** (`libEGL.so`) -- primary path:
- Wrapper around stock EGL loaded via libhybris
- Intercepts Wayland platform calls, implements AHB-based buffer management
- Allocates AHardwareBuffer pool, creates EGLImages, wraps in FBOs
- On `eglSwapBuffers`: sends AHB via side-channel socket, commits via `tawc_buffer_v1`
- Passes through all GL/EGL rendering calls to stock driver
- First in `LD_LIBRARY_PATH` so apps find it before any system libEGL

**Vulkan implicit layer** (`tawc_wsi_vulkan.so` + manifest JSON) -- stretch goal:
- Intercepts WSI calls, passes through everything else
- Implements `VK_KHR_wayland_surface` and `VK_KHR_swapchain` on top of
  `VK_ANDROID_external_memory_android_hardware_buffer`
- Same AHB side-channel mechanism as EGL wrapper
- Activated automatically via implicit layer manifest
- **Depends on libhybris Vulkan maturity** -- compatibility varies by GPU vendor

### 4. app_process Relay (`app/src/.../Relay.kt`)

Lightweight Kotlin class run via `app_process` from Termux. Creates Wayland socket,
hands listening fd to compositor app via Binder, exits. Uses TermuxAm pattern (direct
`IActivityManager` calls via reflection) since `app_process` has no Android Context.

---

## Crate Structure

```
tawc-protocol/
+-- tawc_buffer_v1.xml     # Custom Wayland protocol for AHB buffer sharing

smithay-android/
+-- Cargo.toml
+-- src/
    +-- lib.rs              # JNI entry points
    +-- egl.rs              # Android EGL display/context/surface
    +-- import.rs           # AHB import (recv + eglGetNativeClientBufferANDROID)
    +-- output.rs           # eglSwapBuffers presentation
    +-- input.rs            # AndroidInputBackend
    +-- keymap.rs           # AKEYCODE -> Linux scancode mapping
    +-- compositor.rs       # Wayland state machine + tawc_buffer_v1 protocol
    +-- window.rs           # Per-toplevel state

tawc-wsi/
+-- egl-wrapper/
|   +-- egl_wrapper.c       # EGL wrapper using libhybris (or Rust + FFI)
+-- vulkan-layer/            # Stretch goal
    +-- tawc_wsi_layer.c     # Vulkan implicit layer
    +-- tawc_wsi_layer.json  # Layer manifest
```

Android app:
```
app/
+-- src/main/
    +-- java/com/tawc/
    |   +-- MainActivity.kt
    |   +-- SurfaceViewActivity.kt
    |   +-- NativeBridge.kt           # JNI extern declarations
    |   +-- RelayReceiver.kt          # Receives fd from relay
    |   +-- Relay.kt                  # app_process entry point
    +-- res/
+-- build.gradle.kts
```

Dependencies:
- `smithay` (`default-features = false`, features: `wayland_frontend`, `renderer_gl`)
  Avoids libdrm, libgbm, libinput, libudev, libseat, X11. **Requires patching
  `libEGL.so.1` -> `libEGL.so` for Android** (see notes.md).
- `ndk` -- AHardwareBuffer, ANativeWindow bindings
- `jni` -- JNI interop
- `crossbeam-channel` -- lock-free MPSC for input events
- `xkbcommon` -- keymap handling (requires cross-compiled libxkbcommon.so)
- `wayland-scanner` -- generate Rust bindings for `tawc_buffer_v1` custom protocol

---

## Build System

- Rust compositor cross-compiled for `aarch64-linux-android` via `cargo-ndk`
- Gradle invokes cargo-ndk, copies `.so` into `jniLibs/arm64-v8a/`
- Target API level: 29 (Android 10) minimum
- libxkbcommon cross-compiled from source with NDK toolchain
- wayland-rs uses pure Rust backend (no libwayland dependency)

Client-side WSI layer:
- Cross-compiled for `aarch64-linux-gnu` (glibc, for Termux chroot)
- Links against libhybris
- Installed in chroot's library path

---

## Implementation Order

### Phase 1: Build Toolchain & EGL Proof ✅ COMPLETE (2026-03-28)
1. ✅ Android app scaffold: single Activity with SurfaceView
2. ✅ Cross-compile toolchain: cargo-ndk, NDK, cross-compile libxkbcommon
3. ✅ Rust JNI library: receive ANativeWindow, create EGL context via Smithay
4. ✅ Render solid color to EGLSurface via GlesRenderer + `eglSwapBuffers`
5. ✅ **Milestone: GlesRenderer renders to Android Surface**

### Phase 2: libhybris + AHB Buffer Sharing Proof of Concept
6. Set up libhybris in Termux chroot, verify stock EGL/GLES loads via libhybris
7. Write minimal test: allocate AHardwareBuffer, create EGLImage, render to
   FBO, send AHB over Unix socket via `AHardwareBuffer_sendHandleToUnixSocket`
8. Write minimal compositor-side test: receive AHB, import via
   `eglGetNativeClientBufferANDROID` + `eglCreateImageKHR`, display as texture
9. Test buffer round-trip: client renders with stock driver via libhybris,
   compositor imports AHB and displays
10. **Milestone: zero-copy GPU buffer sharing proven end-to-end**

### Phase 3: Wayland Server + Socket Relay
11. Patch Smithay: `libEGL.so.1` -> `libEGL.so` for Android (compositor side --
    Smithay needs to load Android's system EGL in the app process; see notes.md)
12. Define `tawc_buffer_v1` Wayland protocol extension (AHB buffer lifecycle)
13. Initialize Smithay Wayland state (Display, compositor, xdg_shell, wl_output,
    tawc_buffer_v1)
14. Build `app_process` relay for listening socket handoff
15. Test: Termux client connects to `$XDG_RUNTIME_DIR/wayland-0`
16. Handle client buffer commit: receive AHB -> EGLImage -> GL texture ->
    composite -> present
17. Test with a simple Wayland EGL client from chroot
18. **Milestone: GPU-accelerated Wayland client visible on screen**

### Phase 4: Input
19. Implement AndroidInputBackend (touch + pointer first, keyboard second)
20. Wire Kotlin events -> JNI -> crossbeam channel -> Smithay wl_seat
21. AKEYCODE -> Linux scancode mapping + XKB keymap
22. **Milestone: can interact with a Wayland client**

### Phase 5: Multi-Window
23. JNI callback: compositor notifies Kotlin of new xdg_toplevels
24. MainActivity spawns SurfaceViewActivity per toplevel
25. Each Activity's SurfaceView gets its own EGLSurface
26. Window lifecycle (map, unmap, close, resize)
27. Popups (`xdg_popup`) composited onto parent Activity surface (NOT separate Activities)
28. **Milestone: multiple Wayland windows as separate Android Activities**

### Phase 6: Polish & Protocols
29. Frame callbacks (`wl_surface.frame`)
30. Server-side decorations (xdg-decoration)
31. Cursor handling (compositor-side cursor rendering)
32. Fractional scaling (wp-fractional-scale) for high-DPI Android screens
33. Clipboard bridge (wl_data_device <-> Android ClipboardManager)
34. IME bridge (zwp_text_input_v3 <-> Android InputMethodManager)
35. `wl_shm` support (software rendering fallback -- deliberately last so that
    absence of `wl_shm` serves as proof that AHB hardware path is actually working)

### Phase 7: Vulkan WSI (stretch goal)
36. Verify libhybris Vulkan loads stock GPU driver (may need unmerged PRs)
37. Write Vulkan implicit layer using `VK_ANDROID_external_memory_android_hardware_buffer`
38. Same AHB side-channel mechanism as EGL wrapper
39. Test with vkcube, vkmark
40. **Milestone: Vulkan apps work via libhybris + our WSI layer**

---

## Known Risks

| Risk | Mitigation |
|---|---|
| libhybris can't load stock EGL/GLES from chroot | EGL/GLES via libhybris is battle-tested (Sailfish OS, Ubuntu Touch). Test early in Phase 2. |
| libhybris Vulkan compatibility varies by vendor | Vulkan is a stretch goal. EGL/GLES covers most Linux desktop apps. android-vulkan-bridge project has proven Mali path. |
| Stock driver needs Binder/gralloc from chroot | Process UID/SELinux context is unchanged. Normal Android apps use gralloc under same SELinux domain. GPU device nodes are typically world-accessible (e.g. `/dev/kgsl-3d0` on Qualcomm, `/dev/mali0` on ARM Mali). Bind-mount `/vendor` and `/system`. |
| `eglGetNativeClientBufferANDROID` not available | Widely available on Android 8+ but is a driver extension, not guaranteed. Runtime-check required. No known alternative for AHB -> EGLImage import. |
| AHB side-channel socket adds complexity | Necessary because AHB serialization has its own wire format (`sendHandleToUnixSocket` uses multiple fds + metadata). Alternative: implement AHB serialization directly in the Wayland protocol layer (complex but eliminates side channel). |
| Custom Wayland protocol (`tawc_buffer_v1`) breaks standard clients | Clients must use our WSI layer anyway (stock driver via libhybris). The WSI layer handles the protocol. Standard `wl_shm` provides software rendering fallback. |
| SELinux blocks cross-app Wayland socket | `app_process` relay with fd handoff (proven by wlroots-android-bridge, Termux:X11). |
| Smithay has never been built for Android | `default-features = false` avoids Linux deps. wayland-rs pure Rust backend. `EGLDisplay::from_raw()` avoids GBM. Must patch Smithay's EGL loader to find Android's system `libEGL.so` (it hardcodes `libEGL.so.1`). Test early. |
| Hidden API reflection in relay breaks across Android versions | TermuxAm pattern has worked through Android 15. Monitor for breakage. |
| Phantom Process Killer (Android 12+) | Relay exits after fd handoff. Users need Developer Options toggle for other Termux processes. |
| Vendor-specific GPU quirks | Architecture is vendor-neutral -- works for any device where libhybris can load the stock GPU driver (Adreno, Mali, PowerVR, etc.). Each vendor needs testing for driver-specific quirks. `wl_shm` fallback for unsupported devices. |
| Freeform windowing not universal | On phones, each Activity is fullscreen (switch via recents). True freeform on Samsung DeX, ChromeOS, Android 15+ desktop mode. |

---

## References

- [libhybris/libhybris](https://github.com/libhybris/libhybris) -- bionic compatibility layer
- [Smithay](https://github.com/Smithay/smithay) -- Rust Wayland compositor library
- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge)
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [Vulkan Layer mechanism](https://vulkan.lunarg.com/doc/view/latest/linux/loader_and_layer_interface.html)
- [ndk crate](https://crates.io/crates/ndk) -- Rust NDK bindings
