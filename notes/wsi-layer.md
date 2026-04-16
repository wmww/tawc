# Client-Side WSI: libhybris Wayland EGL Platform

Chroot apps reach the GPU through the stock libhybris Wayland EGL plugin.
Wayland apps set `HYBRIS_EGLPLATFORM=wayland`, libhybris's
`libEGL.so` at `/usr/local/lib` is loaded by name, and buffer sharing
goes over the `android_wlegl` Wayland protocol. We carry no custom
client-side EGL code — the only local changes are inside our libhybris
fork. Two tiny shim libraries (`libgl-shim.c`, `libglesv2-shim.c`,
~30 lines each) sit in front of the distro's libGL.so/libGLESv2.so to
keep libglvnd/Mesa GLX out of the picture; see "Why GL shims still
exist" below.

## Chroot environment

Set by `client/arch-chroot-run` via `/etc/profile.d/01-tawc.sh`:

```
WAYLAND_DISPLAY=wayland-0
XDG_RUNTIME_DIR=/tmp
LD_LIBRARY_PATH=/tmp/gl-shims:/usr/local/lib
HYBRIS_EGLPLATFORM=wayland
LD_PRELOAD=/tmp/memfd-selinux-shim/libmemfd-selinux-shim.so
```

`/tmp/gl-shims` holds the GL shims. `/usr/local/lib` is where
`bash client/build-libhybris` installs libhybris (libEGL.so,
libGLESv2.so, libGLESv1_CM.so, plus its eglplatform_*.so plugins under
`/usr/local/lib/libhybris/`). The memfd SELinux shim is unrelated to
WSI but still required on stock firmware.

We deliberately build libhybris **without** `--enable-glvnd` (see
`client/build-libhybris`): the glvnd path drags `libglvnd` in as a hard
runtime dep, which in turn drags `libGLX_mesa.so` — that can't init
without a DRI render node and breaks Firefox's startup probes. So
libhybris installs as plain `libEGL.so`, and putting `/usr/local/lib`
on `LD_LIBRARY_PATH` ahead of the system path picks it up by name.

## Client flow

1. App calls `eglGetDisplay(wl_display)` → loads `/usr/local/lib/libEGL.so`
   (libhybris).
2. libhybris loads `libhybris/eglplatform_wayland.so`, binds the
   compositor's `android_wlegl` global, and creates a gralloc-backed
   `wl_egl_window` wrapper.
3. On each frame, libhybris calls `hybris_gralloc_allocate` (our AHB
   backend), renders via the vendor EGL driver, then ships the
   `native_handle_t` over `android_wlegl` (`create_handle` →
   `add_fd…` → `create_buffer`) and attaches the resulting `wl_buffer`.

Triple buffering, vsync-style frame throttling, damage forwarding,
resize, and swap-interval handling all come from upstream libhybris —
we don't reimplement any of it.

## Why GL shims still exist

Without the shims, when an app dlopens `libGL.so` / `libGLESv2.so` by
name, it picks up the distro's libglvnd dispatcher. libglvnd routes
`glX*` calls through `libGLX_mesa.so`, which can't init in this chroot
(no DRI render node). Two real-world consequences observed:

- **Firefox** (`/usr/lib/firefox/glxtest -w`) probes for EGL via
  `dlsym`s on `libGL.so` and never reaches our libhybris libEGL.so
  because Mesa GLX has already failed first; it ForceDisable's
  `Feature::HW_COMPOSITING` and falls back to software WebRender (all
  surfaces become magenta-tinted SHM).
- **GTK / libepoxy** dlsyms `glXGetCurrentContext` on libGLESv2 to
  detect context type, and `abort()`s if the symbol is found in a
  vendor that doesn't actually serve GLX.

The shims (`client/libgl-shim.c`,
`client/libglesv2-shim.c`) export NULL-returning `glX*`
stubs and DT_NEEDED-link to a renamed copy of libhybris's GLES
(`libGLESv2_real.so`) so `dlsym(handle, "glBindTexture")` still
resolves through the dependency chain. Built and pushed to
`/tmp/gl-shims` by `bash client/build-libhybris`.

If we ever switch back on `--enable-glvnd` in libhybris and arrange
for libglvnd to be present *without* Mesa (or with Mesa neutered),
the shims become unnecessary. Until then they're the lesser evil.

## libhybris fork: AHB gralloc backend

The fork adds a fourth gralloc backend (version=3) in
`hybris/gralloc/gralloc.c` that routes every call through the public
NDK `AHardwareBuffer_*` API from `libnativewindow.so`. It's preferred
over `GRALLOC_COMPAT`, `GRALLOC1`, and `GRALLOC0` because:

- **Handle layout matches the system mapper.** Allocating via
  `AHardwareBuffer_allocate` produces a `private_handle_t` the device's
  current gralloc mapper (gralloc4 on modern Android) understands, so
  the compositor's `AHardwareBuffer_createFromHandle` accepts it.
  Falling back to gralloc1 produces stale-layout handles that modern
  qdgralloc rejects with `ver(12/12) ints(8/23) fds(1/2)`.
- **Public NDK only.** No dependency on `libui_compat_layer.so`
  (Halium blob) or on matching vendor gralloc quirks per device.
