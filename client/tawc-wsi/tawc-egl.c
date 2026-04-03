/**
 * tawc EGL wrapper -- drop-in libEGL.so replacement for Wayland EGL apps.
 *
 * Routes buffer sharing through the tawc_buffer_v1 protocol + AHardwareBuffer
 * side channel. Apps load it via LD_LIBRARY_PATH (before libhybris's EGL).
 *
 * Phase 4: Robust EGL WSI layer -- full EGL 1.5 API, thread safety,
 * buffer age/damage, resize, GTK3/Firefox compatibility.
 *
 * Build in chroot:
 *   gcc -shared -fPIC -o libEGL.so tawc-egl.c tawc-buffer-v1-client.c \
 *       -lwayland-client -lhybris-common -lGLESv2 -ldl -lpthread -I. -Wall -g
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/personality.h>
#include <sys/mman.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

/* GL types and constants -- avoid including GLES2 headers since we don't
 * link libGLESv2 at build time (to prevent transitive Android symbol deps). */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef void GLvoid;
#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_STENCIL_ATTACHMENT     0x8D20
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_LINEAR                 0x2601
#define GL_TEXTURE_EXTERNAL_OES   0x8D65
#define GL_DEPTH24_STENCIL8_OES   0x88F0

#include <wayland-client.h>
#include <wayland-egl-backend.h>

/* Load libhybris-common via dlopen to avoid baking execstack into our .so.
 * libhybris-common requires executable stack, and the kernel refuses to
 * dlopen such libraries into processes that didn't start with execstack.
 * By dlopening it ourselves (RTLD_LAZY), we defer the load. */
typedef void *(*fn_android_dlopen)(const char *, int);
typedef void *(*fn_android_dlsym)(void *, const char *);
static fn_android_dlopen android_dlopen;
static fn_android_dlsym android_dlsym;

#include "tawc-buffer-v1-client.h"

/* ------------------------------------------------------------------ */
/* AHB types and function pointers (loaded via libhybris)              */
/* ------------------------------------------------------------------ */

typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

#define AHB_FORMAT_RGBA8         1
#define AHB_USAGE_GPU_SAMPLED    (1ULL << 8)
#define AHB_USAGE_GPU_COLOR      (1ULL << 9)

