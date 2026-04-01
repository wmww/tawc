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
Env: `HYBRIS_PATCH_TLS=1`.

### libhybris + libwayland-client Compatibility

`HYBRIS_PATCH_TLS=1` is required. When linking libhybris-common.so at compile time
alongside libwayland-client, the TLS patcher's constructor must run before any bionic
library is loaded.

**dlopen approach:** Loading libhybris-common.so via `dlopen()` requires executable stack
handling. Fixed by `patchelf --clear-execstack` on libhybris-common.so, dlopen instead of
link-time dependency, and `personality(READ_IMPLIES_EXEC)` before loading.

## Buffer Sharing via AHardwareBuffer

Android's native cross-process GPU buffer primitive. Stock drivers don't support dmabuf
extensions (`VK_EXT_external_memory_dma_buf`, `EGL_EXT_image_dma_buf_import`).

Buffer sharing path:
1. Client allocates AHB with GPU usage flags (via NDK API through libhybris)
2. Client creates EGLImage from AHB, attaches to FBO, renders with GLES
3. Client sends AHB via `AHardwareBuffer_sendHandleToUnixSocket()` on side-channel socket
4. Compositor receives via `AHardwareBuffer_recvHandleFromUnixSocket()`
5. Compositor imports: `eglGetNativeClientBufferANDROID(ahb)` -> `eglCreateImageKHR`
   with `EGL_NATIVE_BUFFER_ANDROID` -> GL texture
6. Compositor composites and presents

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

## libhybris Vulkan (Stretch Goal)

libhybris has built-in Vulkan support: loads stock `libvulkan.so` via `android_dlopen()`,
performs surface extension swap (`VK_KHR_android_surface` <-> `VK_KHR_wayland_surface`).
Used in Sailfish OS ecosystem.

**Unmerged PRs needed:**
- PR #604: Mali `currentExtent` fix, `maxImageExtent` raise, opaque alpha support
- PR #607: Replace `gnu_indirect_function` with regular wrappers (musl/relocation fix)

**For tawc:** libhybris's existing Vulkan WSI is designed for Sailfish's `android_wlegl`
protocol. We'd need our own WSI layer using AHB-based buffer sharing. The core insight
is valid: `android_dlopen` can successfully load stock Vulkan drivers from glibc.

## Termux:X11 Comparison

Both Termux:X11 paths involve CPU readback:

**Path 1 (`MESA_VK_WSI_DEBUG=sw`):** Client renders with Turnip on GPU -> Mesa WSI reads
back to CPU -> sent to X server -> uploaded as GL texture. **GPU -> CPU -> GPU.**

**Path 2 (DRI3):** Turnip exports dmabuf -> server does `mmap(fd, PROT_READ)` -> uploaded
as GL texture. **GPU -> CPU mmap -> GPU.**

Nobody in Termux has achieved true zero-copy GPU buffer sharing.
