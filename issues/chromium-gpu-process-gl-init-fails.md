# Chromium's GPU process fails GL init; falls back to software

Chromium and Electron start under tawcroot with libhybris, but the GPU
process exits during init and they render and composite in software
(OnePlus 9 / Adreno 660, Pixel 9 Pro / Mali-G715; ALARM, Chromium 153;
last seen 2026-10-08). Upstream report: https://github.com/wmww/tawc/issues/1.

    ANGLE Display::initialize error 12289: Failed to get system egl display
    Initialization of all (2) EGL display types failed.
    Exiting GPU process due to errors during initialization

Electron apps survive it now: ChatGPT 26.1002.52244 (arm64 .deb in
ALARM, `--no-sandbox`) fails the GPU process 3x, then renders in
software. With the `cpu` backend (distro Mesa llvmpipe) GPU init is
clean, but that's software too.

## Cause (per upstream source; re-verify on device)

In multi-process mode the GPU process has no wl_display
(`connection_` is null). `GLOzoneEGLWayland::GetNativeDisplay`
(`ui/ozone/platform/wayland/gpu/wayland_surface_factory.cc`) then picks
GBM only if it opened a GBM device (it can't: no `/dev/dri`), else
`EGL_PLATFORM_SURFACELESS_MESA` if advertised, else
`EGL_DEFAULT_DISPLAY`. ANGLE's `FunctionsEGL::getPlatformDisplay`
requires the system EGL to advertise `EGL_EXT_platform_base` and the
matching platform extension; libhybris advertises only the Wayland
ones (`deps/libhybris/hybris/egl/egl.c`, `eglQueryString`), so ANGLE
gets no display. Which platform Chromium 153 actually requests hasn't
been confirmed by tracing.

An earlier LD_LIBRARY_PATH shim that advertised `EGL_KHR_platform_gbm`
and mapped GBM/NULL to `eglGetDisplay(EGL_DEFAULT_DISPLAY)` made GL
init succeed (ANGLE on Adreno 660; rasterization, canvas, WebGL on;
`gpu_compositing` still `disabled_software`). Faking GBM is not the
fix: we can't accept a real `gbm_device`, and notes/xwayland.md already
rejected GBM aliasing. Also note that with `HYBRIS_EGLPLATFORM=wayland`
(RootfsEnv.kt) the default display opens its own Wayland connection
(`waylandws_GetDisplay`), and aborts if it can't.

## Option 1: surfaceless in libhybris

Advertise `EGL_MESA_platform_surfaceless` and back it with the `null`
ws. Fixes GL (GPU rasterization/WebGL) for every Chromium-family app
however launched; compositing stays software (readback to SHM, magenta
tinted). Plan: [plans/libhybris-surfaceless.md](../plans/libhybris-surfaceless.md).

## Option 2: `--in-process-gpu`

GPU work then runs in the browser process, which has the real
wl_display, so `GetNativeDisplay` returns the Wayland platform, which
libhybris supports. If Chromium then renders through `wl_egl_window`,
frames go AHB → `android_wlegl` like Firefox: real GPU compositing with
no dmabuf. Untested; Chromium may still insist on its NativePixmap
(dmabuf) path. Costs: only launcher-started apps get the flag, a GPU
crash kills the app, and it's a less-tested upstream mode. Try after
option 1; ship in the launcher only if it really gets GPU compositing.

`zwp_linux_dmabuf_v1` is not an option: stock drivers can't import
dmabufs (notes/gpu-strategy.md).

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
Electron defaults to X11 here; `--ozone-platform=wayland` picks Wayland.

Sandbox: Electron gets `--no-sandbox` from `ELECTRON_DISABLE_SANDBOX=1`
in the guest env; launcher entries for Chromium-family apps get it
appended (notes/launcher.md). Shell launches still need it by hand.
