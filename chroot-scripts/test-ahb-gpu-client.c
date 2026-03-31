/* GPU client that:
 * 1. Allocates an AHardwareBuffer
 * 2. Creates EGL context + EGLImage from the AHB
 * 3. Renders a test pattern with GLES (GPU-only, no CPU)
 * 4. Sends the AHB to the compositor via Unix socket
 *
 * Build inside chroot:
 *   gcc -o test-ahb-gpu-client test-ahb-gpu-client.c \
 *       -lhybris-common -lEGL -lGLESv2 -ldl -lm
 *
 * Run:
 *   HYBRIS_PATCH_TLS=1 ./test-ahb-gpu-client <socket-path>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hybris/common/binding.h>

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

#define AHARDWAREBUFFER_FORMAT_RGBA8 1
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR   (1ULL << 9)
#define EGL_NATIVE_BUFFER_ANDROID 0x3140

typedef int (*AHB_allocate_t)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*AHB_release_t)(AHardwareBuffer*);
typedef int (*AHB_sendHandle_t)(const AHardwareBuffer*, int);

static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        exit(1);
    }
    return s;
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <socket-path>\n", argv[0]);
        return 1;
    }

    printf("=== AHB GPU Client ===\n");

    /* Load AHB functions */
    void *nw = android_dlopen("/system/lib64/libnativewindow.so", 2);
    if (!nw) die("Failed to load libnativewindow.so");

    AHB_allocate_t ahb_alloc = (AHB_allocate_t)android_dlsym(nw, "AHardwareBuffer_allocate");
    AHB_release_t ahb_release = (AHB_release_t)android_dlsym(nw, "AHardwareBuffer_release");
    AHB_sendHandle_t ahb_sendHandle =
        (AHB_sendHandle_t)android_dlsym(nw, "AHardwareBuffer_sendHandleToUnixSocket");

    if (!ahb_alloc || !ahb_sendHandle)
        die("Missing AHB functions");

    /* Step 1: Allocate AHB */
    printf("1. Allocating AHB (256x256 RGBA8 GPU)...\n");
    AHardwareBuffer_Desc desc = {
        .width = 256, .height = 256, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_RGBA8,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED | AHARDWAREBUFFER_USAGE_GPU_COLOR,
    };
    AHardwareBuffer *ahb = NULL;
    if (ahb_alloc(&desc, &ahb) != 0) die("AHB allocate failed");
    printf("   AHB allocated at %p\n", ahb);

    /* Step 2: Initialize EGL */
    printf("2. Initializing EGL...\n");
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) die("eglGetDisplay failed");
    if (!eglInitialize(dpy, NULL, NULL)) die("eglInitialize failed");

    EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    eglChooseConfig(dpy, cfg_attrs, &config, 1, &num_configs);
    if (num_configs == 0) die("No EGL config found");

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) die("eglCreateContext failed");

    EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, config, pb_attrs);
    eglMakeCurrent(dpy, surf, surf, ctx);
    printf("   EGL initialized: %s\n", glGetString(GL_RENDERER));

    /* Step 3: Create EGLImage from AHB */
    printf("3. Creating EGLImage from AHB...\n");

    typedef EGLClientBuffer (*eglGetNativeClientBufferANDROID_t)(const void*);
    eglGetNativeClientBufferANDROID_t getClientBuffer =
        (eglGetNativeClientBufferANDROID_t)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    if (!getClientBuffer) die("eglGetNativeClientBufferANDROID not found");
    if (!eglCreateImageKHR) die("eglCreateImageKHR not found");

    EGLClientBuffer client_buf = getClientBuffer(ahb);
    if (!client_buf) die("eglGetNativeClientBufferANDROID returned null");

    EGLImageKHR image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID, client_buf, NULL);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "eglCreateImageKHR failed: 0x%x\n", eglGetError());
        die("EGLImage creation failed");
    }
    printf("   EGLImage created\n");

    /* Step 4: Create texture + FBO from EGLImage */
    printf("4. Setting up FBO for GPU rendering...\n");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!glEGLImageTargetTexture2DOES) die("glEGLImageTargetTexture2DOES not found");

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_EXTERNAL_OES, tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete: 0x%x\n", status);
        die("Framebuffer not complete");
    }
    printf("   FBO complete\n");

    /* Step 5: Render a gradient test pattern (pure GPU, no CPU) */
    printf("5. Rendering test pattern with GPU...\n");
    const char *vert_src =
        "attribute vec2 pos;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vUV = pos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(pos, 0.0, 1.0);\n"
        "}\n";
    const char *frag_src =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(vUV.x, vUV.y, 0.5, 1.0);\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    glUseProgram(prog);

    float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glViewport(0, 0, 256, 256);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    printf("   GPU render complete (gradient pattern)\n");

    /* Step 6: Connect to compositor and send AHB */
    printf("6. Connecting to compositor at %s...\n", argv[1]);
    int sock = connect_socket(argv[1]);
    if (sock < 0) die("Failed to connect to compositor");

    printf("   Sending AHB via sendHandleToUnixSocket...\n");
    int ret = ahb_sendHandle(ahb, sock);
    if (ret != 0) {
        fprintf(stderr, "AHardwareBuffer_sendHandleToUnixSocket failed: %d\n", ret);
        die("Failed to send AHB");
    }
    printf("   AHB sent successfully\n");

    /* Step 7: Keep alive so buffer stays valid */
    printf("7. Buffer sent. Keeping alive for 30 seconds...\n");
    fflush(stdout);
    sleep(30);

    /* Cleanup */
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    eglDestroyImageKHR(dpy, image);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    if (ahb_release) ahb_release(ahb);
    close(sock);

    printf("=== Done ===\n");
    return 0;
}
