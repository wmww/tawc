/*
 * Bionic / NDK port of the Adreno struct-varying reproducer.
 *
 * Goal: prove or disprove that libhybris is implicated in the bug.
 * Same shaders as the Wayland/glibc version, but built with the
 * Android NDK and run directly from `adb shell` against the vendor's
 * native EGL/GLES libraries (no libhybris, no glibc, no Wayland, no
 * tawc compositor). Uses an EGL pbuffer surface so it doesn't need a
 * window system at all.
 *
 * Modes (--mode=struct|array): identical to the Wayland version.
 *
 * Exit codes:
 *   0  GREEN — pattern works
 *   2  EGL / shader setup failed
 *   3  BROKEN — fragment output is not green
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define WIN_W 256
#define WIN_H 256

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
    fprintf(stderr, "adreno-struct-varying-bionic: mode=%s\n", mode);

    EGLDisplay edpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (edpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay\n"); return 2; }
    EGLint emaj = 0, emin = 0;
    if (!eglInitialize(edpy, &emaj, &emin)) { fprintf(stderr, "eglInitialize\n"); return 2; }
    fprintf(stderr, "EGL %d.%d (%s)\n", emaj, emin, eglQueryString(edpy, EGL_VENDOR));

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!eglChooseConfig(edpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "eglChooseConfig\n"); return 2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint pb_attribs[] = { EGL_WIDTH, WIN_W, EGL_HEIGHT, WIN_H, EGL_NONE };
    EGLSurface esurf = eglCreatePbufferSurface(edpy, cfg, pb_attribs);
    if (esurf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreatePbufferSurface\n"); return 2; }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ectx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext\n"); return 2; }
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
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);  /* red — only survives if draw produces no fragments */
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    unsigned char px[4] = {0};
    glReadPixels(WIN_W / 2, WIN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "RESULT mode=%s centre=R%u G%u B%u A%u\n",
            mode, px[0], px[1], px[2], px[3]);

    int is_green = (px[1] >= 200 && px[0] < 50 && px[2] < 50);
    if (is_green) {
        fprintf(stderr, "verdict: GREEN — pattern '%s' works on this driver\n", mode);
        return 0;
    }
    fprintf(stderr, "verdict: BROKEN — fragment output for pattern '%s' is "
                    "not the expected green\n", mode);
    return 3;
}
