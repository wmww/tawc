# Custom desktop-GL-on-GLES3 translator

**Status:** design sketch, not started. Follows the 2026-07-06 gl4es
evaluation (verdict recorded in
[notes/gpu-strategy.md](../notes/gpu-strategy.md)): translating desktop
GL to GLES at the call level demonstrably works on our stack, but gl4es
caps at GL 2.1/GLSL 1.20, which misses every app we actually care about
(kitty, modern toolkits, GL 3.x games). This plan is the "do it
ourselves, against ES 3.x" option.

## Why it's feasible

- Our devices' GLES is modern even when their Vulkan isn't: Adreno 660
  exposes **ES 3.2**, which is feature-wise roughly GL 4.3-class
  (geometry/tessellation shaders, instancing, UBOs/SSBOs, MRT, compute).
  ES 3.0 was itself derived from GL 3.3-era desktop GL, so for a **core
  profile** target most API calls map 1:1. No fixed-function emulation —
  that's the bulk of gl4es's complexity and none of our targets need it.
- The historically-hard part, the shader dialect (GLSL 330+ →
  ESSL 300/310/320), now has an off-the-shelf recipe:
  **glslang** (GLSL → SPIR-V) → **SPIRV-Cross** (SPIR-V → ESSL). The
  Minecraft-community wrappers (MobileGlues: GL 4.0 on ES 3.2; LTW)
  prove this pipeline works on Android GLES drivers. They're
  app-shaped and variously licensed (check before borrowing code), but
  they de-risk the architecture.
- Windowing is already solved in-tree: libhybris gives us
  EGL/ES 3.2 contexts on Wayland (`wayland` ws) and on XWayland
  (`x11` ws + TAWC-DRI present, proven end-to-end by the gl4es spike
  and `test_es2gears_x11_renders_via_ahb`). The translator is a pure
  client-side library; the compositor needs nothing new.

## Shape

Target: **GL 3.3 core profile** honestly advertised (extensions added
only as implemented), stretch goals from 4.x where ES 3.2 has the
feature. No compatibility profile, no GL ≤ 2.1 (gl4es territory,
rejected).

Three layers:

1. **Entry points / windowing glue.**
   - `libGL.so.1` exporting GL entry points + GLX-on-EGL for XWayland
     clients (kitty via glfw-x11, games). Reuse the spike's learnings:
     plain `eglGetDisplay(x11_display)` + `HYBRIS_EGLPLATFORM=x11`; do
     hardware probing against the app's own display, not
     `EGL_DEFAULT_DISPLAY` (gl4es's probe-vs-window display-mapping
     crash; see gpu-strategy note).
   - A small interposing `libEGL.so.1` for Wayland-native clients:
     advertise `EGL_OPENGL_BIT` in configs, accept
     `eglBindAPI(EGL_OPENGL_API)`, map desktop-GL context attribs
     (3.3 core) to `EGL_CONTEXT_CLIENT_VERSION=3` ES contexts, pass
     everything else through to libhybris. gl4es's 268-line
     `EGL_WRAPPER` is the existence proof for this shape.
2. **Dispatch + state shim.** Per-context state, thread-local current
   context from day one (gl4es's single-global-context design is its
   second-worst flaw and disqualifies it as a base). Most entries are
   thin passthroughs; the real work is a long tail of semantic
   mismatches, roughly in priority order:
   - `glMapBuffer` (full-buffer, and `GL_MAP_PERSISTENT_BIT`) → ES
     ranged mapping / shadow-copy emulation.
   - `glGetTexImage` / PBO readback paths → FBO + `glReadPixels`.
   - Enum/format translation tables (sized internal formats mostly
     align in ES3; the exceptions are enumerable).
   - `GL_TEXTURE_1D` (emulate as Nx1 2D), `glPolygonMode(GL_LINE)`
     (no ES equivalent — stub or geometry hack), `glLogicOp` (absent),
     `SAMPLES_PASSED` occlusion queries (ES has `ANY_SAMPLES_PASSED`),
     `gl_ClipDistance` (`EXT_clip_cull_distance` where present),
     seamless cubemaps (always-on in ES3), wide lines.
   - Extension/version string curation so version-sniffing apps see a
     consistent 3.3-core story.