typedef int  (*fn_ahb_allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
typedef void (*fn_ahb_release)(AHardwareBuffer *);
typedef void (*fn_ahb_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int  (*fn_ahb_send)(const AHardwareBuffer *, int fd);

static fn_ahb_allocate  ahb_allocate;
static fn_ahb_release   ahb_release;
static fn_ahb_describe  ahb_describe;
static fn_ahb_send      ahb_send;

/* ------------------------------------------------------------------ */
/* EGL extension function pointers                                     */
/* ------------------------------------------------------------------ */

typedef EGLClientBuffer (*fn_eglGetNativeClientBufferANDROID)(const void *);
typedef EGLImageKHR     (*fn_eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean      (*fn_eglDestroyImageKHR)(EGLDisplay, EGLImageKHR);

static fn_eglGetNativeClientBufferANDROID pfn_getClientBuffer;
static fn_eglCreateImageKHR              pfn_createImage;
static fn_eglDestroyImageKHR             pfn_destroyImage;

typedef void (*fn_glEGLImageTargetTexture2DOES)(GLenum, void *);
static fn_glEGLImageTargetTexture2DOES pfn_imageTargetTex;

/* GL function pointers -- resolved lazily via eglGetProcAddress to avoid
 * linking libGLESv2.so at build time. The libhybris GLESv2 has transitive
 * deps on Android EGL symbols that cause dlopen failures in Firefox. */
#define GL_FUNCTIONS(X) \
    X(void,  glGenTextures,            (GLsizei n, GLuint *textures)) \
    X(void,  glBindTexture,            (GLenum target, GLuint texture)) \
    X(void,  glTexParameteri,          (GLenum target, GLenum pname, GLint param)) \
    X(void,  glDeleteTextures,         (GLsizei n, const GLuint *textures)) \
    X(void,  glGenFramebuffers,        (GLsizei n, GLuint *framebuffers)) \
    X(void,  glBindFramebuffer,        (GLenum target, GLuint framebuffer)) \
    X(void,  glFramebufferTexture2D,   (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void,  glDeleteFramebuffers,     (GLsizei n, const GLuint *framebuffers)) \
    X(void,  glGenRenderbuffers,       (GLsizei n, GLuint *renderbuffers)) \
    X(void,  glBindRenderbuffer,       (GLenum target, GLuint renderbuffer)) \
    X(void,  glRenderbufferStorage,    (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void,  glFramebufferRenderbuffer,(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
    X(void,  glDeleteRenderbuffers,    (GLsizei n, const GLuint *renderbuffers)) \
    X(GLenum,glCheckFramebufferStatus, (GLenum target)) \
    X(void,  glViewport,              (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(void,  glFinish,                (void))

#define DECLARE_GL_FN(ret, name, args) static ret (*pfn_##name) args;
GL_FUNCTIONS(DECLARE_GL_FN)
#undef DECLARE_GL_FN

/* Redirect all gl* calls to function pointers */
#define glGenTextures           pfn_glGenTextures
#define glBindTexture           pfn_glBindTexture
#define glTexParameteri         pfn_glTexParameteri
#define glDeleteTextures        pfn_glDeleteTextures
#define glGenFramebuffers       pfn_glGenFramebuffers
#define glBindFramebuffer       pfn_glBindFramebuffer
#define glFramebufferTexture2D  pfn_glFramebufferTexture2D
#define glDeleteFramebuffers    pfn_glDeleteFramebuffers
#define glGenRenderbuffers      pfn_glGenRenderbuffers
#define glBindRenderbuffer      pfn_glBindRenderbuffer
#define glRenderbufferStorage   pfn_glRenderbufferStorage
#define glFramebufferRenderbuffer pfn_glFramebufferRenderbuffer
#define glDeleteRenderbuffers   pfn_glDeleteRenderbuffers
#define glCheckFramebufferStatus pfn_glCheckFramebufferStatus
#define glViewport              pfn_glViewport
#define glFinish                pfn_glFinish

#define EGL_NATIVE_BUFFER_ANDROID 0x3140

/* ------------------------------------------------------------------ */
/* Stock EGL function pointers (real driver via libhybris)             */
/* ------------------------------------------------------------------ */

/* All 44 core EGL 1.5 functions + key extensions */
#define REAL_EGL_FUNCTIONS(X) \
    X(EGLDisplay,  eglGetDisplay,           (EGLNativeDisplayType display_id)) \
    X(EGLBoolean,  eglInitialize,           (EGLDisplay dpy, EGLint *major, EGLint *minor)) \
    X(EGLBoolean,  eglTerminate,            (EGLDisplay dpy)) \
    X(const char*, eglQueryString,           (EGLDisplay dpy, EGLint name)) \
    X(EGLBoolean,  eglGetConfigs,           (EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config)) \
    X(EGLBoolean,  eglChooseConfig,         (EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config)) \
    X(EGLBoolean,  eglGetConfigAttrib,      (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)) \
    X(EGLSurface,  eglCreateWindowSurface,  (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)) \
    X(EGLSurface,  eglCreatePbufferSurface, (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)) \
    X(EGLSurface,  eglCreatePixmapSurface,  (EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap, const EGLint *attrib_list)) \
    X(EGLBoolean,  eglDestroySurface,       (EGLDisplay dpy, EGLSurface surface)) \
    X(EGLBoolean,  eglQuerySurface,         (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)) \
    X(EGLBoolean,  eglBindAPI,              (EGLenum api)) \
    X(EGLenum,     eglQueryAPI,             (void)) \
    X(EGLBoolean,  eglWaitClient,           (void)) \
    X(EGLBoolean,  eglReleaseThread,        (void)) \
    X(EGLSurface,  eglCreatePbufferFromClientBuffer, (EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, const EGLint *attrib_list)) \
    X(EGLBoolean,  eglSurfaceAttrib,        (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value)) \
    X(EGLBoolean,  eglBindTexImage,         (EGLDisplay dpy, EGLSurface surface, EGLint buffer)) \
    X(EGLBoolean,  eglReleaseTexImage,      (EGLDisplay dpy, EGLSurface surface, EGLint buffer)) \
    X(EGLBoolean,  eglSwapInterval,         (EGLDisplay dpy, EGLint interval)) \
    X(EGLContext,  eglCreateContext,         (EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list)) \
    X(EGLBoolean,  eglDestroyContext,       (EGLDisplay dpy, EGLContext ctx)) \
    X(EGLBoolean,  eglMakeCurrent,          (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)) \
    X(EGLContext,  eglGetCurrentContext,     (void)) \
    X(EGLSurface,  eglGetCurrentSurface,    (EGLint readdraw)) \
    X(EGLDisplay,  eglGetCurrentDisplay,     (void)) \
    X(EGLBoolean,  eglSwapBuffers,          (EGLDisplay dpy, EGLSurface surface)) \
    X(EGLBoolean,  eglCopyBuffers,          (EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target)) \
    X(EGLint,      eglGetError,             (void)) \
    X(EGLBoolean,  eglQueryContext,          (EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value)) \
    X(EGLBoolean,  eglWaitGL,               (void)) \
    X(EGLBoolean,  eglWaitNative,           (EGLint engine)) \
    X(void*,       eglGetProcAddress,       (const char *procname)) \
    /* EGL 1.5 */ \
    X(EGLSync,     eglCreateSync,           (EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list)) \
    X(EGLBoolean,  eglDestroySync,          (EGLDisplay dpy, EGLSync sync)) \
    X(EGLint,      eglClientWaitSync,       (EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)) \
    X(EGLBoolean,  eglGetSyncAttrib,        (EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value)) \
    X(EGLImage,    eglCreateImage,          (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list)) \
    X(EGLBoolean,  eglDestroyImage,         (EGLDisplay dpy, EGLImage image)) \
    X(EGLDisplay,  eglGetPlatformDisplay,   (EGLenum platform, void *native_display, const EGLAttrib *attrib_list)) \
    X(EGLSurface,  eglCreatePlatformWindowSurface,  (EGLDisplay dpy, EGLConfig config, void *native_window, const EGLAttrib *attrib_list)) \
    X(EGLSurface,  eglCreatePlatformPixmapSurface,  (EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLAttrib *attrib_list)) \
    X(EGLBoolean,  eglWaitSync,             (EGLDisplay dpy, EGLSync sync, EGLint flags))

/* Declare real_* function pointers */
#define DECL_REAL(ret, name, args) static ret (*real_##name) args;
REAL_EGL_FUNCTIONS(DECL_REAL)
#undef DECL_REAL

/* ------------------------------------------------------------------ */
/* Thread safety primitives                                            */
/* ------------------------------------------------------------------ */

static pthread_once_t init_once_ctrl = PTHREAD_ONCE_INIT;
static pthread_mutex_t surfaces_mutex = PTHREAD_MUTEX_INITIALIZER;
static int init_result = -1;  /* 0 = success */

static __thread struct tawc_surface *tls_current_surface = NULL;
static __thread EGLContext tls_current_context = EGL_NO_CONTEXT;

/* ------------------------------------------------------------------ */
/* Buffer pool for a Wayland surface                                   */
/* ------------------------------------------------------------------ */

#define NUM_BUFFERS 2
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define EGL_BUFFER_AGE_EXT 0x313D

struct tawc_buffer {
    AHardwareBuffer *ahb;
    EGLImageKHR      image;
    GLuint           tex;
    GLuint           fbo;
    GLuint           depth_rb;
    int              width, height;
};

struct tawc_surface {
    struct wl_surface             *wl_surf;
    struct wl_egl_window          *wl_win;    /* for resize detection */
    struct tawc_ahb_channel_v1    *channel;
    int                            side_fd;
    struct tawc_buffer             buffers[NUM_BUFFERS];
    int                            current;
    int                            width, height;
    int                            swap_count; /* total swaps, for buffer age */
    EGLSurface                     real_pbuffer;
    EGLConfig                      config;    /* saved for resize */
    int                            in_use;    /* slot occupied */
};

#define MAX_SURFACES 16
static struct tawc_surface surfaces[MAX_SURFACES];

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static int initialized = 0;
static EGLDisplay stock_display = EGL_NO_DISPLAY;
static struct wl_display *wayland_display = NULL;
static void *real_egl_lib = NULL;

/* tawc protocol objects */
static struct tawc_buffer_manager_v1 *buffer_manager = NULL;

/* Cached extension string (real driver extensions + our additions) */
static char *cached_extensions = NULL;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[tawc-egl] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* tawc protocol listener                                              */
/* ------------------------------------------------------------------ */

static void channel_fd_handler(void *data, struct tawc_ahb_channel_v1 *ch, int32_t fd)
{
    struct tawc_surface *s = (struct tawc_surface *)data;
    s->side_fd = fd;
    log_msg("received side channel fd=%d", fd);
}

static void channel_release_handler(void *data, struct tawc_ahb_channel_v1 *ch)
{
    /* Buffer released -- could track for smarter rotation */
}

static const struct tawc_ahb_channel_v1_listener channel_listener = {
    .channel_fd = channel_fd_handler,
    .release = channel_release_handler,
};

/* ------------------------------------------------------------------ */
/* Wayland registry -- bind tawc_buffer_manager_v1                     */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver)
{
    if (strcmp(iface, "tawc_buffer_manager_v1") == 0) {
        buffer_manager = wl_registry_bind(reg, name,
                                          &tawc_buffer_manager_v1_interface, 1);
        log_msg("bound tawc_buffer_manager_v1");
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

static int load_ahb_functions(void)
{
    void *nw = android_dlopen("/system/lib64/libnativewindow.so", 2);
    if (!nw) { log_msg("FATAL: can't load libnativewindow.so"); return -1; }

    ahb_allocate = (fn_ahb_allocate)android_dlsym(nw, "AHardwareBuffer_allocate");
    ahb_release  = (fn_ahb_release) android_dlsym(nw, "AHardwareBuffer_release");
    ahb_describe = (fn_ahb_describe)android_dlsym(nw, "AHardwareBuffer_describe");
    ahb_send     = (fn_ahb_send)    android_dlsym(nw, "AHardwareBuffer_sendHandleToUnixSocket");

    if (!ahb_allocate || !ahb_release || !ahb_describe || !ahb_send) {
        log_msg("FATAL: missing AHB functions");
        return -1;
    }
    return 0;
}

static int load_hybris_common(void)
{
    /* libhybris-common requires executable stack. Enable READ_IMPLIES_EXEC
     * so the kernel allows loading .so files that need execstack. */
    unsigned long pers = personality(0xffffffff);  /* query current */
    if (!(pers & READ_IMPLIES_EXEC)) {
        personality(pers | READ_IMPLIES_EXEC);
        log_msg("enabled READ_IMPLIES_EXEC for libhybris execstack");
    }

    void *hc = dlopen("libhybris-common.so.1", RTLD_NOW);
    if (!hc) { log_msg("FATAL: can't load libhybris-common: %s", dlerror()); return -1; }
    android_dlopen = (fn_android_dlopen)dlsym(hc, "android_dlopen");
    android_dlsym = (fn_android_dlsym)dlsym(hc, "android_dlsym");
    if (!android_dlopen || !android_dlsym) {
        log_msg("FATAL: missing android_dlopen/android_dlsym in libhybris-common");
        return -1;
    }
    log_msg("libhybris-common loaded (android_dlopen=%p)", (void*)android_dlopen);
    return 0;
}

static int load_stock_egl(void)
{
    real_egl_lib = dlopen("/usr/local/lib/libEGL.so.1.0.0", RTLD_NOW);
    if (!real_egl_lib) { log_msg("FATAL: can't load libhybris EGL: %s", dlerror()); return -1; }

    /* Load all real EGL functions. Missing non-1.5 functions are OK (NULL). */
#define LOAD_REQUIRED(name) do { \
    real_##name = (typeof(real_##name))dlsym(real_egl_lib, #name); \
    if (!real_##name) { log_msg("missing required: %s", #name); return -1; } \
} while(0)

#define LOAD_OPTIONAL(name) do { \
    real_##name = (typeof(real_##name))dlsym(real_egl_lib, #name); \
} while(0)

    /* Core EGL 1.0-1.4 (required) */
    LOAD_REQUIRED(eglGetDisplay);
    LOAD_REQUIRED(eglInitialize);
    LOAD_REQUIRED(eglTerminate);
    LOAD_REQUIRED(eglQueryString);
    LOAD_REQUIRED(eglGetConfigs);
    LOAD_REQUIRED(eglChooseConfig);
    LOAD_REQUIRED(eglGetConfigAttrib);
    LOAD_REQUIRED(eglCreateWindowSurface);
    LOAD_REQUIRED(eglCreatePbufferSurface);
    LOAD_REQUIRED(eglCreatePixmapSurface);
    LOAD_REQUIRED(eglDestroySurface);
    LOAD_REQUIRED(eglQuerySurface);
    LOAD_REQUIRED(eglBindAPI);
    LOAD_REQUIRED(eglReleaseThread);
    LOAD_REQUIRED(eglSurfaceAttrib);
    LOAD_REQUIRED(eglSwapInterval);
    LOAD_REQUIRED(eglCreateContext);
    LOAD_REQUIRED(eglDestroyContext);
    LOAD_REQUIRED(eglMakeCurrent);
    LOAD_REQUIRED(eglGetCurrentContext);
    LOAD_REQUIRED(eglGetCurrentSurface);
    LOAD_REQUIRED(eglGetCurrentDisplay);
    LOAD_REQUIRED(eglSwapBuffers);
    LOAD_REQUIRED(eglCopyBuffers);
    LOAD_REQUIRED(eglGetError);
    LOAD_REQUIRED(eglQueryContext);
    LOAD_REQUIRED(eglWaitGL);
    LOAD_REQUIRED(eglWaitNative);
    LOAD_REQUIRED(eglGetProcAddress);

    /* Optional (EGL 1.5 or may not exist on older libhybris) */
    LOAD_OPTIONAL(eglQueryAPI);
    LOAD_OPTIONAL(eglWaitClient);
    LOAD_OPTIONAL(eglCreatePbufferFromClientBuffer);
    LOAD_OPTIONAL(eglBindTexImage);
    LOAD_OPTIONAL(eglReleaseTexImage);
    LOAD_OPTIONAL(eglCreateSync);
    LOAD_OPTIONAL(eglDestroySync);
    LOAD_OPTIONAL(eglClientWaitSync);
    LOAD_OPTIONAL(eglGetSyncAttrib);
    LOAD_OPTIONAL(eglCreateImage);
    LOAD_OPTIONAL(eglDestroyImage);
    LOAD_OPTIONAL(eglGetPlatformDisplay);
    LOAD_OPTIONAL(eglCreatePlatformWindowSurface);
    LOAD_OPTIONAL(eglCreatePlatformPixmapSurface);
    LOAD_OPTIONAL(eglWaitSync);

#undef LOAD_REQUIRED
#undef LOAD_OPTIONAL

    return 0;
}

static int load_extensions(void)
{
    pfn_getClientBuffer = (fn_eglGetNativeClientBufferANDROID)
        real_eglGetProcAddress("eglGetNativeClientBufferANDROID");
    pfn_createImage = (fn_eglCreateImageKHR)
        real_eglGetProcAddress("eglCreateImageKHR");
    pfn_destroyImage = (fn_eglDestroyImageKHR)
        real_eglGetProcAddress("eglDestroyImageKHR");
    pfn_imageTargetTex = (fn_glEGLImageTargetTexture2DOES)
        real_eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!pfn_getClientBuffer || !pfn_createImage || !pfn_imageTargetTex) {
        log_msg("FATAL: missing EGL/GL extensions");
        return -1;
    }

    /* Resolve GL function pointers via eglGetProcAddress */
#define RESOLVE_GL_FN(ret, name, args) \
    pfn_##name = (ret (*) args)real_eglGetProcAddress(#name); \
    if (!pfn_##name) { log_msg("FATAL: missing GL function: " #name); return -1; }
    GL_FUNCTIONS(RESOLVE_GL_FN)
#undef RESOLVE_GL_FN
    log_msg("all GL functions resolved via eglGetProcAddress");

    return 0;
}

static void build_extension_string(void)
{
    /* Get real driver extensions */
    const char *real_ext = real_eglQueryString(stock_display, EGL_EXTENSIONS);
    if (!real_ext) real_ext = "";

    /* Extensions we add for Wayland compatibility */
    static const char *added_exts[] = {
        "EGL_KHR_platform_wayland",
        "EGL_EXT_platform_wayland",
        "EGL_EXT_platform_base",
        "EGL_EXT_buffer_age",
        "EGL_EXT_swap_buffers_with_damage",
        "EGL_KHR_create_context",
        "EGL_KHR_surfaceless_context",
        NULL,
    };

    /* Calculate size */
    size_t len = strlen(real_ext) + 1;
    for (int i = 0; added_exts[i]; i++) {
        if (!strstr(real_ext, added_exts[i]))
            len += strlen(added_exts[i]) + 1;
    }

    cached_extensions = malloc(len + 1);
    strcpy(cached_extensions, real_ext);

    for (int i = 0; added_exts[i]; i++) {
        if (!strstr(real_ext, added_exts[i])) {
            if (cached_extensions[0] && cached_extensions[strlen(cached_extensions)-1] != ' ')
                strcat(cached_extensions, " ");
            strcat(cached_extensions, added_exts[i]);
        }
    }
}

/* Patch bionic's __cfi_slowpath to a no-op return.
 * Android vendor libraries are compiled with Control Flow Integrity (CFI).
 * CFI checks call __cfi_slowpath in libdl.so, which looks up a shadow table.
 * In the libhybris/glibc environment, the CFI shadow table is never
 * initialized (it's normally set up by the bionic dynamic linker at process
 * start). The uninitialized NULL pointer causes a crash in eglInitialize.
 * Fix: patch __cfi_slowpath to just return immediately. */
static void patch_bionic_cfi(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) < 3) continue;
        if (!strstr(line, "libdl.so")) continue;
        if (perms[0] != 'r' || perms[2] != 'x') continue;

        uint32_t *code = (uint32_t *)lo;
        size_t count = (hi - lo) / 4;
        for (size_t i = 0; i + 1 < count; i++) {
            /* Android 14: sub w8,w1,#0x7f,lsl#12 + ubfx x9,x8,#31,#7
             * Android 16+: xpaclri + movn x9,#0xaf40,lsl#16 */
            if ((code[i] == 0xd351fc28 && code[i+1] == 0xd35f9909) ||
                (code[i] == 0xd50320ff && code[i+1] == 0x92b5e809)) {
                unsigned long page = (unsigned long)&code[i] & ~0xFFFUL;
                if (mprotect((void*)page, 4096, PROT_READ|PROT_WRITE|PROT_EXEC) == 0) {
                    code[i] = 0xd65f03c0; /* ret */
                    __builtin___clear_cache((char*)&code[i], (char*)&code[i+1]);
                    log_msg("patched __cfi_slowpath in bionic libdl.so");
                }
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
}

static void do_init(void)
{
    log_msg("initializing tawc EGL wrapper");

    if (load_hybris_common() < 0) return;
    if (load_stock_egl() < 0) return;

    real_eglBindAPI(EGL_OPENGL_ES_API);
    stock_display = real_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (stock_display == EGL_NO_DISPLAY) {
        log_msg("FATAL: stock eglGetDisplay failed: 0x%x", real_eglGetError());
        return;
    }

    /* Patch CFI after eglGetDisplay loads bionic libraries but before
     * eglInitialize triggers CFI-checked indirect calls */
    patch_bionic_cfi();

    if (!real_eglInitialize(stock_display, NULL, NULL)) {
        log_msg("FATAL: stock eglInitialize failed: 0x%x", real_eglGetError());
        return;
    }
    if (load_extensions() < 0) return;

    build_extension_string();

    log_msg("stock EGL initialized, display=%p", stock_display);
    initialized = 1;
    init_result = 0;
}

static int ensure_init(void)
{
    pthread_once(&init_once_ctrl, do_init);
    return init_result;
}

/* ------------------------------------------------------------------ */
/* Surface helpers (must hold surfaces_mutex)                          */
/* ------------------------------------------------------------------ */

static struct tawc_surface *find_surface_locked(EGLSurface surface)
{
    struct tawc_surface *ts = (struct tawc_surface *)surface;
    for (int i = 0; i < MAX_SURFACES; i++)
        if (surfaces[i].in_use && &surfaces[i] == ts) return ts;
    return NULL;
}

static struct tawc_surface *find_surface(EGLSurface surface)
{
    pthread_mutex_lock(&surfaces_mutex);
    struct tawc_surface *ts = find_surface_locked(surface);
    pthread_mutex_unlock(&surfaces_mutex);
    return ts;
}

static int ensure_ahb_loaded(void)
{
    if (ahb_allocate) return 0;
    return load_ahb_functions();
}

/* ------------------------------------------------------------------ */
/* Buffer allocation / deallocation                                    */
/* ------------------------------------------------------------------ */

static int alloc_buffer(struct tawc_buffer *buf, int w, int h)
{
    if (ensure_ahb_loaded() < 0) return -1;
    AHardwareBuffer_Desc desc = {
        .width = w, .height = h, .layers = 1,
        .format = AHB_FORMAT_RGBA8,
        .usage = AHB_USAGE_GPU_SAMPLED | AHB_USAGE_GPU_COLOR,
    };

    if (ahb_allocate(&desc, &buf->ahb) != 0) {
        log_msg("AHB allocate %dx%d failed", w, h);
        return -1;
    }

    EGLClientBuffer cb = pfn_getClientBuffer(buf->ahb);
    if (!cb) { log_msg("getClientBuffer failed"); return -1; }

    buf->image = pfn_createImage(stock_display, EGL_NO_CONTEXT,
                                 EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
    if (buf->image == EGL_NO_IMAGE_KHR) {
        log_msg("createImage failed: 0x%x", real_eglGetError());
        return -1;
    }

    glGenTextures(1, &buf->tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, buf->tex);
    pfn_imageTargetTex(GL_TEXTURE_EXTERNAL_OES, buf->image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &buf->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_EXTERNAL_OES, buf->tex, 0);

    glGenRenderbuffers(1, &buf->depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, buf->depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, buf->depth_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, buf->depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_msg("FBO incomplete: 0x%x", status);
        return -1;
    }

    buf->width = w;
    buf->height = h;
    log_msg("buffer allocated: %dx%d fbo=%u tex=%u", w, h, buf->fbo, buf->tex);
    return 0;
}

static void free_buffer(struct tawc_buffer *buf)
{
    if (buf->fbo) glDeleteFramebuffers(1, &buf->fbo);
    if (buf->depth_rb) glDeleteRenderbuffers(1, &buf->depth_rb);
    if (buf->tex) glDeleteTextures(1, &buf->tex);
    if (buf->image && pfn_destroyImage) pfn_destroyImage(stock_display, buf->image);
    if (buf->ahb) ahb_release(buf->ahb);
    memset(buf, 0, sizeof(*buf));
}

static int reallocate_buffers(struct tawc_surface *ts, int w, int h)
{
    log_msg("reallocating buffers: %dx%d -> %dx%d", ts->width, ts->height, w, h);
    for (int i = 0; i < NUM_BUFFERS; i++)
        free_buffer(&ts->buffers[i]);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (alloc_buffer(&ts->buffers[i], w, h) < 0) {
            log_msg("realloc failed at buffer %d", i);
            return -1;
        }
    }
    ts->width = w;
    ts->height = h;
    ts->current = 0;
    ts->swap_count = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* EGL API -- intercepted functions                                    */
/* ------------------------------------------------------------------ */

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    /* If given a wayland display, do protocol binding FIRST (before any
     * libhybris/bionic initialization, since TLS patching deadlocks
     * wl_display_roundtrip_queue) */
    if (display_id != EGL_DEFAULT_DISPLAY && !wayland_display) {
        wayland_display = (struct wl_display *)display_id;
        log_msg("eglGetDisplay: wayland display %p", wayland_display);

        /* Non-blocking registry bind -- eglGetDisplay may be called from
         * inside wl_display_dispatch (e.g. from a registry callback) */
        struct wl_registry *reg = wl_display_get_registry(wayland_display);
        wl_registry_add_listener(reg, &registry_listener, NULL);
        wl_display_flush(wayland_display);
        wl_display_dispatch_pending(wayland_display);
        log_msg("buffer_manager after pending dispatch: %p", (void*)buffer_manager);
    }

    if (ensure_init() < 0) return EGL_NO_DISPLAY;
    return stock_display;
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
                                 const EGLAttrib *attrib_list)
{
    return eglGetDisplay((EGLNativeDisplayType)native_display);
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    log_msg("eglInitialize: dpy=%p", dpy);
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    /* Do NOT forward to real driver. The stock display is managed by the
     * wrapper and must remain alive. Firefox's WebRender calls eglTerminate
     * then eglInitialize to retry with different config, and our eglInitialize
     * doesn't re-init the stock display. */
    (void)dpy;
    return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    /* Android GPU drivers only support GLES, not desktop GL.
     * Reject EGL_OPENGL_API so callers know desktop GL is unavailable.
     * GTK3 with GDK_GL=gles calls eglBindAPI(EGL_OPENGL_ES_API) directly.
     * GTK3 3.24.35+ gracefully falls back to SHM on this failure. */
    if (api == EGL_OPENGL_API) {
        log_msg("eglBindAPI: rejecting EGL_OPENGL_API (no desktop GL)");
        return EGL_FALSE;
    }
    if (initialized)
        return real_eglBindAPI(api);
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    if (ensure_init() < 0) return EGL_FALSE;

    /* Modify config attributes:
     * 1. Add PBUFFER_BIT to surface type (we use pbuffer as dummy surface)
     * 2. Ensure EGL_RENDERABLE_TYPE includes GLES2 (default is GLES1 only) */
    EGLint modified[64];
    int i = 0, j = 0;
    int has_renderable_type = 0;
    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 58) {
            if (attrib_list[i] == EGL_SURFACE_TYPE) {
                modified[j++] = EGL_SURFACE_TYPE;
                modified[j++] = attrib_list[i+1] | EGL_PBUFFER_BIT;
                i += 2;
            } else if (attrib_list[i] == EGL_RENDERABLE_TYPE) {
                modified[j++] = EGL_RENDERABLE_TYPE;
                modified[j++] = attrib_list[i+1] | EGL_OPENGL_ES2_BIT;
                has_renderable_type = 1;
                i += 2;
            } else {
                modified[j++] = attrib_list[i++];
                modified[j++] = attrib_list[i++];
            }
        }
    }
    if (!has_renderable_type && j < 60) {
        modified[j++] = EGL_RENDERABLE_TYPE;
        modified[j++] = EGL_OPENGL_ES2_BIT;
    }
    modified[j] = EGL_NONE;

    EGLint count = 0;
    EGLBoolean ret = real_eglChooseConfig(stock_display, modified, configs, config_size, &count);
    log_msg("eglChooseConfig: ret=%d count=%d config_size=%d", ret, count, config_size);
    if (num_config) *num_config = count;
    return ret;
}

#define EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FE
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x30FB
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FD
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR 0x00000002
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR 0x00000004
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR 0x31BD
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT 0x30BF

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share,
                            const EGLint *attribs)
{
    if (ensure_init() < 0) return EGL_NO_CONTEXT;

    /* Normalize desktop-GL context attributes to GLES-compatible ones.
     * GTK3 thinks it's doing desktop GL and passes KHR attributes that
     * stock Android GLES drivers don't understand. We:
     * 1. Strip PROFILE_MASK (desktop GL only)
     * 2. Strip FORWARD_COMPATIBLE flag (desktop GL only)
     * 3. Convert MAJOR/MINOR_VERSION_KHR to EGL_CONTEXT_CLIENT_VERSION
     * 4. Clamp to GLES 3.x (stock drivers support up to GLES 3.2) */
    EGLint filtered[32];
    int j = 0;
    int has_client_version = 0;
    int requested_major = 0;

    if (attribs) {
        for (int i = 0; attribs[i] != EGL_NONE; i += 2) {
            switch (attribs[i]) {
            case EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR:
                /* Desktop GL only -- skip */
                break;
            case EGL_CONTEXT_MINOR_VERSION_KHR:
                /* GLES doesn't use minor version -- skip */
                break;
            case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR:
            case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT:
            case EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT:
                /* Robustness extensions -- Android GLES drivers don't support these.
                 * Firefox requests them for crash resilience but they're optional. */
                break;
            case EGL_CONTEXT_FLAGS_KHR:
                {
                    EGLint flags = attribs[i+1]
                        & ~EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR
                        & ~EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
                    if (flags && j < 28) {
                        filtered[j++] = attribs[i];
                        filtered[j++] = flags;
                    }
                }
                break;
            case EGL_CONTEXT_MAJOR_VERSION_KHR:
                /* Convert to EGL_CONTEXT_CLIENT_VERSION for GLES.
                 * Request GLES 3 (highest that makes sense). */
                if (!has_client_version && j < 28) {
                    requested_major = attribs[i+1];
                    filtered[j++] = EGL_CONTEXT_CLIENT_VERSION;
                    filtered[j++] = (requested_major >= 3) ? 3 : requested_major;
                    has_client_version = 1;
                }
                break;
            case EGL_CONTEXT_CLIENT_VERSION:
                if (!has_client_version && j < 28) {
                    filtered[j++] = attribs[i];
                    filtered[j++] = attribs[i+1];
                    has_client_version = 1;
                }
                break;
            default:
                if (j < 28) {
                    filtered[j++] = attribs[i];
                    filtered[j++] = attribs[i+1];
                }
                break;
            }
        }
    }
    /* Default to GLES 3 if no version specified */
    if (!has_client_version && j < 28) {
        filtered[j++] = EGL_CONTEXT_CLIENT_VERSION;
        filtered[j++] = 3;
    }
    filtered[j] = EGL_NONE;

    log_msg("eglCreateContext: config=%p share=%p (%d attribs)", config, share, j/2);
    EGLContext ctx = real_eglCreateContext(stock_display, config, share, filtered);
    if (ctx == EGL_NO_CONTEXT)
        log_msg("eglCreateContext FAILED: error=0x%x", real_eglGetError());
    return ctx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    if (!initialized) return EGL_FALSE;
    return real_eglDestroyContext(stock_display, ctx);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                   EGLNativeWindowType win, const EGLint *attribs)
{
    if (ensure_init() < 0) return EGL_NO_SURFACE;

    struct wl_egl_window *wl_win = (struct wl_egl_window *)win;
    int w = wl_win->width;
    int h = wl_win->height;
    struct wl_surface *wl_surf = wl_win->surface;

    log_msg("eglCreateWindowSurface: %dx%d surface=%p", w, h, wl_surf);

    /* Ensure buffer_manager is bound (lazy binding from eglGetDisplay) */
    if (!buffer_manager && wayland_display) {
        log_msg("buffer_manager not yet bound, doing roundtrip...");
        wl_display_roundtrip(wayland_display);
        log_msg("buffer_manager after roundtrip: %p", (void*)buffer_manager);
    }

    pthread_mutex_lock(&surfaces_mutex);

    /* Find free slot */
    struct tawc_surface *ts = NULL;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].in_use) {
            ts = &surfaces[i];
            break;
        }
    }
    if (!ts) {
        pthread_mutex_unlock(&surfaces_mutex);
        log_msg("too many surfaces");
        return EGL_NO_SURFACE;
    }

    memset(ts, 0, sizeof(*ts));
    ts->wl_surf = wl_surf;
    ts->wl_win = wl_win;
    ts->width = w;
    ts->height = h;
    ts->side_fd = -1;
    ts->current = 0;
    ts->swap_count = 0;
    ts->config = config;
    ts->in_use = 1;

    pthread_mutex_unlock(&surfaces_mutex);

    /* Create real pbuffer for MakeCurrent (1x1, just for context binding) */
    EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ts->real_pbuffer = real_eglCreatePbufferSurface(stock_display, config, pb_attrs);
    if (ts->real_pbuffer == EGL_NO_SURFACE) {
        log_msg("pbuffer creation failed");
        ts->in_use = 0;
        return EGL_NO_SURFACE;
    }

    /* Request AHB channel from compositor */
    if (buffer_manager) {
        ts->channel = tawc_buffer_manager_v1_get_channel(buffer_manager, wl_surf);
        tawc_ahb_channel_v1_add_listener(ts->channel, &channel_listener, ts);

        wl_display_roundtrip(wayland_display);

        if (ts->side_fd < 0) {
            log_msg("WARNING: didn't receive side channel fd");
        } else {
            log_msg("got side channel fd=%d for surface %p", ts->side_fd, wl_surf);
        }
    }

    return (EGLSurface)ts;
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
                                           void *native_window,
                                           const EGLAttrib *attrib_list)
{
    return eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)native_window, NULL);
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                    const EGLint *attribs)
{
    if (ensure_init() < 0) return EGL_NO_SURFACE;
    return real_eglCreatePbufferSurface(stock_display, config, attribs);
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    pthread_mutex_lock(&surfaces_mutex);
    struct tawc_surface *ts = find_surface_locked(surface);
    if (ts) {
        for (int j = 0; j < NUM_BUFFERS; j++)
            free_buffer(&ts->buffers[j]);
        if (ts->channel)
            tawc_ahb_channel_v1_destroy(ts->channel);
        if (ts->real_pbuffer)
            real_eglDestroySurface(stock_display, ts->real_pbuffer);
        if (ts->side_fd >= 0)
            close(ts->side_fd);
        /* Clear TLS if this was the current surface */
        if (tls_current_surface == ts)
            tls_current_surface = NULL;
        ts->in_use = 0;
        pthread_mutex_unlock(&surfaces_mutex);
        log_msg("surface destroyed");
        return EGL_TRUE;
    }
    pthread_mutex_unlock(&surfaces_mutex);
    if (!initialized) return EGL_FALSE;
    return real_eglDestroySurface(stock_display, surface);
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                           EGLContext ctx)
{
    struct tawc_surface *ts_draw = find_surface(draw);
    struct tawc_surface *ts_read = find_surface(read);

    /* Unbind case */
    if (ctx == EGL_NO_CONTEXT) {
        tls_current_surface = NULL;
        tls_current_context = EGL_NO_CONTEXT;
        if (!initialized) return EGL_TRUE;
        return real_eglMakeCurrent(stock_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (!ts_draw) {
        /* Not our surface, pass through */
        tls_current_surface = NULL;
        tls_current_context = ctx;
        return real_eglMakeCurrent(stock_display, draw, read, ctx);
    }

    /* Bind the real pbuffer to make the GL context current */
    EGLSurface read_surf = ts_read ? ts_read->real_pbuffer : ts_draw->real_pbuffer;
    if (!real_eglMakeCurrent(stock_display, ts_draw->real_pbuffer, read_surf, ctx)) {
        log_msg("MakeCurrent pbuffer failed: 0x%x", real_eglGetError());
        return EGL_FALSE;
    }

    /* Allocate buffers if not yet done (needs active GL context) */
    if (ts_draw->buffers[0].fbo == 0) {
        log_msg("allocating %d buffers (%dx%d)", NUM_BUFFERS, ts_draw->width, ts_draw->height);
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (alloc_buffer(&ts_draw->buffers[i], ts_draw->width, ts_draw->height) < 0) {
                log_msg("buffer alloc failed");
                return EGL_FALSE;
            }
        }
    }

    /* Bind current buffer's FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, ts_draw->buffers[ts_draw->current].fbo);
    glViewport(0, 0, ts_draw->width, ts_draw->height);

    tls_current_surface = ts_draw;
    tls_current_context = ctx;

    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    struct tawc_surface *ts = find_surface(surface);
    if (!ts) {
        if (!initialized) return EGL_FALSE;
        return real_eglSwapBuffers(stock_display, surface);
    }

    /* Check for resize (wl_egl_window_resize modifies fields in place) */
    if (ts->wl_win &&
        (ts->wl_win->width != ts->width || ts->wl_win->height != ts->height)) {
        int new_w = ts->wl_win->width;
        int new_h = ts->wl_win->height;
        if (new_w > 0 && new_h > 0) {
            if (reallocate_buffers(ts, new_w, new_h) < 0) {
                log_msg("resize realloc failed");
                return EGL_FALSE;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, ts->buffers[ts->current].fbo);
            glViewport(0, 0, new_w, new_h);
        }
    }

    glFinish();

    struct tawc_buffer *buf = &ts->buffers[ts->current];

    if (ts->side_fd >= 0 && ts->channel && ahb_send) {
        if (ahb_send(buf->ahb, ts->side_fd) != 0) {
            log_msg("AHB send failed");
            return EGL_FALSE;
        }

        tawc_ahb_channel_v1_attach(ts->channel, buf->width, buf->height);
        wl_surface_commit(ts->wl_surf);
        wl_display_flush(wayland_display);
    }

    ts->swap_count++;

    /* Rotate to next buffer */
    ts->current = (ts->current + 1) % NUM_BUFFERS;
    glBindFramebuffer(GL_FRAMEBUFFER, ts->buffers[ts->current].fbo);
    glViewport(0, 0, ts->width, ts->height);

    return EGL_TRUE;
}

static EGLBoolean tawc_eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface,
                                                     const EGLint *rects, EGLint n_rects)
{
    /* We don't use damage info yet -- just swap normally */
    return eglSwapBuffers(dpy, surface);
}

static EGLBoolean tawc_eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surface,
                                               EGLint *rects, EGLint n_rects)
{
    /* No-op: we don't track damage regions. Firefox calls this before
     * eglSwapBuffersWithDamageKHR to mark which regions will be redrawn. */
    (void)dpy; (void)surface; (void)rects; (void)n_rects;
    return EGL_TRUE;
}

