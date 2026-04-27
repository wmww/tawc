# Compositor Architecture

## Module Layout

The compositor (`server/compositor/src/`) is split into:

- **lib.rs** -- JNI entry points + `run_compositor()` which sets up EGL, Wayland display,
  output, and listening socket, then hands off to the event loop.
- **event_loop.rs** -- Calloop-based event loop. Sources: Wayland display fd
  (dispatch client messages), listener socket (accept connections), touch input channel
  (Android touch -> wl_touch), text-input channel, state-query channel, surface-event
  channel (per-Activity `SurfaceView` lifecycle), frame timer (~60 fps render loop).
  Also owns `set_host_foreground` and the per-host render iteration.
- **host.rs** -- `OutputHost` (per-Activity render target — owns ANativeWindow + EGLSurface
  + foreground bool), `SurfaceEvent` (Register / SurfaceChanged / SurfaceDestroyed /
  ActivityDestroyed / FocusChanged), and the JNI -> compositor surface-event channel.
  See [multi-activity.md](multi-activity.md).
- **input.rs** -- Touch input delivery from Android JNI to the compositor via calloop
  channel. Events carry the `ActivityId` of the SurfaceView that produced them.
  Global `OnceLock<Sender>` allows JNI callbacks to send events cross-thread.
- **text_input.rs** -- `zwp_text_input_v3` server impl bridging Android InputConnection.
- **compositor.rs** -- `TawcState` (Wayland protocol state, including `hosts`,
  `toplevel_to_host`, `single_activity_mode`, `foreground_host`) and all Smithay handler
  trait impls including `assign_toplevel_to_host` (the multi-window policy). Does NOT
  hold rendering state.
- **render.rs** -- `RenderState` (GPU/EGL state), buffer import (AHB + SHM -> GL textures),
  per-host frame rendering, foreground-gated frame callbacks, the SHM magenta tint /
  wlegl-opaque shaders, and `create_egl_surface_for_window` for binding new hosts.
- **background.rs** -- Black-to-turquoise gradient drawn behind every frame.
- **egl_android.rs** -- Raw EGL context creation (with `EGL_KHR_surfaceless_context`
  default-current) and `AndroidNativeSurface` for Smithay.
- **wlegl.rs** -- `android_wlegl` server: reconstruct client-allocated gralloc buffers
  into AHardwareBuffers via the C helper, expose them as wl_buffers.
- **gl_import.rs** -- GL/EGL extension loading and texture import. Imports AHardwareBuffers
  as GlesTextures and provides shared GL utilities (dummy textures) to other modules.