3. **Shader runtime.** glslang + SPIRV-Cross in-process at
   `glCompileShader`/`glLinkProgram` time. Compile cost amortized via
   `glGetProgramBinary`-backed on-disk program cache (Adreno supports
   binary formats; gl4es already used this successfully). Fallback
   diagnostics: dump original + translated source on compile failure
   (`LIBGL_LOGSHADERERROR`-equivalent from day one — debugging this on
   device is otherwise brutal, see
   [issues/rootfs-crash-exit-code-masked.md](../issues/rootfs-crash-exit-code-masked.md)).

Ships like the other client-side GL options: cross-built per ABI with
the `aarch64-linux-gnu` toolchain + host sysroot (the gl4es spike
recipe: X11 via a stub libdir, `-idirafter` for headers), packed as an
asset, installed into the rootfs under a tawc-owned path, opt-in per
spawn via env (`LD_LIBRARY_PATH` prefix), same pattern as
`GRAPHICS`-header backend selection. Not a new `GraphicsBackend`.

## Relationship to zink

Zink stays the endgame: full GL 4.6, maintained by Mesa, already plumbed
(`libhybris-zink` backend) — but gated on Vulkan 1.3 hardware we don't
have. This translator covers the years-long gap on current devices, at
the cost of owning a translation layer. If Vulkan 1.3 devices become
the norm before the translator matures, kill it in favor of zink.

## Staging

1. **Shader-pipeline spike (cheapest de-risk, no translator needed):**
   take kitty's and glmark2's GLSL 330 shaders, run them through
   glslang → SPIRV-Cross → ESSL offline, feed the output to a hand-rolled
   ES 3.2 test client on the phone. Proves the compiler recipe against
   the Adreno driver before any dispatch code exists.
2. **Minimal vertical slice:** EGL interposer + context lie + ~50
   entry points; render a GLSL-330 triangle from an unmodified desktop-GL
   demo binary.
3. **GLX front-end** reusing the libhybris x11 ws path; glmark2
   (desktop-GL variant) as the workhorse test.
4. **kitty milestone:** first real app. Drives the buffer-mapping and
   instancing paths.
5. **Semantics tail** driven by whatever app list we adopt next
   (games, mpv `gpu` backend, etc.), each adding conformance tests.

Integration tests mirror the zink tripwire pattern: `glxinfo`/`eglinfo`
renderer-string assertions plus AHB-import-counter tests per front-end.

## Risks

- **Long-tail underestimation.** The 1:1-mapping claim is true for 80%
  of calls; the remaining 20% (mapping semantics, readbacks, queries)
  is where wrapper projects go to die. Mitigation: strict app-driven
  scope — implement only what a named target calls, stub loudly
  otherwise.
- **SPIRV-Cross ESSL gaps** for odd constructs; some shaders will need
  per-construct workarounds. MobileGlues's issue tracker is a preview
  of this tail.
- **Driver bugs** now become *our* bugs to work around per-GPU
  (Adreno vs Mali vs PowerVR), the exact burden gl4es carries in its
  hack-flag zoo.
- **Maintenance:** unlike gl4es/zink there is no upstream; this is a
  permanent in-tree component until zink-capable hardware retires it.
- **Visual verification currently blocked** by the X11 black-window
  regression
  ([issues/x11-presented-windows-black.md](../issues/x11-presented-windows-black.md))
  — fix that first or stage 2+ can't be eyeballed.

## Open questions

- Vendor glslang/SPIRV-Cross (large-ish C++ deps, cross-build cost) vs
  a lighter GLSL→ESSL-only path? Start with the full pipeline; only
  optimize if binary size hurts.
- License review of MobileGlues/LTW/NG-GL4ES before reading their
  source in anger — clean-room vs borrow decision.
- ES 3.1-only devices (older Mali): support tier or hard-require 3.2?
