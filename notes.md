# Notes

This file contains design, architecture and implementation notes, primarily written by
and for LLM agents.

## Compositor Architecture (2026-04-01)

The compositor (`server/compositor/src/`) is split into:

- **lib.rs** — JNI entry points + `run_compositor()` which sets up EGL, Wayland display,
  output, and listening socket, then hands off to the event loop.
- **event_loop.rs** — Calloop-based event loop with three sources: Wayland display fd
  (dispatch client messages), listener socket (accept connections), frame timer (~60fps
  render loop). This is the standard smithay pattern.
- **compositor.rs** — `TawcState` (Wayland protocol state) and all smithay handler trait
  impls. Does NOT hold rendering state.
- **render.rs** — `RenderState` (GPU/EGL state), buffer import (AHB + SHM → GL textures),
  frame rendering, frame callbacks, and the SHM magenta tint shader.
- **egl_android.rs** — Raw EGL context creation and `AndroidNativeSurface` for smithay.
- **ahb.rs** — AHardwareBuffer allocation, CPU fill, and cross-process sharing.
- **gl_import.rs** — Import AHB as GlesTexture via EGL/GL extensions.
- **protocol.rs** — wayland-scanner generated code for tawc_buffer_v1.

**Key design decisions:**
- `TawcState` (protocol) and `RenderState` (GPU) are separate structs. Both live in
  `LoopData` which calloop passes to all callbacks.
- `dispatch_clients()` is called in BOTH the display fd callback AND the frame timer.
  The fd callback handles the fast path; the timer catch ensures no messages are delayed
  by more than one frame. Do not remove either call.
- Per-surface state structs (`SurfaceAhbState`, `SurfaceShmState`) live in TawcState
  but contain texture fields written by render.rs. This cross-cutting is documented in
  compositor.rs and is intentional (avoids duplicate lookup tables).

## Building and Deploying (2026-03-31)

**Required environment variables for building:**
```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk  # Java 26 is NOT compatible with AGP 8.5.1
export ANDROID_NDK_HOME=/home/ai/android-sdk/ndk/27.2.12479018
export ANDROID_HOME=/home/ai/android-sdk
```

**Full build and deploy one-liner:**
```bash
cd /home/ai/tawc/server/compositor && \
cargo ndk --target arm64-v8a --platform 29 -- build --release && \
cd ../.. && \
server/gradlew -p server assembleDebug && \
adb install -r server/app/build/outputs/apk/debug/app-debug.apk && \
adb shell am force-stop me.phie.tawc && \
adb shell am start -n me.phie.tawc/.MainActivity
```

**Important:** The system default Java is 26, but the Android Gradle Plugin (8.5.1) doesn't
support it. You must set `JAVA_HOME` to Java 21 before running `gradlew`.

## SHM Buffer Support (2026-03-31)

SHM buffers (`wl_shm`) are supported alongside the AHB path. SHM matters even for
GPU-accelerated clients because cursor themes (`wl_cursor_theme_load`), toolkit
subsurfaces/popups (GTK3/4), and EGL fallback paths all use `wl_shm`.

**SELinux problem (investigated 2026-03-31):** On Android, the compositor runs as
`untrusted_app`. The issue is **SELinux type enforcement** (not MLS as previously thought).
When root processes in the chroot create memfds, they get label `u:object_r:tmpfs:s0`.
The `untrusted_app` domain lacks `{ read write map }` permission on the `tmpfs` type, so
the compositor can't mmap them. The failure manifests as `recvmsg` failing to deliver the
fd (SELinux denies access during fd transfer), which causes the Wayland protocol parser
to get out of sync and drop all subsequent messages from that client.

When app processes create memfds, they get label `u:object_r:appdomain_tmpfs:s0:cXXX,...`
(with MLS categories matching the app). The `untrusted_app` domain has `{ read write map }`
on `appdomain_tmpfs`. MLS categories are **not enforced** for `appdomain_tmpfs` fd access —
any app can access any `appdomain_tmpfs` file regardless of category mismatch. Confirmed
experimentally with `client/shm-test/`.

Permissive mode (`setenforce 0`) also fixes memfd sharing. The earlier claim that
"permissive mode doesn't help" was incorrect — it was likely tested improperly.

**Solution (with root):** An LD_PRELOAD shim (`client/memfd-selinux-shim/`) intercepts
`memfd_create()` and calls `fsetxattr(fd, "security.selinux",
"u:object_r:appdomain_tmpfs:s0", ...)` to relabel the memfd. This is a single syscall
that makes the memfd shareable with any app, with full normal memfd semantics preserved.
Requires root for `fsetxattr`. Build with `cd client/memfd-selinux-shim && ./build`.

Usage: `LD_PRELOAD=/tmp/memfd-selinux-shim/libmemfd-selinux-shim.so weston-simple-shm`

**Shim limitation (discovered 2026-04-01):** The shim intercepts the libc `memfd_create()`
wrapper, but GDK/GLib calls `syscall(SYS_memfd_create, ...)` directly, bypassing the
LD_PRELOAD. This means GTK3/GTK4 apps' SHM pools get the wrong SELinux label. Confirmed
with strace: `wayland-cursor` memfd gets relabeled (intercepted), but `gdk-wayland` memfd
does not (direct syscall). Workaround: `setenforce 0` for testing. Proper fix: intercept
`syscall()` itself, or run clients as the compositor's UID.

**Without root:** Run client processes as the same app/UID as the compositor. Their memfds
natively get `appdomain_tmpfs` label. No shim needed. This is the path to proot from
within the compositor app.

**Magenta tint**: SHM surfaces are rendered with a distinct magenta tint via a custom
`GlesTexProgram` shader. This is intentional -- it makes it visually obvious when a client
falls back to SHM instead of using hardware-accelerated AHB buffers. The tint should be
removed or made optional once the AHB path is mature.

