# Migrate from custom WSI to libhybris Wayland EGL platform

## Summary

Replace our custom `tawc-egl.c` WSI layer and `tawc_buffer_v1` protocol with libhybris's built-in Wayland EGL platform (`eglplatform_wayland.so`) and the standard `android_wlegl` buffer sharing protocol. This eliminates ~1500 lines of custom EGL wrapping, the GL shim libraries, the `LD_LIBRARY_PATH` overrides, and our custom Wayland protocol -- replacing all of it with upstream-maintained libhybris code.

This also solves [libglvnd-egl-vendor.md](libglvnd-egl-vendor.md) as a side effect: libhybris's glvnd support (`--enable-glvnd`) installs as a proper EGL vendor, eliminating the GL shim architecture entirely.

## Background

**Current architecture (tawc-egl.c):**
- Custom `libEGL.so` replacement intercepts all EGL calls
- Allocates `AHardwareBuffer` pools, redirects rendering to FBO-backed AHB textures
- Sends AHBs to compositor via Unix socket side-channel + custom `tawc_buffer_v1` Wayland protocol
- GL shim libraries (`libGL.so`, `libGLESv2.so.2`) with patchelf soname hacks
- Everything loaded via `LD_LIBRARY_PATH=/tmp/tawc-wsi`

**Target architecture (libhybris Wayland platform):**
- libhybris's `eglplatform_wayland.so` plugin handles all EGL/Wayland integration
- Allocates gralloc buffers, sends native handles via `android_wlegl` Wayland protocol
- Compositor reconstructs handles into `AHardwareBuffer` via `AHardwareBuffer_createFromHandle` (API 31+)
- libglvnd dispatches EGL calls to `libEGL_libhybris.so` vendor -- no shims needed
- `HYBRIS_EGLPLATFORM=wayland` selects the platform plugin

## What libhybris's Wayland platform gives us for free

Things we currently implement in tawc-egl.c that libhybris already handles:
- Full `ANativeWindow` implementation with proper buffer lifecycle
- Triple buffering with frame callback vsync (we only have double buffering, no vsync)
- Damage region forwarding to compositor (ours is stubbed)
- Proper `wl_egl_window` integration with resize callbacks
- Buffer format/usage negotiation
- Swap interval control
- `EGL_WL_create_wayland_buffer_from_image` extension
- `EGL_EXT_swap_buffers_with_damage` (real implementation, not stub)

## Prerequisites

- **CFI workaround in libhybris:** ✅ Done — the `__cfi_slowpath` patch lives in `hybris/common/q/cfi_bypass.c` in our libhybris fork. tawc-egl.c no longer needs it. See `libhybris/TAWC_FORK.md`.

## Plan

### Phase 1: Build libhybris with Wayland + glvnd support

Update `client/build-libhybris` configure flags:

```
./configure \
    --enable-arch=arm64 \
    --enable-wayland \
    --disable-wayland_serverside_buffers \
    --enable-glvnd \
    --enable-adreno-quirks \
    --enable-property-cache \
    --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64 \
    --prefix=/usr/local
```