/* ------------------------------------------------------------------ */
/* EGL API -- query/state functions with tawc surface awareness        */
/* ------------------------------------------------------------------ */

EGLint eglGetError(void)
{
    if (!initialized) return EGL_SUCCESS;
    return real_eglGetError();
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    log_msg("eglQueryString: dpy=%p name=0x%x", dpy, name);

    /* EGL_EXTENSIONS with EGL_NO_DISPLAY returns client extensions (EGL 1.5).
     * This is how libepoxy checks for EGL_EXT_platform_base. */
    if (dpy == EGL_NO_DISPLAY && name == EGL_EXTENSIONS) {
        const char *client_ext = "EGL_EXT_platform_base EGL_KHR_platform_wayland "
                                 "EGL_EXT_platform_wayland EGL_EXT_client_extensions";
        log_msg("eglQueryString: returning client extensions: %s", client_ext);
        return client_ext;
    }

    if (ensure_init() < 0) return "";

    switch (name) {
    case EGL_CLIENT_APIS:
        return real_eglQueryString(stock_display, EGL_CLIENT_APIS);
    case EGL_EXTENSIONS:
        log_msg("eglQueryString: returning display extensions");
        return cached_extensions ? cached_extensions : "";
    case EGL_VERSION:
        return "1.5";
    case EGL_VENDOR:
        return "tawc";
    default:
        return real_eglQueryString(stock_display, name);
    }
}

