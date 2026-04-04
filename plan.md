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

## Vulkan WSI (stretch goal)
- Verify libhybris Vulkan with stock driver
- Vulkan implicit layer using VK_ANDROID_external_memory_android_hardware_buffer
- Test with vkcube, vkmark
