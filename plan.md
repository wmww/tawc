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

## libhybris CFI Workaround ✅ (2026-04-11)
- ✅ `hybris/common/q/cfi_bypass.c` finds `__cfi_slowpath` via libdl.so's dynsym and patches it to `ret`
- ✅ Called from `android_linker_init()` (early) and `link_image()` (after each lib loads, where libdl is finally mapped); idempotent via static flag
- ✅ 16K-page safe (`sysconf(_SC_PAGESIZE)`), no W+X window (RW → write → RX mprotect sequence)
- ✅ Removed `patch_bionic_cfi()` and `<sys/mman.h>` include from `client/tawc-wsi/tawc-egl.c`
- ✅ Integration tests pass (text-input, click-cursor, firefox); documented in `libhybris/TAWC_FORK.md`

## Migrate to libhybris Wayland EGL Platform ✅ (2026-04-15)
- ✅ libhybris built with `--enable-wayland --disable-wayland_serverside_buffers` (no glvnd; pulling Mesa GLX in breaks Firefox).
- ✅ `android_wlegl` protocol dispatch in compositor (Rust + ~50-line C helper calling `AHardwareBuffer_createFromHandle`)
- ✅ libhybris fork: AHB gralloc backend (`AHardwareBuffer_*` via libnativewindow.so) to produce modern-format handles — without it the stock vendor gralloc1 path returns handles the Android-side mapper rejects on Android 12+ devices. Plus shared-queue and `queueBuffer`-attach patches needed by Firefox/Adreno. See `libhybris/TAWC_FORK.md`.
- ✅ Chroot env: `HYBRIS_EGLPLATFORM=wayland`, `LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib`.
- ✅ Dead code removed: `client/tawc-wsi/` directory (tawc-egl.c ~1500 lines), `server/compositor/protocols/tawc_buffer_v1.xml`, `src/ahb.rs`, the Unix-socket side-channel, the `surface_ahb` HashMap, the legacy `import_pending_ahbs` render path.
- ✅ Tiny GL shims (`client/libgl-shim.c`, `client/libglesv2-shim.c`, ~30 lines each) built host-side by `bash client/build-libhybris-aarch64` and shipped in the APK. Firefox/glxtest and GTK/libepoxy probe libGL.so/libGLESv2.so by name and need GLX symbols stubbed so Mesa GLX (broken in chroot) doesn't get reached. See `notes/wsi-layer.md` "Why GL shims still exist".
- ✅ Integration tests (text-input SHM, click-cursor AHB, firefox AHB) all pass.

## Vulkan WSI ✅ (2026-04-20)
libhybris has built-in Wayland Vulkan WSI (`vulkanplatform_wayland.so`). It intercepts
`vkCreateWaylandSurfaceKHR`, remaps to `vkCreateAndroidSurfaceKHR`, and uses the same
`android_wlegl` protocol for buffer sharing. No custom implicit layer needed — once the
Wayland platform migration is done and the compositor serves `android_wlegl`, Vulkan
clients should work via `HYBRIS_VULKANPLATFORM=wayland`.

- ✅ `vulkan` subdir built by `bash client/build-libhybris-aarch64`; ships `libvulkan.so.1`
  (shadows `vulkan-icd-loader` via `LD_LIBRARY_PATH`) and `libhybris/vulkanplatform_wayland.so`
- ✅ `vulkan.c` compiles with vulkan-headers 1.4.341 (Cuda NV extension guard switched
  from `VK_HEADER_VERSION >= 269` to `#ifdef VK_NV_cuda_kernel_launch` — the NV Cuda
  symbols got pulled from the C core headers in recent vulkan-headers releases)
- ✅ `vulkaninfo --summary` passes: Adreno Vulkan driver loaded via `android_dlopen`,
  `VK_KHR_wayland_surface` advertised, integration test `test_vulkaninfo_loads_android_driver`
- ✅ `vkcube` renders correctly on OnePlus 9. Two libhybris fixes required:
  (a) `NATIVE_WINDOW_BUFFER_AGE=0` (committed as the Firefox flicker fix; Adreno's
  Vulkan WSI was treating the hardcoded age=2 as "preserved content"), and (b)
  spec-correct `currentExtent = {0xFFFFFFFF, 0xFFFFFFFF}` (undefined extent) +
  `vkCreateSwapchainKHR` interception to resize `WaylandNativeWindow` to match
  the app's `imageExtent`. The dispatch in `vulkan.c` intercepts `vkGetDeviceProcAddr`
  and `vkGetInstanceProcAddr` to hook swapchain creation and surface capabilities.
- GPU synchronization: Android Vulkan driver handles fences internally via `android_wlegl`
- Format negotiation: which VkFormats map to gralloc formats the compositor can import?
- Real apps: Firefox WebGPU, games

## Multi-Window ✅ (2026-04-26, phases 0-7)
Each Wayland toplevel becomes its own Android task / recents card.
See [notes/multi-activity.md](notes/multi-activity.md) for the design and
the as-built notes.

- ✅ Compositor moved into foreground `CompositorService` (specialUse
  type, `START_STICKY`). `nativeStartCompositor` runs once per process
  and outlives any single Activity.
- ✅ `OutputHost` per Activity (`server/compositor/src/host.rs`) owns
  the EGLSurface + ANativeWindow + dimensions. Multiple hosts make
  current per render via the calloop frame timer.
- ✅ `toplevel_to_host: HashMap<WlSurface, ActivityId>` filters the
  render walk; popups/subsurfaces inherit their root's host.