/* EGL_EXT_device_query stubs for Firefox glxtest compatibility.
 * Android drivers don't implement this extension (it's for DRM/DRI devices).
 * glxtest null-checks the function pointers but handles NULL return values. */
static const char *tawc_eglQueryDeviceStringEXT(void *device, EGLint name)
{
    (void)device; (void)name;
    return NULL;
}

static EGLBoolean tawc_eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, intptr_t *value)
{
    (void)dpy; (void)attribute; (void)value;
    return EGL_FALSE;
}

void (*eglGetProcAddress(const char *procname))(void)
{
    /* Intercept our functions first -- no init required */
#define CHECK(name) if (strcmp(procname, #name) == 0) return (void(*)(void))name;
    CHECK(eglGetDisplay)
    CHECK(eglGetPlatformDisplay)
    CHECK(eglInitialize)
    CHECK(eglTerminate)
    CHECK(eglChooseConfig)
    CHECK(eglGetConfigAttrib)
    CHECK(eglCreateContext)
    CHECK(eglDestroyContext)
    CHECK(eglCreateWindowSurface)
    CHECK(eglCreatePlatformWindowSurface)
    CHECK(eglCreatePbufferSurface)
    CHECK(eglDestroySurface)
    CHECK(eglMakeCurrent)
    CHECK(eglSwapBuffers)
    CHECK(eglQuerySurface)
    CHECK(eglQueryString)
    CHECK(eglGetError)
    CHECK(eglGetCurrentContext)
    CHECK(eglGetCurrentSurface)
    CHECK(eglGetCurrentDisplay)
    CHECK(eglBindAPI)
    CHECK(eglSwapInterval)
    CHECK(eglReleaseThread)
    CHECK(eglWaitGL)
    CHECK(eglWaitNative)
    CHECK(eglGetConfigs)
    CHECK(eglSurfaceAttrib)
    CHECK(eglQueryContext)
    CHECK(eglGetProcAddress)
#undef CHECK

    /* Platform display extensions (libepoxy/GTK3 look for these) */
    if (strcmp(procname, "eglGetPlatformDisplayEXT") == 0)
        return (void(*)(void))eglGetPlatformDisplay;
    if (strcmp(procname, "eglCreatePlatformWindowSurfaceEXT") == 0)
        return (void(*)(void))eglCreatePlatformWindowSurface;

    /* Damage extensions (GTK3/Firefox use these) */
    if (strcmp(procname, "eglSwapBuffersWithDamageEXT") == 0)
        return (void(*)(void))tawc_eglSwapBuffersWithDamageEXT;
    if (strcmp(procname, "eglSwapBuffersWithDamageKHR") == 0)
        return (void(*)(void))tawc_eglSwapBuffersWithDamageEXT;
    if (strcmp(procname, "eglSetDamageRegionKHR") == 0)
        return (void(*)(void))tawc_eglSetDamageRegionKHR;

    /* EGL_EXT_device_query stubs -- Firefox's glxtest hard-requires these
     * function pointers even though Android drivers don't implement the
     * extension (it's DRM/DRI-centric). The actual return values are
     * informational; callers handle NULL results gracefully. */
    if (strcmp(procname, "eglQueryDeviceStringEXT") == 0)
        return (void(*)(void))tawc_eglQueryDeviceStringEXT;
    if (strcmp(procname, "eglQueryDisplayAttribEXT") == 0)
        return (void(*)(void))tawc_eglQueryDisplayAttribEXT;

    /* Forward everything else to real driver */
    if (ensure_init() < 0)
        return NULL;
    return (void(*)(void))real_eglGetProcAddress(procname);
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    return EGL_TRUE; /* we control frame pacing */
}

