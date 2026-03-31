# GTK3 GLES Shader Problem

## Summary

GTK3 3.24.52 on Arch Linux ARM can connect to the tawc compositor and render
via SHM (software). When forced to use GL (`GDK_GL=always`), the full EGL
pipeline works -- context creation, window surface, AHB buffer allocation all
succeed -- but GTK3 compiles **desktop GL shaders** (`#version 150`) instead
of GLES shaders, which the Adreno GLES driver rejects with
"ERROR: Invalid #version".

## Root Cause

### The eglBindAPI Problem

GTK3 3.24's Wayland GL init (`gdk_wayland_display_init_gl`) does:

```c
if (GDK_DISPLAY_DEBUG_CHECK (display, GL_GLES))
{
    // GLES path: eglBindAPI(EGL_OPENGL_ES_API), use_gles = TRUE
}
else
{
    // Default path:
    if (!eglBindAPI (EGL_OPENGL_API))
        goto out_no_egl;        // <-- gives up entirely in 3.24.52!
    // use_gles stays FALSE
}
```

Stock Android GPU drivers only support `EGL_OPENGL_ES_API`. Our wrapper maps
`eglBindAPI(EGL_OPENGL_API)` -> `eglBindAPI(EGL_OPENGL_ES_API)` so it
"succeeds". GDK then sets `use_gles = FALSE`, creating desktop GL contexts
and compiling desktop GL shaders (`#version 150`).

If we instead reject `EGL_OPENGL_API` (return `EGL_FALSE`), this version of
GTK3 goes directly to `goto out_no_egl` -- it does **not** fall back to
GLES. The GLES fallback (`if (!eglBindAPI(GL)) try eglBindAPI(GLES)`) exists
in newer GTK3 commits but was NOT backported to 3.24.52.

### The Shader Mismatch

When `use_gles = FALSE`, GDK:
1. Creates contexts with `EGL_CONTEXT_MAJOR_VERSION_KHR=3, MINOR=2` (desktop GL)
2. Compiles shaders from `gl3-texture.vs.glsl` with `#version 150`
3. Never checks `glGetString(GL_VERSION)` to detect that the actual context is GLES

Even though the real driver returns "OpenGL ES 3.2 ..." from `glGetString`,
GDK trusts the `use_gles` flag set during init, not the runtime check.

### Why GDK_DEBUG=gles Doesn't Work

`GDK_DEBUG=gles` should trigger `GDK_DISPLAY_DEBUG_CHECK(display, GL_GLES)`
and take the GLES path. In testing:
- `GDK_DEBUG=gles` alone: GTK3 doesn't attempt GL at all (no tawc-egl log output)
- `GDK_DEBUG=gles` + `GDK_GL=always`: still produces `#version 150` shader errors

Possible explanations:
1. The debug flag name might not be `gles` in this build (though `strings`
   shows "gles" in the binary)
2. `GDK_GL=always` might override `GDK_DEBUG=gles` by forcing the non-GLES path
3. The `GL_GLES` check might be compiled out or not wired to the debug flag
   system in this Arch Linux ARM build

## What Works

- **SHM path**: GTK3 renders via wl_shm (CPU-uploaded buffers). Works perfectly.
  Rendered with magenta tint to distinguish from GPU path.
- **EGL pipeline**: With `GDK_GL=always`, GTK3 successfully:
  - Loads our `libEGL.so.1` via libepoxy
  - Queries client extensions (`EGL_EXT_platform_base`)
  - Calls `eglGetPlatformDisplayEXT` -> our wrapper
  - `eglInitialize` succeeds (reports EGL 1.5)
  - `eglBindAPI` "succeeds" (mapped to GLES)
  - `eglChooseConfig` finds 12 configs
  - `eglCreateContext` succeeds (GLES 3 context)
  - `eglCreateWindowSurface` allocates 1445x792 AHB buffers
  - Gets side-channel fd for AHB transfer
- **weston-simple-egl**: Full GPU-accelerated rendering works end-to-end

## Potential Solutions

### 1. Patch GTK3 to add GLES fallback (recommended)

Add the GLES fallback to this GTK3 build. The patch is small -- change
`gdk/wayland/gdkglcontext-wayland.c`:

```c
// Before (3.24.52):
if (!eglBindAPI (EGL_OPENGL_API))
    goto out_no_egl;

// After:
if (!eglBindAPI (EGL_OPENGL_API))
{
    if (!eglBindAPI (EGL_OPENGL_ES_API))
        goto out_no_egl;
    display_wayland->use_gles = TRUE;
}
```

