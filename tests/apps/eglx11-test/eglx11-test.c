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
 *   TAWC_EGLX11_FRAMES=N      render N frames, then exit (default 60)
 *   TAWC_EGLX11_HOLD_SECS=N   keep the mapped window alive after swaps
 *   TAWC_EGLX11_TRIANGLE=1    draw a solid blue triangle over the clear
 *   TAWC_EGLX11_DEPTH=1       request a depth buffer + depth-test the draw
 *   TAWC_EGLX11_READBACK=1    glReadPixels the centre pixel before swap
 *   TAWC_EGLX11_W/H=N         window size (default 640x240)
 *   TAWC_EGLX11_VBO=1         source triangle vertices from a VBO
 *   TAWC_EGLX11_MVP=1         route positions through a mat4 uniform
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
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define WIN_W 640
#define WIN_H 240

static void log_egl_error(const char *what)
{
    EGLint e = eglGetError();
    fprintf(stderr, "eglx11-test: %s -> EGL error 0x%04x\n", what, e);
}

static GLuint compile_shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "eglx11-test: shader compile failed: %s\n", log);
        return 0;
    }
    return s;
}

/* Solid blue triangle covering the middle of the window, drawn at z=0
 * so it passes a LESS depth test against a cleared (1.0) depth buffer. */
static GLuint triangle_program(int use_mvp)
{
    static const char *vs_plain =
        "attribute vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *vs_mvp =
        "attribute vec2 pos;\n"
        "uniform mat4 mvp;\n"
        "void main() { gl_Position = mvp * vec4(pos, 0.0, 1.0); }\n";
    const char *vs = use_mvp ? vs_mvp : vs_plain;
    static const char *fs =
        "precision mediump float;\n"
        "void main() { gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }\n";
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "eglx11-test: program link failed: %s\n", log);
        return 0;
    }
    return p;
}

static void window_size(Display *xdpy, Window xwin, int *w, int *h)
{
    XWindowAttributes attrs;
    *w = WIN_W;
    *h = WIN_H;
    if (XGetWindowAttributes(xdpy, xwin, &attrs)) {
        if (attrs.width > 0) *w = attrs.width;
        if (attrs.height > 0) *h = attrs.height;
    }
}

int main(void)
{
    int frames = 60;
    int hold_secs = 0;
    int want_triangle = 0;
    int want_depth = 0;
    int want_readback = 0;
    int want_vbo = 0;
    int want_mvp = 0;
    int win_w0 = WIN_W;
    int win_h0 = WIN_H;
    {
        const char *f = getenv("TAWC_EGLX11_FRAMES");
        if (f) frames = atoi(f);
        if (frames <= 0) frames = 60;
        const char *h = getenv("TAWC_EGLX11_HOLD_SECS");
        if (h) hold_secs = atoi(h);
        if (hold_secs < 0) hold_secs = 0;
        const char *t = getenv("TAWC_EGLX11_TRIANGLE");
        if (t) want_triangle = atoi(t);
        const char *d = getenv("TAWC_EGLX11_DEPTH");
        if (d) want_depth = atoi(d);
        const char *r = getenv("TAWC_EGLX11_READBACK");
        if (r) want_readback = atoi(r);
        const char *w = getenv("TAWC_EGLX11_W");
        if (w) win_w0 = atoi(w);
        if (win_w0 <= 0) win_w0 = WIN_W;
        const char *hh = getenv("TAWC_EGLX11_H");
        if (hh) win_h0 = atoi(hh);
        if (win_h0 <= 0) win_h0 = WIN_H;
        const char *v = getenv("TAWC_EGLX11_VBO");
        if (v) want_vbo = atoi(v);
        const char *m = getenv("TAWC_EGLX11_MVP");
        if (m) want_mvp = atoi(m);
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
        xdpy, root, 0, 0, win_w0, win_h0, 0,
        BlackPixel(xdpy, screen),
        BlackPixel(xdpy, screen));
    if (!xwin) {
        fprintf(stderr, "eglx11-test: XCreateSimpleWindow failed\n");
        XCloseDisplay(xdpy);
        return 1;
    }
    XStoreName(xdpy, xwin, "tawc EGL X11 test");
    XClassHint class_hint = {
        .res_name = "eglx11-test",
        .res_class = "TawcX11Debug",
    };
    XSetClassHint(xdpy, xwin, &class_hint);
    XSizeHints hints = {
        .flags = PSize,
        .width = win_w0,
        .height = win_h0,
    };
    XSetWMNormalHints(xdpy, xwin, &hints);
    XResizeWindow(xdpy, xwin, win_w0, win_h0);
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
        EGL_DEPTH_SIZE, want_depth ? 16 : 0,
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
    if (want_depth) {
        EGLint depth_bits = -1;
        eglGetConfigAttrib(edpy, cfg, EGL_DEPTH_SIZE, &depth_bits);
        fprintf(stderr, "eglx11-test: config EGL_DEPTH_SIZE=%d\n", depth_bits);
    }

    GLuint prog = 0;
    GLint mvp_loc = -1;
    GLuint vbo = 0;
    static const GLfloat tri_verts[] = {
        -0.8f, -0.8f,
         0.8f, -0.8f,
         0.0f,  0.8f,
    };
    static const GLfloat identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    if (want_triangle) {
        prog = triangle_program(want_mvp);
        if (!prog)
            return 4;
        if (want_mvp)
            mvp_loc = glGetUniformLocation(prog, "mvp");
        if (want_vbo) {
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(tri_verts), tri_verts,
                         GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }
    if (want_depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
    }

    /* Render loop. Each frame paints a different solid colour so a
     * compositor that caches the first texture forever is visible
     * on screen. */
    int last_w = win_w0;
    int last_h = win_h0;
    for (int f = 0; f < frames; f++) {
        float t = (float)f / (float)frames;
        int win_w = win_w0;
        int win_h = win_h0;
        window_size(xdpy, xwin, &win_w, &win_h);
        if (win_w != last_w || win_h != last_h) {
            fprintf(stderr, "eglx11-test: frame %d window size %dx%d -> %dx%d\n",
                    f, last_w, last_h, win_w, win_h);
            last_w = win_w;
            last_h = win_h;
        }
        glViewport(0, 0, win_w, win_h);
        glClearColor(t, 1.0f - t, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | (want_depth ? GL_DEPTH_BUFFER_BIT : 0));
        if (want_triangle) {
            glUseProgram(prog);
            if (want_mvp)
                glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, identity);
            glEnableVertexAttribArray(0);
            if (want_vbo) {
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
            } else {
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri_verts);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (want_readback && (f == frames - 1 || f % 1000 == 0)) {
            unsigned char px[4] = {0};
            glReadPixels(win_w / 2, win_h / 2, 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr,
                    "eglx11-test: frame %d centre pixel R%u G%u B%u A%u (glGetError=0x%x)\n",
                    f, px[0], px[1], px[2], px[3], glGetError());
        }
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
    if (hold_secs > 0) {
        fprintf(stderr, "eglx11-test: holding window for %d seconds\n", hold_secs);
        sleep((unsigned int)hold_secs);
    }
    eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(edpy, esurf);
    eglDestroyContext(edpy, ectx);
    eglTerminate(edpy);
    XDestroyWindow(xdpy, xwin);
    XCloseDisplay(xdpy);
    fprintf(stderr, "eglx11-test: OK\n");
    return 0;
}