New flags:
- `--enable-wayland`: builds the Wayland EGL platform plugin and links against wayland-client/wayland-egl
- `--disable-wayland_serverside_buffers`: use client-side buffer allocation (simpler -- compositor doesn't need to allocate buffers or extract native handles from AHBs)
- `--enable-glvnd`: builds as `libEGL_libhybris.so` vendor with JSON ICD, requires `libglvnd` dev package in chroot

New chroot package deps: `libglvnd` (provides `glvnd/libeglabi.h` and the dispatch libraries).

The build script's per-directory build order needs updating to include the wayland platform directory.

### Phase 2: Implement `android_wlegl` protocol in the compositor (Rust + C helper)

The compositor needs to implement the server side of `android_wlegl` (defined in `libhybris/hybris/platforms/common/wayland-android.xml`). This replaces the current `tawc_buffer_v1` protocol handling.

The tricky part is buffer import: reconstructing a `native_handle_t`, registering it with gralloc, and constructing an `ANativeWindowBuffer` with the correct binary layout (magic numbers, version, refcount function pointers). This struct layout varies across Android versions and getting it wrong causes silent failures or crashes.

**Use libhybris's server_wlegl as a C helper library** rather than reimplementing buffer import in Rust. The code already exists (~450 lines in `hybris/platforms/common/server_wlegl*.cpp` + `windowbuffer.h`) and handles all the layout details:

- `server_wlegl_handle` reconstructs `native_handle_t` from protocol fds/ints
- `server_wlegl_buffer` calls `hybris_gralloc_import_buffer()` and constructs a `RemoteWindowBuffer` (correct `ANativeWindowBuffer` subclass with proper refcounting)
- `RemoteWindowBuffer` is the `EGLClientBuffer` we pass to `eglCreateImageKHR`

**Architecture split:**

- **Wayland protocol dispatch (Rust):** Register the `android_wlegl` global via Smithay, handle `create_handle`/`add_fd`/`create_buffer` requests, manage `wl_buffer` lifecycle and release events. This integrates with Smithay's event loop and surface state naturally.
- **Buffer import (C helper):** Thin C wrapper around libhybris's gralloc + `RemoteWindowBuffer`. Rust calls into it via FFI with the raw fds/ints/metadata, gets back an opaque `EGLClientBuffer` pointer. Something like:

```c
// C helper API (linked against libhybris-platformcommon)
typedef struct wlegl_buffer wlegl_buffer;

// Import a client-allocated gralloc buffer from its native_handle components.
// fds/ints are the raw data from the android_wlegl protocol.
// Returns NULL on failure.
wlegl_buffer *wlegl_buffer_import(
    int32_t width, int32_t height, int32_t stride,
    int32_t format, int32_t usage,
    const int *fds, int num_fds,
    const int *ints, int num_ints);

// Get the EGLClientBuffer (ANativeWindowBuffer*) for EGL import.
void *wlegl_buffer_get_egl_client_buffer(wlegl_buffer *buf);

// Release (decrements refcount, frees when zero).
void wlegl_buffer_release(wlegl_buffer *buf);
```

This is a small .c file (maybe 50-100 lines) that wraps the existing libhybris code. It links against `libhybris-platformcommon` (which provides `hybris_gralloc_import_buffer` and the `RemoteWindowBuffer` class) and is compiled as part of the compositor's native build.

**Protocol operations (Rust side):**

1. **`android_wlegl` global** (version 2): register via Smithay's `wayland_server`
2. **`create_handle(id, num_fds, ints[])`**: store the ints array, prepare to receive fds
3. **`android_wlegl_handle.add_fd(fd)`**: accumulate fds (called `num_fds` times)
4. **`create_buffer(id, width, height, stride, format, usage, handle)`**:
   - Call into C helper with accumulated fds/ints + buffer metadata
   - Get back `EGLClientBuffer` pointer
   - Import to EGLImage -> GL texture (existing `gl_import.rs` path, minor adaptation)
   - Create `wl_buffer` resource with release semantics

Server-side allocation (`get_server_buffer_handle`) is NOT needed -- we use client-side allocation mode.

**Buffer import path:**

```
native_handle_t (from protocol fds + ints)
  -> C helper: hybris_gralloc_import_buffer() [registers with GPU driver]
    -> C helper: RemoteWindowBuffer construction [correct ANativeWindowBuffer layout]
      -> Rust: eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, anwb) [existing gl_import.rs]
        -> Rust: glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES) [existing]
```

This is exactly how every existing libhybris compositor (Lipstick/SailfishOS, etc.) imports buffers. The proven path, with no struct layout guesswork.

**Build considerations:**

The C helper links against `libhybris-platformcommon.so`, which is installed in the chroot at `/usr/local/lib`. But the compositor runs on the Android side, not in the chroot. Options:
- Build the C helper inside the chroot (where libhybris headers/libs are available), produce a .so, and load it from the compositor at runtime via `dlopen`
- Or cross-compile it as part of the compositor's Android NDK build, pointing at libhybris headers. This is trickier since libhybris is built for glibc, not bionic.

The `dlopen` approach is more natural: the helper runs in the same process as the compositor but loads `libhybris-platformcommon` (which itself loads bionic/vendor libs via its hooks). This is the same process-level mixing that already works for the client side. Need to verify there aren't issues with having both bionic (compositor) and glibc (libhybris-platformcommon) in the same process on the server side.

**Wait -- important architectural question:** libhybris is designed to let glibc programs call bionic libraries. The compositor is a *bionic* program (Android app). Loading `libhybris-platformcommon.so` (a glibc library) into a bionic process is the reverse direction and won't work directly. Two options:

1. **Run the C helper as a separate process/service** that the compositor talks to over a socket or pipe. The helper runs in the chroot (glibc), receives fd/int data, does the gralloc import, and sends back... hmm, this doesn't work either because the `ANativeWindowBuffer` pointer needs to be in the compositor's address space for `eglCreateImageKHR`.

2. **Do the gralloc import directly from bionic** without libhybris. The compositor is already a native Android app with access to the gralloc HAL. Call `hw_get_module(GRALLOC_HARDWARE_MODULE_ID)` to get the gralloc module, then `gralloc->registerBuffer(handle)` to import the native handle. Construct the `ANativeWindowBuffer` in C (not C++) with the correct layout for the device's Android version. This bypasses libhybris entirely on the server side.

Option 2 is the right approach. The C helper doesn't wrap libhybris -- it calls the Android gralloc HAL directly, same as any native Android compositor would. This means:
- The helper is compiled with the Android NDK as part of the compositor build
- It uses `<hardware/gralloc.h>` and `<system/window.h>` from the NDK/platform headers
- `ANativeWindowBuffer` layout comes from the platform headers, which are correct for the target device by definition
- No glibc/bionic mixing on the server side

```c
// C helper (compiled with Android NDK, linked into compositor .so)
#include <hardware/gralloc.h>
#include <system/window.h>
#include <cutils/native_handle.h>

static gralloc_module_t *gralloc_module = NULL;

int wlegl_init(void) {
    return hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                         (const hw_module_t **)&gralloc_module);
}

// ... import buffer, construct ANativeWindowBuffer, return as EGLClientBuffer ...
```

The `ANativeWindowBuffer` struct from `<system/window.h>` is the *real* definition -- no guesswork about layout needed. libhybris's `RemoteWindowBuffer` is a copy of this struct for use outside Android; since we're *on* Android, we use the real thing.

### Phase 3: Environment setup changes

Update `client/arch-chroot-run` profile:

```bash
# Before (custom WSI):
export LD_LIBRARY_PATH=/tmp/tawc-wsi:/usr/local/lib
export HYBRIS_EGLPLATFORM=null  # (or unset)

# After (libhybris Wayland platform):
export HYBRIS_EGLPLATFORM=wayland
# LD_LIBRARY_PATH may still be needed for /usr/local/lib (libhybris install prefix)
# but /tmp/tawc-wsi is removed entirely
```

Install libglvnd JSON ICD file (libhybris build should handle this, but verify):
`/usr/share/glvnd/egl_vendor.d/10_libhybris.json` -> `libEGL_libhybris.so.0`

### Phase 4: Remove dead code

- Delete `client/tawc-wsi/tawc-egl.c`
- Delete `client/tawc-wsi/libgl-shim.c`
- Delete `client/tawc-wsi/libglesv2-shim.c`
- Delete `client/tawc-wsi/build`
- Delete `server/compositor/protocols/tawc_buffer_v1.xml`
- Remove `tawc_buffer_v1` protocol code from compositor (`protocol.rs`, `compositor.rs`, `render.rs`)
- Remove AHB side-channel socket code from compositor
- Update `notes/wsi-layer.md` to reflect the new architecture
- Delete `issues/libglvnd-egl-vendor.md` (solved)

## Unknowns

### 1. Gralloc HAL availability
The C helper calls `hw_get_module(GRALLOC_HARDWARE_MODULE_ID)` to load gralloc0. Modern Android has moved to HIDL/AIDL HALs, but most devices still provide a gralloc0 compatibility shim. Needs verification on the target device. If gralloc0 is gone, alternatives: use `GraphicBuffer` from `libui.so` (C++ but stable on-device), or call the mapper HAL directly.

### 2. Platform headers in NDK build
The C helper needs `<hardware/gralloc.h>`, `<system/window.h>`, and `<cutils/native_handle.h>`. These aren't in the public NDK -- they're platform/vendor headers. The compositor build may need to pull these from the device or from the android-headers package. We already use android-headers in the chroot for libhybris; may need them on the host build side too, or compile the helper in the chroot and load it at runtime.

### 3. libglvnd + libhybris interaction on Arch Linux ARM
libhybris's glvnd support was developed for Ubuntu Touch. It should work on Arch, but the package layout differs. Need to verify:
- `glvnd/libeglabi.h` is available (likely in `libglvnd` package)
- JSON ICD file gets installed to a path that libglvnd scans
- libglvnd's `libEGL.so` correctly dispatches to `libEGL_libhybris.so`
- `libGLESv2.so` and `libGL.so` dispatch works without our shims (libglvnd handles this natively)

### 4. EGL context attribute filtering
tawc-egl.c strips desktop-GL-only context attributes (`EGL_CONTEXT_OPENGL_PROFILE_MASK`, `EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE`) and converts `EGL_CONTEXT_MAJOR_VERSION_KHR` to `EGL_CONTEXT_CLIENT_VERSION`. libhybris's EGL may or may not handle these gracefully. GTK3 with `GDK_GL=gles:always` probably doesn't hit this path, but Firefox's `glxtest` might. Needs testing.

### 5. `eglBindAPI(EGL_OPENGL_API)` rejection
tawc-egl.c explicitly returns `EGL_FALSE` for desktop GL bind requests. libhybris may do this already (Android has no desktop GL), but need to verify that apps like GTK3 get the right signal to fall back to GLES.

### 6. Server-side buffer allocation (future optimization)
We start with client-side allocation (`--disable-wayland_serverside_buffers`) because it's simpler. Server-side allocation (the default in libhybris) is more efficient but requires the compositor to:
- Allocate gralloc buffers on behalf of clients
- Extract the `native_handle_t` fds/ints to send back via protocol events
- This means opening the gralloc allocator HAL (not just the mapper) from the compositor side

This can be revisited later if client-side allocation works well.

### 7. wayland-server dependency for libhybris build
`--enable-wayland` requires `wayland-server` pkg-config in the chroot build environment. The server_wlegl code in `libhybris/hybris/platforms/common/` (which implements the compositor-side of android_wlegl) gets compiled into `libhybris-platformcommon`. We don't use the server_wlegl code (our compositor is Rust), but it still needs to compile. Should be fine as long as `wayland` package is installed, but worth noting.

## Testing plan

1. Build libhybris with new flags, verify `eglplatform_wayland.so` and `libEGL_libhybris.so` are produced
2. Verify libglvnd dispatch: `EGL_LOG_LEVEL=debug` should show vendor loading
3. Test GTK3 debug app with `GDK_GL=gles:always HYBRIS_EGLPLATFORM=wayland`
4. Test Firefox with the standard launch command
5. Run integration tests
6. Check for regressions: damage, resize, multi-surface, subsurfaces
7. Performance comparison: frame timing with triple-buffer + vsync vs old double-buffer + glFinish

## 2026-04-11 — first implementation attempt and findings

Phases 1–3 of the plan landed cleanly. Phase 1 produced the expected artifacts
(`libEGL_libhybris.so.0`, `eglplatform_wayland.so`, the glvnd ICD JSON). Phase 2
wired `android_wlegl` through Smithay with a small C helper in
`server/compositor/native/wlegl_import.c`. Phase 3 flipped the chroot profile
to `HYBRIS_EGLPLATFORM=wayland` and dropped `/tmp/tawc-wsi` from
`LD_LIBRARY_PATH`. End-to-end testing with `weston-simple-egl` then hit a
device-specific gralloc handle format mismatch that blocks the whole
client-side-allocation path on our test device (Pixel 4a, Adreno 660,
LineageOS / Android 16).

### What fails, in order

1. **Client-side allocation produces an old-format handle.** libhybris's
   gralloc initialization in `hybris/gralloc/gralloc.c:109` tries three
   backends in order: GRALLOC_COMPAT (`graphic_buffer_allocator_allocate`
   from libui, via `libui_compat_layer.so`), GRALLOC1 (the vendor gralloc1
   HAL), and GRALLOC0. libhybris logs
   `library "libui_compat_layer.so" not found` and falls through to gralloc1.
   Vendor gralloc1 (`/vendor/lib64/hw/gralloc.default.so`) on this device
   returns a `private_handle_t` with `numFds=1, numInts=8`.

2. **Modern qdgralloc rejects that handle.** The Android-side mapper
   (gralloc4) expects `numFds=2, numInts=23` and logs
   `qdgralloc: Invalid gralloc handle: ver(12/12) ints(8/23) fds(1/2)` every
   time any consumer touches it. We observed this both on the client side
   (during allocation) and on the compositor side (during import).

3. **No import path on the compositor side accepts the handle:**

   - `AHardwareBuffer_createFromHandle(REGISTER)` → `rc=2`
     (`HIDL IMapper::Error::BAD_BUFFER`).
   - `AHardwareBuffer_createFromHandle(CLONE)` → `rc=-22` (EINVAL after
     `native_handle_clone` / importBuffer).
   - `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, anwb_wrapper)` without any
     gralloc retain → `EGL_BAD_ACCESS`, kernel log
     `gsl_memory_map_ext_fd failed`. The Adreno EGL driver internally
     validates through qdgralloc and rejects for the same reason.
   - Direct vendor gralloc1 `retain` from the compositor process was
     attempted but blocked at load time: the untrusted-app linker namespace
     refuses `libhardware.so`, `libcutils.so`, and `libvndksupport.so` (the
     last would expose `android_load_sphal_library`). `<uses-native-library>`
     in AndroidManifest only works for entries in
     `/system/etc/public.libraries.txt`, which on Android 16 consists of the
     NDK libraries only (`libEGL.so`, `libGLESv*.so`, `libnativewindow.so`,
     `libvulkan.so`, `libandroid.so`, …). Vendor HAL modules, libhardware,
     libcutils, and libvndksupport are all excluded.

So the compositor side is boxed in by the app sandbox: the only gralloc path
we can actually reach from untrusted_app is whatever's inside the EGL/Vulkan
drivers themselves, and those drivers use the modern gralloc4 mapper which
refuses libhybris's handles.

### Root cause

libhybris without `libui_compat_layer.so` falls back to a gralloc API
(gralloc1) whose handle layout the device's current vendor gralloc mapper
(gralloc4) no longer understands. The mismatch is symmetric — the compositor
can't import those handles and the client's own EGL driver would reject them
if it went through the mapper path. Previous work with `tawc-egl.c` avoided
the problem by calling `AHardwareBuffer_allocate` directly from the chroot
(bypassing libhybris gralloc entirely), so this is the first time we've hit
it.

Note that libhybris has no gralloc implementation of its own —
`hybris/gralloc/gralloc.c` is a dispatcher that forwards every call to one of
three external backends (GRALLOC_COMPAT via `libui_compat_layer.so`, GRALLOC1
via `hw_get_module`, GRALLOC0). Handle layout comes entirely from whichever
backend gets picked; "using libhybris gralloc" means "using whatever the
vendor library libhybris loaded for us".

**Why we can't just use gralloc1 on the compositor side too.** The vendor
EGL driver (`libEGL_adreno.so`) isn't statically bound to a gralloc version
— it dispatches through whatever gralloc is reachable in its linker
namespace. In the chroot, libhybris pairs the driver with vendor gralloc1
(loaded via `hybris_gralloc`), so client EGL + client gralloc agree. In the
compositor, the Android app linker namespace pairs the same driver binary
with the system gralloc4 mapper (via `libnativewindow.so` →
`libgralloctypes.so` → HIDL mapper). Two independent problems prevent us
from realigning the compositor to gralloc1:

1. **Sandbox.** `libhardware.so`, `libcutils.so`, and `libvndksupport.so`
   aren't in `/system/etc/public.libraries.txt` on Android 16, so the
   untrusted-app namespace refuses to dlopen them. We can't reach
   `hw_get_module` from the compositor to load gralloc1 at all.
2. **In-process validation.** Even if we could load gralloc1 ourselves,
   the EGL driver in the same process resolves its *own* gralloc symbols
   against the system gralloc4 mapper. When we hand it an ANWB wrapping a
   gralloc1 handle, it validates through gralloc4 internally and rejects
   (`BAD_BUFFER` / `gsl_memory_map_ext_fd failed`). We observed this
   directly — wrapping the handle in an ANWB and feeding it to
   `eglCreateImageKHR` without any gralloc-side retain produced the same
   failure as every other import path.

So the client has to produce gralloc4-shaped handles. `AHardwareBuffer_*` is
the NDK path that allocates through whatever gralloc the system currently
ships — on this device, gralloc4 — which is why Option 1 routes libhybris's
dispatcher through it.

### Code state at the point we stopped

- `server/compositor/protocols/android_wlegl.xml` — copy of the libhybris
  protocol, wired via `wayland-scanner`.
- `server/compositor/src/protocol.rs` — generates the `android_wlegl` server
  bindings.
- `server/compositor/src/wlegl.rs` — full `AndroidWlegl` /
  `AndroidWleglHandle` / `WlBuffer<WleglBufferData>` dispatch. Holds an
  opaque `ANativeWindowBuffer*` per buffer and lazily imports to `GlesTexture`.
- `server/compositor/native/wlegl_import.c` — reconstructs `native_handle_t`
  (hand-rolled, since libcutils isn't reachable), tries to open vendor
  gralloc via `android_load_sphal_library` (fails at the dlopen of
  `libvndksupport.so`), then wraps the handle in an `ANativeWindowBuffer`
  and returns it as an EGLClientBuffer. The gralloc retain path is gated on
  availability and currently always `NULL`.
- `server/compositor/native/include/` — subset of halium android-headers
  (`hardware/`, `cutils/`, `system/`, `nativebase/`, `apex/`, `vndk/`, `log/`,
  `android/`) vendored for the C build.
- `server/compositor/src/gl_import.rs` — added `import_client_buffer` that
  takes a raw EGLClientBuffer and runs the existing
  `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` → `GL_TEXTURE_EXTERNAL_OES`
  pipeline.
- `server/compositor/src/{compositor,render,event_loop}.rs` — swapped
  `tawc_buffer_v1` / `SurfaceAhbState` for `android_wlegl` /
  `SurfaceWleglState`. Commit handler picks up attached `WleglBufferData`
  buffers, releases the previous one, and tracks the current one for
  rendering. `import_wlegl_buffers` walks surfaces and lazily runs the C
  helper through `gl_import`.
- `client/build-libhybris` — adds `--enable-wayland`,
  `--disable-wayland_serverside_buffers`, `--enable-glvnd`, installs
  `libglvnd python wayland-protocols`, symlinks the ICD JSON into
  `/usr/share/glvnd/egl_vendor.d/`, and cleans stale non-`_libhybris`
  `libEGL.so`/`libGLESv2.so` left over from the previous build.
- `client/arch-chroot-run` — `HYBRIS_EGLPLATFORM=wayland`,
  `LD_LIBRARY_PATH=/usr/local/lib` (no more `/tmp/tawc-wsi`).

Phase 4 dead-code cleanup was deliberately not done yet; `tawc-egl.c`,
`libgl-shim.c`, `libglesv2-shim.c`, and `tawc_buffer_v1.xml` are still present
so we can fall back to the old path if we decide to abandon this migration.

### Options to unblock

Three viable routes, ranked by how fork-local they are:

1. **Patch libhybris gralloc to allocate via `AHardwareBuffer_allocate`.**
   Add a new backend to `hybris/gralloc/gralloc.c` that wraps libnativewindow
   (`AHardwareBuffer_allocate`, `AHardwareBuffer_getNativeHandle`,
   `AHardwareBuffer_describe`, `AHardwareBuffer_release`). Route `lock`,
   `unlock`, and `retain`/`release` through `AHardwareBuffer_lock`/
   `AHardwareBuffer_unlock` and AHB refcounting. Make this backend preferred
   when `libnativewindow.so` resolves at runtime. Modern handle format by
   construction; our compositor's `AHardwareBuffer_createFromHandle` path
   then works. Fork-local change in `libhybris`, ~1–2 days. This also
   side-steps `libui_compat_layer.so` entirely for every future Halium-less
   chroot.

2. **Server-side buffer allocation.** Rebuild libhybris with
   `--enable-wayland_serverside_buffers`. Implement `get_server_buffer_handle`
   in the compositor: allocate via `AHardwareBuffer_allocate`, pull out the
   native handle via `AHardwareBuffer_getNativeHandle` (public on
   libnativewindow.so — already verified on the device), emit `buffer_fd` /
   `buffer_ints` / `buffer` events. The *compositor* then owns allocation and
   the handle format is modern by construction on our side. The open risk is
   whether the **client** (libhybris gralloc1 in the chroot) can import
   those modern handles; `private_handle_t` is shared between gralloc1 and
   gralloc4 inside Qualcomm's vendor library, so in practice it usually
   works, but this needs to be verified before committing.

3. **Build `libui_compat_layer.so` from Halium.** Restores libhybris's
   preferred GRALLOC_COMPAT path (`graphic_buffer_allocator_allocate`), which
   routes through modern `GraphicBuffer` and produces gralloc4-compatible
   handles. `libui_compat_layer.so` is a Halium-specific C shim that
   re-exports a stable subset of Android's C++ `libui.so`
   (`GraphicBufferAllocator::allocate`, `GraphicBuffer` ctor/dtor, lock/unlock)
   through an ABI libhybris can `dlopen`. It has to be compiled against a
   specific Android version's libui inside that Android's build system, so
   getting one requires Halium's AOSP manifest and build tooling (repo,
   lunch, soong/make) targeted at Android 16. Halium ships it as part of
   their system overlay; stock LineageOS has no reason to and doesn't. High
   effort, device/version-specific, and has to be rebuilt per Android
   release — strictly worse than Option 1 for our use case.

A fourth option is to revert — keep `tawc-egl.c`. That loses the gains
described at the top of this document (triple buffering, vsync, proper
damage, fewer lines of our own code, etc.).

### Recommendation

Option 1. It's the smallest change that actually fixes the root cause, it's
entirely within our libhybris fork, and it improves every other libhybris
consumer in the chroot at the same time. The NDK path (`AHardwareBuffer_*`) is
public and stable, so there's nothing device-specific to maintain.

### Option 1 design notes

**What changes, what doesn't.**

- **libhybris:** add an AHB backend alongside GRALLOC_COMPAT / GRALLOC1 /
  GRALLOC0 in `hybris/gralloc/gralloc.c`. Probe for `libnativewindow.so` at
  init time (it's always present on Android ≥ 8, but we want a runtime check
  so non-Android builds still compile). Prefer it over GRALLOC_COMPAT when
  available.
- **libhybris Wayland plugin:** no changes. `eglplatform_wayland.so` calls
  the dispatcher (`hybris_gralloc_allocate`, `_lock`, `_release`, etc.) and
  doesn't care which backend answers.
- **`android_wlegl` protocol:** no changes. The protocol already carries
  `native_handle_t` by splitting it across `create_handle(ints[])` +
  `handle.add_fd(fd)` × N + `create_buffer(w, h, stride, format, usage, …)`.
  AHB native handles serialize through this unchanged.
- **Side-channel Unix socket:** gone. Everything the socket carries today
  (the fds and a couple of ints) rides inside the `android_wlegl` messages
  listed above. SCM_RIGHTS on the Wayland socket handles fd transfer;
  width/height/stride/format/usage come through `create_buffer`, which is
  exactly the set of fields `AHardwareBuffer_Desc` needs for
  `createFromHandle`.
- **`tawc_buffer_v1` protocol:** gone — Phase 4 cleanup proceeds as
  originally planned.

**Compositor-side cleanup enabled by modern handles.** Once handles are
gralloc4-format, the compositor's `wlegl_import.c` can drop the manual
`ANativeWindowBuffer` wrapping and switch to the public NDK path:

```
native_handle_t (from protocol fds + ints)
  -> AHardwareBuffer_createFromHandle(desc, handle, REGISTER_HANDLE, &ahb)
    -> eglGetNativeClientBufferANDROID(ahb)
      -> eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, client_buffer)
        -> glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)
```

That removes:
- The hand-rolled `native_handle_t` allocator in `wlegl_import.c`
- The manual `ANativeWindowBuffer` construction and refcount stubs
- The `android_load_sphal_library` attempt and its dead gralloc1
  retain/release path
- The vendored halium android-headers under `server/compositor/native/include/`
  (`hardware/`, `cutils/`, `system/`, `nativebase/`, `apex/`, `vndk/`, `log/`,
  `android/`) — we only need the NDK surface from now on

Roughly ~300 lines of compositor-side plumbing collapse into a few NDK calls
that are guaranteed to produce the same result as the system's own AHB
import path.

**Pre-flight verification (before committing to the libhybris patch).**

1. Confirm `libnativewindow.so` resolves inside libhybris's bionic bridge in
   the chroot. It's a bionic library and libhybris already loads bionic libs
   via its `hybris_hook` mechanism, but the symbols we need
   (`AHardwareBuffer_allocate`, `_describe`, `_lock`, `_unlock`, `_acquire`,
   `_release`, `_getNativeHandle`, `_createFromHandle`) all need to be
   resolvable. Quick `dlopen` + `dlsym` smoke test before writing the
   backend.
2. Format/usage translation. `AHARDWAREBUFFER_FORMAT_*` values differ from
   `HAL_PIXEL_FORMAT_*` constants, and AHB usage flags
   (`AHARDWAREBUFFER_USAGE_*`) differ from gralloc1 producer/consumer
   usage. Need a translation table similar to the existing
   `GrallocUsageConversion.cpp` — both directions (caller passes gralloc
   flags, AHB backend converts).
3. Lock semantics. `AHardwareBuffer_lock` takes a fence fd and is
   asynchronous; gralloc1 `lock` callers expect synchronous behaviour.
   Passing `-1` as the fence and letting the driver wait is correct but
   conservative — revisit if perf demands it.
4. Refcount mapping:
   - gralloc `retain` → `AHardwareBuffer_acquire`
   - gralloc `release` → `AHardwareBuffer_release`
   - `importBuffer(handle)` → `AHardwareBuffer_createFromHandle(…,
     CLONE_HANDLE, …)` on the client side if we ever need to round-trip
     handles locally.

**Deferred for later.** Server-side allocation (Option 2) is still worth
revisiting after Option 1 is stable, but for a narrower reason than "lower
latency": it's a prerequisite for direct scanout / hardware overlay paths
(where the buffer layout must match the display controller's requirements,
and only the compositor knows those). For our current GL-composited
SurfaceView architecture, server-side and client-side perform equivalently
once buffer pools warm up, so there's no urgency.