This is the upstream fix from newer GTK3/GTK4. Could be applied as a
PKGBUILD patch in the chroot's Arch Linux ARM environment.

**Pros**: Correct fix, no wrapper hacks, benefits other GLES-only platforms.
**Cons**: Requires rebuilding GTK3 in the chroot (takes time, needs build deps).

### 2. Intercept glGetString via LD_PRELOAD shim

Write a small `libgl-version-shim.so` that intercepts `glGetString(GL_VERSION)`
and returns a desktop GL version string like "4.6" instead of "OpenGL ES 3.2".
This would make GDK think it has desktop GL. The GLES driver can actually
execute most desktop GL shader code if we also patch the `#version` directive.

Actually this doesn't work -- the problem is the shader `#version` lines, not
the GL version string. GDK checks `use_gles` to select which shaders to
compile, and `use_gles` is set from `eglBindAPI`, not `glGetString`.

### 3. Intercept glShaderSource to patch shader versions

Write an LD_PRELOAD shim (or add to our libEGL.so) that wraps `glShaderSource`
and rewrites `#version 150` -> `#version 300 es` (adding `precision mediump float;`
as needed). This is fragile but could work for GDK's simple shaders.

**Pros**: No GTK3 rebuild needed.
**Cons**: Fragile, needs to handle all shader differences between GL 3.2 and
GLES 3.0 (different qualifiers, built-in names, etc). GDK's shaders are simple
enough that this might work but it's ugly.

### 4. Export a glShaderSource wrapper from libEGL.so

Since libepoxy resolves GL functions via `dlsym` then `eglGetProcAddress`,
we can return a wrapper for `glShaderSource` from `eglGetProcAddress` that
patches shader source on the fly. Same fragility concerns as #3 but no extra
LD_PRELOAD.

### 5. Build a libGL.so wrapper that maps GL -> GLES

Instead of pretending we have desktop GL, build a `libGL.so` that translates
desktop GL calls to GLES. Projects like `gl4es` do this. Heavy approach but
would enable any desktop GL app, not just GTK3.

**Pros**: Universal solution for GL apps.
**Cons**: Massive effort, many edge cases, probably not worth it when GLES
wrappers work for most real apps.

### 6. Use GTK4 instead of GTK3

GTK4 has proper GLES fallback built in. If the target apps (like Firefox)
can use GTK4, this sidesteps the issue entirely.

**Pros**: Just works, no hacking.
**Cons**: Not all apps support GTK4. Firefox still uses GTK3.

### 7. Investigate why GDK_DEBUG=gles doesn't work

The `gles` debug flag SHOULD trigger the GLES path. If we can figure out why
it's not working (build config? flag not wired? wrong env var name?), setting
it might be the simplest fix.

Try:
- `GDK_DEBUG=gl-gles` (alternate name?)
- `G_MESSAGES_DEBUG=all GDK_DEBUG=gles` (enable debug output to see what flags are active)
- Check if `GDK_GL=always` conflicts with `GDK_DEBUG=gles`
- Disassemble `gdk_wayland_display_init_gl` to verify the debug check is present

## Recommendation

**Short term**: Solution #1 (patch GTK3). The patch is 5 lines and is the
correct upstream fix. Build GTK3 once in the chroot with the patch.

**Medium term**: Solution #3 or #4 (shader source rewriting). More robust
than relying on GTK3 patches, and handles other apps that assume desktop GL.

**Validation**: After any fix, test with:
```bash
GDK_GL=always gtk3-widget-factory  # should render without shader errors
GDK_GL=always firefox              # the real target
```

## Debugging Commands

```bash
# SHM rendering (works now):
LD_LIBRARY_PATH=/tmp/tawc-wsi:/usr/local/lib \
WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0 \
HYBRIS_PATCH_TLS=1 GDK_BACKEND=wayland XDG_RUNTIME_DIR=/tmp \
gtk3-widget-factory

# Force GL (hits shader error):
GDK_GL=always <same as above>

# Force GLES (should work but doesn't yet):
GDK_DEBUG=gles GDK_GL=always <same as above>

# Check what EGL functions are called:
# All calls logged to stderr with [tawc-egl] prefix

# Check compositor side:
adb logcat -s tawc-native | grep -v renderer_gles2_frame
```

## Related Files

- `client/tawc-wsi/tawc-egl.c` -- EGL wrapper (all Phase 4 changes)
- `server/compositor/src/compositor.rs` -- wl_data_device_manager stub
- `notes.md` -- Phase 4 implementation notes
