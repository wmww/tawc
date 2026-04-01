# Compositor Architecture

## Module Layout

The compositor (`server/compositor/src/`) is split into:

- **lib.rs** -- JNI entry points + `run_compositor()` which sets up EGL, Wayland display,
  output, and listening socket, then hands off to the event loop.
- **event_loop.rs** -- Calloop-based event loop with four sources: Wayland display fd
  (dispatch client messages), listener socket (accept connections), touch input channel
  (Android touch -> wl_touch), frame timer (~60fps render loop).
- **input.rs** -- Touch input delivery from Android JNI to the compositor via calloop
  channel. Global `OnceLock<Sender>` allows JNI callbacks to send events cross-thread.
- **compositor.rs** -- `TawcState` (Wayland protocol state) and all Smithay handler trait
  impls. Does NOT hold rendering state.
- **render.rs** -- `RenderState` (GPU/EGL state), buffer import (AHB + SHM -> GL textures),
  frame rendering, frame callbacks, and the SHM magenta tint shader.
- **egl_android.rs** -- Raw EGL context creation and `AndroidNativeSurface` for Smithay.
- **ahb.rs** -- AHardwareBuffer allocation, CPU fill, and cross-process sharing.
- **gl_import.rs** -- Import AHB as GlesTexture via EGL/GL extensions.
- **protocol.rs** -- wayland-scanner generated code for tawc_buffer_v1.

## Key Design Decisions

- `TawcState` (protocol) and `RenderState` (GPU) are separate structs. Both live in
  `LoopData` which calloop passes to all callbacks.
- `dispatch_clients()` is called in BOTH the display fd callback AND the frame timer.
  The fd callback handles the fast path; the timer catch ensures no messages are delayed
  by more than one frame. Do not remove either call.
- Per-surface state structs (`SurfaceAhbState`, `SurfaceShmState`) live in TawcState
  but contain texture fields written by render.rs. This cross-cutting is intentional
  (avoids duplicate lookup tables).

## Wayland Protocols Implemented

- `wl_compositor`, `xdg_wm_base` (v6), `wl_shm`, `wl_seat`, `wl_output`
- `xdg_decoration` (always requests server-side, no actual decorations drawn)
- `wl_data_device_manager` (clipboard/DnD, stub implementations)
- Custom: `tawc_buffer_manager_v1` / `tawc_ahb_channel_v1` (GPU buffer sharing)

## tawc_buffer_v1 Protocol

- `tawc_buffer_manager_v1` (global): client binds, calls `get_channel(surface)` to
  create a per-surface AHB channel.
- `tawc_ahb_channel_v1`: compositor sends `channel_fd` event with a socketpair fd for
  AHB transfer. Client calls `attach(width, height)` after sending an AHB on the side
  channel. Standard `wl_surface.commit` triggers presentation.
- Side channel uses `AHardwareBuffer_sendHandleToUnixSocket` /
  `recvHandleFromUnixSocket` (multi-fd serialization that doesn't fit Wayland's wire format).

**Socket path:** `/data/data/me.phie.tawc/wayland-0` -- app's own data dir ensures write
access. Chroot clients access via root. Uses `ListeningSocket::bind_absolute()`.

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
| AHB side-channel socket complexity | Accepted | Necessary (AHB serialization uses multi-fd wire format). |
| Custom protocol breaks standard clients | Accepted | Clients use our WSI layer. `wl_shm` provides fallback. |
| SELinux blocks socket without root | Documented | Root: direct connect. No-root: Binder fd passing. |
| Smithay on Android | ✅ Solved | `default-features = false`, patched EGL loader. Proven in Phase 1. |
| Phantom Process Killer (Android 12+) | Open | Users need Developer Options toggle for Termux processes. |
| Vendor-specific GPU quirks | Open | Architecture is vendor-neutral. Each vendor needs testing. `wl_shm` fallback. |
| Freeform windowing not universal | Accepted | Phones: fullscreen Activities. Freeform on DeX/ChromeOS/Android 15+. |

## References

- [libhybris/libhybris](https://github.com/libhybris/libhybris) -- bionic compatibility layer
- [Smithay](https://github.com/Smithay/smithay) -- Rust Wayland compositor library
- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge)
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [Vulkan Layer mechanism](https://vulkan.lunarg.com/doc/view/latest/linux/loader_and_layer_interface.html)
- [ndk crate](https://crates.io/crates/ndk) -- Rust NDK bindings
