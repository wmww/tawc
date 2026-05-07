/*
 * Minimal reproducer for the Adreno Android driver bug GTK4 hits in its
 * GSK GPU renderer (upstream issue #7531, fixed by commit 00beb04753 in
 * GTK 4.20).
 *
 * The driver mishandles a *struct-typed varying* whose value is then
 * passed into a function/subroutine in the fragment shader. With the
 * "struct" mode below, the function call produces zero / garbage and
 * the cleared red background survives. With the "array" mode (vec4[]
 * instead of struct), everything works — same data, just wrapped
 * differently.
 *
 * GTK's workaround replaces `Rect`/`RoundedRect` struct varyings in
 * gsk/gpu/shaders/{rect,roundedrect,common,…}.glsl with `vec4[1]` /
 * `vec4[3]` arrays. This test isolates the offending pattern in ~200
 * lines of GLES so it can stand alone as a regression check / upstream
 * bug report attachment.
 *
 * Modes:
 *   --mode=struct (default) — broken pattern: VS outputs a struct
 *     varying, FS receives it, FS calls a subroutine that takes the
 *     struct by value and returns its colour field.
 *   --mode=array            — fixed pattern: same data shape, but the
 *     varying is `vec4[1]` and the subroutine takes the same array.
 *
 * Output: glReadPixels the centre of the default framebuffer after one
 * draw, print the RGBA, classify GREEN (good) vs RED (bug present).
 *
 * Exit codes:
 *   0 GREEN — pattern works on this driver
 *   1 wayland / egl / xdg setup failed
 *   2 shader / context setup failed
 *   3 RED — broken render observed (Adreno struct-varying bug, expected
 *           on libhybris/Adreno when --mode=struct)
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "xdg-shell-client-protocol.h"

#define WIN_W 256
#define WIN_H 256

static struct wl_compositor *g_compositor;
static struct xdg_wm_base *g_wm_base;
static struct wl_surface *g_wsurf;
static struct xdg_surface *g_xsurf;
static struct xdg_toplevel *g_toplevel;
static int g_configured;
static int g_running = 1;

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
{ (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t id,
                            const char *iface, uint32_t v)
{
    (void)data;
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = wl_registry_bind(r, id, &wl_compositor_interface, v >= 4 ? 4 : v);
    else if (strcmp(iface, "xdg_wm_base") == 0) {
        g_wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wm_base, &wm_base_listener, NULL);
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t id)
{ (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{ (void)d; xdg_surface_ack_configure(s, serial); g_configured = 1; }
static const struct xdg_surface_listener xsurf_listener = { .configure = xsurf_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{ (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g_running = 0; }
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure, .close = tl_close,
    .configure_bounds = tl_bounds, .wm_capabilities = tl_caps,
};

static void on_signal(int s) { (void)s; g_running = 0; }

static GLuint compile(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n--- src ---\n%s\n", log, src);
        return 0;
    }
    return s;
}

/* "struct" mode: a struct varying carrying a vec4 colour, passed into
 * a subroutine in the fragment shader. This is the pattern Adreno
 * mishandles. */
static const char *vs_struct =
    "#version 300 es\n"
    "struct Rect { vec4 col; };\n"
    "out Rect v_rect;\n"
    "void main() {\n"
    "  vec2 xs = vec2(-1.0, 1.0);\n"
    "  vec2 ys = vec2(-1.0, 1.0);\n"
    "  vec2 p = vec2(xs[(gl_VertexID >> 0) & 1], ys[(gl_VertexID >> 1) & 1]);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_rect.col = vec4(0.0, 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *fs_struct =
    "#version 300 es\n"
    "precision mediump float;\n"
    "struct Rect { vec4 col; };\n"
    "in Rect v_rect;\n"
    "out vec4 frag;\n"
    "vec4 extract(Rect r) { return r.col; }\n"
    "void main() { frag = extract(v_rect); }\n";

/* "array" mode: same data, vec4[1] varying instead of struct. The
 * subroutine takes a vec4[1] argument. This is the pattern GTK 4.20+
 * uses, and the Adreno driver handles it correctly. */
