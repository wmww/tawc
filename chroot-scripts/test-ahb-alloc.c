/* Minimal test: can we call AHardwareBuffer_allocate from libhybris?
 *
 * Build inside chroot:
 *   gcc -o test-ahb-alloc test-ahb-alloc.c -lhybris-common -ldl
 *
 * Run:
 *   HYBRIS_PATCH_TLS=1 ./test-ahb-alloc
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <hybris/common/binding.h>

/* AHardwareBuffer types from Android NDK */
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

/* AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1 */
#define AHARDWAREBUFFER_FORMAT_RGBA8 1
/* GPU_SAMPLED_IMAGE = 1<<8, GPU_COLOR_OUTPUT = 1<<9 */
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR   (1ULL << 9)

typedef int (*AHardwareBuffer_allocate_t)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*AHardwareBuffer_release_t)(AHardwareBuffer*);

static volatile const char *crash_context = "startup";

void crash_handler(int sig) {
    const char *ctx = (const char*)crash_context;
    write(2, "CRASH (", 7);
    write(2, sig == SIGSEGV ? "SIGSEGV" : "SIGABRT", 7);
    write(2, ") during: ", 10);
    write(2, ctx, strlen(ctx));
    write(2, "\n", 1);
    _exit(128 + sig);
}

int main(void) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);

    printf("test-ahb-alloc: loading libnativewindow.so...\n");
    fflush(stdout);

    crash_context = "android_dlopen(libnativewindow.so)";
    void *lib = android_dlopen("/system/lib64/libnativewindow.so", 1 /* RTLD_LAZY */);
    if (!lib) {
        fprintf(stderr, "Failed to load libnativewindow.so: %s\n", android_dlerror());
        return 1;
    }
    printf("  libnativewindow.so loaded OK\n");
    fflush(stdout);

    crash_context = "dlsym(AHardwareBuffer_allocate)";
    AHardwareBuffer_allocate_t ahb_alloc =
        (AHardwareBuffer_allocate_t)android_dlsym(lib, "AHardwareBuffer_allocate");
    AHardwareBuffer_release_t ahb_release =
        (AHardwareBuffer_release_t)android_dlsym(lib, "AHardwareBuffer_release");

    if (!ahb_alloc) {
        fprintf(stderr, "Failed to find AHardwareBuffer_allocate\n");
        return 1;
    }
    printf("  AHardwareBuffer_allocate found at %p\n", (void*)ahb_alloc);
    fflush(stdout);

    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = 256;
    desc.height = 256;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_RGBA8;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED | AHARDWAREBUFFER_USAGE_GPU_COLOR;

    printf("  Attempting AHardwareBuffer_allocate(256x256 RGBA8)...\n");
    fflush(stdout);

    crash_context = "AHardwareBuffer_allocate";
    AHardwareBuffer *buf = NULL;
    int ret = ahb_alloc(&desc, &buf);

    if (ret != 0) {
        fprintf(stderr, "  FAILED: AHardwareBuffer_allocate returned %d\n", ret);
        return 1;
    }

    printf("  SUCCESS: AHardwareBuffer allocated at %p\n", (void*)buf);
    fflush(stdout);

    if (ahb_release) {
        crash_context = "AHardwareBuffer_release";
        ahb_release(buf);
        printf("  Released OK\n");
    }

    printf("test-ahb-alloc: PASSED\n");
    return 0;
}
