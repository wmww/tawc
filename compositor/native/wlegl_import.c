// Import an android_wlegl native_handle_t (fds + ints from the wire) into
// an AHardwareBuffer.
//
// With the libhybris AHB gralloc backend client-side, buffers are allocated
// through AHardwareBuffer_allocate and the handle layout matches what the
// system's gralloc4 mapper expects. That lets us do the reverse on this side
// via the public NDK: AHardwareBuffer_createFromHandle. No direct gralloc,
// no manual ANativeWindowBuffer, no sphal namespace gymnastics.
//
// The returned AHB becomes the EGLClientBuffer via
// eglGetNativeClientBufferANDROID on the Rust side, then feeds into the
// existing gl_import.rs path (eglCreateImageKHR + GL_TEXTURE_EXTERNAL_OES).

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "tawc-native"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// native_handle_t isn't in the public NDK. Mirror the tiny struct here —
// it's a stable ABI documented by cutils/native_handle.h.
struct tawc_native_handle {
    int version;   // sizeof(native_handle_t) = 12
    int numFds;
    int numInts;
    int data[0];   // numFds fds followed by numInts ints
};

// AHardwareBuffer_createFromHandle method constants, from AOSP
// frameworks/native/libs/nativewindow/include/private/android/
// AHardwareBufferHelpers.h.
#define AHB_METHOD_CLONE    1
#define AHB_METHOD_REGISTER 2

typedef int (*fn_create_from_handle_t)(
    const AHardwareBuffer_Desc *desc,
    const struct tawc_native_handle *handle,
    int32_t method,
    AHardwareBuffer **out);

static pthread_once_t          g_once = PTHREAD_ONCE_INIT;
static fn_create_from_handle_t g_create_from_handle;
static void                  (*g_release)(AHardwareBuffer *);

static void load_symbols(void) {
    void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        LOGE("dlopen(libnativewindow.so) failed: %s", dlerror());
        return;
    }
    g_create_from_handle = (fn_create_from_handle_t)
        dlsym(lib, "AHardwareBuffer_createFromHandle");
    g_release = (void (*)(AHardwareBuffer *))
        dlsym(lib, "AHardwareBuffer_release");
    if (!g_create_from_handle || !g_release) {
        LOGE("dlsym AHardwareBuffer_* failed (createFromHandle=%p release=%p)",
             g_create_from_handle, g_release);
    }
}

// Import a client-allocated gralloc buffer. Returns an AHardwareBuffer* that
// the caller owns (release with tawc_wlegl_buffer_release). Returns NULL on
// failure.
AHardwareBuffer *tawc_wlegl_import(
    uint32_t width, uint32_t height, uint32_t stride,
    uint32_t format, uint64_t usage,
    const int *fds, int num_fds,
    const int32_t *ints, int num_ints)
{
    pthread_once(&g_once, load_symbols);
    if (!g_create_from_handle) return NULL;
    if (num_fds < 0 || num_ints < 0) {
        LOGE("tawc_wlegl_import: bad counts num_fds=%d num_ints=%d", num_fds, num_ints);
        return NULL;
    }

    // Build a native_handle_t inline: struct header + numFds fds + numInts ints.
    size_t sz = sizeof(struct tawc_native_handle)
              + (size_t)(num_fds + num_ints) * sizeof(int);
    struct tawc_native_handle *h = (struct tawc_native_handle *)malloc(sz);
    if (!h) return NULL;
    h->version = (int)sizeof(struct tawc_native_handle);
    h->numFds  = num_fds;
    h->numInts = num_ints;
    for (int i = 0; i < num_fds;  i++) h->data[i]           = fds[i];
    for (int i = 0; i < num_ints; i++) h->data[num_fds + i] = ints[i];

    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width  = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = format;
    desc.usage  = usage;
    desc.stride = stride;

    AHardwareBuffer *ahb = NULL;
    // REGISTER: AHB takes ownership of `h` (both the handle memory and its
    // fds). Do NOT free(h) on success — the underlying GraphicBuffer will
    // close+delete the handle when the AHB is released, and double-freeing
    // trips Scudo's chunk-state check. On failure, AHB doesn't retain
    // anything, so we free.
    int rc = g_create_from_handle(&desc, h, AHB_METHOD_REGISTER, &ahb);
    if (rc != 0 || !ahb) {
        LOGE("AHardwareBuffer_createFromHandle failed: rc=%d (%d %s)",
             rc, errno, strerror(errno));
        free(h);
        return NULL;
    }

    AHardwareBuffer_Desc actual;
    AHardwareBuffer_describe(ahb, &actual);
    if (actual.width != width || actual.height != height ||
        actual.stride != stride || actual.format != format) {
        LOGE("tawc_wlegl_import: desc mismatch: "
             "client=%ux%u stride=%u fmt=%u, "
             "actual=%ux%u stride=%u fmt=%u",
             width, height, stride, format,
             actual.width, actual.height, actual.stride, actual.format);
        g_release(ahb);
        return NULL;
    }

    return ahb;
}

void tawc_wlegl_buffer_release(AHardwareBuffer *ahb) {
    if (!ahb) return;
    pthread_once(&g_once, load_symbols);
    if (g_release) g_release(ahb);
}