static const char *vs_array =
    "#version 300 es\n"
    "out vec4 v_rect[1];\n"
    "void main() {\n"
    "  vec2 xs = vec2(-1.0, 1.0);\n"
    "  vec2 ys = vec2(-1.0, 1.0);\n"
    "  vec2 p = vec2(xs[(gl_VertexID >> 0) & 1], ys[(gl_VertexID >> 1) & 1]);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_rect[0] = vec4(0.0, 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *fs_array =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_rect[1];\n"
    "out vec4 frag;\n"
    "vec4 extract(vec4 r[1]) { return r[0]; }\n"
    "void main() { frag = extract(v_rect); }\n";

int main(int argc, char **argv)
{
    const char *mode = "struct";
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--mode=", 7) == 0) mode = argv[i] + 7;
    }
    int mode_struct = strcmp(mode, "struct") == 0;
    int mode_array  = strcmp(mode, "array")  == 0;
    if (!mode_struct && !mode_array) {
        fprintf(stderr, "unknown mode '%s' (want struct|array)\n", mode);
        return 1;
    }
    fprintf(stderr, "adreno-struct-varying: mode=%s\n", mode);

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    struct wl_display *wdpy = wl_display_connect(NULL);
    if (!wdpy) { fprintf(stderr, "wl_display_connect\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(wdpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(wdpy);
    if (!g_compositor || !g_wm_base) { fprintf(stderr, "no compositor/wm_base\n"); return 1; }
    g_wsurf = wl_compositor_create_surface(g_compositor);
    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm_base, g_wsurf);
    xdg_surface_add_listener(g_xsurf, &xsurf_listener, NULL);
    g_toplevel = xdg_surface_get_toplevel(g_xsurf);
    xdg_toplevel_add_listener(g_toplevel, &tl_listener, NULL);
    xdg_toplevel_set_title(g_toplevel, "adreno-struct-varying");
    xdg_toplevel_set_app_id(g_toplevel, "adreno-struct-varying");
    wl_surface_commit(g_wsurf);
    wl_display_roundtrip(wdpy);
    if (!g_configured) { fprintf(stderr, "no configure\n"); return 1; }

    struct wl_egl_window *ewin = wl_egl_window_create(g_wsurf, WIN_W, WIN_H);
    if (!ewin) { fprintf(stderr, "wl_egl_window_create\n"); return 1; }

    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)wdpy);
    if (!eglInitialize(edpy, NULL, NULL)) { fprintf(stderr, "eglInitialize\n"); return 2; }
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!eglChooseConfig(edpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "eglChooseConfig\n"); return 2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    EGLSurface esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)ewin, NULL);
    if (!eglMakeCurrent(edpy, esurf, esurf, ectx)) { fprintf(stderr, "makeCurrent\n"); return 2; }

    fprintf(stderr, "GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION:  %s\n", glGetString(GL_VERSION));

    GLuint vs = compile(GL_VERTEX_SHADER,   mode_struct ? vs_struct : vs_array);
    GLuint fs = compile(GL_FRAGMENT_SHADER, mode_struct ? fs_struct : fs_array);
    if (!vs || !fs) return 2;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0}; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "link failed: %s\n", log);
        return 2;
    }
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);

    glViewport(0, 0, WIN_W, WIN_H);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);  /* RED = "draw produced no fragments" */
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    unsigned char px[4] = {0};
    glReadPixels(WIN_W / 2, WIN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "RESULT mode=%s centre=R%u G%u B%u A%u\n",
            mode, px[0], px[1], px[2], px[3]);

    int is_green = (px[1] >= 200 && px[0] < 50 && px[2] < 50);
    eglSwapBuffers(edpy, esurf);
    if (is_green) {
        fprintf(stderr, "verdict: GREEN — pattern '%s' works on this driver\n", mode);
        return 0;
    }
    fprintf(stderr, "verdict: BROKEN — fragment output for pattern '%s' is "
                    "not the expected green. On Adreno+libhybris, mode=struct "
                    "is expected to fail this way (driver mishandles struct "
                    "varyings passed into a function); mode=array is the "
                    "GTK 4.20+ workaround.\n", mode);
    return 3;
}