EGLBoolean eglReleaseThread(void)
{
    tls_current_surface = NULL;
    tls_current_context = EGL_NO_CONTEXT;
    if (!initialized) return EGL_TRUE;
    return real_eglReleaseThread();
}

EGLContext eglGetCurrentContext(void)
{
    return tls_current_context;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw)
{
    if (tls_current_surface)
        return (EGLSurface)tls_current_surface;
    if (!initialized) return EGL_NO_SURFACE;
    return real_eglGetCurrentSurface(readdraw);
}

EGLDisplay eglGetCurrentDisplay(void)
{
    if (tls_current_surface || tls_current_context != EGL_NO_CONTEXT)
        return stock_display;
    if (!initialized) return EGL_NO_DISPLAY;
    return stock_display;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint *value)
{
    struct tawc_surface *ts = find_surface(surface);
    if (ts) {
        switch (attr) {
        case EGL_WIDTH:
            *value = ts->width;
            return EGL_TRUE;
        case EGL_HEIGHT:
            *value = ts->height;
            return EGL_TRUE;
        case EGL_BUFFER_AGE_EXT:
            /* With N buffers, after the first N swaps each buffer was last
             * used N frames ago. Before that, age = 0 (undefined). */
            if (ts->swap_count >= NUM_BUFFERS)
                *value = NUM_BUFFERS;
            else
                *value = 0;
            return EGL_TRUE;
        default:
            /* Forward other queries to the real pbuffer */
            if (!initialized) return EGL_FALSE;
            return real_eglQuerySurface(stock_display, ts->real_pbuffer, attr, value);
        }
    }
    if (!initialized) return EGL_FALSE;
    return real_eglQuerySurface(stock_display, surface, attr, value);
}