- **No server-side libhybris required.** Compositor-side import uses
  `AHardwareBuffer_createFromHandle` from libnativewindow.so (public),
  not any libhybris code.

Implementation notes:
- Handle↔AHB map is a Mutex-protected linked list. `allocate` inserts,
  `retain` increments a shadow refcount and calls
  `AHardwareBuffer_acquire`, `release` decrements and only calls
  `AHardwareBuffer_release` when the shadow count hits zero.
  Remove-on-zero keeps the map bounded.
- `AHardwareBuffer_Desc.usage` is a u64; gralloc passes i32 usage.
  Lower 32 bits map 1:1 for the common CPU/GPU/COMPOSER bits
  (`USAGE_SW_READ_*`, `HW_TEXTURE`, `HW_RENDER`, `HW_COMPOSER`,
  `PROTECTED`), which covers every current libhybris caller.
- `AHardwareBuffer_format` values equal `HAL_PIXEL_FORMAT_*` for the
  formats clients actually use (RGBA_8888, RGBX_8888, RGB_888,
  RGB_565, RGBA_FP16, RGBA_1010102).
- `hybris_gralloc_import_buffer` returns `-ENOSYS` in AHB mode —
  `AHardwareBuffer_createFromHandle` needs `AHardwareBuffer_Desc`
  (width, height, format, usage, stride) that the `import_buffer`
  signature doesn't carry. Only the server-side-buffer-allocation
  code path in `wayland_window_common.cpp` calls it; we build with
  `--disable-wayland_serverside_buffers` so that path isn't exercised.
- Framebuffer usage (`hybris_gralloc_fbdev_*`) still routes through
  GRALLOC0 because AHB has no framebuffer-device operations. The AHB
  backend refuses to init when `framebuffer=1`.

See `libhybris/TAWC_FORK.md` for the full patch lineage.

## Compositor-side: `android_wlegl` server

`server/compositor/src/wlegl.rs` implements the server side of
`android_wlegl` (version 2, client-side allocation only — we reject
`get_server_buffer_handle` with BAD_VALUE).

Flow:
1. `android_wlegl.create_handle(id, num_fds, ints[])` — store fds
   count + ints array as user-data on the new `android_wlegl_handle`
   resource.
2. `android_wlegl_handle.add_fd(fd)` — accumulate `OwnedFd`s.
3. `android_wlegl.create_buffer(id, w, h, stride, format, usage,
   handle)` — pass accumulated (fds, ints, w, h, stride, format,
   usage) to the C helper, which calls
   `AHardwareBuffer_createFromHandle(REGISTER)`. On success
   `mem::forget` the `OwnedFd`s (REGISTER takes ownership), stash the
   `*mut AHardwareBuffer` in `WleglBufferData` as user-data on the new
   `wl_buffer`.

The C helper (`server/compositor/native/wlegl_import.c`) is ~50 lines:
it builds a `native_handle_t` in-line, runs `createFromHandle(REGISTER)`,
and returns the AHB. On `WleglBufferData::drop` we call
`tawc_wlegl_buffer_release` → `AHardwareBuffer_release`, which closes
the fds and frees the backing gralloc buffer.

Texture import is lazy: `render::import_wlegl_buffers` walks
`state.surface_wlegl`, finds attached buffers without a cached
`GlesTexture`, and runs the import (`AhbTextureImporter::import_ahb` →
`eglGetNativeClientBufferANDROID` →
`eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` →
`glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)`). The texture
is cached on the buffer's `WleglBufferData` so re-attaches reuse it.

## `wl_buffer.release` for libhybris's pool

libhybris's wayland-egl plugin allocates a fixed pool of `wl_buffer`s
(`setBufferCount(3)` in `WaylandNativeWindow`'s ctor) and blocks in
`dequeueBuffer` until one is released back. Smithay's
`SurfaceAttributes::merge_into` automatically sends `wl_buffer.release`
when a *new* buffer replaces an old one during commit — but that's
too late for libhybris: it needs a release before it can dequeue the
next buffer, and without one the very first commit blocks forever.

So `render::release_consumed_wlegl_buffers` sends `wl_buffer.release`
itself once we've imported the buffer's texture, then clears Smithay's
cached `BufferAssignment::NewBuffer` to suppress Smithay's
auto-release at the next commit (releasing twice trips libhybris's
`assert(it != fronted.end())` in
`wayland_window_common.cpp::releaseBuffer`).

The "clear Smithay's cache" trick depends on Smithay's internal cache
layout — see `issues/wlegl-release-clears-smithay-cache-fragile.md`.

## What this replaces

Prior to migration we carried:
- A custom `libEGL.so` wrapper (`client/tawc-wsi/tawc-egl.c`,
  ~1500 lines) that wrapped `AHardwareBuffer_allocate` into an EGL
  surface backed by FBO-to-AHB textures.
- A custom `tawc_buffer_v1` Wayland protocol + Unix-socket
  side-channel carrying AHBs via
  `AHardwareBuffer_sendHandleToUnixSocket`.

Both are gone. The tiny GL shims (libgl-shim, libglesv2-shim) are
all that's left (`client/libgl-shim.c`, `client/libglesv2-shim.c`,
built as part of `bash client/build-libhybris`).
