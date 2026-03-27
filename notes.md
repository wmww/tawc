# Notes

This file contains design, architecture and implementation notes, primarily written by and for LLM agents.

## Research Findings (2026-03-27)

### 1. Unix Domain Socket Access Between Android Apps (Critical)

**Summary: Cross-app Unix socket communication is blocked by SELinux on Android 9+ (API 28+). There is no reliable workaround for connecting from an arbitrary third-party app like Termux to another app's socket.**

**Detailed findings:**

- **SELinux is the primary barrier, not filesystem permissions.** On Android 9+, each app gets its own SELinux domain. The AOSP `app_neverallows.te` and `untrusted_app_all.te` policies do NOT grant `connectto` permission between `untrusted_app` domains for `unix_stream_socket`. SELinux uses a whitelist model: anything not explicitly allowed is denied. The only `connectto` grant for `untrusted_app_all` is to `runas_app` (for Android Studio debugging).

- **Abstract namespace sockets are also blocked by SELinux.** Although abstract sockets bypass filesystem DAC permissions (no file node exists), SELinux MAC checks still apply to abstract socket operations. Two different `untrusted_app` processes cannot connect via abstract sockets.

- **Filesystem sockets face both DAC and MAC barriers.** `/data/data/<package>/` is mode 0700 per-app; other apps cannot traverse the directory. Shared locations like `/sdcard/` do not support Unix sockets (FAT/FUSE filesystems). `/data/local/tmp/` is only writable by shell/adb.

- **The NDK issue #1469 confirms this.** A developer found cross-app UDS worked only with `targetSdkVersion <= 27` (Android 8). Setting `targetSdkVersion >= 28` caused `avc: denied { connectto }` SELinux errors. The suggested workaround was to use ContentProviders instead.

- **How Termux solves this internally:** Termux and its plugins (Termux:API, Termux:X11) all declare `sharedUserId="com.termux"` in their manifests, which means they run under the **same Linux UID**. This bypasses both DAC and SELinux inter-app restrictions because they are effectively the same "app" from the kernel's perspective. The `termux-am-socket` uses a filesystem socket at `/data/data/com.termux/files/apps/termux-app/termux-am/am.sock` -- accessible because the client binary runs under the same UID.