/* ------------------------------------------------------------------ */
/* EGL API -- pure pass-through / stub functions                       */
/* ------------------------------------------------------------------ */

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attr, EGLint *value)
{
    if (ensure_init() < 0) return EGL_FALSE;
    EGLBoolean ret = real_eglGetConfigAttrib(stock_display, config, attr, value);
    /* Report WINDOW_BIT support even though we use pbuffers internally */
    if (ret && attr == EGL_SURFACE_TYPE && value)
        *value |= EGL_WINDOW_BIT;
    return ret;
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    if (ensure_init() < 0) return EGL_FALSE;
    return real_eglGetConfigs(stock_display, configs, config_size, num_config);
}

EGLenum eglQueryAPI(void)
{
    if (real_eglQueryAPI) return real_eglQueryAPI();
    return EGL_OPENGL_ES_API;
}

EGLBoolean eglWaitClient(void)
{
    if (real_eglWaitClient) return real_eglWaitClient();
    return EGL_TRUE;
}

EGLBoolean eglWaitGL(void)
{
    if (!initialized) return EGL_TRUE;
    return real_eglWaitGL();
}

EGLBoolean eglWaitNative(EGLint engine)
{
    if (!initialized) return EGL_TRUE;
    return real_eglWaitNative(engine);
}

EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target)
{
    return EGL_FALSE;
}

EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
                                    EGLNativePixmapType pixmap, const EGLint *attribs)
{
    return EGL_NO_SURFACE;
}

EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config,
                                            void *native_pixmap, const EGLAttrib *attribs)
{
    return EGL_NO_SURFACE;
}

EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype,
                                              EGLClientBuffer buffer, EGLConfig config,
                                              const EGLint *attribs)
{
    if (real_eglCreatePbufferFromClientBuffer)
        return real_eglCreatePbufferFromClientBuffer(stock_display, buftype, buffer, config, attribs);
    return EGL_NO_SURFACE;
}

EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint value)
{
    struct tawc_surface *ts = find_surface(surface);
    if (ts) return EGL_TRUE;  /* accept silently */
    if (!initialized) return EGL_TRUE;
    return real_eglSurfaceAttrib(stock_display, surface, attr, value);
}

EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    if (real_eglBindTexImage)
        return real_eglBindTexImage(stock_display, surface, buffer);
    return EGL_FALSE;
}

EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    if (real_eglReleaseTexImage)
        return real_eglReleaseTexImage(stock_display, surface, buffer);
    return EGL_FALSE;
}

/* eglQueryContext -- missing from original wrapper, GTK3/libepoxy needs it */
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value)
{
    if (ensure_init() < 0) return EGL_FALSE;
    return real_eglQueryContext(stock_display, ctx, attribute, value);
}

