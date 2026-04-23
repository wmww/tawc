# GPU Driver Strategy and Buffer Sharing

## Prior Art

### wlroots-android-bridge
[Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge) --
wlroots/labwc compositor on Android. Key design decisions we borrow:
- **One Android Activity per Wayland toplevel** -- Android's window manager handles
  task switching, recents, window positioning
- **ASurfaceTransaction for presentation** -- submit rendered buffers to SurfaceFlinger

Why it doesn't work for us: depends on Mesa + minigbm, only works on Intel/x86.

### Termux:X11
Current state of the art for graphical Linux apps on Android. Works but **all paths
involve CPU readback** -- no zero-copy GPU buffer sharing. See "Termux:X11 Comparison"
section below.

### libhybris
[libhybris/libhybris](https://github.com/libhybris/libhybris) -- compatibility layer
allowing glibc programs to load bionic-linked Android shared libraries. Used by Sailfish
OS and Ubuntu Touch. **Actively maintained** -- Android 16 support merged March 2026.
This is what enables our architecture.

### ARM vulkan-wsi-layer
[ArmSoM/vulkan-wsi-layer](https://github.com/ArmSoM/vulkan-wsi-layer) -- open-source
Vulkan layer implementing Wayland/X11 WSI independently of the GPU driver. **Not usable
for our architecture** -- requires `VK_EXT_external_memory_dma_buf` which stock Android
drivers don't support. Useful as structural reference for writing a Vulkan implicit layer.

### libhybris Vulkan WSI
libhybris has a built-in Vulkan WSI that swaps `VK_KHR_android_surface` for
`VK_KHR_wayland_surface` and presents via the `android_wlegl` protocol (Sailfish OS
ecosystem). Uses `WaylandNativeWindow` (inherits `ANativeWindow`) + gralloc for buffer
allocation. See "libhybris Vulkan" section below.

## The Problem

A Wayland compositor on Android needs GPU buffer sharing between clients (Linux programs
in a chroot) and the compositor (Android app). On desktop Linux both sides use Mesa and
dmabufs just work. On Android, the client traditionally uses Mesa Turnip while the
compositor uses the stock proprietary driver -- two completely different driver stacks
that can't share buffers.

The Termux ecosystem has **never achieved zero-copy GPU buffer sharing** between Mesa
Turnip and the stock Android driver.

## Our Solution: Same Driver on Both Sides via libhybris

Instead of fighting cross-driver buffer sharing, we eliminate it. Both client and
compositor use the **stock Android GPU driver**:

- **Compositor**: Normal Android app. Stock driver natively.
- **Client**: glibc program in chroot. Uses **libhybris** to load the stock Android GPU
  driver's bionic `.so` files. Our custom **WSI layer** implements Wayland surface/swapchain
  support.

Same driver = buffer fds are natively compatible. No cross-driver import needed.

## libhybris

[libhybris/libhybris](https://github.com/libhybris/libhybris) -- compatibility layer
allowing glibc programs to load bionic-linked Android shared libraries. Used by Sailfish
OS and Ubuntu Touch. **Actively maintained** -- Android 16 support merged March 2026.

We use [our fork](https://github.com/wmww/libhybris) with stock Android TLS fixes.
Local checkout: `./libhybris`. Build/deploy: `bash client/build-libhybris`.

Loading chain in a client:
```
App (glibc-linked)
  -> dlopen("libEGL.so")  -- finds OUR wrapper (glibc-linked, in LD_LIBRARY_PATH)
    -> our wrapper calls libhybris
      -> libhybris loads /vendor/lib64/egl/libEGL_<vendor>.so (bionic-linked)
      -> libhybris loads /vendor/lib64/egl/libGLESv2_<vendor>.so (bionic-linked)
```

### libhybris on Stock (Unpatched) Android (SOLVED 2026-03-31)

All prior libhybris deployments required patched Android firmware. We solved this:

**Problem:** Bionic's `TLS_SLOT_BIONIC_TLS` (slot -1, at `TPIDR_EL0 - 8`) points to a
~12KB `bionic_tls` struct. The lindroid TLS thunk patcher redirects `TPIDR_EL0` reads
to `tls_hooks[]`, but slot -1 maps to `tls_hooks[-1]` which is NULL -> SIGSEGV.

**Fix (in our libhybris fork's `hooks.c`):**
1. Changed `tls_hooks[16]` to `struct { void *bionic_tls_ptr; void *slots[16]; } tls_area`
   so that `slots[-1]` reads `bionic_tls_ptr` (contiguous in memory).
2. Lazy allocation: `calloc(1, 16384)` on first call per thread, stored in `bionic_tls_ptr`.
3. Thread wrapping: `_hybris_hook_pthread_create()` wraps `start_routine` to ensure allocation.

**Result:** EGL 1.5 initializes on Pixel 4a (Adreno 618), Android 16, stock LineageOS.
TLS patching is always active (no env var needed).

### libhybris + libwayland-client Compatibility

TLS patching is always active. When linking libhybris-common.so at compile time
alongside libwayland-client, the TLS patcher's constructor must run before any bionic
library is loaded.

**dlopen approach:** Loading libhybris-common.so via `dlopen()` requires executable stack
handling. Fixed by `patchelf --clear-execstack` on libhybris-common.so, dlopen instead of
link-time dependency, and `personality(READ_IMPLIES_EXEC)` before loading.

## Buffer Sharing via AHardwareBuffer

Android's native cross-process GPU buffer primitive. Stock drivers don't support dmabuf
extensions (`VK_EXT_external_memory_dma_buf`, `EGL_EXT_image_dma_buf_import`).

Buffer sharing path (post-libhybris-migration):
1. Client's libhybris allocates a gralloc buffer via the AHB backend
   (`AHardwareBuffer_allocate` from libnativewindow.so).
2. Vendor EGL driver renders into the buffer through libhybris's
   `wl_egl_window` integration.
3. Client posts the buffer to the compositor via the standard
   `android_wlegl` Wayland protocol (`create_handle` + N × `add_fd` +
   `create_buffer`). Native handle fds + ints travel inside the
   Wayland connection — no side-channel socket.
4. Compositor reconstructs the handle and calls
   `AHardwareBuffer_createFromHandle(REGISTER)` (via the C helper in
   `server/compositor/native/wlegl_import.c`) to get an AHB pointer.
5. Lazy texture import: `eglGetNativeClientBufferANDROID(ahb)` →
   `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` →
   `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)`. Cached on
   the wl_buffer's user-data so re-attaches reuse the texture.
6. Compositor composites and presents.

For this to work on stock Android ≥ 12, our libhybris fork's gralloc
backend must allocate via `AHardwareBuffer_*` so the handle layout
matches what `AHardwareBuffer_createFromHandle` expects on the
compositor side. See `notes/wsi-layer.md` and `libhybris/TAWC_FORK.md`.

### Gralloc Mapper HAL (SOLVED 2026-03-31)

`AHardwareBuffer_allocate` requires HIDL service management, which checks for
`hwservicemanager` at `/system_ext/bin/hwservicemanager`. Without `/system_ext`
bind-mounted in chroot, all passthrough HAL lookups are skipped.

**Fix:** Add `/system_ext` to chroot bind mounts.

**Chroot bind mounts:** `/dev/binderfs` must be bind-mounted separately from `/dev`
(binderfs is a separate filesystem). Without this, EGL init may work but AHB operations
fail (need `/dev/binderfs/hwbinder` for HAL access).

## Vulkan External Memory on Android

Stock drivers support `VK_KHR_external_memory_fd` (AVP 2025, ~80% devices) but these
are **opaque fds**, not dmabufs. `VK_EXT_external_memory_dma_buf` is NOT available.

`VK_ANDROID_external_memory_android_hardware_buffer` (AVP 2022, ~86.5% devices) is
the most robust path for buffer sharing. Both sides are under our control, so custom
Wayland protocol (not `zwp_linux_dmabuf_v1`) is fine.

## libhybris Vulkan

libhybris has built-in Vulkan support: loads stock `libvulkan.so` via `android_dlopen()`,
performs surface extension swap (`VK_KHR_android_surface` <-> `VK_KHR_wayland_surface`)
in `vulkanplatform_wayland.so`, presents via `android_wlegl`. Used in Sailfish OS.

**Status on tawc (OnePlus 9 / Adreno 660 / Android 16 LineageOS):** ✅ working.
- `bash client/build-libhybris` builds the `vulkan` subdir and installs
  `/usr/local/lib/libvulkan.so.1` and `/usr/local/lib/libhybris/vulkanplatform_wayland.so`.
- `vulkaninfo --summary` works end-to-end: `android_dlopen("libvulkan.so")` succeeds,
  the Adreno Vulkan driver enumerates as GPU0, `VK_KHR_wayland_surface` is advertised.
  Covered by `test_vulkaninfo_loads_android_driver`.
- `vkcube` renders correctly as of 2026-04-20 through the standard direct-render path,
  no FBO workaround needed.

### What `vkcube` actually needs (2026-04-20)

Two fixes were needed:

1. **`NATIVE_WINDOW_BUFFER_AGE=0`** — landed as a Firefox flicker fix
   (libhybris commit `59b9a58`, tawc companion commit `12bca6b`).
   Upstream hardcoded age=2; Adreno's Vulkan WSI used that as a hint to
   preserve 2-frame-old content and did `LOAD_OP_LOAD` on images still
   in `VK_IMAGE_LAYOUT_UNDEFINED`, so the frame was effectively
   discarded.
2. **Spec-correct undefined `currentExtent` + swapchain resize** —
   per the Vulkan spec for Wayland, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`
   reports `currentExtent = {0xFFFFFFFF, 0xFFFFFFFF}` (undefined), letting the
   app choose its own size. `maxImageExtent` is raised to 16384x16384.
   The `wl_egl_window` is created at 1x1; `vkCreateSwapchainKHR` is
   intercepted (via `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr`)
   to resize the `WaylandNativeWindow` to match the app's `imageExtent`.

**Known compositor limitation:** the tawc compositor currently renders
wlegl/Vulkan buffers 1:1 to physical pixels without applying output
scale. Apps that query `wl_output` get logical dimensions (e.g.
540x1200 at 2x scale) and create a swapchain at that size, which
then only covers part of the physical display. The fix belongs in
the compositor (scale wlegl surfaces by output scale factor).

### Vulkan dispatch interception (2026-04-20)

The Vulkan dispatch in `vulkan.c` intercepts several calls for the
Wayland platform (when `WANT_WAYLAND` is defined):

- **`vkGetInstanceProcAddr`** — returns our wrappers for surface
  creation/destruction, capabilities, swapchain, and device proc addr
- **`vkGetDeviceProcAddr`** — returns our `vkCreateSwapchainKHR`
  wrapper. Critical because apps (including vkcube) `dlopen` libvulkan
  and resolve device-level functions via `vkGetDeviceProcAddr`, not PLT
- **`vkGetPhysicalDeviceSurfaceCapabilitiesKHR`** — calls through to
  the Android driver, then patches `currentExtent` to undefined
  (0xFFFFFFFF) and raises `maxImageExtent` to 16384
- **`vkCreateSwapchainKHR`** — resizes the `WaylandNativeWindow` to
  match `imageExtent` before calling the real driver

Also pending in the libhybris working tree: a fence-order fix in the
Vulkan platform's `queueBuffer` (move `presentBuffer` from before to
after `sync_wait(fenceFd)`) and a header-skew fix for the Cuda NV
extension guard. Both are ready to commit; see `libhybris/TAWC_FORK.md`.

**Known libhybris-side header-skew fix we carry:** `vulkan.c`'s Cuda NV extension
block was guarded on `VK_HEADER_VERSION >= 269`, but in vulkan-headers 1.4.341 the
NV Cuda symbols are no longer in `vulkan_core.h` (moved behind a beta/compile-time
flag). We switched the guard to `#ifdef VK_NV_cuda_kernel_launch`, which
correctly follows the extension's feature-test macro.

### Vulkan IFUNC crash (SOLVED)

Upstream libhybris's `vulkan.c` uses `VULKAN_IDLOAD()` which creates GNU IFUNC
symbols for every Vulkan entry point. IFUNC resolvers run during the dynamic
linker's early relocation phase. Each resolver calls `_init_androidvulkan()` →
`android_dlopen("libvulkan.so")`, which loads the entire Android runtime (bionic
linker, vendor Vulkan driver) while the process is still in the ELF startup phase.
This crashes when loaded alongside complex library trees like GTK4 (which links
`libvulkan.so.1` at build time via its `vulkan-icd-loader` dependency).

**Fix (in our fork):** Replaced `VULKAN_IDLOAD` with arm64 assembly trampolines.
Each symbol gets a 3-instruction stub (`adrp`+`ldr`+`br x16`) that tail-calls
through a function pointer. A `__attribute__((constructor))` resolves all pointers
via a linker-set after relocation completes. No IFUNC resolvers, no android_dlopen
during relocation. See `vulkan.c` and `libhybris/TAWC_FORK.md`.

Upstream PR #607 takes a different approach (hand-written C wrappers for every
function); ours is smaller because the macro generates the assembly.

**Upstream PRs worth re-evaluating if we push further on vkcube:**
- PR #604: Mali `currentExtent` fix, `maxImageExtent` raise, opaque alpha support

## Termux:X11 Comparison

Both Termux:X11 paths involve CPU readback:

**Path 1 (`MESA_VK_WSI_DEBUG=sw`):** Client renders with Turnip on GPU -> Mesa WSI reads
back to CPU -> sent to X server -> uploaded as GL texture. **GPU -> CPU -> GPU.**

**Path 2 (DRI3):** Turnip exports dmabuf -> server does `mmap(fd, PROT_READ)` -> uploaded
as GL texture. **GPU -> CPU mmap -> GPU.**

Nobody in Termux has achieved true zero-copy GPU buffer sharing.
