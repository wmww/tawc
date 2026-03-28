# Notes

This file contains design, architecture and implementation notes, primarily written by
and for LLM agents.

## GPU Driver Strategy (2026-03-28)

### The Problem

A Wayland compositor on Android needs GPU buffer sharing between clients (Linux programs
in a Termux chroot) and the compositor (Android app). On desktop Linux both sides use
Mesa and dmabufs just work. On Android, the client traditionally uses Mesa Turnip while
the compositor uses the stock proprietary driver -- two completely different driver stacks
that can't share buffers.

The Termux ecosystem has **never achieved zero-copy GPU buffer sharing** between Mesa
Turnip and the stock Android driver. Termux:X11 works around it with CPU readback
(GPU -> CPU -> GPU). See "Termux:X11 Analysis" section below.

### Our Solution: Same Driver on Both Sides via libhybris

Instead of fighting cross-driver buffer sharing, we eliminate it. Both client and
compositor use the **stock Android GPU driver**:

- **Compositor**: Normal Android app. Stock driver natively. Nothing special.
- **Client**: glibc program in Termux chroot. Uses **libhybris** to load the stock
  Android GPU driver's bionic `.so` files. Our custom **WSI layer** implements Wayland
  surface/swapchain support.

Same driver = buffer fds are natively compatible. No cross-driver import needed.

### libhybris Status (verified 2026-03-28)

