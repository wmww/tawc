# libhybris: EGL_MESA_platform_surfaceless

Give headless EGL clients a working display, mainly so Chromium's GPU
process gets GL on libhybris. Context and the alternative
(`--in-process-gpu`): [issues/chromium-gpu-process-gl-init-fails.md](../issues/chromium-gpu-process-gl-init-fails.md).

Goal: GPU rasterization/canvas/WebGL in Chromium-family apps with no
per-app flags. Not a goal: GPU compositing (stays SHM readback).

## Step 0: confirm the request

Before changing libhybris, trace what Chromium 153's GPU process passes
to `eglGetPlatformDisplay[EXT]` (LD_PRELOAD logger, or the shim from
the issue). Expect `EGL_PLATFORM_SURFACELESS_MESA` (only if ANGLE's own
client extensions include it) or `EGL_DEFAULT_DISPLAY` with no platform.
If it's anything else, revisit this plan.

## Change (deps/libhybris/hybris/egl/egl.c)

- `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`: append
  `EGL_EXT_platform_base EGL_MESA_platform_surfaceless`. Only advertise
  what we implement: no GBM, no `EGL_EXT_device_*`.
- `__eglHybrisGetPlatformDisplayCommon`: map
  `EGL_PLATFORM_SURFACELESS_MESA` to the `null` ws. Not the default
  ws: `HYBRIS_EGLPLATFORM=wayland` would open a Wayland connection, or
  abort without one (`waylandws_GetDisplay`). Spec requires
  `display_id == EGL_DEFAULT_DISPLAY`, else `EGL_BAD_PARAMETER`.
- Window and pixmap surfaces on that display fail with
  `EGL_BAD_NATIVE_WINDOW` / `EGL_BAD_NATIVE_PIXMAP` (the null ws's
  `CreateWindow` would otherwise pass the handle to the driver).
  Pbuffers and `EGL_KHR_surfaceless_context` pass through.
- `ws_init` allows one ws per process. A surfaceless request in a
  process that already opened the Wayland ws (or vice versa) fails with
  `EGL_BAD_PARAMETER`; that's acceptable, but keep it a clean error,
  not a crash. Doesn't affect `--in-process-gpu`, which uses Wayland.
- Update `TAWC_FORK.md` (amend into the final commit), rebuild with
  `scripts/build-libhybris.sh`, bump the pin in `deps/deps.list`, tag
  per CLAUDE.md. Only commit to the fork when asked.

## Verify

On the phone (and on Mali if available):

- Chromium `--ozone-platform=wayland`: no GPU-process exits; CDP
  `SystemInfo.getInfo` shows ANGLE on the vendor GPU, rasterization and
  WebGL enabled. Pages render (magenta tint expected).
- ChatGPT Electron app: same, and starts without the 3x retry delay.
- Regressions: existing libhybris integration tests (Firefox via AHB,
  GTK, Xwayland, vkcube/zink) still pass. Those use the Wayland or
  Android platforms, so they shouldn't see the change.
- Add an integration test that asserts Chromium's GPU process reports
  hardware GL, if one can run in reasonable time.

## Risks

- Clients that prefer surfaceless even when they have a display would
  switch to offscreen rendering. Uncommon; watch for it in GStreamer GL
  and Qt `offscreen`.
- Drivers without `EGL_KHR_surfaceless_context` or pbuffer configs get a
  display that can't do much; ANGLE should fail cleanly as it does now.

When done: delete this plan, move the outcome into notes (likely
notes/gpu-strategy.md and the fork's `TAWC_FORK.md`), and shrink the
issue to whatever remains (option 2 / GPU compositing).
