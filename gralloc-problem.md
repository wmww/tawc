# Gralloc Mapper HAL Loading from glibc Chroot

## Summary

AHardwareBuffer operations from a glibc process in a chroot (via libhybris) fail
because the gralloc mapper HIDL passthrough HAL can't be loaded. The error is
"gralloc-mapper is missing" followed by SIGABRT from `libui.so`.

This blocks client-side AHB allocation, which means the compositor must allocate
AHBs and send them to the client (current workaround).

## What Works

GPU rendering from the chroot is fully functional:
- EGL 1.5 initializes (Qualcomm, Adreno 618, OpenGL ES 3.2)
- Shader compilation, draw calls, glFinish — all work
- FBO rendering to regular textures works (proven via glReadPixels)
- Receiving raw native handle fds (via SCM_RIGHTS) from the compositor works
- Constructing `ANativeWindowBuffer` manually and creating `EGLImage` works
- Rendering to a compositor-allocated AHB via FBO works end-to-end (visible on screen)

The **only** thing that doesn't work is any `AHardwareBuffer_*` API that touches
`GraphicBufferMapper`, which is all of:
- `AHardwareBuffer_allocate`
- `AHardwareBuffer_recvHandleFromUnixSocket`
- `AHardwareBuffer_createFromHandle`

## Root Cause

### The Loading Chain

```
AHardwareBuffer_allocate (libnativewindow.so)
  → GraphicBuffer::allocate (libui.so)
    → GraphicBufferMapper::getInstance()
      → Gralloc4Mapper() constructor
        → IMapper::getService("default")  [libhidlbase.so]
          → PassthroughServiceManager::get()
            → openLibs("android.hardware.graphics.mapper@4.0")
              → android_load_sphal_library()  [libvndksupport.so]
                → android_get_exported_namespace("sphal")  [linker]
                → android_dlopen_ext(lib, flags, {namespace=sphal})
```

### The Failure Point

`android_load_sphal_library` in `libvndksupport.so`:
1. Calls `android_get_exported_namespace("sphal")` to get the "sphal" linker namespace
2. If namespace found: calls `android_dlopen_ext` with `ANDROID_DLEXT_USE_NAMESPACE`
3. If namespace is NULL: falls back to regular `dlopen` (which should work!)