The SHM path is tracked separately from AHB: `surface_shm` HashMap holds `SurfaceShmState`
per surface. Surfaces using the AHB channel protocol are never checked for SHM buffers.

**Toplevel lifecycle:** Toplevels are retained as long as `ToplevelSurface::alive()` returns
true (not based on whether they have buffers). SHM state is cleaned up when the toplevel
dies. This is important because SHM clients don't create buffer state until after the first
configure event, so buffer-based retain logic would immediately remove new toplevels.

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

### libhybris Status (verified 2026-03-28, stock Android support 2026-03-31)

libhybris is **actively maintained**. Key facts:
- Android 16 support merged March 25, 2026 (PR #609)
- Android 14 AIDL HAL support merged January 2026 (PR #578)
- Active contributors from Sailfish OS ecosystem
- EGL/GLES via libhybris is battle-tested (Sailfish OS, Ubuntu Touch, years of production)
- Vulkan support is newer -- active PRs (#604, #607) for improvements
- Loads bionic `.so` files into glibc processes by reimplementing bionic's linker

### libhybris on Stock (Unpatched) Android -- SOLVED (2026-03-31)

All prior libhybris deployments required patched Android firmware (Halium/hybris-patches)
to disable TLS usage in bionic's locale functions. We solved this for stock firmware:

**Problem:** Bionic's `TLS_SLOT_BIONIC_TLS` (slot -1, at `TPIDR_EL0 - 8`) points to a
~12KB `bionic_tls` struct. The lindroid TLS thunk patcher redirects `TPIDR_EL0` reads
to `tls_hooks[]`, but slot -1 maps to `tls_hooks[-1]` which is NULL → SIGSEGV.

**Fix (in our libhybris fork's `hooks.c`):**
1. Changed `tls_hooks[16]` to `struct { void *bionic_tls_ptr; void *slots[16]; } tls_area`
   so that `slots[-1]` reads `bionic_tls_ptr` (contiguous in memory).
2. Lazy allocation: `_hybris_hook___get_tls_hooks()` calls `calloc(1, 16384)` on first
   call per thread, stores in `bionic_tls_ptr`. Zero-init works because bionic checks
   for NULL locale and falls back to C locale.
3. Thread wrapping: `_hybris_hook_pthread_create()` wraps `start_routine` to call
   `__get_tls_hooks()` before bionic code runs, ensuring allocation.

**Result:** EGL 1.5 initializes on Pixel 4a (Adreno 618), Android 16, stock LineageOS.
Loaded libraries: libc.so, libm.so, libc++.so, liblog.so, libcutils.so,
libnativewindow.so, libEGL.so, libEGL_adreno.so, libGLESv2_adreno.so.
Env: `HYBRIS_PATCH_TLS=1`.

**Upstream relevance:** This approach could be contributed to libhybris upstream
(issue #559, PR #575). It builds on NotKit's tls-patcher-v2 thunk infrastructure.

### Gralloc Mapper HAL — SOLVED (2026-03-31)

`AHardwareBuffer_allocate` and all gralloc operations now work from the chroot.

**Root cause:** `/system_ext` was not bind-mounted into the chroot. The HIDL
service management code checks for `hwservicemanager` at
`/system_ext/bin/hwservicemanager` as a feature flag. Without it, HIDL is
considered "not supported" and all passthrough HAL lookups are skipped entirely.
The PassthroughServiceManager is never tried, so the mapper `.so` is never loaded.

**Fix:** Add `/system_ext` to chroot bind mounts. One-line fix in `arch-chroot-run`.

**Working flow (client allocates):**
1. Client allocates AHB with `AHardwareBuffer_allocate` (works via HIDL mapper)
2. Client creates EGLImage via `eglGetNativeClientBufferANDROID` + `eglCreateImageKHR`
3. Client renders to AHB via FBO with GLES (pure GPU, zero CPU copies)
4. Client sends AHB via `AHardwareBuffer_sendHandleToUnixSocket`
5. Compositor receives via `AHardwareBuffer_recvHandleFromUnixSocket`
6. Compositor imports as GL texture, displays on screen

See `gralloc-problem.md` for full debugging history.

### Cross-Process AHB Sharing (2026-03-31)

Standard Android AHB socket APIs (`sendHandleToUnixSocket`/`recvHandleFromUnixSocket`)
work between the chroot and compositor now that gralloc mapper is available.

**Chroot bind mounts:** `/dev/binderfs` must be bind-mounted separately from `/dev`
because binderfs is a separate filesystem that doesn't propagate through bind mounts.
Without this, EGL init may work (accesses GPU directly via `/dev/kgsl-3d0`) but
AHB operations fail (need `/dev/binderfs/hwbinder` for HAL access).

## Phase 3: Wayland Compositor (2026-03-31)

### Architecture

The compositor is now a proper Smithay-based Wayland server running inside the Android
app process. Key components:

**Wayland protocols:** `wl_compositor`, `xdg_wm_base` (v6), `wl_shm`, `wl_seat`,
`wl_output`, and custom `tawc_buffer_manager_v1` / `tawc_ahb_channel_v1`.

**tawc_buffer_v1 protocol design:**
- `tawc_buffer_manager_v1` (global): client binds, calls `get_channel(surface)` to
  create a per-surface AHB channel
- `tawc_ahb_channel_v1`: compositor sends `channel_fd` event with a socketpair fd for
  AHB transfer. Client calls `attach(width, height)` after sending an AHB on the side
  channel. Standard `wl_surface.commit` triggers presentation.
- Side channel uses `AHardwareBuffer_sendHandleToUnixSocket` /
  `recvHandleFromUnixSocket` (multi-fd serialization that doesn't fit Wayland's wire format)

**Socket path:** `/data/data/me.phie.tawc/wayland-0` -- app's own data dir ensures write
access. Chroot clients access via root. Uses `ListeningSocket::bind_absolute()`.

**Render loop:** Poll-based (no calloop), ~60fps. Each iteration:
1. `listener.accept()` for new clients
2. `display.dispatch_clients()` + `flush_clients()`
3. Import pending AHBs (recv from side channel -> EGLImage -> GL texture)
4. Import pending SHM buffers (from toplevel surfaces not using AHB)
5. GlesRenderer: clear + render AHB textures + render SHM textures (magenta tint)
6. `eglSwapBuffers()`
7. Send frame callbacks for all toplevel surfaces
8. Retain toplevels based on `alive()`, clean up dead SHM state

### libhybris + libwayland-client Compatibility

**HYBRIS_PATCH_TLS=1** is required for libhybris to work on stock Android (TLS thunk
patching). When linking libhybris-common.so at compile time alongside libwayland-client,
`HYBRIS_PATCH_TLS=1` must be set. The order matters: libhybris' `android_dlopen` can
be called before or after `wl_display_connect` -- both work. However, the TLS patcher's
constructor must run before any bionic library is loaded.

**dlopen approach:** Loading libhybris-common.so via `dlopen()` requires the binary to
be compiled with `-z execstack` (libhybris needs executable stack). Without this, the
kernel blocks the dlopen with "cannot enable executable stack".

### Verified Working Flow

1. Client (glibc, chroot): loads AHB functions via libhybris `android_dlopen`
2. Client: connects to Wayland, binds globals, creates xdg_toplevel
3. Client: gets `tawc_ahb_channel_v1`, receives side-channel socket fd
4. Client: allocates AHB (CPU write), fills with pattern
5. Client: sends AHB via `AHardwareBuffer_sendHandleToUnixSocket` on side channel
6. Client: calls `tawc_ahb_channel_v1.attach(256, 256)` + `wl_surface.commit()`
7. Compositor: receives AHB, imports as GL texture (EXTERNAL_OES)
8. Compositor: renders texture centered on screen via GlesRenderer
9. Visual: green/yellow checkerboard visible on Android phone screen

### EGL WSI Layer (2026-03-31)

Drop-in `libEGL.so` wrapper that makes unmodified Wayland EGL apps work with tawc.
Located at `client/tawc-wsi/tawc-egl.c`, built as a shared library placed first in
`LD_LIBRARY_PATH`.

**Intercepted EGL calls:**
- `eglGetDisplay(wl_display)` → stores wl_display, binds tawc_buffer_manager_v1 (non-blocking),
  loads stock EGL via `dlopen("/usr/local/lib/libEGL.so.1.0.0")` (libhybris's EGL)
- `eglCreateWindowSurface(wl_egl_window)` → allocates AHB pool (double-buffered),
  creates EGLImage→texture→FBO per buffer, gets tawc side channel
- `eglSwapBuffers` → `glFinish()`, sends AHB via side channel, calls
  `tawc_ahb_channel_v1.attach` + `wl_surface.commit`, rotates to next buffer
- `eglChooseConfig` → rewrites `EGL_SURFACE_TYPE` from WINDOW to PBUFFER
- Everything else → passed through to stock driver

**Key design decisions:**
- Non-blocking protocol binding in `eglGetDisplay` -- apps call this from within
  `wl_display_dispatch` callbacks, so a blocking roundtrip deadlocks. Protocol globals
  arrive via the app's own event loop dispatch.
- Lazy AHB function loading -- `android_dlopen` for libnativewindow.so deferred to
  first buffer allocation (after EGL context is active).
- Real 1x1 pbuffer surface created for `eglMakeCurrent` context binding, then FBO
  redirect to AHB-backed renderbuffer.
- Depth/stencil renderbuffer attached to FBO for 3D apps.
- `libEGL.so.1` symlink required since apps link against soname.

**Verified:** `weston-simple-egl` (250x250 animated RGB triangle) renders correctly
via the WSI layer on Pixel 4a (Adreno 618). Zero-copy GPU path confirmed.

**Remaining issues (addressed in Phase 4):**
- ~~No resize handling yet~~ → Phase 4: `eglSwapBuffers` detects `wl_egl_window` size changes
- No frame callback throttling → client renders as fast as possible
- Toplevel lifecycle tracking needs work (using surface_ahb as source of truth)
- ~~GTK3 GL path requires desktop-GL-to-GLES shader fix~~ → Fixed: `GDK_GL=gles:always`

### Phase 4 Implementation (2026-03-31)

**Status:** Complete. All code changes deployed and tested on device.

**EGL wrapper rewrite (`client/tawc-wsi/tawc-egl.c`):**
- All 44 EGL 1.5 core functions exported (was ~36). Missing functions that crashed
  libepoxy/GTK3 are now either intercepted or forwarded to the real driver.
- X-macro (`REAL_EGL_FUNCTIONS`) declares all real_* function pointers. Functions
  split into LOAD_REQUIRED (EGL 1.0-1.4) and LOAD_OPTIONAL (EGL 1.5, may not exist
  in libhybris). Optional functions stub gracefully if NULL.
- `eglGetProcAddress` now returns intercepted functions for all our exports AND
  forwards unknown names to real driver. Specifically intercepts:
  - `eglGetPlatformDisplayEXT` / `eglCreatePlatformWindowSurfaceEXT` (libepoxy)
  - `eglSwapBuffersWithDamageEXT` / `eglSwapBuffersWithDamageKHR` (GTK3)
- Thread safety: `pthread_once` for init, `pthread_mutex_t` for surface list,
  `__thread` for per-thread current surface + context tracking.
- `eglGetCurrentSurface` / `eglGetCurrentDisplay` / `eglGetCurrentContext` use
  TLS (`tls_current_surface`, `tls_current_context`), set by `eglMakeCurrent`.
- `eglQueryString(EGL_EXTENSIONS)` returns real driver extensions + appended
  Wayland-specific extensions: `EGL_KHR_platform_wayland`, `EGL_EXT_platform_wayland`,
  `EGL_EXT_platform_base`, `EGL_EXT_buffer_age`, `EGL_EXT_swap_buffers_with_damage`,
  `EGL_KHR_create_context`, `EGL_KHR_surfaceless_context`.
- Buffer age: `eglQuerySurface(EGL_BUFFER_AGE_EXT)` returns `NUM_BUFFERS` after
  first N swaps, 0 before (per EGL_EXT_buffer_age spec).
- `eglSwapBuffersWithDamageEXT` implemented (ignores damage rects, delegates to
  `eglSwapBuffers` -- damage optimization can come later).
- Resize: `eglSwapBuffers` checks `wl_egl_window->width/height` against current
  size. If changed, frees entire AHB pool and reallocates at new dimensions.
- `eglChooseConfig` now ORs in `EGL_PBUFFER_BIT` (was replacing WINDOW with PBUFFER).
- `eglGetConfigAttrib(EGL_SURFACE_TYPE)` ORs in `EGL_WINDOW_BIT` so apps see
  window surface support.
- Surface slots use `in_use` flag instead of linear `num_surfaces` counter, so
  slots can be reused after `eglDestroySurface`.
- `tawc_surface` now stores `wl_egl_window*` (for resize) and `EGLConfig` (for
  potential pbuffer recreation).

**Compositor changes (`server/compositor/src/compositor.rs`):**
- Added `wl_data_device_manager` global via Smithay's `DataDeviceState`. GTK3
  binds this during init for clipboard/DnD -- without it, GTK3 aborts.
- Stub implementations: `DataDeviceHandler`, `ClientDndGrabHandler`,
  `ServerDndGrabHandler`, `SelectionHandler` all use default (no-op) impls.
- `delegate_data_device!(TawcState)` wires up the Wayland protocol dispatch.

**Additional fixes discovered during testing:**
- `eglQueryContext` was missing -- libepoxy resolves it via `dlsym` and crashes
  on undefined symbol. Added as a pass-through to real driver.
- EGL 1.5 client extensions: `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`
  must return platform extensions for libepoxy to detect Wayland support.
  Returns `EGL_EXT_platform_base EGL_KHR_platform_wayland EGL_EXT_platform_wayland
  EGL_EXT_client_extensions`.
- `eglBindAPI(EGL_OPENGL_API)`: Android drivers lack desktop GL. Returns
  `EGL_FALSE` so callers know desktop GL is unavailable. GTK3 with
  `GDK_GL=gles` calls `eglBindAPI(EGL_OPENGL_ES_API)` directly, which works.
- `eglCreateContext` attribute filtering: strips `EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR`,
  `EGL_CONTEXT_MINOR_VERSION_KHR`, and forward-compatible flags. Converts
  `EGL_CONTEXT_MAJOR_VERSION_KHR` to `EGL_CONTEXT_CLIENT_VERSION` for GLES.
- libhybris-common execstack: the library's ELF `GNU_STACK` had `RWE` (execute).
  The kernel refused to dlopen it into processes not compiled with `-z execstack`
  (like GTK3). Fixed by `patchelf --clear-execstack` on libhybris-common.so.
  Also changed our wrapper to dlopen libhybris-common at runtime instead of
  linking at build time, and call `personality(READ_IMPLIES_EXEC)` before loading.
- Build script updated: removed `-lhybris-common` link flag, added `-lpthread`.

**Test results:**
- `weston-simple-egl`: GPU-accelerated rendering works end-to-end. RGB triangle
  visible on phone screen via AHB zero-copy path.
- `gtk3-widget-factory` (SHM): renders correctly via wl_shm fallback (magenta tint).
- `gtk3-widget-factory` (GL, `GDK_GL=gles:always`): GPU-accelerated GLES rendering
  works. Normal GTK colors (no magenta tint).

**GTK3 GLES env var gotcha:** The correct GTK3 env var for GLES is `GDK_GL=gles`
(parsed in `gdk/gdk.c`, sets `GDK_GL_GLES` flag). `GDK_DEBUG=gl-gles` is a GTK4
thing and does nothing in GTK3. Combine with `always` to force GL: `GDK_GL=gles:always`.
With `GDK_GL=gles`, GTK3 calls `eglBindAPI(EGL_OPENGL_ES_API)` directly, sets
`use_gles=TRUE`, and compiles GLES shaders (`#version 300 es`). The GLES-aware
codepath in `gdk_wayland_display_init_gl()` was fixed in GTK 3.24.35 (commit
`0e5fe45ea2`), so 3.24.52 has it.

### Phase 4 Design Notes: Robust EGL WSI

**Goal:** make the WSI layer work for GTK3 apps and Firefox. Current wrapper is a
proof-of-concept that works for `weston-simple-egl` but will crash complex apps.

**GTK3 EGL analysis (2026-03-31):** GTK3 uses **libepoxy** for EGL/GL dispatch.
Libepoxy resolves EGL functions via `dlsym` on our `libEGL.so`, falling back to
`eglGetProcAddress`. Every missing export is a crash.

GTK3 GDK Wayland backend EGL calls (from `strings /usr/lib/libgdk-3.so.0`):
- Core: `eglGetPlatformDisplay[EXT]`, `eglInitialize`, `eglBindAPI`,
  `eglChooseConfig`, `eglGetConfigAttrib`, `eglCreateContext`,
  `eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`,
  `eglSwapInterval`, `eglQuerySurface`, `eglGetCurrentContext`,
  `eglGetProcAddress`, `eglDestroySurface`, `eglDestroyContext`
- Extensions: `eglSwapBuffersWithDamageEXT` (preferred over `eglSwapBuffers`)
- Extension checks: `EGL_EXT_buffer_age`, `EGL_EXT_swap_buffers_with_damage`,
  `EGL_KHR_create_context`, `EGL_KHR_surfaceless_context`,
  `EGL_EXT_platform_base`

**Architecture: universal forwarding.** Instead of manually wrapping each function:
1. At init, `dlopen` the real libhybris EGL (`/usr/local/lib/libEGL.so.1.0.0`)
2. For each of the 44 core EGL functions, export a symbol that either:
   - Intercepts (for Wayland-specific behavior): ~8 functions
   - Forwards to real driver via loaded function pointer: ~36 functions
3. Use a macro to generate the forwarding stubs to avoid boilerplate
4. `eglGetProcAddress` forwards unknown names to real driver

**Functions that need interception (not just forwarding):**
- `eglGetDisplay` / `eglGetPlatformDisplay` -- detect Wayland display, bind protocol
- `eglInitialize` -- return cached result (already initialized)
- `eglCreateWindowSurface` / `eglCreatePlatformWindowSurface` -- AHB pool + FBO
- `eglDestroySurface` -- cleanup our state if tawc_surface
- `eglMakeCurrent` -- bind FBO if tawc_surface
- `eglSwapBuffers` -- send AHB + commit if tawc_surface
- `eglQuerySurface` -- handle width/height/buffer_age for tawc_surface
- `eglChooseConfig` -- rewrite WINDOW_BIT to PBUFFER_BIT

**Thread safety design:**
- `pthread_once` for one-time init (stock EGL loading, AHB function loading)
- `pthread_mutex_t surfaces_mutex` protects `surfaces[]` / `num_surfaces`
  (locked in `eglCreateWindowSurface`, `eglDestroySurface`, `find_surface`)
- `__thread struct tawc_surface *current_tawc_surface` tracks per-thread binding
  (set in `eglMakeCurrent`, read in `eglGetCurrentSurface`/`eglSwapBuffers`)
- Per-surface `current` index only written by `eglSwapBuffers` (Wayland guarantees
  one rendering thread per surface), so no lock needed

**eglChooseConfig strategy:** Current approach rewrites `EGL_SURFACE_TYPE` from
`WINDOW_BIT` to `PBUFFER_BIT`. This works but may exclude configs the app wants.
Better: request configs with `PBUFFER_BIT` (required for our dummy pbuffer) but
also add `WINDOW_BIT` configs from the real driver. The app gets a superset.
Actually, simplest: just add PBUFFER_BIT to whatever the app requests (OR it in).

**Buffer age:** GTK3 queries `EGL_BUFFER_AGE_EXT` via `eglQuerySurface` to optimize
repainting (only repaint damaged regions from previous frames). With double
buffering, buffer age alternates: after swap, the "current" buffer was last used
2 frames ago → age = 2. Return `NUM_BUFFERS` as the buffer age.

**Resize:** `wl_egl_window_resize` modifies `wl_egl_window.width`/`.height` in place.
Check these fields at each `eglSwapBuffers`. If changed, free old AHB pool,
allocate new one at new size. This also requires reallocating the depth/stencil
renderbuffer.

**Compositor-side for GTK3 support:**
- `wl_data_device_manager` global: GTK3 binds this for clipboard/DnD. Stub
  implementation that accepts bind and ignores all requests.
- `wl_shm`: needed for cursor themes and some GTK fallback rendering (being
  handled separately).

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

## Unix Domain Socket Access (2026-03-27, updated 2026-03-31)

Cross-app Unix socket communication is blocked by SELinux on Android 9+.

- SELinux `app_neverallows.te` does not grant `connectto` between `untrusted_app` domains
- Abstract namespace sockets also blocked by SELinux MAC checks
- Filesystem sockets face both DAC (app dirs are 0700) and MAC barriers

**With root (chroot):** Root bypasses SELinux MAC checks on `connect()`. The compositor
creates a socket at a known path, chmod 777s it, and chroot clients connect directly.
This is the current development approach (proven in Phase 2 cross-process AHB tests).

**Without root (proot, future):** Two options:

1. **Binder fd passing (preferred):** Compositor creates `socketpair()`, passes one end
   to Termux via a ContentProvider or bound Service as `ParcelFileDescriptor`. No
   `connect()` syscall ever happens, so SELinux `connectto` check is never triggered.
   Cleaner than `app_process` relay — no hidden API reflection, no dependency on
   `app_process` internals.

2. **Shared UID:** `sharedUserId="com.termux"` makes both apps run as same UID/SELinux
   domain. Used by Termux plugins. Deprecated since API 33 but still functional.
   Limits distribution (must sign with Termux's key).

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

## Client Environment: proot vs chroot (2026-03-28)

Linux Wayland clients need a glibc-based Linux environment (because libhybris requires
glibc). This means a full Linux distribution (Arch Linux ARM, Ubuntu, Debian, etc.)
running on the Android device. There are two ways to do this:

### proot (no root required)

Termux's `proot-distro` installs aarch64 Linux distros under proot. This is what most
Termux users already use for desktop Linux apps (via Termux:X11).

- **No root required** -- works on any device
- **Existing ecosystem** -- users already have distros set up, standard package managers
  (`pacman`, `apt`) provide GTK/Qt/SDL apps out of the box
- **Syscall overhead** -- proot uses ptrace to intercept every syscall (~5-10x slower
  for syscall-heavy operations). GPU rendering calls go through the kernel driver
  directly (no ptrace interception on the hot path), but file I/O, process creation,
  network operations, etc. are affected
- **Path translation concerns** -- proot translates filesystem paths, which may
  interfere with libhybris trying to open vendor libraries under `/vendor/lib64/`.
  Needs testing to confirm paths are visible and not mangled
- **Some syscalls unsupported** -- proot can't fully emulate `chroot()`, and some
  `/proc` entries behave differently. Most desktop apps don't care, but edge cases exist

### Real chroot (requires root)

Same distro packages, mounted in a real chroot. Requires root (e.g., Magisk).

- **Native performance** -- no ptrace overhead, syscalls execute at full speed
- **Same distro packages** as proot-distro -- `pacman`, `apt`, etc.
- **Requires root** -- limits the user base to rooted devices
- **Must bind-mount** `/dev`, `/proc`, `/sys`, `/vendor`, `/system`, `/linkerconfig`,
  and `/apex` (with `--rbind`) into the chroot. GPU device nodes (e.g. `/dev/kgsl-3d0`)
  come in via the `/dev` bind mount.
- **`/apex` requires `mount --rbind`** -- each APEX is a separate loop mount, so plain
  `mount --bind` gives empty directories. Without `/apex`, bionic `libc.so` can't be
  found (it's symlinked from `/system/lib64/` to `/apex/com.android.runtime/lib64/bionic/`)
- **`/linkerconfig`** must be mounted for Android 11+ linker namespace config
- **PATH fix needed** -- `/system/bin` leaks into PATH via login shell and breaks
  everything. Add `/etc/profile.d/00-path.sh` to set PATH explicitly
- **Simpler path semantics** -- no path translation layer, libhybris sees real
  filesystem paths
- **Teardown: MUST use `client/arch-chroot-destroy` to remove the chroot.** It unmounts
  all bind mounts before deleting. Never `rm -rf` the chroot directory directly — the
  `--rbind` of `/apex` pulls in dozens of system loop mounts, and removing or
  lazy-unmounting them can destabilize system services and cause a reboot.

### Which to target

Both should work. The WSI layer and libhybris are the same in either case -- the
difference is only in how the root filesystem is set up and whether syscalls go through
ptrace. **proot is the higher-priority target** because it doesn't require root and has
a much larger user base. If proot + libhybris interaction proves problematic (path
translation issues, ptrace interference with bionic linker), chroot is the fallback.

### What apps are available

In either case, apps come from **standard aarch64 Linux distro repositories**. No
custom compilation needed for the apps themselves. For example:
- Arch Linux ARM: `pacman -S firefox gtk3 mpv`
- Ubuntu/Debian: `apt install firefox libgtk-3-0 mpv`

What IS custom and must be installed into the distro:
- **libhybris** -- must be compiled for the target distro (not in standard repos)
- **Our WSI layer** (`libEGL.so` wrapper) -- placed first in `LD_LIBRARY_PATH`
- **Environment variables** -- `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `LD_LIBRARY_PATH`

---

## Phantom Process Killer (Android 12+)

Android 12 introduced `PhantomProcessKiller` -- kills child processes of apps when >32
exist. Mitigations:
- Android 12-13: `adb shell device_config put activity_manager max_phantom_processes 2147483647`
- Android 14+: Developer Options "Disable child process restrictions"

Our relay exits after fd handoff so only needs to survive briefly.

---

## Chroot Setup Results (2026-03-28)

### Working chroot at `/data/local/arch-chroot/`

Arch Linux ARM chroot created via `client/arch-chroot-create`. ~5.5GB with
base-devel, git, gdb, wayland, libtool, pkg-config installed via pacman.

### Running commands via adb

Commands can be run in the chroot from the host machine via:
```bash
adb shell "su -c 'chroot /data/local/arch-chroot /bin/bash -lc \"command here\"'"
```

The `-l` flag is important for picking up the PATH fix in `/etc/profile.d/00-path.sh`.
For complex commands with quoting issues, push a script to
`/data/local/claude-debug/` then copy into the chroot and execute.

### New device bringup checklist

1. **Tear down old chroot** (`arch-chroot-destroy` or manual unmount + rm -rf)
2. **Create chroot** (`arch-chroot-create` -- tarball is cached in `/data/local/tmp/`)
   - If pacman-key fails, it's because the setup script doesn't export PATH. Run
     the package install step manually with `export PATH=/usr/local/sbin:...:/bin`
3. **Build libhybris** (`build-libhybris-lindroid`)
4. **Build WSI + memfd shim** (`client/tawc-wsi/build` and `client/memfd-selinux-shim/build`)
5. **Install & launch compositor** (build with gradlew, adb install, am start)
6. **Test SHM first** (`weston-simple-shm`) -- this doesn't need the WSI layer and
   verifies the compositor and Wayland socket are working
7. **Test EGL** (`weston-simple-egl -f`) -- if this crashes in eglInitialize, check:
   - `cannot locate symbol` errors → likely vendor/system library mismatch (libbinder fix)
   - SIGSEGV in `__cfi_slowpath` → CFI shadow table not initialized (already fixed in WSI)
   - Other crashes → use the signal handler + /proc/self/maps technique from the
     OnePlus 9 debugging to identify which library and offset
8. **Test GTK3** (`GDK_GL=gles:always gtk3-widget-factory`) -- set `TMPDIR=/tmp`

**Common env vars for running clients in chroot:**
```bash
export WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0
export XDG_RUNTIME_DIR=/tmp
export TMPDIR=/tmp
export HOME=/root
export LD_LIBRARY_PATH=/tmp/tawc-wsi:/usr/local/lib
export LD_PRELOAD=/tmp/memfd-selinux-shim/libmemfd-selinux-shim.so
export HYBRIS_PATCH_TLS=1
export GDK_GL=gles:always  # for GTK3
```

### Debugging tips for new devices

- **Bionic libraries loaded by libhybris don't appear in maps until after
  `eglGetDisplay`** (not after dlopen of libhybris). This tripped up CFI patching --
  the patch must run after eglGetDisplay, not before.
- **Android linker namespace issues** cause "cannot locate symbol" errors even when
  the symbol exists on the device. The bionic linker restricts which libraries can
  see each other. Check if vendor and system copies of the same library differ in size.
- **Writing test C programs**: don't try to inline them in shell heredocs through
  multiple layers of adb/su/chroot quoting. Instead, write the file on the host,
  `adb push` it, then `su -c 'cp ... chroot/tmp/'` and compile inside.
- **`LD_PRELOAD` breaks Android system binaries** (toybox/sleep/timeout) with
  namespace errors. Use bash builtins or unset LD_PRELOAD for system commands.
- **The `arch-chroot-create` script's setup.sh doesn't set PATH**, so pacman-key
  and pacman aren't found. Run the package install manually if it fails.

### Bind mounts are ephemeral

All bind mounts are lost on reboot. `arch-chroot-run` re-establishes them idempotently.
The `/apex` recursive bind creates ~80 submounts (one per APEX module).

### Tested devices

**Pixel 4a (sunfish):**
- **Android version:** 16 (API 36)
- **GPU:** Qualcomm Adreno 618
- **Vendor EGL:** `/vendor/lib64/egl/libEGL_adreno.so`
- **Status:** Fully working (SHM, EGL, GTK3)

**OnePlus 9 (LE2115):**
- **Android version:** 14 (API 34)
- **GPU:** Qualcomm Adreno 660v2 (Snapdragon 888, lahaina)
- **Vendor EGL:** `/vendor/lib64/egl/libEGL_adreno.so`
- **Status:** Fully working (SHM, EGL, GTK3) after two fixes (see below)
- **GPU device node:** `/dev/kgsl-3d0` (world rw)

### OnePlus 9 / Android 14 fixes (2026-03-31)

Two issues needed fixing on this device:

**1. Vendor libbinder.so missing symbols:**
`/system/lib64/libbinder_ndk.so` references `openDeclaredPassthroughHal` which
exists in `/system/lib64/libbinder.so` but NOT in `/vendor/lib64/libbinder.so`
(795KB vs 816KB). The bionic linker loads vendor libs in a namespace that finds
the vendor copy first. Fix: bind-mount system's copy over vendor's in the chroot.
Added to `arch-chroot-run`.

**2. CFI (Control Flow Integrity) crash in eglInitialize:**
Android vendor libraries are compiled with CFI enabled. CFI checks indirect call
targets via `__cfi_slowpath` in bionic's `libdl.so`, which looks up a shadow
table. In the libhybris/glibc environment, this shadow table is never initialized
(normally done by bionic's dynamic linker at process start), leaving a NULL pointer
that crashes during `eglInitialize`. The crash manifests as SIGSEGV at
`libdl.so:__cfi_slowpath+0x18` (the `ldrh w8, [x9, x8]` instruction where x9 is
the NULL shadow table pointer).

Fix: patch `__cfi_slowpath` to a no-op `ret` instruction at runtime, after
`eglGetDisplay` loads bionic's libdl.so but before `eglInitialize` triggers
CFI-checked calls. The patch scans `/proc/self/maps` for libdl.so, finds the
function by its instruction signature, and replaces the first instruction with
`ret` (0xd65f03c0). Two signatures are supported:
- Android 14: `sub w8,w1,#0x7f,lsl#12` (0xd351fc28) + `ubfx x9,x8,#31,#7` (0xd35f9909)
- Android 16+: `xpaclri` (0xd50320ff) + `movn x9,#0xaf40,lsl#16` (0x92b5e809)

This fix is in `tawc-egl.c:patch_bionic_cfi()`. Note: this was NOT needed on
the Pixel 4a / Android 16, suggesting either CFI is disabled on that build or
the shadow table is initialized differently.

### What's installed in the chroot

- **Android headers:** Halium `halium-11.0` branch from `Halium/android-headers`,
  version-bumped to 16 in `android-version.h` and `.pc` file, with GCC compatibility
  fixes (script: `client/fix-android-headers`). Installed to `/usr/local/include/android/`.
- **libhybris (tawc fork):** Built from `wmww/libhybris` master. This fork is
  based on `Linux-on-droid/libhybris` `lindroid-21` with: TLS thunk patcher
  (cherry-picked from `lindroid-drm` branch, original author TheKit), and our
  bionic_tls allocation fix for stock Android. Installed to `/usr/local/lib/`.
  Source at `/root/libhybris/`, build script: `client/build-libhybris-lindroid`.

### libhybris TLS problem -- SOLVED (2026-03-31)

Loading Android C++ libraries (`libc++.so`) crashes because bionic's inline
`mrs tpidr_el0` reads glibc's TLS pointer, then dereferences bionic-specific TLS
slots that don't exist in glibc's layout. The crash is in `__ctype_get_mb_cur_max`
reading `TLS_SLOT_BIONIC_TLS` (slot -1 at TP-8), which expects a pointer to a
~12KB `bionic_tls` struct.

The lindroid TLS thunk patcher (`HYBRIS_PATCH_TLS=1`) redirects `MRS TPIDR_EL0`
instructions to point at `tls_hooks[]`, but slot -1 maps to `tls_hooks[-1]` which
is before the array — NULL → crash. The thunk patcher was designed for GPU TLS
slots (3-4, positive offsets), not bionic's slot -1.

**Fix:** See "libhybris on Stock (Unpatched) Android" section above.

All existing libhybris deployments use patched bionic. Prior art:

| Project | Approach | Stock Android? |
|---------|----------|---------------|
| **Lindroid** | Custom ROM + LXC container + libhybris | No |
| **Droidian** | Halium (patched AOSP) + libhybris | No |
| **Sailfish OS** | Halium (patched AOSP) + libhybris | No |
| **Ubuntu Touch** | Halium (patched AOSP) + libhybris | No |

Bionic TLS slot layout (aarch64, for reference):

| Offset from TP | Slot | Name |
|---|---|---|
| TP - 8 | -1 | `TLS_SLOT_BIONIC_TLS` (~12KB struct pointer) |
| TP + 0 | 0 | `TLS_SLOT_DTV` |
| TP + 8 | 1 | `TLS_SLOT_THREAD_ID` |
| TP + 16 | 2 | `TLS_SLOT_APP` |
| TP + 24 | 3 | `TLS_SLOT_OPENGL` |
| TP + 32 | 4 | `TLS_SLOT_OPENGL_API` |
| TP + 40 | 5 | `TLS_SLOT_STACK_GUARD` |

Upstream references: libhybris issue #559 (TLS swap proposal), PR #575 (TLS
patcher v1), NotKit's `tls-patcher-v2` branch.

### libhybris build configuration

Both builds configured with:
```
--enable-arch=arm64 --enable-adreno-quirks --enable-property-cache
--with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64
--prefix=/usr/local
```

Source trees in chroot `/root/`: `libhybris/` (tawc fork, currently installed).
Headers from `Halium/android-headers` branch `halium-11.0`, version-bumped to 16
with GCC compatibility fixes.

### libhybris fork (wmww/libhybris)

Our fork at `https://github.com/wmww/libhybris` (master branch) is based on
`Linux-on-droid/libhybris` `lindroid-21` with two additional commits:

1. **TLS thunk patcher** (`b6e3de9`): Cherry-picked from `lindroid-drm` branch
   (original commit `75be4aa` by TheKit). Patches `MRS TPIDR_EL0` instructions
   in loaded bionic libraries to redirect TLS access through libhybris-managed
   slots. Enabled by `HYBRIS_PATCH_TLS=1`.

2. **bionic_tls compat** (`9517311`): Our fix for stock (unpatched) Android.
   Replaces flat `tls_hooks[16]` with a struct that has a `bionic_tls_ptr`
   pre-slot satisfying bionic's slot -1 TLS access. Lazily allocates 16KB
   zero-filled bionic_tls per thread. Wraps `pthread_create` to ensure
   bionic_tls is initialized on new threads.

The fork also includes lindroid-21's linker namespace bypass attempts
(`android_dlopen_ext`, `android_get_exported_namespace`) which are investigatory
for gralloc support.

### libsync fix for Android 16+ (2026-03-31)

The `libsync` component fails to build with Android 16 headers because the
version guard `#if (ANDROID_VERSION_MAJOR >= 10) && (ANDROID_VERSION_MAJOR < 12)`
excludes versions 12+, leaving `sync_get_fence_info` and `sync_file_info_free`
undeclared. Fix: change to `#if (ANDROID_VERSION_MAJOR >= 10)`. This is handled
automatically by the `build-libhybris-lindroid` script.

---

## Phase 1 Results (2026-03-28)

### What was built
- Android app scaffold (Kotlin, single Activity + SurfaceView)
- Rust JNI library (`server/compositor` crate, cdylib targeting `aarch64-linux-android`)
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
- Cross-process AHB round-trip: chroot client allocates AHB, renders with GPU,
  sends via `AHardwareBuffer_sendHandleToUnixSocket`, compositor receives and displays
- External mode: compositor listens on a Unix socket for cross-process clients
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

### Cross-process test (chroot GPU client)

In external mode, the chroot client allocates an AHB, renders with GPU, and sends
it to the compositor using the standard `AHardwareBuffer_sendHandleToUnixSocket` API.

To test:
```bash
# Ensure bind mounts include app data dir (for socket access)
adb shell "su -c 'mkdir -p /data/local/arch-chroot/data/data/me.phie.tawc && \
  mountpoint -q /data/local/arch-chroot/data/data/me.phie.tawc 2>/dev/null || \
  mount --bind /data/data/me.phie.tawc /data/local/arch-chroot/data/data/me.phie.tawc'"

# Enable external mode and launch compositor
adb shell "su -c 'touch /data/data/me.phie.tawc/external-mode'"
adb shell am force-stop me.phie.tawc
adb shell am start -n me.phie.tawc/.MainActivity
sleep 3

# Run chroot client
adb shell "su -c 'chroot /data/local/arch-chroot /bin/bash -lc \
  \"HYBRIS_PATCH_TLS=1 /tmp/test-ahb-gpu-client /data/data/me.phie.tawc/ahb-test.sock\"'"
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
cd server/compositor
cargo ndk --target arm64-v8a --platform 29 -- build --release
```
Output: `server/compositor/target/aarch64-linux-android/release/libcompositor.so`

**2. Copy .so and build APK:**
```bash
cp server/compositor/target/aarch64-linux-android/release/libcompositor.so \
   server/app/src/main/jniLibs/arm64-v8a/
export ANDROID_HOME=/home/ai/android-sdk
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
server/gradlew -p server assembleDebug
```
Output: `server/app/build/outputs/apk/debug/app-debug.apk`

**3. Deploy and run:**
```bash
adb install -r server/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n me.phie.tawc/.MainActivity
```

### Quick rebuild cycle (Rust changes only, run from repo root)
```bash
export ANDROID_NDK_HOME=/home/ai/android-sdk/ndk/27.2.12479018
cd server/compositor && cargo ndk --target arm64-v8a --platform 29 -- build --release && cd ../.. && \
ANDROID_HOME=/home/ai/android-sdk JAVA_HOME=/usr/lib/jvm/java-21-openjdk \
server/gradlew -p server assembleDebug && \
adb install -r server/app/build/outputs/apk/debug/app-debug.apk && \
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
