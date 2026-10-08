# Qt Quick windows never draw on libhybris (no EGLConfig)

qmlkonsole on Pixel 9 Pro (Mali-G715, ALARM, qt6-base 6.11.2,
`libhybris`) maps an xdg_toplevel but never attaches a buffer: blank
window, or from the launcher no window ever shows. Works on the
emulator (Mesa EGL has desktop-GL configs). Likely every Qt Quick /
QOpenGL app on libhybris; only qmlkonsole was checked (2026-10-08).

    Cannot find EGLConfig, returning null config
    qt.qpa.wayland: Could not create EGL surface (EGL error 0x3005)
    qt.qpa.wayland: eglSwapBuffers failed with 0x300d, surface: 0x0

Repro: `QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES="qt.qpa.wayland*=true;qt.scenegraph.general=true" qmlkonsole`.

## Cause

Arch's Qt is a desktop-GL build (`openGLModuleType() == LibGL`). For a
default-renderable-type format, `QEglConfigChooser::chooseConfig`
(qtbase `src/gui/opengl/platform/egl/qeglconvenience.cpp`) puts
`EGL_RENDERABLE_TYPE = EGL_OPENGL_BIT` in the request, and
`q_reduceConfigAttributes` never drops it. Android EGL has no desktop-GL
configs: on Mali this returns success with 0 configs (Adreno returns
`EGL_BAD_ATTRIBUTE`; see plans/gl-on-gles-translator.md). Yet
`QWaylandGLContext` binds `EGL_OPENGL_ES_API` for that same default type,
and `q_glFormatFromConfig` falls back to ES when the config lacks
`EGL_OPENGL_BIT`. So the config request is the only thing that's wrong:
with an ES config, Qt works fine.

Same root cause as the SDL3 / gui-doom-game failure
(plans/gl-on-gles-translator.md "Measured"), but Qt needs only the
config half; it never asks for a desktop-GL context.

## Facts measured on device

- `eglChooseConfig` with `EGL_OPENGL_BIT`: 0 configs; with ES2 or ES3:
  6 configs (Python ctypes through glvnd).
- `eglBindAPI(EGL_OPENGL_API)` fails with `EGL_BAD_PARAMETER`; the
  bound API stays ES.
- glvnd resolves libhybris entry points through `eglGetProcAddress`
  (`hybris/egl/glvnd/eglglvnd.cpp`), so a wrapper must also go in
  `_eglHybrisOverrideFunctions` in `hybris/egl/egl.c`. Today
  `eglChooseConfig` (and `eglGetConfigAttrib`) reach Android directly
  under glvnd.

## Tried (reverted)

The libhybris `eglChooseConfig` wrapper replaced `EGL_OPENGL_BIT` in
`EGL_RENDERABLE_TYPE` with `EGL_OPENGL_ES2_BIT`, registered in the
override table. qmlkonsole then rendered correctly and took input on
the phone. Reverted over the risk below.

## The risk with a global remap

At config time Qt's request can't be told apart from an app probing
for desktop GL. Apps that bind the API first (mpv) are unaffected, since
the bind fails before the config query. But an app that treats "0
configs for `EGL_OPENGL_BIT`" as "no desktop GL, use ES" would instead
commit to desktop GL and fail later, at `eglBindAPI` or
`eglCreateContext`. If it ignores the bind failure, it runs with an ES
context while thinking it has desktop GL. Through
`gl-shims/libGL.so.1` that gives missing functions, GLSL 330 compile
errors or crashes, far from the real cause. Which real apps probe this
way is unsurveyed.

## Options

1. **Remap only in Qt processes.** Apply the wrapper only when
   `dlopen("libQt6Gui.so.6", RTLD_NOLOAD)` succeeds (cached). Qt's
   behaviour is known, and every other process keeps Android's honest
   answer. Downsides: a toolkit-specific hack in libhybris; Qt5 is
   unchecked; it also applies to non-Qt GL code inside a Qt process
   (fine if that code binds first).
2. **Global remap plus a survey.** Grep the EGL users we care about for
   config-before-bind probing, run the integration suite, and add a
   debug-level hybris log line on each remap. This catches today's
   apps, not future ones.
3. **Fix on the Qt side.** No known runtime switch moves a desktop-GL
   Qt build to ES. `QSG_RHI_BACKEND=vulkan` or
   `QT_QUICK_BACKEND=software` cover Qt Quick only; software is slow
   and Vulkan through libhybris is untested with Qt.
4. **Wait for the translator's EGL interposer**
   (plans/gl-on-gles-translator.md layer 1), which handles config and
   context together, opted into per spawn. Qt Quick stays broken until
   then.