- ✅ Policy in `TawcState::assign_toplevel_to_host` — child toplevels
  ride on parent's host; otherwise mint a fresh `ActivityId` and ask
  Kotlin to spawn an Activity for it. `single_activity_mode` flag
  (default `false`) collapses everything onto the existing host.
- ✅ `CompositorActivity` reads its id from `intent.data?.lastPathSegment`
  (URI scheme `tawc://activity/<id>`). Manifest uses
  `documentLaunchMode="intoExisting"` + `taskAffinity=""` so each id
  gets its own task.
- ✅ Touch input tagged with `activity_id` per event; routed to first
  alive toplevel of THAT host. `nativeOnActivityFocusChanged` drives
  per-host keyboard / text-input focus. (Removed the
  `touch-focus-single-window-only` issue.)
- ✅ Suspend round-trip: hosts carry a `foreground` bool; transitioning
  it sends `Activated`/`Suspended` configures to assigned toplevels.
  Backgrounded hosts skip rendering and frame callbacks. Last toplevel
  per host dying calls `finishAndRemoveTask` so the recents card goes
  away with the window.
- Polish (TODO): per-task labels/icons via `set_title`/`set_app_id` →
  `ActivityManager.TaskDescription`, refused-close handling, settings
  UI for the single-Activity-mode toggle, freeform-windowing story.
  Pending-host config (Activity exists, surface arrives later) is
  smoothed over by the calloop channel buffering events; explicit
  HostState::Pending might still be worth adding for diagnostics.

## Xwayland (bionic baseline) ✅
Bionic-cross-compiled Xwayland-24.1.11 spawned by the compositor; X
clients connect via `:0` and render through the `wl_shm` path. See
[notes/xwayland.md](notes/xwayland.md) for the dep tree, bionic
patches, packaging, and the AHB-everywhere Phase 2 plan that follows.

- ✅ Cross-compile against the NDK `aarch64-linux-android29` toolchain.
- ✅ Vendored bionic compat patches (FIONREAD inline, `link()`→`symlink()`,
  `/tmp` prefix swap, libxfont2 OPEN_MAX, xorgproto passwd shape,
  Xwayland setuid drop). Mostly forward-ports of termux-packages.
- ✅ Pack into the APK (`packXwayland`), extract from
  `CompositorService.onCreate`.
- ✅ Compositor-side `XWayland::spawn` + `X11Wm` wiring; tawc-patches
  on smithay for the runtime-dir + `-ac` argv.
- Server-rendered X pixmaps flow as `wl_shm` (preserves magenta tint).
- Phase 2 (next): libhybris EGL-on-X11 platform plugin (chroot-side) +
  `xwayland-tawc.c` (server-side AHB shipping via `android_wlegl`) +
  `TAWC-DRI` X11 extension. AHBs end-to-end. See the "Phase 2
  implementation order" section in `notes/xwayland.md` for the build
  sequence.
- Phase 3 (probably skip): server-side GL acceleration.

## wl_keyboard (non-text keys)
Arrow keys, escape, tab, Ctrl+C/V/Z need wl_keyboard (no text-input-v3 equivalent).

- Solve xkbcommon on Android (XKB_CONFIG_ROOT -> chroot, or embed keymap)
- seat.add_keyboard() with US layout
- Map Android key events to wl_keyboard scancodes
- Modifier state tracking, Bluetooth keyboard support

## Polish & Protocols
- Server-side decorations (xdg-decoration)
- Cursor rendering
- Fractional scaling (wp-fractional-scale)
- Clipboard bridge (wl_data_device <-> Android ClipboardManager)
- Non-root socket sharing (Binder fd passing)

## tawcroot — systrap-based proot replacement
From-scratch C implementation of a fake chroot using seccomp `RET_TRAP`
+ in-process `SIGSYS` rewriting (no ptrace, no tracer process). Goal:
strict superset of proot's TAWC use case at ~1.5–3× native instead of
proot's 5–10× — primarily to unblock painfully slow `pacman` ops on
the rootless install method. See [notes/tawcroot.md](notes/tawcroot.md)
for the full design.

- Phase 1 — host-side MVP (Linux x86_64 dev box): scaffold `tawcroot/`,
  argv parse, rootfs `O_PATH` fd, seccomp filter for openat/stat/access,
  SIGSYS handler, x86_64 `arch_*` register helpers, basic path
  translation. Run `/bin/sh -c 'ls /'` against a fake rootfs.
- Phase 2 — execve handling: ELF/`PT_INTERP` parse, re-exec-into-self
  trampoline so the SIGSYS handler survives `execve`, multi-process
  correctness, `/proc/self/exe` synthesis, `getcwd` reverse-translate.
- Phase 3 — full path-syscall surface: every entry in `notes/tawcroot.md`
  §"Which syscalls need translation" wired through the dispatch table.
- Phase 4 — emulator integration (x86_64 AVD): `client/build-tawcroot`,
  jniLib packaging as `libtawcroot.so`, `TawcrootMethod.kt` next to
  `ProotMethod.kt`, dispatch in `client/tawc-chroot-run`, wrapper
  script. Run `pacman -Syu` to completion; verify the lp64-`access`-on-
  x86_64 stacked-filter case (only fires on x86_64).
- Phase 5 — aarch64 port (real device): `arch/aarch64.h` + stub asm,
  libhybris/Firefox smoke tests, measure `pacman -Syu` wall-time vs
  proot.
- Phase 6 — hardening + perf: stacked-filter edge cases, Firefox
  sandbox specifics, tune the trapped-syscall set.
