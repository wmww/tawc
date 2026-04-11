# Implementation Plan

## Build Toolchain & EGL Proof ✅ (2026-03-28)
- ✅ Android app scaffold with SurfaceView
- ✅ Cross-compile toolchain: cargo-ndk, NDK, libxkbcommon
- ✅ Rust JNI library: ANativeWindow -> EGL context via Smithay
- ✅ Render solid color via GlesRenderer + `eglSwapBuffers`

## AHB Buffer Sharing ✅ (2026-03-31)
- ✅ libhybris in chroot loading stock EGL/GLES (solved bionic TLS conflict)
- ✅ AHB allocate, CPU-fill, send over Unix socket
- ✅ Compositor receives AHB, imports as GL texture, displays
- ✅ Cross-process buffer round-trip confirmed

## Wayland Server ✅ (2026-03-31)
- ✅ Smithay patched (`libEGL.so.1` -> `libEGL.so`)
- ✅ `tawc_buffer_v1` custom protocol for AHB lifecycle
- ✅ Wayland display, xdg_shell, wl_output, listening socket
- ✅ Client connects from chroot, AHB -> GL texture -> composite -> present

## Robust EGL WSI Layer ✅ (2026-03-31)
- ✅ All 44 EGL 1.5 functions exported, universal forwarding
- ✅ Thread safety, TLS tracking, extension handling
- ✅ Buffer age, resize, surface slot reuse
- ✅ GTK3 compatibility (libepoxy, data_device_manager stub)
- ✅ Tested: weston-simple-egl, gtk3-widget-factory (SHM + GL)

## Touch Input ✅ (2026-04-01)
- ✅ Android MotionEvent -> JNI -> calloop channel -> wl_touch
- ✅ Multi-touch, coordinate transform (physical / scale -> logical)
- ✅ Immersive fullscreen (no dead zones)

## Text Input
Bridges Android InputConnection (soft keyboard) to `zwp_text_input_v3`.
See [notes/text-input.md](notes/text-input.md) for design.

**Custom text-input-v3 handler** ✅
- ✅ Implement `zwp_text_input_manager_v3` / `zwp_text_input_v3` directly
    (Smithay's built-in requires input-method Wayland client; ours is Android IME)
- ✅ Track instances per client, manage focus (enter/leave)
- ✅ Handle client requests: enable, disable, surrounding_text, content_type, commit

**Android InputConnection** ✅
- ✅ `TawcInputConnection` extending `BaseInputConnection`
- ✅ commitText -> commit_string + done
- ✅ setComposingText -> preedit_string + done
- ✅ deleteSurroundingText / sendKeyEvent(backspace) -> delete_surrounding_text + done
- ✅ SurfaceView focusable, returns TawcInputConnection from onCreateInputConnection

**Keyboard show/hide** ✅
- ✅ Reverse JNI channel (compositor -> Android via cached JavaVM + GlobalRef)
- ✅ Client enable -> showSoftInput; disable -> hideSoftInputFromWindow
- Map set_content_type -> EditorInfo.inputType (deferred: basic type works)
- Feed set_surrounding_text back to InputConnection (deferred: basic input works without it)

**Polish**
- Edge cases: focus changes during composition, multiple instances
- Test with Firefox, foot, GTK apps

## Testing Infrastructure ✅
- ✅ GTK3 debug app (`testing/gtk3-debug-app/`) -- C, built in chroot
- ✅ Integration test harness (`testing/integration/`) -- Rust, runs on host
- ✅ Broadcast-based input injection (reliable, bypasses IME)
- ✅ Text input tests (basic text, backspace, multi-word)
See [notes/testing.md](notes/testing.md) for details.

## Vulkan WSI
Buffer sharing uses the same AHB side-channel as EGL. The compositor doesn't need changes
— it already imports AHBs as GL textures regardless of how clients rendered them.

**Verify libhybris Vulkan basics**
- Load stock `libvulkan.so` via libhybris `android_dlopen()` from chroot
- Enumerate physical devices and confirm correct GPU shows up
- Check instance/device extensions, specifically `VK_ANDROID_external_memory_android_hardware_buffer`
- Test: can we create a VkInstance + VkDevice + allocate a VkImage backed by an AHB?
- Watch for bionic TLS issues (EGL needed a fix, Vulkan may need similar)

**Vulkan implicit layer** (`client/tawc-vulkan/`)
- Standard Khronos implicit layer (JSON manifest + shared library)
- NOT using libhybris's built-in Vulkan WSI (it uses Sailfish's `android_wlegl` protocol)
- Intercept instance-level: advertise `VK_KHR_wayland_surface`, hide `VK_KHR_android_surface`
- Intercept device-level: implement `VK_KHR_swapchain`
- Swapchain images: allocate AHBs, import as VkImage via
  `VK_ANDROID_external_memory_android_hardware_buffer` + `VK_KHR_external_memory`
- Present: explicit Vulkan fence/semaphore wait, then send AHB over side-channel socket
  (same `tawc_buffer_v1` protocol + `tawc_ahb_channel_v1` as EGL layer)
- Surface capabilities: report extent from `wl_egl_window` (or equivalent), FIFO present mode
- Thread safety: Vulkan's threading model differs from EGL — `vkAcquireNextImageKHR` and
  `vkQueuePresentKHR` may be called from different threads, side-channel fd needs guarding
- Reference: ARM's [vulkan-wsi-layer](https://github.com/ArmSoM/vulkan-wsi-layer) for
  layer structure (but it uses dmabufs which we can't use)

**Open questions needing research**
- GPU synchronization: EGL layer does `glFinish()` before sending AHB. Vulkan needs explicit
  fence wait. Does AHB cross-process sync work correctly with Vulkan fences through libhybris?
- Validation layers: may conflict with libhybris symbol interception. Test without them first.
- Format negotiation: which VkFormats map to AHB formats the compositor can import?
  (RGBA8 should work, others need testing)

**Testing**
- vkcube (basic triangle rendering + present)
- vkmark (stress test various rendering patterns)
- Real apps: Firefox WebGPU, games

## wl_keyboard (non-text keys)
Arrow keys, escape, tab, Ctrl+C/V/Z need wl_keyboard (no text-input-v3 equivalent).

- Solve xkbcommon on Android (XKB_CONFIG_ROOT -> chroot, or embed keymap)
- seat.add_keyboard() with US layout
- Map Android key events to wl_keyboard scancodes
- Modifier state tracking, Bluetooth keyboard support

## Multi-Window
- JNI callback for new xdg_toplevels -> spawn Activities
- One SurfaceView/EGLSurface per Activity
- Window lifecycle (map, unmap, close, resize)
- Popups composited onto parent (not separate Activities)

## Polish & Protocols
- Server-side decorations (xdg-decoration)
- Cursor rendering
- Fractional scaling (wp-fractional-scale)
- Clipboard bridge (wl_data_device <-> Android ClipboardManager)
- Non-root socket sharing (Binder fd passing)
