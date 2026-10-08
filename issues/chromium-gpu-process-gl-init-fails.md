# Chromium's GPU process fails GL init; falls back to software

Chromium and Electron start under tawcroot (OnePlus 9, Arch Linux ARM,
libhybris, 2026-09-28) but the GPU process exits during init and they
composite in software. Reported upstream as
https://github.com/wmww/tawc/issues/1 (ChatGPT Electron app, Pixel 9
Pro XL / Mali, Debian sid): that app refuses software fallback, so it
exits with `GPU process isn't usable`. Still reproduces on Pixel 9 Pro
(Mali-G715, ALARM, Chromium 153, 2026-10-08).

    ANGLE Display::initialize error 12289: Failed to get system egl display
    Initialization of all (2) EGL display types failed.
    Exiting GPU process due to errors during initialization

## Cause (verified, Chromium 153, 2026-09-28)

With the Wayland ozone platform the GPU process has no wl_display: ANGLE
(`FunctionsEGL::initialize`, GL backend) calls
`eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, NULL)`. It gates that on
`EGL_EXT_platform_base` + `EGL_KHR_platform_gbm` in the client extension
string. libhybris advertises neither (only `EGL_EXT_platform_wayland`/
`EGL_KHR_platform_wayland`, `deps/libhybris/hybris/egl/egl.c`), and
`__eglHybrisGetPlatformDisplayCommon` rejects GBM with
`EGL_BAD_PARAMETER`. The device-enumeration fallbacks need
`EGL_EXT_device_*`, also absent. So ANGLE never calls into the driver.

The browser process itself is fine: it uses
`EGL_PLATFORM_WAYLAND_KHR` with its real wl_display.

## Verified workaround

An LD_LIBRARY_PATH shim `libEGL.so.1` in front of hybris that appends
`EGL_EXT_platform_base EGL_KHR_platform_gbm` to the client extensions
and maps `EGL_PLATFORM_GBM_KHR`/NULL to hybris `eglGetDisplay(EGL_DEFAULT_DISPLAY)`
makes the GPU process initialize. `SystemInfo.getInfo` (CDP) then shows:

- GL: `ANGLE (Qualcomm, Adreno (TM) 660, OpenGL ES 3.2 ...)`,
  `gl=egl-angle,angle=opengl`
- rasterization, 2d_canvas, opengl: enabled; webgl/webgpu:
  `enabled_readback`
- gpu_compositing: still `disabled_software`

Pages render correctly (checked NTP).

## Proposed fix (libhybris fork)

In `egl.c`: advertise `EGL_EXT_platform_base EGL_KHR_platform_gbm`
(maybe `EGL_MESA_platform_surfaceless` too), and treat
`EGL_PLATFORM_GBM_KHR` with a NULL device (and
`EGL_PLATFORM_SURFACELESS_MESA`) as the default ws with a NULL native
display. Reject a non-NULL gbm_device with `EGL_BAD_PARAMETER`. Risk:
other clients that see `EGL_KHR_platform_gbm` may try GBM, but they
cannot open `/dev/dri` here anyway.

## Remaining: GPU compositing

Even with GL working, Chromium's Wayland GPU side needs a DRM render
node / GBM to allocate dmabufs for the compositor
(`drmGetDevices2() has not found any devices: Permission denied`), so
compositing stays software (readback to SHM). Real GPU compositing
needs a buffer-sharing path Chromium understands (dmabuf via
zwp_linux_dmabuf, or similar); not investigated.

## CPU backend avoids it

With the `cpu` graphics backend (no hybris on `LD_LIBRARY_PATH`, distro
Mesa llvmpipe) the GPU process initializes with no EGL errors and
Chromium renders (Pixel 9 Pro, 2026-10-08).

ChatGPT 26.1002.52244 (arm64 .deb unpacked into ALARM, `--no-sandbox`)
no longer dies with `GPU process isn't usable` even on libhybris: the
GPU process fails 3x, then it renders in software and shows its window.
Same on cpu (clean GPU init). Defaults to X11 (no GLX here);
`--ozone-platform=wayland` picks Wayland. Its "Organization settings
could not be loaded" dialog was its bundled `codex app-server` dying
under tawcroot (fixed; notes/tawcroot/sigsys-handler.md); it now
reaches the sign-in screen.

## ANGLE Vulkan (`--use-angle=vulkan`)

Fails with `VK_ERROR_INCOMPATIBLE_DRIVER` (-9) even though `vulkaninfo`
works. Chromium/Electron dlopen their own bundled `libvulkan.so.1`
(e.g. `/usr/lib/chromium/libvulkan.so.1`), not hybris's loader
replacement in `/usr/lib/hybris`, and no ICD manifest exists, so the
bundled Khronos loader finds no drivers. A temp manifest pointing at
`/usr/lib/hybris/libvulkan.so.1` (`VK_DRIVER_FILES=...`, it only exports
`vkGetInstanceProcAddr`) gets past this, but the GPU process then
crashes silently (`exit_code=7`). Not investigated further.

## Other notes

Under X11 (Xwayland) it fails earlier with `Could not load GLX entry
point glXCreateContext` (`GLX is not present` with the cpu backend).

Sandbox: Electron gets `--no-sandbox` from `ELECTRON_DISABLE_SANDBOX=1`
in the guest env; launcher entries for Chromium-family apps get it
appended (notes/launcher.md). Shell launches still need it by hand.