/* EGL 1.5 sync/image -- forward to real driver or stub */

EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attribs)
{
    if (real_eglCreateSync) return real_eglCreateSync(stock_display, type, attribs);
    return EGL_NO_SYNC;
}

EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync)
{
    if (real_eglDestroySync) return real_eglDestroySync(stock_display, sync);
    return EGL_TRUE;
}

EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)
{
    if (real_eglClientWaitSync) return real_eglClientWaitSync(stock_display, sync, flags, timeout);
    return EGL_CONDITION_SATISFIED;
}

EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attr, EGLAttrib *value)
{
    if (real_eglGetSyncAttrib) return real_eglGetSyncAttrib(stock_display, sync, attr, value);
    return EGL_FALSE;
}

EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                          EGLClientBuffer buffer, const EGLAttrib *attribs)
{
    if (real_eglCreateImage) return real_eglCreateImage(stock_display, ctx, target, buffer, attribs);
    return EGL_NO_IMAGE;
}

EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image)
{
    if (real_eglDestroyImage) return real_eglDestroyImage(stock_display, image);
    return EGL_TRUE;
}

EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags)
{
    if (real_eglWaitSync) return real_eglWaitSync(stock_display, sync, flags);
    return EGL_TRUE;
}

/* ------------------------------------------------------------------ */
/* Test helper -- creates a tawc_surface without compositor protocol   */
/* ------------------------------------------------------------------ */

EGLSurface tawc_create_test_surface(EGLDisplay dpy, EGLConfig config,
                                     int width, int height)
{
    if (ensure_init() < 0) return EGL_NO_SURFACE;

    pthread_mutex_lock(&surfaces_mutex);
    struct tawc_surface *ts = NULL;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].in_use) {
            ts = &surfaces[i];
            break;
        }
    }
    if (!ts) {
        pthread_mutex_unlock(&surfaces_mutex);
        log_msg("tawc_create_test_surface: no free slot");
        return EGL_NO_SURFACE;
    }

    memset(ts, 0, sizeof(*ts));
    ts->width = width;
    ts->height = height;
    ts->side_fd = -1;
    ts->config = config;
    ts->in_use = 1;
    pthread_mutex_unlock(&surfaces_mutex);

    EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ts->real_pbuffer = real_eglCreatePbufferSurface(stock_display, config, pb_attrs);
    if (ts->real_pbuffer == EGL_NO_SURFACE) {
        log_msg("tawc_create_test_surface: pbuffer failed");
        ts->in_use = 0;
        return EGL_NO_SURFACE;
    }

    log_msg("tawc_create_test_surface: %dx%d", width, height);
    return (EGLSurface)ts;
}