- **protocol.rs** -- wayland-scanner generated code for `android_wlegl`.
- **native/wlegl_import.c** -- ~50-line C helper calling
  `AHardwareBuffer_createFromHandle(REGISTER)` (dlsym'd from libnativewindow.so).

Kotlin side (`server/app/src/main/java/me/phie/tawc/`):

- **MainActivity.kt** -- Launcher (only Activity in `category.LAUNCHER`). Starts
  `CompositorService` so the Wayland socket is up, then renders the installed-distros
  list + "Install new distro" button. `CompositorActivity` is spawned indirectly: a
  Wayland app mapping a window triggers the native `spawnActivity` reverse-JNI.
- **ui/Scaffold.kt** -- Helpers shared by the non-compositor activities — builds the
  `MaterialToolbar` (with back/up arrow on child screens) plus the content column, and
  exposes `primaryButton` (yellow-orange accent) / `destructiveButton` (red) factories.
- **compositor/CompositorService.kt** -- Foreground service (`specialUse` type) that owns
  the Rust compositor thread. Activities bind to it; it tracks them by `activityId` so
  reverse-JNI calls can find the right Activity.
- **compositor/CompositorActivity.kt** -- One per Wayland window. Reads `activityId` from
  `intent.data?.lastPathSegment` (UUID under `tawc://activity/<id>`); falls back to
  `"primary"` for the launcher path. Forwards `SurfaceHolder` / touch / focus events
  to native tagged with the id.
- **compositor/NativeBridge.kt** -- JNI surface, `attachService` to capture app context
  for the `spawnActivity` / `finishActivity` reverse-JNI callbacks.

## Key Design Decisions

- `TawcState` (protocol) and `RenderState` (GPU) are separate structs. Both live in
  `LoopData` which calloop passes to all callbacks.
- `dispatch_clients()` is called in BOTH the display fd callback AND the frame timer.
  The fd callback handles the fast path; the timer catch ensures no messages are delayed
  by more than one frame. Do not remove either call.
- Per-surface state structs (`SurfaceWleglState`, `SurfaceShmState`) live in
  `TawcState` but contain texture fields written by `render.rs`. This cross-cutting
  is intentional (avoids duplicate lookup tables).

## Wayland Protocols Implemented

- `wl_compositor`, `xdg_wm_base` (v6), `wl_shm`, `wl_seat`, `wl_output`
- `wp_viewporter` (required by Firefox/WebRender — see rendering notes)
- `xdg_decoration` (always requests server-side, no actual decorations drawn)
- `wl_data_device_manager` (clipboard/DnD, stub implementations)
- `zwp_text_input_v3` (custom impl bridging Android IME)
- `android_wlegl` (libhybris's standard GPU buffer sharing protocol; client-side
  allocation only — `get_server_buffer_handle` is rejected)

**Socket path:** `/data/data/me.phie.tawc/wayland-0` -- app's own data dir ensures
write access. Chroot clients access via root. Uses `ListeningSocket::bind_absolute()`.

## Smithay Integration

Using Smithay 0.7 from a custom fork (`wmww/smithay`, branch `tawc-patches`).

**Features:** `wayland_frontend`, `renderer_gl`, `desktop`. Avoids all Linux-specific
backends (DRM, GBM, libinput, udev, libseat).

**Patch:** `libEGL.so.1` -> `libEGL.so` for Android (in `src/backend/egl/ffi.rs`).

**wayland-rs:** Pure Rust Wayland protocol implementation (do NOT enable `server_system`
feature -- no `libwayland-server.so` needed).

**Key APIs used:**
- `EGLContext::from_raw(display, config_id, context)` -- wraps pre-existing EGL context
- `GlesRenderer` -- composites GL textures. Has `ImportDma` for dmabufs but NO
  AHardwareBuffer support -- our `gl_import.rs` handles AHB import manually.
- `DisplayHandle::insert_client(stream, data)` -- accepts new Wayland client connections

**Native dependencies:**
| Dependency | Status |
|---|---|
| EGL/GLESv2 | Provided by Android NDK |
| libxkbcommon | Cross-compiled for aarch64-linux-android |
| libwayland | Not needed (pure Rust backend) |
| libdrm/libgbm | Not needed (disabled features) |

## Toplevel Lifecycle

Toplevels are retained as long as `ToplevelSurface::alive()` returns true (not based on
whether they have buffers). SHM state is cleaned up when the toplevel dies. This is
important because SHM clients don't create buffer state until after the first configure
event, so buffer-based retain logic would immediately remove new toplevels.

## Known Risks and Constraints

| Risk | Status | Mitigation |
|---|---|---|
| libhybris can't load stock EGL/GLES | ✅ Solved | Battle-tested (Sailfish OS). Proven in Phase 2. |
| libhybris Vulkan varies by vendor | Open | Stretch goal. EGL/GLES covers most apps. Mali needs unmerged PRs (#604, #607). |
| Stock driver needs Binder/gralloc | ✅ Solved | Bind-mount `/vendor`, `/system`, `/system_ext`, `/dev/binderfs`. |
| `eglGetNativeClientBufferANDROID` not available | Low risk | Widely available Android 8+. Runtime-check required. |
| `android_wlegl` is libhybris-specific | Accepted | All chroot apps reach the GPU via libhybris; `wl_shm` is the standard fallback. |
| SELinux blocks socket without root | Documented | Root: direct connect. No-root: Binder fd passing. |
| Smithay on Android | ✅ Solved | `default-features = false`, patched EGL loader. Proven in Phase 1. |
| Phantom Process Killer (Android 12+) | Open | Users need Developer Options toggle for Termux processes. |
| Vendor-specific GPU quirks | Open | Architecture is vendor-neutral. Each vendor needs testing. `wl_shm` fallback. |
| Freeform windowing not universal | Accepted | Phones: fullscreen Activities. Freeform on DeX/ChromeOS/Android 15+. See [multi-activity.md](multi-activity.md). |

## References

- [wmww/libhybris](https://github.com/wmww/libhybris) -- our fork of libhybris (bionic compatibility layer), local checkout at `./libhybris`
- [Smithay](https://github.com/Smithay/smithay) -- Rust Wayland compositor library
- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge)
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [Vulkan Layer mechanism](https://vulkan.lunarg.com/doc/view/latest/linux/loader_and_layer_interface.html)
- [ndk crate](https://crates.io/crates/ndk) -- Rust NDK bindings