libhybris is **actively maintained**. Key facts:
- Android 16 support merged March 25, 2026 (PR #609)
- Android 14 AIDL HAL support merged January 2026 (PR #578)
- Active contributors from Sailfish OS ecosystem
- EGL/GLES via libhybris is battle-tested (Sailfish OS, Ubuntu Touch, years of production)
- Vulkan support is newer -- active PRs (#604, #607) for improvements
- Loads bionic `.so` files into glibc processes by reimplementing bionic's linker

### libhybris Vulkan Deep Dive (researched 2026-03-28)

**Can it load stock Android Vulkan drivers?** YES, with caveats. The code
(`hybris/vulkan/vulkan.c`) loads the Android `libvulkan.so` via `android_dlopen()` (or a
custom path from `$LIBVULKAN` env var). Every Vulkan function is either intercepted or
forwarded to the Android driver via `android_dlsym()`. The original implementation uses
GNU indirect functions (`gnu_indirect_function` / IDLOAD macro) for near-zero call overhead
after the first resolution. This has been working on Sailfish OS devices since 2022 (Jolla
copyright on the Vulkan code).

**Extension translation -- how it works:** libhybris performs a **surface extension swap**,
NOT a full translation layer:

1. `vkEnumerateInstanceExtensionProperties`: Scans the extension list returned by the
   Android driver. Replaces `VK_KHR_android_surface` with `VK_KHR_wayland_surface` in
   the returned list. Apps think they're getting Wayland support.
2. `vkCreateInstance`: Reverses the swap -- when the app requests `VK_KHR_wayland_surface`,
   libhybris substitutes `VK_KHR_android_surface` before passing to the real driver.
3. `vkCreateWaylandSurfaceKHR`: Translates Wayland surface info into an Android surface.
   Creates a `WaylandNativeWindow` (inherits `ANativeWindow`), wraps it in
   `VkAndroidSurfaceCreateInfoKHR`, calls the real `vkCreateAndroidSurfaceKHR`.
4. `vkGetPhysicalDeviceWaylandPresentationSupportKHR`: Always returns `VK_TRUE`.
5. `vkGetInstanceProcAddr`: Intercepts the 5 WSI functions above. Everything else is
   passed through to the real Android driver's `vkGetInstanceProcAddr`.

**Other VK_ANDROID_* extensions are NOT translated.** Only the surface/WSI path is
intercepted. Extensions like `VK_ANDROID_external_memory_android_hardware_buffer` and
`VK_ANDROID_native_buffer` are passed through directly from the Android driver. Apps see
and can use these Android-specific extensions -- they are not hidden or remapped. This
means a glibc app linked via libhybris sees the raw Android driver extension set (minus
the surface swap).

**PR #604 (OPEN, unmerged):** "vulkan: hook vkCreateSwapchainKHR and
vkGetPhysicalDeviceSurfaceCapabilitiesKHR". Fixes real-world issues with Mali drivers:
- Mali's Vulkan driver reports `currentExtent` as 1x1, which prevents the swapchain from
  sizing correctly under Wayland. Fix: override `currentExtent` to `UINT32_MAX` (Vulkan
  spec signal for "compositor decides size").
- Raises `maxImageExtent` from 4096x4096 to 16384x16384 (Android driver too conservative).
- Adds `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR` support (apps like vkgears require it).
- Also fixes a build issue with newer Vulkan headers where `VK_ENABLE_BETA_EXTENSIONS`
  now guards CUDA functions.
- **Implication:** Without this PR, Vulkan via libhybris may not work on Mali GPUs.

**PR #607 (OPEN, unmerged):** "hybris/vulkan: Do Vulkan symbol resolution without
VULKAN_IDLOAD". Replaces `gnu_indirect_function` with regular wrapper functions:
- Fixes musl libc support (IDLOAD is a glibc-only GNU extension)
- Fixes a crash where relocated/snapped libhybris `libvulkan.so` resolves symbols before
  `environ` is valid, so `getenv("HYBRIS_LINKER_DIR")` segfaults during early startup
- Generated by an auto-conversion script; ~700 lines of boilerplate wrappers
- Also adds X11 surface stubs (`--enable-vulkan-x11-stubs` configure flag)

**Known issues (open):**
- Issue #572: Vulkan on musl libc (Chimera Linux) -- IDLOAD crashes. PR #607 addresses this.
- Issue #573: Snapped (relocatable) libhybris Vulkan crashes due to early symbol resolution
  before `environ` is set. PR #607 addresses this too.
- Adreno GPUs reported as having "bad compatibility from lack of extensions" via libhybris
  Vulkan. The `android-vulkan-bridge` project (kde-yyds) recommends Turnip instead for
  Adreno, but uses libhybris Vulkan for Mali.
- The Wayland platform backend depends on `android_wlegl` protocol (from Sailfish/Mer
  ecosystem) for buffer passing -- the compositor must implement this protocol extension.
- `vkGetPhysicalDeviceWaylandPresentationSupportKHR` unconditionally returns `VK_TRUE`
  without actually checking. Optimistic but potentially wrong.
- Swapchain presentation goes through `WaylandNativeWindow` -> gralloc -> `android_wlegl`
  protocol. This is Sailfish OS's compositor protocol, not `zwp_linux_dmabuf_v1`.

**Has anyone run Vulkan apps via libhybris?** Yes:
- Jolla's test app (`test_vulkan.cpp` in libhybris repo) is a full Vulkan swapchain
  rendering loop (creates Wayland surface, acquires images, clears with animated colors,
  presents). This was written by Jolla engineers in 2022.
- The `android-vulkan-bridge` project (kde-yyds) ran vkmark and vkcube on Mali GPUs
  in LXC containers using libhybris Vulkan, reporting ~7000 FPS on Mali-G610.
- No public reports of Sailfish OS shipping Vulkan to end users yet -- it remains
  development/testing quality.

**Implications for tawc:** libhybris's existing Vulkan layer is designed for the
Sailfish OS ecosystem (compositor implements `android_wlegl` protocol). For our use case
(client-side in Termux chroot, compositor is an Android app), we'd need a different WSI
strategy. The core insight is valid: `android_dlopen` + `android_dlsym` can successfully
load and call stock Android Vulkan drivers from glibc. But the WSI/presentation layer
needs to be our own -- which is what our "custom WSI layer" design already plans.

### WSI Layer Design

The stock Android GPU driver has `VK_KHR_android_surface` (Vulkan) and
`EGL_PLATFORM_ANDROID` (EGL), but not the Wayland equivalents that Linux apps expect.
Our WSI layer bridges this. Buffer sharing uses **AHardwareBuffer** (not dmabufs --
stock Android drivers lack `VK_EXT_external_memory_dma_buf` and
`EGL_EXT_image_dma_buf_import`).

**EGL path (wrapper libEGL.so) -- primary:**
- Wrapper library, first in `LD_LIBRARY_PATH`
- Uses libhybris to load real stock EGL/GLES
- Intercepts `eglGetPlatformDisplay(WAYLAND)`, `eglCreateWindowSurface(wl_surface)`,
  `eglSwapBuffers` -- implements AHB-based buffer management
- Allocates AHardwareBuffer pool, creates EGLImages, wraps in FBOs
- On swap: sends AHB via `AHardwareBuffer_sendHandleToUnixSocket` on side channel,
  coordinates via `tawc_buffer_v1` custom Wayland protocol
- Passes through all other EGL/GL calls to stock driver

**Vulkan path (implicit layer) -- stretch goal:**
- Standard Khronos layer mechanism -- zero app changes needed
- Advertises `VK_KHR_wayland_surface` + `VK_KHR_swapchain`
- Intercepts WSI calls, passes through all rendering calls
- Allocates VkImages backed by AHBs via
  `VK_ANDROID_external_memory_android_hardware_buffer`
- Same AHB side-channel mechanism as EGL wrapper
- **Risk:** libhybris Vulkan compatibility varies by vendor; unmerged PRs (#604, #607) needed for some

### Open Questions for This Strategy

1. **libhybris Vulkan maturity.** EGL/GLES is proven. Vulkan has active PRs but may
   not be fully baked; compatibility varies by vendor. EGL/GLES covers most Linux
   desktop apps.

2. **Stock driver dependencies from chroot.** Need `/vendor/lib64/` bind-mounted.
   Binder access to gralloc needed for AHardwareBuffer allocation -- should work since
   UID/SELinux context is unchanged. Test early.

3. **AHB side-channel complexity.** `AHardwareBuffer_sendHandleToUnixSocket` uses its
   own wire format (multiple fds + metadata via SCM_RIGHTS). Requires a side-channel
   socket alongside the Wayland socket, plus a custom protocol (`tawc_buffer_v1`) to
   coordinate. Alternative: implement AHB serialization directly in the Wayland
   protocol layer (more complex but eliminates side channel).

4. **Device breadth.** libhybris + stock driver is vendor-neutral by design (Adreno,
   Mali, PowerVR, etc.), unlike GPU-specific solutions like Mesa Turnip. Needs
   testing across vendors to confirm.

---

## Vulkan External Memory on Android (researched 2026-03-28)

### VK_KHR_external_memory_fd availability on Android

Stock Android GPU drivers (Adreno, Mali, etc.) **do support `VK_KHR_external_memory_fd`**
on modern devices. It is required by Android Vulkan Profile 2025 (AVP 2025), which has
~80% active device coverage. It was NOT in AVP 2022 (86.5% coverage).

**However**, the fds exported by `VK_KHR_external_memory_fd` are **opaque fds**
(`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`), NOT dmabufs. The extension
`VK_EXT_external_memory_dma_buf` (which would give `DMA_BUF_BIT_EXT` handles) is NOT
in any Android Vulkan Profile and is generally not supported by stock Android drivers.

### What stock Android drivers support

| Extension | Since | Coverage |
|---|---|---|
| `VK_ANDROID_external_memory_android_hardware_buffer` | AVP 2022 | ~86.5% |
| `VK_KHR_external_memory_fd` | AVP 2025 | ~80% |
| `VK_KHR_external_fence_fd` | AVP 2025 | ~80% |
| `VK_KHR_external_semaphore_fd` | AVP 2025 | ~80% |
| `VK_EXT_external_memory_dma_buf` | NOT in any profile | ~0% |

### Problem with current plan

The plan says the WSI layer exports via `VK_KHR_external_memory_fd` and sends fds via
`zwp_linux_dmabuf_v1`. This has two issues:

1. `VK_KHR_external_memory_fd` produces opaque fds, not dmabuf fds
2. `zwp_linux_dmabuf_v1` protocol expects actual dmabuf fds
3. Smithay's `ImportDma` trait expects dmabuf fds
4. ARM's vulkan-wsi-layer (cited as prior art) requires `VK_EXT_external_memory_dma_buf`
   which is NOT available on stock Android drivers

Since both sides (WSI layer + compositor) are under our control, we have options:

**Option A: Opaque fds via VK_KHR_external_memory_fd**
- Export opaque fd on client, import opaque fd on compositor
- Same driver = opaque fds are compatible between processes
- Cannot use `zwp_linux_dmabuf_v1` -- need a custom Wayland protocol
- Compositor imports via `vkAllocateMemory` + `VkImportMemoryFdInfoKHR`
- Requires AVP 2025 (~80% devices)

**Option B: AHardwareBuffer (VK_ANDROID_external_memory_android_hardware_buffer)**
- Export AHB from VkDeviceMemory via `vkGetMemoryAndroidHardwareBufferANDROID`
- Send AHB across process boundary via `AHardwareBuffer_sendHandleToUnixSocket`
- Compositor imports AHB -> EGLImage via `eglGetNativeClientBufferANDROID` +
  `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)`
- Or import to Vulkan via `VkImportAndroidHardwareBufferInfoANDROID`
- Wider device support (AVP 2022, ~86.5%)
- Uses Android-native buffer sharing, well-tested path

**Option C: AHardwareBuffer dmabuf extraction (fragile hack)**
- The first fd in an AHB's `native_handle_t` is "always the dmabuf fd" (per Termux
  community reports)
- Could extract it and use `zwp_linux_dmabuf_v1` normally
- UNDOCUMENTED behavior, could break across vendors/versions
- Not recommended for production

**Recommendation:** Option B (AHardwareBuffer) is the most robust. Widest device
support, documented Android API path, avoids the opaque-fd-vs-dmabuf mismatch.
Both options A and B require a custom Wayland protocol extension (not
`zwp_linux_dmabuf_v1`). The `android_wlegl` protocol used by Sailfish/libhybris is
prior art for exactly this kind of Android-specific buffer passing over Wayland.

### EGL_EXT_image_dma_buf_import on Android

This extension is **not generally available** on stock Android drivers. Android uses its
own equivalent workflow:
- `eglGetNativeClientBufferANDROID(AHardwareBuffer*)` -> `EGLClientBuffer`
- `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, clientBuffer)` -> `EGLImage`

Mali Linux drivers (ODROID boards) partially support `EGL_EXT_image_dma_buf_import` but
only for RGBA8888. Stock Android Mali/Adreno drivers use AHB-based import instead.

### ARM vulkan-wsi-layer cannot work on stock Android

The ARM vulkan-wsi-layer requires both `VK_EXT_external_memory_dma_buf` AND
`VK_KHR_external_memory_fd` from the underlying driver. Since stock Android drivers
do NOT support `VK_EXT_external_memory_dma_buf`, the ARM vulkan-wsi-layer **cannot work
as-is** on stock Android drivers. It is designed for Linux with Mesa or proprietary
Linux drivers, not Android. Our WSI layer must use a different buffer export strategy.

---

## Termux:X11 Analysis (2026-03-27)

How Termux:X11 handles GPU buffers today -- both paths involve CPU readback:

**Path 1: `MESA_VK_WSI_DEBUG=sw` (common/default)**
1. Client renders with Mesa Turnip (Vulkan) on GPU
2. Mesa WSI reads frame back to CPU memory
3. Pixels sent to X server via `xcb_put_image()` or MIT-SHM
4. Lorie X server uploads as GL texture on stock driver
5. **GPU -> CPU -> GPU**

**Path 2: DRI3**
1. Turnip exports dmabuf fd via DRI3
2. Server does `mmap(fd, PROT_READ)` -- CPU mmap, not GPU import
3. Pixel data uploaded as GL texture
4. **GPU -> CPU mmap -> GPU**

Nobody in the Termux ecosystem has achieved true zero-copy GPU buffer sharing between
Mesa Turnip and the stock Android driver.

---

## Unix Domain Socket Access (2026-03-27)

Cross-app Unix socket communication is blocked by SELinux on Android 9+.

- SELinux `app_neverallows.te` does not grant `connectto` between `untrusted_app` domains
- Abstract namespace sockets also blocked by SELinux MAC checks
- Filesystem sockets face both DAC (app dirs are 0700) and MAC barriers

**How Termux works around it:** Termux and plugins share `sharedUserId="com.termux"` =
same UID = bypasses both DAC and SELinux.

**Our solution:** `app_process` relay creates socket in Termux's `$XDG_RUNTIME_DIR`,
passes **listening fd** to compositor app via Binder/Intent. SELinux checks apply to
`connect()`/`bind()`, not to `accept()`/`read()`/`write()` on inherited fds.

---

## app_process Relay (2026-03-27)

`app_process` does NOT provide an Android `Context`. The relay cannot call
`bindService()`. Solution: reverse the Binder flow.

The relay creates Binder objects and sends them TO the app by launching an Activity via
direct `IActivityManager.startActivityAsUser()` calls (reflection, bypassing Context).
The TermuxAm and wlroots-android-bridge projects both use this pattern.

Flow:
1. Relay creates `$XDG_RUNTIME_DIR/wayland-0` listening socket
2. Wraps fd in `ParcelFileDescriptor` inside Binder-compatible object
3. Sends to tawc app via `IActivityManager.startActivity()` reflection
4. Tawc Activity extracts fd, passes to compositor via JNI
5. Relay exits

---

## Smithay Integration (verified 2026-03-28 against source code)

Latest version: **Smithay 0.7.0** (released 2025-06-24).

### 1. Compilation for aarch64-linux-android

**Feature set:** `default-features = false, features = ["wayland_frontend", "renderer_gl"]`

This pulls in these dependency chains:
- `renderer_gl` -> `gl_generator` (build-time codegen) + `backend_egl` -> `libloading`
- `wayland_frontend` -> `wayland-server`, `wayland-protocols`, `wayland-protocols-wlr`,
  `wayland-protocols-misc`, `tempfile`
- Always-on: `calloop`, `rustix`, `xkbcommon`, `libc`, `tracing`, etc.

**Known blockers for Android:**

1. **`libEGL.so.1` hardcoded path.** In `src/backend/egl/ffi.rs` line 148:
   `Library::new("libEGL.so.1")`. Android names it `libEGL.so` (no `.1` suffix).
   The `EGLDisplay::from_raw()` path still calls `make_sure_egl_is_loaded()` which
   triggers this load. **Must patch** to try `libEGL.so` on Android, e.g.:
   ```rust
   #[cfg(target_os = "android")]
   Library::new("libEGL.so")
   #[cfg(not(target_os = "android"))]
   Library::new("libEGL.so.1")
   ```

2. **`xkbcommon` crate links to system `libxkbcommon.so`.** Uses `#[link(name = "xkbcommon")]`
   in FFI bindings. Must cross-compile libxkbcommon for aarch64-linux-android and provide
   it to the linker. No Rust-pure alternative exists. (The `xkbcommon` crate's "wayland"
   feature only adds `memmap2`, not a wayland system dependency.)

3. **`calloop` ping implementation.** Uses `eventfd` on `target_os = "linux"` but falls
   back to pipes on other targets. Android is `target_os = "android"`, so it gets the
   pipe fallback. This should work fine but is worth noting.

4. **`rustix` features.** Uses `event`, `fs`, `mm`, `net`, `pipe`, `process`, `shm`,
   `time`. rustix supports Android (uses linux-raw or libc backend). Should compile.

5. **No other Linux-specific system libraries leak through** with these features.
   DRM, GBM, libinput, udev, libseat are all behind disabled feature flags.

### 2. EGLDisplay::from_raw() -- confirmed exists in 0.7.0

```rust
pub unsafe fn from_raw(
    display: *const c_void,
    config_id: *const c_void,
) -> Result<EGLDisplay, Error>
```

Takes raw `EGLDisplay` and `EGLConfig` pointers. Skips `eglTerminate` on drop (caller
manages lifetime). Internally still calls `make_sure_egl_is_loaded()` to load EGL
function pointers, then queries extensions, dmabuf formats, etc.

There is also `EGLContext::from_raw(display, config_id, context)` which wraps a
pre-existing EGL context (internally creates an EGLDisplay via `EGLDisplay::from_raw`).

### 3. GlesRenderer dmabuf and AHardwareBuffer support

**dmabuf import: YES.** `GlesRenderer` implements `ImportDma` trait:
```rust
fn import_dmabuf(&mut self, buffer: &Dmabuf, _damage: Option<&[...]>) -> Result<GlesTexture, GlesError>
```
Internally uses `EGLDisplay::create_image_from_dmabuf()` which requires
`EGL_EXT_image_dma_buf_import` (and `EGL_EXT_image_dma_buf_import_modifiers` if
modifiers are used). The build.rs generates bindings for both extensions.

**AHardwareBuffer import: NO.** Zero references to `AHardwareBuffer`,
`EGL_ANDROID_get_native_client_buffer`, or `EGL_ANDROID_image_native_buffer` anywhere
in the Smithay codebase. To import AHardwareBuffers, we would need to:
1. Call `eglGetNativeClientBufferANDROID(ahb)` to get an `EGLClientBuffer`
2. Call `eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, ...)` to
   get an `EGLImage`
3. Use `GlesRenderer::import_egl_image()` (if public) or work around it

This is custom code we must write ourselves -- Smithay will not help here.

### 4. DisplayHandle::insert_client() -- confirmed exists

This is on `wayland_server::DisplayHandle` (from the wayland-server crate, not Smithay):
```rust
pub fn insert_client(
    &mut self,
    stream: UnixStream,
    data: Arc<dyn ClientData>,
) -> Result<Client>
```
Accepts a `std::os::unix::net::UnixStream`. Smithay's own `ListeningSocketSource`
documents using this in its callback. For our use case (fd received via Binder), we
create a `UnixStream` from the raw fd and call this.

### Minimal Feature Set
```toml
smithay = {
    version = "0.7",
    default-features = false,
    features = ["wayland_frontend", "renderer_gl"]
}
```
Avoids all Linux-specific backends (DRM, GBM, libinput, udev, libseat).

### wayland-rs: Pure Rust Backend
The `wayland-backend` crate defaults to a pure Rust Wayland protocol implementation.
Do NOT enable `server_system` feature. No `libwayland-server.so` needed. Proven by
the EWC compositor project.

### Native Dependencies
| Dependency | Status |
|---|---|
| EGL/GLESv2 | Provided by Android NDK |
| libxkbcommon | Must cross-compile for aarch64-linux-android |
| libwayland | Not needed (pure Rust backend) |
| libdrm/libgbm | Not needed (disabled features) |

---

## EGL Context and Surfaces (2026-03-27)

- An EGL context CAN move between threads (release on old, bind on new), but expensive
- One thread can render to multiple EGLSurfaces via `eglMakeCurrent` switches
- Each switch flushes the pipeline -- overhead per switch
- Recommended: single render thread, one context, switch surfaces per window
- `ASurfaceTransaction` + AHB avoids `eglMakeCurrent` overhead entirely (future opt)

---

## Multiple Activities (2026-03-27)

- All Activities in one app share the same process (single heap, static state, threads)
- One SurfaceView per Activity avoids Z-ordering issues
- Single background render thread maintains list of active surfaces
- Activity launch creates visual transitions -- suppress with
  `overridePendingTransition(0, 0)`
- Activities may be killed under memory pressure -- handle surface loss gracefully

---

## DRM <-> AHardwareBuffer Format Mapping

DRM fourcc = MSB-to-LSB channel order. AHB = memory byte order. On little-endian:

| DRM Format | AHB Format |
|---|---|
| `DRM_FORMAT_ABGR8888` | `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` |
| `DRM_FORMAT_XBGR8888` | `AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM` |
| `DRM_FORMAT_RGB565` | `AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM` |

---

## AHardwareBuffer EGL Import

`eglGetNativeClientBufferANDROID` is a **driver extension**, not NDK API-level-gated.
Must query at runtime via `eglQueryString` + `eglGetProcAddress`. Widely available on
Android 8+ but not guaranteed. Always check:
1. `eglQueryString(display, EGL_EXTENSIONS)` for `"EGL_ANDROID_get_native_client_buffer"`
2. `eglGetProcAddress("eglGetNativeClientBufferANDROID")` -- verify non-NULL

---

## ASurfaceTransaction (2026-03-27)

`ASurfaceTransaction_setBuffer` has accepted `AHardwareBuffer` since API 29 (Android 10).
The "API 34 claim" from older notes was incorrect. Signature:
```c
void ASurfaceTransaction_setBuffer(
    ASurfaceTransaction *transaction,
    ASurfaceControl *surface_control,
    AHardwareBuffer *buffer,
    int acquire_fence_fd
);
```

---

## Frame Scheduling: AChoreographer (2026-03-27)

- API 29: `AChoreographer_postFrameCallback64()` -- use this
- API 30: `AChoreographer_registerRefreshRateCallback()` -- refresh rate changes
- API 33: `AChoreographer_postVsyncCallback()` -- multi-timeline frame pacing
- Requires thread with `ALooper` -- compositor thread must call `ALooper_prepare()`

---

## Freeform Windowing (2026-03-27)

The "one Activity per toplevel" design gives full multi-window only with freeform:
- Samsung DeX
- ChromeOS
- Android 15+ desktop mode
- Some custom ROMs

On standard phones: each Activity is fullscreen, switch via recents.

---

## Phantom Process Killer (Android 12+)

Android 12 introduced `PhantomProcessKiller` -- kills child processes of apps when >32
exist. Mitigations:
- Android 12-13: `adb shell device_config put activity_manager max_phantom_processes 2147483647`
- Android 14+: Developer Options "Disable child process restrictions"

Our relay exits after fd handoff so only needs to survive briefly.

---

## Phase 1 Results (2026-03-28)

### What was built
- Android app scaffold (Kotlin, single Activity + SurfaceView)
- Rust JNI library (`smithay-android` crate, cdylib targeting `aarch64-linux-android`)
- GlesRenderer rendering animated solid colors to Android Surface at ~60fps

### Device: OnePlus (Qualcomm Adreno GPU)
- EGL 1.5, GLES with full Adreno extension set
- `EGL_ANDROID_get_native_client_buffer` confirmed available (needed for Phase 2 AHB import)
- `EGL_KHR_no_config_context`, `EGL_KHR_surfaceless_context` available
- `EGL_ANDROID_native_fence_sync` available (useful for frame synchronization)
- Dmabuf import extensions NOT available (as expected -- confirms AHB path is correct)

### Build toolchain
- cargo-ndk 4.1.2 + NDK r27c (27.2.12479018)
- Rust target: `aarch64-linux-android`, min API 29
- libxkbcommon 1.7.0 cross-compiled as static lib (meson, no wayland/x11/registry)
- JDK 21 for Gradle (JDK 26 not supported by Gradle 8.12)
- AGP 8.9.1, Kotlin 2.1.20

### Smithay patching
- **One patch required:** `libEGL.so.1` -> `libEGL.so` on Android in
  `src/backend/egl/ffi.rs`. Applied to local clone at `/home/ai/smithay-patched/`.
  Used via `[patch.crates-io]` in Cargo.toml.
- `EGLDisplay::from_raw()` works perfectly -- we create the raw EGL display/config/context
  ourselves via direct EGL calls, then wrap in Smithay types.
- `EGLNativeSurface` implemented for ANativeWindow (`eglCreateWindowSurface`).
- `GlesRenderer::new()` + `Bind<EGLSurface>` + `Renderer::render()` all work on Android.

### Architecture notes
- Raw EGL context created manually (eglGetDisplay + eglInitialize + eglChooseConfig +
  eglCreateContext), then wrapped via `EGLContext::from_raw()`. This avoids needing
  `EGL_KHR_platform_android` support in Smithay's platform negotiation.
- Render thread is a plain `std::thread` spawned from JNI callback. ANativeWindow
  reference counting handled via `ANativeWindow_acquire`/`release`.
- Smithay's trace-level logging is very verbose (one log per frame). Should reduce log
  level in production.

---

## Phase 2 Results (2026-03-28)

### What was built
- AHardwareBuffer allocation, CPU fill, and Unix socket send/receive (`ahb.rs`)
- AHB import as GL texture via EGL extensions (`gl_import.rs`)
- Same-process AHB round-trip: allocate -> fill -> send over socketpair -> receive ->
  import as EGLImage -> render as GL texture via Smithay GlesRenderer
- Cross-process AHB round-trip: standalone `ahb-test-client` binary sends AHB from
  separate process, compositor receives and displays it
- External mode: compositor can listen on a Unix socket for cross-process clients
  (flag file `/data/data/me.phie.tawc/external-mode` triggers this)

### AHB Import Pipeline (proven on Pixel 4a, Adreno 618)
1. `AHardwareBuffer_recvHandleFromUnixSocket(fd)` -- receive AHB from socket
2. `eglGetNativeClientBufferANDROID(ahb)` -- get EGLClientBuffer (loaded via
   `eglGetProcAddress`, extension `EGL_ANDROID_get_native_client_buffer`)
3. `eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, ...)` --
   create EGLImage (from Smithay's EGL FFI)
4. `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image)` -- attach to texture
5. `GlesTexture::from_raw_with_flags(is_external=true)` -- wrap for Smithay compositing

### Key discoveries
- **GL_TEXTURE_EXTERNAL_OES required for AHB on Adreno.** Binding AHB-backed EGLImages
  to `GL_TEXTURE_2D` produces a black texture. Must use `GL_TEXTURE_EXTERNAL_OES` and
  Smithay's external texture shader (`samplerExternalOES`).
- **Smithay's `Bind` doesn't persist EGL context.** After `renderer.bind()` returns,
  the EGL context may not be current. Must manually call `eglMakeCurrent()` before
  raw GL calls outside of a frame render. Within `renderer.render()` frames, context is
  current.
- **Smithay's `GlesTexture::from_raw()` doesn't support external textures.** Added
  `from_raw_with_flags()` to Smithay (second patch) to set `is_external: true`.
- **Damage rects in `render_texture_from_to` are relative to dest rect origin**, not
  absolute screen coordinates. Passing absolute coords results in zero-size clamped
  damage (invisible texture).
- **GL core functions via `eglGetProcAddress` may return stubs on Android.** Must load
  `glGenTextures`, `glBindTexture`, etc. from `libGLESv2.so` via `libloading` instead.
  EGL extension functions (e.g. `eglGetNativeClientBufferANDROID`,
  `glEGLImageTargetTexture2DOES`) work fine via `eglGetProcAddress`.

### Smithay patching (updated)
Two patches required to `/home/ai/smithay-patched/`:
1. **`libEGL.so.1` -> `libEGL.so`** in `src/backend/egl/ffi.rs` (Phase 1)
2. **`GlesTexture::from_raw_with_flags()`** in `src/backend/renderer/gles/texture.rs`
   (Phase 2) -- adds `is_external` and `y_inverted` parameters for AHB texture import

### Cross-process test
The `ahb-test-client` binary (`ahb-test-client/` crate) is a standalone Android native
binary that allocates an AHB, fills with a green/yellow checkerboard, and sends it over
a Unix socket. To test cross-process:
```bash
# Push and set up
adb push ahb-test-client/target/aarch64-linux-android/release/ahb-test-client /data/local/tmp/
adb shell "run-as me.phie.tawc cp /data/local/tmp/ahb-test-client . && chmod 755 ahb-test-client"
adb shell "run-as me.phie.tawc touch /data/data/me.phie.tawc/external-mode"

# Restart app (now in external mode, waits for client)
adb shell am force-stop me.phie.tawc
adb shell am start -n me.phie.tawc/.MainActivity

# Run external client
adb shell "run-as me.phie.tawc ./ahb-test-client /data/data/me.phie.tawc/ahb-test.sock"

# Clean up after testing
adb shell "run-as me.phie.tawc rm external-mode ahb-test.sock ahb-test-client"
```

---

## Build, Debug, and Iteration Guide

### Prerequisites (already installed on this machine)
- Rust with `aarch64-linux-android` target (`rustup target add aarch64-linux-android`)
- `cargo-ndk` (`cargo install cargo-ndk`)
- Android SDK at `/home/ai/android-sdk` with NDK r27c (`ndk/27.2.12479018`)
- JDK 21 at `/usr/lib/jvm/java-21-openjdk` (Gradle 8.12 doesn't support JDK 26)
- libxkbcommon cross-compiled at `/home/ai/libxkbcommon/builddir/libxkbcommon.a`
- Smithay patched at `/home/ai/smithay-patched/` (one-line `libEGL.so` fix)

### Build Steps

**1. Build Rust native library:**
```bash
export ANDROID_NDK_HOME=/home/ai/android-sdk/ndk/27.2.12479018
cd smithay-android
cargo ndk --target arm64-v8a --platform 29 -- build --release
```
Output: `smithay-android/target/aarch64-linux-android/release/libsmithay_android.so`

**2. Copy .so and build APK:**
```bash
cp smithay-android/target/aarch64-linux-android/release/libsmithay_android.so \
   app/src/main/jniLibs/arm64-v8a/
export ANDROID_HOME=/home/ai/android-sdk
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
./gradlew assembleDebug
```
Output: `app/build/outputs/apk/debug/app-debug.apk`

**3. Deploy and run:**
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n me.phie.tawc/.MainActivity
```

### Quick rebuild cycle (Rust changes only)
```bash
export ANDROID_NDK_HOME=/home/ai/android-sdk/ndk/27.2.12479018
cd smithay-android && cargo ndk --target arm64-v8a --platform 29 -- build --release && \
cp target/aarch64-linux-android/release/libsmithay_android.so \
   ../app/src/main/jniLibs/arm64-v8a/ && \
cd .. && ANDROID_HOME=/home/ai/android-sdk JAVA_HOME=/usr/lib/jvm/java-21-openjdk \
./gradlew assembleDebug && \
adb install -r app/build/outputs/apk/debug/app-debug.apk && \
adb shell am force-stop me.phie.tawc && \
adb shell am start -n me.phie.tawc/.MainActivity
```

### Debugging
- **Rust/native logs:** `adb logcat -s tawc-native`
- **All app logs:** `adb logcat --pid=$(adb shell pidof me.phie.tawc)`
- **Crash traces:** `adb logcat -s DEBUG` (for native crashes / tombstones)
- **Take screenshot:** `adb shell screencap -p /sdcard/s.png && adb pull /sdcard/s.png /tmp/s.png`
  (remember to delete from device after: `adb shell rm /sdcard/s.png`)

### ADB permissions
ADB may need `sudo` to start the server if USB permissions are denied:
```bash
sudo adb kill-server && sudo adb start-server
```
After that, regular `adb` commands work.

### External dependencies (NOT in this repo)
These live outside the repo and are referenced by absolute path:
- `/home/ai/smithay-patched/` -- Smithay 0.7.0 with `libEGL.so` Android patch
- `/home/ai/libxkbcommon/builddir/libxkbcommon.a` -- cross-compiled static lib
- `/home/ai/android-sdk/` -- Android SDK + NDK
