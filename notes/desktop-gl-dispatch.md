# Desktop GL via Mesa+Zink, dispatched in libhybris's libEGL

**Status:** design only, not implemented. Triggered by kitty (issue: kitty
segfaults on startup; root cause is desktop-GL-only). Same architectural gap
also blocks alacritty, supertuxkart-class games, and any other desktop-GL-only
app.

## The problem

Adreno's userspace driver (and every Android GPU driver) is GLES-only. libhybris's
libEGL is a thin wrapper over Android EGL, so when an app asks
`eglChooseConfig` for `EGL_RENDERABLE_TYPE = EGL_OPENGL_BIT`, no configs match
and the call returns 0. The well-behaved client response is to print an error
and bail; some clients (kitty's vendored GLFW) ignore the failure return and
crash on a NULL function pointer downstream.

The `LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib` we set in
`RootfsEnv.kt` deliberately prefers libhybris's libEGL over the distro's mesa
libEGL because libhybris is the hardware path. There is currently no way for an
app to opt into mesa without a wrapper that clears `LD_LIBRARY_PATH`.

The "rebuild distro packages against libGLESv2 instead of libGL" approach
doesn't help — apps that need desktop GL need it at the *source* level (e.g.
kitty's `#version 140` GLSL), not just at the linkage level. Re-linking
just moves the failure.

## Why the shim must be in libEGL

EGL is a single library that creates contexts of either API. There is no
`libEGL_GL.so` vs `libEGL_GLES.so` split. The choice is made by attributes
passed to `eglChooseConfig` / `eglBindAPI` / `eglCreateContext`. So routing
between mesa and libhybris has to live inside whichever library answers
`libEGL.so.1`. That's our shim.

GL function dispatch (`glFoo()` calls after context creation) does *not* need a
shim from us:

- `libGL.so.1` (desktop-GL only) → mesa, via libglvnd's existing per-context
  dispatcher. The current `gl-shims/libGL.so.1 → libGLESv2.so.2` symlink
  workaround goes away.
- `libGLESv2.so.2` (GLES only) → libhybris GLES, direct.
- libglvnd handles the corner case where an app linked `-lGL` but created a
  GLES context, by routing per-context to whichever vendor owns it. libhybris
  needs to be registered as a glvnd vendor for this — its source already has
  `hybris/egl/glvnd/` for exactly this purpose.

## Dispatcher design

libhybris's libEGL becomes a thin dispatcher that lazily loads two backends:

- **hybris backend**: today's libhybris EGL code, unchanged. Used for GLES.
- **mesa backend**: `dlopen("libEGL_mesa.so.0")` (or `libEGL.so.1` via
  glvnd, doesn't matter which). Used for desktop GL.

`EGLDisplay` / `EGLContext` / `EGLSurface` handles are wrapped in
`{ backend_tag, real_handle }` pairs. Every EGL entry point unwraps, looks
up the backend, and dispatches.

### Selection rules

The first call that reveals the app's intent picks the backend for that display:

| Call | Routes to |
|---|---|
| `eglBindAPI(EGL_OPENGL_API)` | mesa |
| `eglChooseConfig` with `EGL_RENDERABLE_TYPE` containing `EGL_OPENGL_BIT` | mesa |
| `eglCreateContext` with `EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR` set | mesa |
| Anything else | hybris (default; today's behavior preserved) |

Ambiguity (e.g. an app creates two displays, one for each API in the same
process) is allowed: each display is independent.

### What mesa actually renders with

Mesa uses Gallium with the **Zink** driver, which translates desktop GL → SPIR-V
→ Vulkan. The Vulkan loader finds `libvulkan.so.1`, which (given our existing
`LD_LIBRARY_PATH`) is libhybris's vulkan. So mesa+Zink ends up driving the
Adreno through the *same* vulkan ICD that native Vulkan apps already use:

```
desktop-GL app
  └─ glFoo()                         → mesa libGL (via libglvnd)
  └─ eglFoo()                        → libhybris libEGL (shim) → mesa libEGL
                                         └─ Gallium → Zink
                                              └─ vkFoo()
                                                   └─ libhybris libvulkan
                                                        └─ Android vulkan → Adreno HW
```

Plumbing required: an ICD JSON file pointing at libhybris's libvulkan, plus
`VK_ICD_FILENAMES` in `RootfsEnv.kt`. Mesa already supports llvmpipe as an
automatic fallback when Zink fails to init, so the path degrades gracefully on
the emulator (no working vulkan).

### Performance properties

- **GLES apps**: routed to libhybris → Adreno directly. No mesa code loaded for
  these apps. Identical to today.
- **Vulkan apps**: untouched. Identical to today.
- **Desktop-GL apps**: mesa+Zink → libhybris vulkan → Adreno. New capability.
  ~5-15% slower than hypothetical native desktop GL, but native desktop GL
  doesn't exist for us, so the meaningful comparison is HW-via-Zink vs
  software-via-llvmpipe, which Zink wins by orders of magnitude on anything
  non-trivial.
- **Compositor side**: desktop-GL frames arrive as Vulkan WSI buffers via the
  existing libhybris vulkan WSI path. No new compositor work.

The selection is **opt-in by the app's own EGL call**, not by anything we do
globally. Apps that don't ask for desktop GL don't pay any cost — mesa isn't
even loaded into their process.

## Code shape

All in libhybris. Roughly:

- `hybris/egl/egl_dispatcher.c` (new, ~600-800 LOC): wraps every EGL entry
  point. Per-display backend tag, dispatch table for each backend, mesa
  loader (~50 LOC), handle-wrapping infrastructure (~100 LOC).
- `hybris/egl/egl.c` (existing, minor changes): becomes the "hybris backend"
  whose entry points are called by the dispatcher instead of being exposed as
  the public symbols.
- `hybris/egl/glvnd/` (existing, currently unused): activate it so libhybris
  registers as a glvnd vendor. Removes the need for our `gl-shims/libGL.so.1
  → libGLESv2.so.2` symlink and the `libgl-shim.c` GLX-NULL stubs (libglvnd
  provides those properly).

Outside libhybris:

- An ICD JSON file pointing mesa's vulkan loader at libhybris's libvulkan.
  Either ship it in the libhybris asset tree and reference via
  `VK_ICD_FILENAMES`, or drop it into the rootfs's `/usr/share/vulkan/icd.d/`.
- `RootfsEnv.kt`: drop the `gl-shims` prefix from `LD_LIBRARY_PATH`, add
  `VK_ICD_FILENAMES`.
- `LibhybrisLinker.kt`: stop creating the `gl-shims` symlinks at install time;
  install the libhybris glvnd vendor `.json` instead.

Total new code: ~1000 LOC, all in libhybris. Minus deletions in `gl-shims`
the net is smaller.

## Implementation milestones

Each milestone is independently testable.

1. **Dispatcher with mesa = llvmpipe (no Zink yet).**
   Build the libEGL dispatcher. Default everything to the hybris branch
   (preserving today's behavior). Route `EGL_OPENGL_BIT` requests to mesa with
   `LIBGL_ALWAYS_SOFTWARE=1`. Verify kitty/alacritty render via llvmpipe. No
   regression for GLES/Vulkan apps. Activate libhybris's glvnd vendor; remove
   the `gl-shims` `libGL.so.1` symlink. Confirm Firefox/GTK still work.

2. **Vulkan ICD plumbing.**
   Wire `VK_ICD_FILENAMES` (or the standard `/usr/share/vulkan/icd.d/`
   location) so mesa's vulkan loader finds libhybris's libvulkan. Verify with
   `vulkaninfo` that mesa sees the Adreno through libhybris. No app behavior
   change yet.

3. **Enable Zink.**
   Drop `LIBGL_ALWAYS_SOFTWARE=1`. Mesa should now use Zink. Verify
   GL_RENDERER on a desktop-GL app reports Zink-on-Adreno. If Zink fails to
   init, mesa falls back to llvmpipe automatically and we investigate
   libhybris vulkan extension gaps separately.

4. **Investigate the kitty + mesa + tawcroot crash.**
   Mesa-rendered kitty currently crashes inside `libtawcroot.so+0x3988`
   regardless of Zink/llvmpipe. Probably a tawcroot syscall handler issue
   triggered by something in the mesa init path. Independent of the
   dispatcher work; could be done at any milestone.

## Risks and open questions

- **Zink + libhybris vulkan compatibility.** Zink wants a fairly thick set of
  Vulkan features. Adreno's vulkan is feature-rich so likely OK, but the
  shape of the failure if a needed extension is missing is "Zink refuses to
  init, fall back to llvmpipe" — graceful degrade, not a crash. Fixable in
  libhybris if it bites.
- **Mesa's wl_drm/dmabuf on the compositor side.** Zink presents via Vulkan
  WSI, which uses libhybris's existing vulkan WSI path. Compositor probably
  needs nothing new but worth a smoke test once we have a working desktop-GL
  app.
- **`eglGetProcAddress` ambiguity.** The function pointer returned should
  itself dispatch per-current-context. Mirrors how libglvnd does it.
- **Multi-context apps that mix backends** in the same process. Allowed by
  the design; rare in practice.
- **The kitty + tawcroot crash** isn't blocked by any of this and probably
  isn't dispatcher-related, but is worth a separate investigation since it'd
  block the obvious "kitty works now" demo.