- **Termux:X11 Wayland approach:** Termux:X11 creates its Wayland socket in `$TMPDIR` (under Termux's data directory) and shares it with the compositor via the shared UID. This only works because Termux:X11 is a Termux plugin with the same `sharedUserId`.

- **Implications for tawc:** If tawc is a standalone app (different package, different UID from Termux), a Wayland client running in Termux **cannot** connect to tawc's Wayland socket via Unix domain sockets on Android 9+. Possible approaches:
  1. **Make tawc a Termux plugin** with `sharedUserId="com.termux"` -- requires signing with the Termux key, which is impractical for a third-party app.
  2. **Use a ContentProvider or bound Service** to pass file descriptors (the socket pair approach from Termux issue #2654). The compositor would create a `socketpair()`, pass one end to the client via `ContentProvider.openFile()` or a bound `Service`. This avoids the filesystem/SELinux socket restrictions entirely.
  3. **Use TCP sockets on localhost** -- works but loses the ability to pass file descriptors (needed for buffer sharing).
  4. **Require the user to set SELinux to permissive** (via root/Magisk) -- not a realistic requirement for general use.

### 2. Smithay API Findings

- **`Allocator` trait:** Matches plan's sketch exactly. `create_buffer(width, height, fourcc, modifiers)`.
- **`GlesRenderer`:** Requires Smithay's `EGLContext` wrapper (not raw EGL). Must go through
  `EGLDisplay` → `EGLContext` → `GlesRenderer`.
- **`EGLDisplay::from_raw(display, config_id)`:** Escape hatch that accepts pre-initialized
  raw EGL handles. No GBM assumption — we create Android EGL display ourselves, then wrap it.
  `from_raw` skips `eglTerminate` on drop (we manage lifetime externally).
- **`InputBackend`:** ~25 associated types. Use `UnusedEvent` for unsupported event categories.
- **Buffer import:** `ImportDma` already implemented in `GlesRenderer`. AHardwareBuffer can
  export to dmabuf fd on Android 10+, potentially leveraging existing code.
- **Wayland socket:** `ListeningSocketSource::with_name(name)` uses `$XDG_RUNTIME_DIR`. Can
  also use `DisplayHandle::insert_client(unix_stream, data)` to add clients from custom sockets.
- **No existing Android support** in Smithay (greenfield).

### 3. DRM ↔ AHardwareBuffer Format Mapping

DRM fourcc names are MSB-to-LSB channel order. AHB format names are memory byte order.
On little-endian: `DRM_FORMAT_ARGB8888` = BGRA in memory ≠ `R8G8B8A8_UNORM` = RGBA.

Correct mappings:
- `DRM_FORMAT_ABGR8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` (RGBA)
- `DRM_FORMAT_XBGR8888` → `AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM` (RGBX)
- `DRM_FORMAT_RGB565`   → `AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM`
- `DRM_FORMAT_ARGB8888` → `HAL_PIXEL_FORMAT_BGRA_8888` (value 5, if vendor supports)

### 4. AHardwareBuffer EGL Import (eglGetNativeClientBufferANDROID)

**Summary: It is an EGL extension, not an NDK API-level-gated function. It must be queried at runtime and is NOT universally guaranteed on Android 10+.**

- `eglGetNativeClientBufferANDROID` is part of the `EGL_ANDROID_get_native_client_buffer` extension. It converts an `AHardwareBuffer` into an `EGLClientBuffer` for use with `eglCreateImageKHR`.
- It requires EGL 1.2, `EGL_KHR_image_base`, and `EGL_ANDROID_image_native_buffer`.
- It is a **driver-provided EGL extension**, not a platform API. Its availability depends on the GPU vendor's EGL driver, not the Android API level. You must:
  1. Check `eglQueryString(display, EGL_EXTENSIONS)` for `"EGL_ANDROID_get_native_client_buffer"`
  2. Obtain the function pointer via `eglGetProcAddress("eglGetNativeClientBufferANDROID")`
  3. Verify the pointer is non-NULL (there are historical Android bugs where extensions are reported but `eglGetProcAddress` returns NULL)
- **In practice**, virtually all Android 8+ (API 26+) devices with Qualcomm, ARM Mali, or PowerVR GPUs support this extension, since `AHardwareBuffer` itself was introduced at API 26 and the ecosystem adopted it widely. But it is not a guarantee -- always check at runtime.

### 5. ASurfaceTransaction_setBuffer and AHardwareBuffer

**Summary: `ASurfaceTransaction_setBuffer` has accepted `AHardwareBuffer` since its introduction at API 29. There was no API 34 signature change.**

- The function signature since API 29 is:
  ```c
  void ASurfaceTransaction_setBuffer(
      ASurfaceTransaction *transaction,
      ASurfaceControl *surface_control,
      AHardwareBuffer *buffer,
      int acquire_fence_fd
  );
  ```
- This function has always taken `AHardwareBuffer*` directly. There is no overload or version that takes `ANativeWindow_Buffer` (which is a completely different type used with `ANativeWindow_lock()`).
- At API 36, a new variant `ASurfaceTransaction_setBufferWithRelease` was added (includes a release callback), but the original function signature is unchanged.
- The "API 34 claim" from the plan files appears to be incorrect. The function is available from API 29 (Android 10).

### 6. EGL Context Thread Affinity

**Summary: An EGL context CAN be moved between threads, but it should be done rarely. One context CAN render to multiple surfaces sequentially on one thread.**

- **Moving contexts between threads:** Per the EGL spec, you CANNOT call `eglMakeCurrent` with a context that is currently bound to another thread. You must first release it on the old thread (`eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)`) and then bind it on the new thread. This is legal but expensive -- it forces a full pipeline flush.
- **One thread, multiple surfaces:** A single thread can render to multiple `EGLSurface`s (backed by different `ANativeWindow`s) by calling `eglMakeCurrent` to switch the current surface. This is the standard approach. Each switch flushes outstanding GL commands.
- **Performance guidance (from Imagination Technologies / ARM):**
  - Frequent `eglMakeCurrent` calls are costly due to mandatory pipeline flushes.
  - The worst pattern is bouncing one context between threads repeatedly.
  - For multi-window rendering, the recommended approach is one context per thread, with shared GL objects via `eglCreateContext` with a shared context parameter.
  - For a compositor rendering to N windows from one thread: create one EGL context, create N `EGLSurface`s, and `eglMakeCurrent` + render + `eglSwapBuffers` for each surface sequentially. This works but each surface switch has overhead.
- **Practical recommendation for tawc:** Use a single dedicated render thread with one EGL context. Switch surfaces to render to each Activity's window. If performance is a concern with many windows, consider using `AHardwareBuffer` + `ASurfaceTransaction` instead of EGL surfaces for composition (avoids the `eglMakeCurrent` overhead entirely).

### 7. Multiple Activities with Independent Rendering

**Summary: Launching N Activities from one app, each with its own SurfaceView, is architecturally viable but has lifecycle complexity.**

- **Lifecycle challenges:** Each Activity has its own lifecycle, and each SurfaceView has a semi-independent surface lifecycle. The surface may be created after `onResume()` or destroyed independently of the Activity. When the power button is pressed, `onPause()` fires but the surface may remain alive. Starting/stopping activities quickly can cause `surfaceCreated()` to fire after `onPause()`.
- **Same-process Activities:** By default, all Activities in one app run in the same process (unless `android:process` is specified). They share the same heap, static state, and threads. This is actually advantageous for a compositor -- a single render thread can access all surfaces.
- **Multiple SurfaceViews:** Having multiple SurfaceViews in the same Activity is problematic (Z-ordering issues, undefined overlap behavior). But having one SurfaceView per Activity (each Activity in its own window) avoids this entirely -- each Activity gets its own window in the window manager.
- **Rendering from a single background thread:** This is the recommended pattern. The render thread should:
  1. Maintain a list of active surfaces (added on `surfaceCreated`, removed on `surfaceDestroyed`)
  2. For each frame, iterate through active surfaces and render to each one
  3. Use `eglMakeCurrent` to switch between surfaces, or use `ASurfaceTransaction` + `AHardwareBuffer` for buffer submission
- **Practical issues:**
  - Activity launch/destroy creates visual transitions (animations) that may not be desirable for a compositor. Consider using `overridePendingTransition(0, 0)` or window flags to suppress animations.
  - Each Activity occupies a slot in the recent apps / task stack. Use `android:excludeFromRecents="true"` and task affinity management.
  - Activities may be killed by the system under memory pressure. A compositor should handle surface loss gracefully.
  - On foldables and multi-display devices, Activities may be placed on different displays, which can complicate rendering (different EGL configs, refresh rates).
  - An alternative to multiple Activities is a single Activity with multiple `SurfaceView`s managed via `WindowManager.addView()` or using `Presentation` for multi-display.

### 8. Cross-Compiling Smithay/wayland-rs for Android (aarch64-linux-android)

#### 8a. wayland-rs / wayland-backend: Pure Rust Backend Available

**Summary: wayland-rs provides a pure Rust Wayland protocol implementation. No libwayland-server.so is required.**

- The `wayland-backend` crate (used by `wayland-server`) provides two backends:
  1. **Rust backend** (`rs` module): Pure Rust implementation of the Wayland protocol. Always available.
  2. **System backend** (`sys` module): FFI bindings to the C libwayland libraries. Only enabled via cargo features `client_system` / `server_system`.
- The default backend is the Rust one unless a `*_system` feature is enabled. This has been the case since wayland-rs 0.21.
- The `dlopen` feature allows the system backend to dynamically load libwayland at runtime instead of linking at compile time.
- **For Android cross-compilation:** Simply do NOT enable `server_system` (or `client_system`). The pure Rust backend will be used, eliminating the libwayland dependency entirely. This is the key enabler for Android support.
- **Caveat from upstream:** The original wayland-rs 0.21 announcement noted that "abandoning the C wayland libraries is really cutting oneself from many interactions" -- specifically OpenGL/EGL integration that expects C wayland pointers. However, for a compositor (server side), this is less of a concern since we control the EGL setup ourselves.
- **EWC project** (github.com/MaxVerevkin/ewc) demonstrates a full Wayland compositor built with zero libwayland dependency using the pure Rust backend.

#### 8b. xkbcommon: C Library Required (but Pure Rust Alternative Exists)

**Summary: The `xkbcommon` crate (v0.9.0, used by Smithay) requires linking to the C `libxkbcommon.so`. A pure Rust alternative exists but is not yet integrated with Smithay.**

- The `xkbcommon` crate (from rust-x-bindings) is FFI bindings to the C library. Its `src/xkb/ffi.rs` contains `#[link(name = "xkbcommon")] extern "C" { ... }` -- it directly links to `libxkbcommon.so`.
- **For Android:** libxkbcommon is not part of the NDK. It would need to be cross-compiled from source for `aarch64-linux-android` and provided to the linker. This is doable but adds build complexity.
- **Pure Rust alternative:** The `xkbcommon-rs` crate (by wysiwys, github.com/wysiwys/xkbcommon-rs) is a pure Rust port of libxkbcommon 1.7.0 with zero C dependencies. It provides Send+Sync types and claims compositor-side functionality.
  - Status as of early 2025: v0.1.2, actively developed, "strives to be as close a reimplementation as possible" but acknowledges "some features are not implemented yet."
  - Smithay does NOT currently use this crate -- it would require patching Smithay's keyboard handling to use the pure Rust API instead.
  - Alternatively, we could cross-compile the C libxkbcommon for Android (it has minimal dependencies -- just needs a C compiler and optionally wayland-scanner for protocol headers).

#### 8c. Other Smithay Native Dependencies

**Summary: Smithay has many optional C library dependencies, but most are irrelevant for Android. A minimal feature set can avoid nearly all of them.**

Smithay's feature-gated native dependencies:

| Feature | C Library | Needed for Android? |
|---|---|---|
| `backend_drm` | libdrm | No -- Android uses SurfaceFlinger/ASurfaceTransaction, not DRM/KMS |
| `backend_gbm` | libgbm (Mesa) | No -- Android uses AHardwareBuffer, not GBM |
| `backend_libinput` | libinput | No -- Android has its own input system (InputManager) |
| `backend_udev` | libudev | No -- Android does not use udev |
| `backend_session_libseat` | libseat | No -- Android has no seat management |
| `backend_x11` | libX11/xcb | No |
| `backend_winit` | (Rust crate) | Possibly for dev/testing only |
| `renderer_gl` | EGL/GLESv2 | Yes -- but these are provided by Android NDK |
| `renderer_pixman` | libpixman | Optional -- would need cross-compilation |
| `backend_vulkan` | Vulkan loader | Yes -- provided by Android |
| `xwayland` | Xwayland binary | No |
| (always) | libxkbcommon | Yes -- see section 8b |
| (always) | libc | Yes -- provided by Android NDK bionic |

**Minimal viable Smithay feature set for Android:**
```toml
smithay = {
    version = "0.7",
    default-features = false,
    features = ["wayland_frontend", "renderer_gl"]
}
```

This avoids all Linux-specific backends (DRM, GBM, libinput, udev, libseat) and only requires:
- EGL/GLESv2 (provided by Android)
- libxkbcommon (must be cross-compiled or replaced with pure Rust alternative)
- The wayland protocol layer (pure Rust backend, no libwayland needed)

#### 8d. Known Issues and Risks

- **No existing Android support in Smithay.** This is greenfield work. No one has publicly reported cross-compiling Smithay for Android. Expect undiscovered issues.
- **EGL integration with Rust backend:** The wayland-rs Rust backend does not provide C-compatible `wl_display*` pointers. Some EGL/Mesa interactions on Linux expect these. On Android this is less of a concern since we use Android's EGL directly, not Mesa.
- **Smithay's EGL module:** `smithay::backend::egl` wraps EGL using `khronos-egl` and expects to create its own EGL display. On Android, we may need `EGLDisplay::from_raw()` to wrap an Android-created display, or create one from an `ANativeWindow`.
- **Build system:** Cross-compiling requires the Android NDK toolchain. The `cc` crate and `pkg-config` crate need to be configured to find NDK sysroot headers and libraries. `PKG_CONFIG_SYSROOT_DIR` and `PKG_CONFIG_PATH` must point to cross-compiled dependency prefixes.

### 9. app_process Relay: Binder Flow and Context Problem (2026-03-27)

**Summary: `app_process` does NOT provide an Android `Context`. The relay cannot call `bindService()`. The Binder flow must be reversed from the original plan.**

- When `app_process` is launched from Termux, it runs under **Termux's UID** (not tawc's). It inherits Termux's SELinux context (`untrusted_app`). It CAN access Termux's filesystem and CAN use Binder IPC — but it does NOT have an Android `Context`.

- **How wlroots-android-bridge solves this:** The `app_process` side creates its own Binder stub objects and passes them TO the Android app by launching an Activity via direct `IActivityManager.startActivityAsUser()` Binder calls (reflection, bypassing Context). The Binder objects are embedded in the Intent's Bundle. The Android app then calls back to the `app_process` via those received Binders.

- **How Termux:X11 solves this:** Uses `sun.misc.Unsafe` to allocate an `ActivityThread` without calling its constructor, then calls `getSystemContext()` on it. This gives a limited system Context. Falls back to direct `IActivityManager` calls via reflection when the Context approach fails.

- **Recommended approach for tawc:** Use the TermuxAm/wlroots-android-bridge pattern:
  1. Relay creates the listening socket at `$XDG_RUNTIME_DIR/wayland-0`
  2. Relay wraps the socket fd in a `ParcelFileDescriptor` inside a Binder-compatible object
  3. Relay sends the Binder to tawc's app via `IActivityManager.startActivity()` (reflection)
  4. Tawc's Activity/BroadcastReceiver extracts the fd and passes it to the compositor via JNI
  5. Relay exits

- **`ParcelFileDescriptor` can transfer any fd type** including listening sockets. Binder's kernel-level fd passing (`BINDER_TYPE_FD`) is type-agnostic — it uses `fget()`/`task_fd_install()` which work on any file descriptor.

### 10. Phantom Process Killer (Android 12+) (2026-03-27)

Android 12 introduced `PhantomProcessKiller` which kills child processes of apps when more than 32 exist. An `app_process` launched from Termux counts as a phantom process.

- **Android 12-13:** Workaround: `adb shell device_config put activity_manager max_phantom_processes 2147483647`
- **Android 14+:** Developer Options toggle "Disable child process restrictions"
- **Android 15 (OnePlus):** Reports of aggressive process killing (termux/termux-app#4219)

Since tawc's relay exits after fd handoff, it only needs to survive briefly. But users should be aware of this for other Termux processes.

### 11. Frame Scheduling: AChoreographer NDK API (2026-03-27)

Android NDK provides `AChoreographer` for vsync callbacks from native code:
- **API 24:** `AChoreographer_getInstance()` + `AChoreographer_postFrameCallback()` (deprecated — timestamp overflow bug)
- **API 29:** `AChoreographer_postFrameCallback64()` — fixed version, use this
- **API 30:** `AChoreographer_registerRefreshRateCallback()` — notifies of refresh rate changes
- **API 33:** `AChoreographer_postVsyncCallback()` with `AChoreographerFrameCallbackData` — multi-timeline frame pacing

**Requirement:** AChoreographer needs a thread with an `ALooper`. The compositor thread must call `ALooper_prepare()` before using AChoreographer.

### 12. Rendering Architecture Decision (2026-03-27, revised)

**Decision: GPU composition pass via GlesRenderer + eglSwapBuffers.**

The compositor imports client dmabuf buffers as GL textures, composites all surfaces
(toplevel + popups + subsurfaces) with GlesRenderer onto an EGLSurface backed by the
Activity's ANativeWindow, and calls eglSwapBuffers. One GPU composition pass per frame,
no CPU buffer copies. Standard Wayland compositor architecture.

**Why not ASurfaceControl per Wayland surface (zero-copy to SurfaceFlinger)?**
This was investigated and is theoretically possible but impractical:
- Requires converting client dmabuf fds to AHardwareBuffers, which has no clean public
  NDK API (see §15)
- The GPU composition pass is fast (sub-millisecond for typical surface counts) and the
  architecture is much simpler
- Popups/subsurfaces are naturally handled by the compositor's render pass without
  needing separate Android surface management
- See §15 for detailed analysis of the dmabuf→AHB problem

The ASurfaceControl API details are preserved below for reference in case this is
revisited in the future.

<details>
<summary>ASurfaceControl API reference (for future zero-copy investigation)</summary>

**ASurfaceControl API (API 29+):**
- `ASurfaceControl_createFromWindow(ANativeWindow*, name)` — child from SurfaceView's window
- `ASurfaceControl_create(ASurfaceControl*, name)` — child from existing SC
- `ASurfaceTransaction_setBuffer(txn, sc, AHardwareBuffer*, fence_fd)` — submit buffer directly
- `ASurfaceTransaction_setPosition(txn, sc, x, y)` — position relative to parent
- `ASurfaceTransaction_setZOrder(txn, sc, z)` — stacking order among siblings
- `ASurfaceTransaction_apply(txn)` — atomic batch commit

**Java vs NDK API:** The Java `SurfaceControl` API (API 29+) is a superset of the NDK API.
`SurfaceControl.Transaction.reparent(sc, newParent)` is Java-only.

**Parent clipping:** Child ASurfaceControls are clipped to parent bounds. Popups extending
beyond the toplevel would be clipped. Solution: flat sibling hierarchy under root SC.

**Layer limits:** ~4 HW overlay planes, GPU composition fallback beyond that. 10+ layers fine.
</details>

### 13. Freeform Windowing Availability (2026-03-27)

The "one Activity per toplevel" design gives full multi-window only on devices with freeform windowing:
- Samsung DeX
- ChromeOS (Android apps)
- Android 15+ desktop mode (when connected to external display)
- Some custom ROMs

On standard phones/tablets, each Activity is fullscreen. The user experience becomes "one Wayland window visible at a time, switch via Android recents." This is still useful but should be documented.

### 14. Smithay API Verification (2026-03-27)

Confirmed against Smithay 0.7.0 (published 2025-06-24):

- **`EGLDisplay::from_raw(display, config_id)`** — exists at `src/backend/egl/display.rs:335`. Takes raw `*const c_void` for EGLDisplay and EGLConfig. Skips `eglTerminate` on drop.
- **`DisplayHandle::insert_client(stream, data)`** — exists in `wayland-server`. Takes `UnixStream` + `Arc<dyn ClientData>`.
- **`UnusedEvent`** — exists at `src/backend/input/mod.rs:~85`. Uninhabited enum implementing all event traits.
- **`InputBackend`** — requires exactly 25 associated types (24 event types + Device).
- **`GlesRenderer`** — implements `ImportDma`, `ImportDmaWl`, `ImportMem`, `ImportMemWl`, `ImportEgl`, `ExportMem`, `Bind`, `Blit`, `Offscreen`.
- **Features** `wayland_frontend` and `renderer_gl` exist. `renderer_gl` pulls in `backend_egl` automatically.

### 15. dmabuf fd to AHardwareBuffer: The Critical Buffer Import Question (2026-03-27)

**Summary: There is NO public NDK API to create an AHardwareBuffer from a dmabuf fd. The viable path is compositor-allocated AHardwareBuffers with the `AHardwareBuffer_sendHandleToUnixSocket` / `recvHandleFromUnixSocket` NDK functions, combined with the zwp_linux_dmabuf_v1 feedback mechanism to guide client allocation. A Vulkan cross-handle-type conversion path is theoretically possible but practically unreliable on Android.**

#### 15a. No Public NDK API for dmabuf Import

The complete AHardwareBuffer NDK API (as of API 36) includes: `allocate`, `acquire`, `release`, `describe`, `lock`, `lockAndGetInfo`, `lockPlanes`, `unlock`, `isSupported`, `getId`, `sendHandleToUnixSocket`, `recvHandleFromUnixSocket`, `toHardwareBuffer`, `fromHardwareBuffer`, `writeToParcel`, `readFromParcel`. **None of these accept a dmabuf fd or native_handle_t as input to create an AHardwareBuffer.**

There ARE two non-public (VNDK) functions in `frameworks/native/libs/nativewindow/`:
- `AHardwareBuffer_getNativeHandle(const AHardwareBuffer*)` -- returns the internal `native_handle_t*` (which contains the dmabuf fd at `handle->data[0]`). Available in VNDK since Android 8.1.
- `AHardwareBuffer_createFromHandle(...)` -- exists in AOSP source (`AHardwareBuffer.cpp`), creates an AHardwareBuffer from a `native_handle_t`. This is the function that `recvHandleFromUnixSocket` internally calls after deserializing.

**These are VNDK-only, not public NDK.** Using them from an app requires either:
1. `dlopen("libnativewindow.so")` + `dlsym()` (fragile, may break across Android versions, blocked by linker namespace restrictions on Android 7+)
2. Rebuilding the app as a vendor/system process (impractical)

**Verdict: Not a viable path for a third-party app.**

#### 15b. Vulkan Path: dmabuf fd -> VkDeviceMemory -> AHardwareBuffer (NOT VIABLE)

The theoretical chain:
1. Import dmabuf fd via `VK_EXT_external_memory_dma_buf` (using `VkImportMemoryFdInfoKHR` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`)
2. Export the same `VkDeviceMemory` as AHardwareBuffer via `VK_ANDROID_external_memory_android_hardware_buffer` (using `vkGetMemoryAndroidHardwareBufferANDROID`)

**Why this does NOT work:**

1. **`VK_EXT_external_memory_dma_buf` is NOT available on stock Android GPU drivers.** The Android Vulkan Profile 2025 (AVP 2025) requires `VK_KHR_external_memory_fd` (opaque fd) and `VK_ANDROID_external_memory_android_hardware_buffer`, but does NOT require `VK_EXT_external_memory_dma_buf`. Stock Qualcomm and ARM Mali drivers on Android do not expose this extension. Only Mesa drivers (Turnip, Freedreno) do, and only with patches.

2. **Cross-handle-type export is not guaranteed.** The Vulkan spec's `compatibleHandleTypes` and `exportFromImportedHandleTypes` bitmasks in `VkExternalMemoryProperties` determine what combinations are valid. Importing as DMA_BUF and exporting as ANDROID_HARDWARE_BUFFER requires both bits to appear in `compatibleHandleTypes` for the allocation. There is no spec requirement that drivers support this combination, and stock Android drivers almost certainly do not.

3. **Even `VK_KHR_external_memory_fd` with OPAQUE_FD won't help.** OPAQUE_FD is NOT the same as DMA_BUF. A dmabuf fd from a Wayland client is `DMA_BUF_BIT_EXT`, not `OPAQUE_FD_BIT`. You cannot import a dmabuf fd using `OPAQUE_FD_BIT` -- the fd types are incompatible.

**Verdict: Not viable on stock Android. Would only work with Mesa drivers (Turnip) in very specific configurations, and even then the cross-handle-type export is not guaranteed.**

#### 15c. EGL Path: dmabuf fd -> EGLImage -> ??? (REQUIRES GPU COPY)

`EGL_EXT_image_dma_buf_import` can import a dmabuf fd as an EGLImage. This works on Android when using Mesa drivers (e.g., Turnip/Freedreno in a chroot). However:

- There is NO API to go from EGLImage to AHardwareBuffer. The path is one-way: AHardwareBuffer -> EGLClientBuffer -> EGLImage (via `eglGetNativeClientBufferANDROID`). There is no reverse.
- To get the content to SurfaceFlinger, you would need to: create an AHardwareBuffer, bind it as a GL renderbuffer/texture via EGLImage, then `glBlitFramebuffer` or texture-blit from the dmabuf EGLImage to the AHB EGLImage. This is a **GPU copy**, not zero-copy.
- Stock Android EGL drivers (Qualcomm, Mali) likely do NOT support `EGL_EXT_image_dma_buf_import` at all -- this extension is Mesa-specific.

**Verdict: Not zero-copy. Requires a GPU blit. Also driver-dependent.**

#### 15d. Compositor-Allocated AHardwareBuffers (THE VIABLE PATH)

**This is the correct approach.** Instead of importing client-allocated dmabufs, the compositor allocates AHardwareBuffers and makes them available to clients for rendering.

**How it works with zwp_linux_dmabuf_v1:**

1. **Compositor allocates AHardwareBuffers** using `AHardwareBuffer_allocate()` with GPU_SAMPLED_IMAGE and GPU_COLOR_OUTPUT usage flags.

2. **Compositor extracts dmabuf fds** from the AHardwareBuffers. This can be done via:
   - Vulkan: Import AHB via `VK_ANDROID_external_memory_android_hardware_buffer`, then export as opaque fd via `VK_KHR_external_memory_fd`. (Both extensions are in AVP 2025.)
   - The VNDK `AHardwareBuffer_getNativeHandle()` function (risky, non-public).
   - EGL: `eglGetNativeClientBufferANDROID` -> `eglCreateImageKHR` -> `eglExportDMABUFImageMESA` (Mesa-only).

3. **Compositor advertises supported formats/modifiers** via the `zwp_linux_dmabuf_feedback_v1` interface. The feedback includes the device node and format+modifier pairs. Clients use this to select compatible allocation parameters.

4. **Client allocates buffers using the compositor's advertised parameters.** In the standard Wayland dmabuf protocol, the **client always allocates**. The compositor cannot send pre-allocated buffers to clients via `zwp_linux_dmabuf_v1`.

5. **The key insight:** If the client runs in a chroot/proot with Mesa (Turnip/Freedreno), and the compositor advertises the correct device and format/modifier pairs that match AHardwareBuffer capabilities, the client's Mesa driver will allocate buffers that are compatible with the host's gralloc. Both use the same underlying kernel GPU driver and the same dma-buf heap. The dmabuf fds the client sends will reference buffers that can be wrapped as AHardwareBuffers.

**The problem: wrapping a client-allocated dmabuf as AHardwareBuffer still requires `AHardwareBuffer_createFromHandle` (VNDK-only) or `AHardwareBuffer_recvHandleFromUnixSocket` (requires the AHB serialization format).**

#### 15e. AHardwareBuffer_sendHandleToUnixSocket Path (MOST PROMISING)

These NDK functions (available since API 26) serialize/deserialize AHardwareBuffers over Unix domain sockets:
- `AHardwareBuffer_sendHandleToUnixSocket(buffer, sockfd)` -- flattens the GraphicBuffer and sends it via `sendmsg()` with `SCM_RIGHTS` for the dmabuf fds
- `AHardwareBuffer_recvHandleFromUnixSocket(sockfd, &buffer)` -- receives and reconstructs the AHardwareBuffer

**Internally, these use `sendmsg`/`recvmsg` with `SCM_RIGHTS` to pass the native_handle's file descriptors.** The serialization format includes the buffer metadata (width, height, format, stride, etc.) plus the dmabuf fds.

**Two potential approaches:**

**Approach A: Custom Wayland protocol extension (android_buffer)**
- Define a custom Wayland protocol where the compositor sends serialized AHardwareBuffer handles to clients
- Client receives them, maps them as render targets, renders, and commits
- Problem: Standard Wayland clients (GTK, Qt) would not understand this protocol. Would require custom client-side code or a client-side Wayland protocol adapter library.

**Approach B: Compositor as AHB allocator with dmabuf round-trip**
- Compositor allocates AHardwareBuffer
- Compositor extracts the dmabuf fd (via Vulkan AHB->opaque_fd export)
- Compositor makes the fd available to clients (via the standard zwp_linux_dmabuf_v1 protocol or shared memory)
- Client renders into the dmabuf
- Client commits via wl_surface.attach + commit
- Compositor receives the dmabuf fd back, but now needs to reconstruct the AHardwareBuffer
- Since the compositor originally allocated the AHB, it still holds a reference -- it just maps the incoming dmabuf fd back to its original AHB

**Approach B is the winner.** The compositor maintains a mapping of dmabuf fd -> AHardwareBuffer. When a client commits a buffer, the compositor matches the incoming dmabuf fd to its original AHardwareBuffer (by comparing fd identity via `kcmp(2)` or by maintaining a lookup table keyed on the dmabuf global ID from `/proc/self/fdinfo`).

#### 15f. What wlroots-android-bridge Actually Does

Based on source code analysis (as of 2026-03):

- The project is in active development/restructuring. The `master` branch has minimal C++ code (stub JNI functions in `labwc.cpp`).
- The README describes the intended architecture: a `wlr_allocator` implementation backed by AHardwareBuffer, with each client's surface rendered onto a compositor-allocated AHB using wlroots' GLES2 renderer, then submitted to SurfaceFlinger via `ASurfaceTransaction_setBuffer`.
- It uses `cros_gralloc_handle.h` from Intel's minigbm to extract buffer metadata (format, stride, planes) from AHardwareBuffer's native_handle. **This is vendor-specific** -- `cros_gralloc_handle` is the handle format used by minigbm/ChromeOS gralloc, NOT by Qualcomm or ARM Mali gralloc.
- **This is why it fails on ARM/Qualcomm:** Stock Android devices use vendor-specific gralloc implementations (Qualcomm's `msm_gralloc_handle`, ARM's gralloc). The `cros_gralloc_handle` struct layout does not match these. Casting a Qualcomm gralloc handle as `cros_gralloc_handle` reads garbage values for format/stride/planes.
- The project was tested with Mesa iris driver (Intel GPU), which uses minigbm gralloc. It does not work with vendor GPU drivers.
- The approach of rendering client textures onto compositor-allocated AHBs is sound -- it just needs a vendor-agnostic way to extract buffer metadata, which Gralloc 4's `IMapper::get()` HIDL/AIDL interface provides (but that requires Java/AIDL, not raw native_handle parsing).

#### 15g. Client GPU Drivers in proot/chroot on Android

**Qualcomm (Adreno) -- Best Support:**
- **Turnip** (Mesa Vulkan driver) can run in proot/chroot, accessing the GPU via `/dev/kgsl-3d0` (Qualcomm's kernel-mode GPU driver interface). No root required for GPU access.
- **Freedreno** (Mesa OpenGL driver) can also work but is less mature.
- Turnip + Zink provides OpenGL-over-Vulkan translation.
- The `tu_kgsl_export_dmabuf` patch (now **upstreamed in Mesa**) enables Turnip to export dmabuf file descriptors. This is critical for buffer sharing.
- DRI3 protocol support allows X11 clients to share buffers via dmabuf fds with the display server (Termux:X11).

**ARM Mali -- Poor Support:**
- Mali has no open-source kernel driver for Android (Panfrost/PanVK target the mainline Linux kernel driver, not Android's proprietary Mali kernel driver).
- No Mesa driver can directly talk to the proprietary Mali kernel interface on stock Android.
- Only VirGL (software rendering proxied through a host renderer) works, which is slow and does not produce dmabuf fds usable by the host.

**MediaTek, Samsung Exynos, etc. -- No Support:**
- Similar to Mali -- proprietary kernel drivers with no Mesa support.
- VirGL is the only option.

**dmabuf compatibility:**
When Mesa Turnip runs in a chroot on a Qualcomm device, the dmabuf fds it produces reference buffers allocated via KGSL, which is the same kernel driver that Android's gralloc uses. **These dmabuf fds are backed by the same physical memory pools and are compatible with the host Android system's AHardwareBuffer/gralloc.** The key challenge is wrapping them as AHardwareBuffers on the compositor side.

#### 15h. Recommended Architecture for tawc

**Primary path (Qualcomm devices with Mesa Turnip clients):**

1. Compositor allocates AHardwareBuffers via `AHardwareBuffer_allocate()`
2. Compositor imports each AHB into Vulkan via `VK_ANDROID_external_memory_android_hardware_buffer`
3. Compositor exports each as opaque fd via `VK_KHR_external_memory_fd` (both extensions in AVP 2025)
4. Compositor serves these fds to clients via `zwp_linux_dmabuf_v1` feedback, advertising compatible formats/modifiers
5. Client (Mesa Turnip in chroot) allocates compatible buffers, renders, commits dmabuf fds
6. Compositor receives dmabuf fds and matches them to known AHBs (if compositor-allocated) or falls back to GPU copy (if client-allocated unknown buffers)
7. AHBs are submitted to SurfaceFlinger via `ASurfaceTransaction_setBuffer()`

**Fallback path (unknown/incompatible buffers):**

For client-allocated dmabufs that the compositor cannot match to a known AHB:
1. Import the dmabuf as an EGLImage via `EGL_EXT_image_dma_buf_import` (requires Mesa EGL, not stock Android EGL)
2. GPU-blit from the EGLImage to a compositor-allocated AHardwareBuffer
3. Submit the AHB to SurfaceFlinger

**wl_shm path (CPU buffers):**

For clients using shared memory (no GPU):
1. `AHardwareBuffer_allocate()` a CPU-accessible AHB
2. `AHardwareBuffer_lock()` + `memcpy()` from wl_shm buffer + `AHardwareBuffer_unlock()`
3. Submit to SurfaceFlinger

**Critical open question:** The Vulkan opaque fd export path (step 3 above) produces OPAQUE_FD handles, not DMA_BUF handles. Can a Mesa Turnip client import an opaque fd from Android's Vulkan driver? Probably not -- opaque fds are driver-specific. The compositor's Vulkan driver is the stock Android driver (Adreno), while the client uses Mesa Turnip. **Opaque fds are not interoperable across different Vulkan driver implementations.**

**This means the "compositor exports fds to clients" path may not work as described.** The more realistic flow is:
1. Client (Mesa Turnip) allocates buffers independently via its own GBM/Vulkan allocator
2. Client sends dmabuf fds to compositor
3. Compositor must somehow wrap these as AHardwareBuffers
4. Since there's no public NDK API for this, the compositor must use either:
   a. The VNDK `AHardwareBuffer_createFromHandle` (non-public, fragile)
   b. Forge the `sendHandleToUnixSocket` serialization format and feed it to `recvHandleFromUnixSocket` on a socketpair (hacky but uses only public NDK API)
   c. Use the Gralloc 4 AIDL/HIDL `IMapper` interface via Java/JNI to construct an AHB from the native handle

**Option (b) -- the socketpair trick -- deserves investigation.** The compositor could:
1. Create a `socketpair(AF_UNIX, SOCK_STREAM, 0)`
2. Construct the GraphicBuffer serialization format manually (the format is defined in AOSP's `GraphicBuffer::flatten()`)
3. Send the constructed message (with the client's dmabuf fd) through one end of the socketpair
4. Call `AHardwareBuffer_recvHandleFromUnixSocket()` on the other end
5. This produces an AHardwareBuffer wrapping the client's dmabuf -- using only public NDK API

This is fragile (depends on the internal serialization format not changing), but the format has been stable since Android 8 and is effectively part of the ABI since `sendHandleToUnixSocket`/`recvHandleFromUnixSocket` are public API.

**Option (c) -- Gralloc 4 IMapper -- is more robust.** Android 11+ (API 30+) standardized `android.hardware.graphics.mapper@4.0` HIDL interface (and AIDL in Android 13+). The `importBuffer(native_handle_t)` method can import a native handle and produce a registered buffer. This can potentially be used to create an AHardwareBuffer. This requires JNI to access the HIDL/AIDL service, but it's the supported vendor-agnostic path for buffer import.