The problem: `android_get_exported_namespace` returns a non-NULL namespace pointer
from libhybris's linker, but this namespace is empty/misconfigured. The subsequent
`android_dlopen_ext` with that namespace fails silently (can't find the mapper .so
in the namespace's search path), causing `IMapper::getService()` to return null,
which causes `GraphicBufferMapper` init to fail with "gralloc-mapper is missing".

### Why the Namespace Fix Doesn't Work

We tried returning NULL from `android_get_exported_namespace` to force the
`dlopen` fallback path. The problem is that there are multiple copies of this
function:

1. **hooks.c wrapper** (`android_get_exported_namespace` at the C API level) — 
   we modified this to return NULL ✓
2. **hooks.c hook** (`_hybris_hook_android_get_exported_namespace`) — 
   we modified this to return NULL ✓
3. **linker dlfcn.cpp** (`__loader_android_get_exported_namespace`) — 
   we modified this to return NULL ✓

But `libvndksupport.so` resolves `android_get_exported_namespace@plt` through the
bionic linker's internal symbol resolution, which may bypass all three of our
modifications. The linker's own symbol table has a version of the function that
calls `get_exported_namespace()` (linker.cpp:4370), and this internal version is
what the PLT resolves to.

The fundamental issue: **bionic→bionic symbol resolution within libhybris-loaded
libraries doesn't go through the hook table**. Hooks only intercept symbols that
would otherwise resolve to glibc. Functions provided by the linker itself (like
`android_get_exported_namespace`, `android_dlopen_ext`) resolve to the linker's
own versions directly.

## What Would Fix This

### Option A: Patch the linker's `get_exported_namespace` directly

Modify `linker.cpp:get_exported_namespace()` to return nullptr when the requested
namespace name is "sphal" (or always). This is the function that ALL paths
eventually call. The challenge: this function is deep in the linker and is also
used during normal library loading — returning NULL might break other things.

### Option B: Patch `libvndksupport.so` binary

Use the TLS patcher approach to scan `libvndksupport.so` for the
`android_get_exported_namespace` call and NOP it or replace the return value.
This is fragile and device/version-specific.

### Option C: Hook at the HIDL level

Instead of fixing the namespace, hook `IMapper::getService()` in `libhidlbase.so`
to manually dlopen the mapper implementation from `/vendor/lib64/hw/`. We already
proved that `android_dlopen` of the mapper .so files works (TLS patches applied
successfully). The issue is just the HIDL service manager lookup.

### Option D: Pre-register the mapper HAL

Before calling any AHB function, use libhybris to call
`IMapper::registerAsService()` or the passthrough HAL registration mechanism to
make the mapper available to the HIDL service manager from within the process.

### Option E: Compositor-allocates (current workaround)

The compositor (Android app process) allocates AHBs because it has full access to
the gralloc HAL. It sends the raw native handle (fds + int data) to the client via
SCM_RIGHTS. The client constructs `ANativeWindowBuffer` manually and creates
`EGLImage` directly. This works end-to-end but adds protocol complexity.

## Current Workaround

The working end-to-end flow:

1. **Compositor** allocates AHB with `GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT` usage
2. **Compositor** extracts native handle via `AHardwareBuffer_getNativeHandle`
3. **Compositor** sends over Unix socket:
   - Data: `numFds(u32) + numInts(u32) + AHardwareBuffer_Desc(48 bytes) + int_data[numInts]`
   - Fds: via `SCM_RIGHTS` ancillary data
4. **Client** receives fds and metadata via `recvmsg`
5. **Client** reconstructs `native_handle_t` from received data
6. **Client** constructs `ANativeWindowBuffer` struct manually (168 bytes, layout from
   `nativebase.h`) with the handle pointer set to the reconstructed native handle
7. **Client** calls `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, anb_ptr)` — this
   works because EGL imports buffers via KGSL directly, not via gralloc mapper
8. **Client** attaches EGLImage to texture (GL_TEXTURE_EXTERNAL_OES), creates FBO
9. **Client** renders with GLES (shaders, draw calls, glFinish)
10. **Client** sends 1-byte "done" signal to compositor
11. **Compositor** imports same AHB as texture, displays on screen

This has been tested end-to-end and produces correct GPU-rendered output visible
on the Android display.

## Environment

- Device: Pixel 4a (sunfish), Qualcomm Adreno 618
- Android 16 (API 36), LineageOS
- Chroot: Arch Linux ARM at `/data/local/arch-chroot`
- libhybris: lindroid fork with TLS thunk patcher + bionic_tls allocation
- Bind mounts: `/dev`, `/dev/pts`, `/dev/binderfs`, `/proc`, `/sys`, `/vendor`,
  `/system`, `/apex` (recursive), `/linkerconfig`

### Gralloc HAL on This Device

```
lshal output:
  X android.hardware.graphics.mapper@3.0 (/vendor/lib64/hw/) (-qti-display)
  X android.hardware.graphics.mapper@4.0 (/vendor/lib64/hw/) (-qti-display)

Implementation files:
  /vendor/lib64/hw/android.hardware.graphics.mapper@3.0-impl-qti-display.so
  /vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-qti-display.so
```

Both are `X` = passthrough (dlopen'd into the calling process, not Binder RPC).

## Files

- `chroot-scripts/libhybris-tawc.patch` — all libhybris patches (bionic_tls fix,
  namespace bypass, pthread wrapping). Applied by `build-libhybris-lindroid`.
- TLS solution details are in `notes.md` under "libhybris on Stock (Unpatched) Android"
- Test programs in `/data/local/arch-chroot/tmp/`:
  - `test-ahb-alloc.c` — minimal AHB allocate test (demonstrates the failure)
  - `hybris-gpu-client.c` — full GPU render client using the workaround
  - `hybris-gpu-render.c` — standalone GPU render test (FBO + readback, no AHB)
