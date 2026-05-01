/*
 * Phase 2 step 4 verification: EGL-on-X11 client driving libhybris's
 * tawc x11 platform plugin (eglplatform_x11.so).
 *
 *   - XOpenDisplay(:0)
 *   - XCreateSimpleWindow + XMapWindow
 *   - eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, dpy, NULL)
 *   - eglInitialize / eglChooseConfig / eglCreateContext
 *   - eglCreateWindowSurface(window=xwin)
 *   - GLES draw + eglSwapBuffers loop
 *
 * The plugin allocates an AHardwareBuffer per dequeued frame and
 * ships it to the Xwayland (tawc fork) X server via TAWCDRIPresentBuffer.
 * Xwayland forwards it to the compositor via android_wlegl. Compositor
 * imports it as a GL texture. End-to-end zero-readback for client GL
 * via X11.
 *
 * Mode selection (env vars):
 *   TAWC_EGLX11_FRAMES=N   render N frames at vsync, then exit (default 60)
 *
 * Exit codes:
 *   0  success
 *   1  X11 setup failed
 *   2  EGL display / config / context failed
 *   3  EGL surface / make-current failed
 *   4  swap loop hit an error
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define WIN_W 320
#define WIN_H 240

static void log_egl_error(const char *what)
{
    EGLint e = eglGetError();
    fprintf(stderr, "eglx11-test: %s -> EGL error 0x%04x\n", what, e);
}

int main(void)
{
    int frames = 60;
    {
        const char *f = getenv("TAWC_EGLX11_FRAMES");
        if (f) frames = atoi(f);
        if (frames <= 0) frames = 60;
    }

    /* X11 setup */
    Display *xdpy = XOpenDisplay(NULL);
    if (!xdpy) {
        fprintf(stderr, "eglx11-test: XOpenDisplay(NULL) failed (DISPLAY=%s)\n",
                getenv("DISPLAY"));
        return 1;
    }
    int screen = DefaultScreen(xdpy);
    Window root = RootWindow(xdpy, screen);
    Window xwin = XCreateSimpleWindow(
        xdpy, root, 0, 0, WIN_W, WIN_H, 0,
        BlackPixel(xdpy, screen),
        BlackPixel(xdpy, screen));
    if (!xwin) {
        fprintf(stderr, "eglx11-test: XCreateSimpleWindow failed\n");
        XCloseDisplay(xdpy);
        return 1;
    }
    XSelectInput(xdpy, xwin, ExposureMask | StructureNotifyMask);
    XMapWindow(xdpy, xwin);
    XSync(xdpy, False);

    /* EGL platform = X11 */
    EGLDisplay edpy = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, xdpy, NULL);
    if (edpy == EGL_NO_DISPLAY) {
        log_egl_error("eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR)");
        XCloseDisplay(xdpy);
        return 2;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(edpy, &major, &minor)) {
        log_egl_error("eglInitialize");
        XCloseDisplay(xdpy);
        return 2;
    }
    fprintf(stderr, "eglx11-test: EGL %d.%d initialized\n", major, minor);
    fprintf(stderr, "eglx11-test: EGL_VENDOR=%s\n",
            eglQueryString(edpy, EGL_VENDOR));

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(edpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        log_egl_error("eglChooseConfig");
        eglTerminate(edpy);
        XCloseDisplay(xdpy);
        return 2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ectx == EGL_NO_CONTEXT) {
        log_egl_error("eglCreateContext");
        eglTerminate(edpy);
        XCloseDisplay(xdpy);
        return 2;
    }

    EGLSurface esurf = eglCreateWindowSurface(
        edpy, cfg, (EGLNativeWindowType)(uintptr_t)xwin, NULL);
    if (esurf == EGL_NO_SURFACE) {
        log_egl_error("eglCreateWindowSurface");
        eglDestroyContext(edpy, ectx);
        eglTerminate(edpy);
        XCloseDisplay(xdpy);
        return 3;
    }
    if (!eglMakeCurrent(edpy, esurf, esurf, ectx)) {
        log_egl_error("eglMakeCurrent");
        eglDestroySurface(edpy, esurf);
        eglDestroyContext(edpy, ectx);
        eglTerminate(edpy);
        XCloseDisplay(xdpy);
        return 3;
    }
    fprintf(stderr, "eglx11-test: GL_VENDOR=%s GL_RENDERER=%s\n",
            (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER));

    /* Render loop. Each frame paints a different solid colour so a
     * compositor that caches the first texture forever is visible
     * on screen. */
    for (int f = 0; f < frames; f++) {
        float t = (float)f / (float)frames;
        glClearColor(t, 1.0f - t, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(edpy, esurf)) {
            log_egl_error("eglSwapBuffers");
            eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(edpy, esurf);
            eglDestroyContext(edpy, ectx);
            eglTerminate(edpy);
            XCloseDisplay(xdpy);
            return 4;
        }
        /* Pump X events so the server doesn't fill its socket buffer. */
        while (XPending(xdpy)) {
            XEvent ev;
            XNextEvent(xdpy, &ev);
        }
    }

    fprintf(stderr, "eglx11-test: rendered %d frames\n", frames);
    eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(edpy, esurf);
    eglDestroyContext(edpy, ectx);
    eglTerminate(edpy);
    XDestroyWindow(xdpy, xwin);
    XCloseDisplay(xdpy);
    fprintf(stderr, "eglx11-test: OK\n");
    return 0;
}
