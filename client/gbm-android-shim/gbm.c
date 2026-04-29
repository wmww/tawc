/*
 * tawc Android-EGL libgbm shim — implementation.
 *
 * Each `gbm_bo` owns one AHardwareBuffer. AHB lets the bionic-side
 * EGL driver wrap it as an EGLImage via `EGL_ANDROID_get_native_client_buffer`,
 * which is what GLAMOR ultimately needs to render into a buffer the
 * compositor can present.
 *
 * Limits are documented in `gbm.h`. The big one: dmabuf-fd export
 * (`gbm_bo_get_fd*`) is NOT supported. Public AHB has no way to
 * extract the gralloc fd; Xwayland's dmabuf-protocol export path
 * will see -1 and fall through. The tawc-specific buffer-sharing
 * glue uses the AHardwareBuffer pointer directly via the
 * `gbm_bo_get_ahardwarebuffer()` accessor (and feeds it to
 * `android_wlegl.create_buffer_v2` on the compositor side).
 *
 * Thread-safety: AHB itself is reference-counted internally and safe
 * to share; bo state is otherwise single-owner. Xwayland never hands
 * a bo across threads.
 */

#include "gbm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <android/hardware_buffer.h>

/* Min API where AHardwareBuffer_allocate / _describe etc. are
 * available is API 26; we target 29. Header is in the NDK sysroot. */

struct gbm_device {
    int fd;       /* not used; we keep the fd for the API but never read /dev/dri */
};

struct gbm_bo {
    struct gbm_device *gbm;
    AHardwareBuffer   *ahb;
    AHardwareBuffer_Desc desc;
    uint32_t           drm_format;
    void              *user_data;
    void             (*user_data_destroy)(struct gbm_bo *, void *);
};

/* The single device singleton. Xwayland creates one device per X
 * screen and passes the same `int fd` we give it (because we don't
 * really hold a DRM fd). Returning the same struct each time is
 * harmless — there's no per-device state. */
static struct gbm_device tawc_gbm_device = { .fd = -1 };

/* ── format mapping ─────────────────────────────────────────────── */

static uint32_t
drm_format_to_ahb_format(uint32_t drm_format)
{
    switch (drm_format) {
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ARGB8888:
        /* Android's R8G8B8A8 maps directly to ARGB8888 in BGRA-byte
         * order on little-endian, matching the X server's expectation
         * for 32-bit visuals. */
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case GBM_FORMAT_XBGR8888:
    case GBM_FORMAT_ABGR8888:
        /* gralloc's R8G8B8A8 is byte-order RGBA → fourcc ABGR.
         * Most Android EGL drivers also accept this for RGB. */
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case GBM_FORMAT_RGB565:
        return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
    case GBM_FORMAT_BGR888:
        return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
    case GBM_FORMAT_ARGB2101010:
    case GBM_FORMAT_XRGB2101010:
        return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    default:
        return 0;
    }
}

static uint32_t
gbm_usage_to_ahb_usage(uint32_t gbm_usage)
{
    /* GLAMOR essentially always wants both render-target + sampler
     * access, and gralloc has to be told both. SCANOUT in our world
     * means "the compositor will sample it" — same as
     * GPU_FRAMEBUFFER + GPU_SAMPLED_IMAGE. CPU_READ/WRITE flags are
     * left off; gralloc may pick a non-CPU-mappable layout, which
     * matches the upstream `gbm_bo` semantics (no mmap by default).
     *
     * Always include CPU_READ_RARELY because some GLAMOR paths
     * `gbm_bo_write` into the buffer to upload pixels (icon, cursor)
     * — without CPU access gralloc returns a layout the CPU can't
     * touch and the write silently corrupts. RARELY (vs OFTEN) keeps
     * the GPU layout efficient on tilers. */
    uint32_t u = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (gbm_usage & GBM_BO_USE_WRITE)
        u |= AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY
           | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    if (gbm_usage & GBM_BO_USE_LINEAR)
        u |= AHARDWAREBUFFER_USAGE_CPU_READ_RARELY
           | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
    return u;
}

/* ── device ─────────────────────────────────────────────────────── */

struct gbm_device *
gbm_create_device(int fd)
{
    tawc_gbm_device.fd = fd;
    return &tawc_gbm_device;
}

void
gbm_device_destroy(struct gbm_device *gbm)
{
    /* Singleton — nothing to free. */
    (void)gbm;
}

int
gbm_device_get_fd(struct gbm_device *gbm)
{
    return gbm ? gbm->fd : -1;
}

const char *
gbm_device_get_backend_name(struct gbm_device *gbm)
{
    (void)gbm;
    /* Xwayland uses this string literally to set glvnd vendor; pick
     * "android" so it's distinct from Mesa's "drm" and an X-server
     * log message reads usefully. */
    return "android";
}

int
gbm_device_is_format_supported(struct gbm_device *gbm,
                               uint32_t format, uint32_t usage)
{
    (void)gbm; (void)usage;
    return drm_format_to_ahb_format(format) != 0;
}

/* ── bo lifecycle ───────────────────────────────────────────────── */

static struct gbm_bo *
allocate_bo(struct gbm_device *gbm, uint32_t width, uint32_t height,
            uint32_t format, uint32_t usage)
{
    uint32_t ahb_format = drm_format_to_ahb_format(format);
    if (ahb_format == 0) {
        fprintf(stderr,
                "tawc-gbm: unsupported drm format 0x%08x\n", format);
        return NULL;
    }

    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    if (!bo)
        return NULL;
    bo->gbm = gbm;
    bo->drm_format = format;

    AHardwareBuffer_Desc desc = {
        .width  = width,
        .height = height,
        .layers = 1,
        .format = ahb_format,
        .usage  = gbm_usage_to_ahb_usage(usage),
    };

    if (AHardwareBuffer_allocate(&desc, &bo->ahb) != 0 || bo->ahb == NULL) {
        fprintf(stderr,
                "tawc-gbm: AHardwareBuffer_allocate(%ux%u fmt=0x%x usage=0x%x) failed: %s\n",
                width, height, format, usage, strerror(errno));
        free(bo);
        return NULL;
    }
    /* Re-describe so we pick up the actual stride/layers/format the
     * gralloc chose. width/height are stable but stride is gralloc's
     * call. */
    AHardwareBuffer_describe(bo->ahb, &bo->desc);
    return bo;
}

struct gbm_bo *
gbm_bo_create(struct gbm_device *gbm,
              uint32_t width, uint32_t height,
              uint32_t format, uint32_t usage)
{
    return allocate_bo(gbm, width, height, format, usage);
}

struct gbm_bo *
gbm_bo_create_with_modifiers(struct gbm_device *gbm,
                             uint32_t width, uint32_t height,
                             uint32_t format,
                             const uint64_t *modifiers,
                             const unsigned int count)
{
    /* We don't honour modifiers; gralloc decides layout. If anything
     * other than INVALID/LINEAR is in the list, accept anyway (we'll
     * report INVALID back). Returns NULL on unsupported format only. */
    (void)modifiers; (void)count;
    return allocate_bo(gbm, width, height, format,
                       GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
}

struct gbm_bo *
gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
                              uint32_t width, uint32_t height,
                              uint32_t format,
                              const uint64_t *modifiers,
                              const unsigned int count,
                              uint32_t usage)
{
    (void)modifiers; (void)count;
    return allocate_bo(gbm, width, height, format, usage);
}

struct gbm_bo *
gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer,
              uint32_t usage)
{
    /* We don't support cross-process import paths (DRI3 / dmabuf
     * passing). All callers should be checking the EGL extension list
     * before walking down this path; if they don't, returning NULL
     * makes them fall back. */
    (void)gbm; (void)type; (void)buffer; (void)usage;
    errno = ENOSYS;
    return NULL;
}

void
gbm_bo_destroy(struct gbm_bo *bo)
{
    if (!bo)
        return;
    if (bo->user_data_destroy)
        bo->user_data_destroy(bo, bo->user_data);
    if (bo->ahb)
        AHardwareBuffer_release(bo->ahb);
    free(bo);
}

/* ── accessors ──────────────────────────────────────────────────── */

uint32_t gbm_bo_get_width(struct gbm_bo *bo)  { return bo->desc.width; }
uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo->desc.height; }
uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo->drm_format; }

uint32_t
gbm_bo_get_stride(struct gbm_bo *bo)
{
    /* AHardwareBuffer_Desc::stride is in pixels (per Android docs);
     * GBM expects bytes. Multiply by bpp. */
    uint32_t bpp = gbm_bo_get_bpp(bo);
    return bo->desc.stride * bpp / 8;
}

uint32_t
gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
    /* Single-plane in our world. */
    (void)plane;
    return gbm_bo_get_stride(bo);
}

uint32_t
gbm_bo_get_bpp(struct gbm_bo *bo)
{
    switch (bo->drm_format) {
    case GBM_FORMAT_RGB565:                return 16;
    case GBM_FORMAT_BGR888:                return 24;
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_XBGR8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_ARGB2101010:
    case GBM_FORMAT_XRGB2101010:           return 32;
    default:                                return 32;
    }
}

uint32_t
gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
    (void)bo; (void)plane;
    return 0;
}

uint64_t
gbm_bo_get_modifier(struct gbm_bo *bo)
{
    (void)bo;
    return GBM_FORMAT_MOD_INVALID;
}

int
gbm_bo_get_plane_count(struct gbm_bo *bo)
{
    (void)bo;
    return 1;
}

int
gbm_bo_get_fd(struct gbm_bo *bo)
{
    /* AHB doesn't expose a gralloc dmabuf fd publicly. Xwayland's
     * dmabuf-protocol export path checks this and treats -1 as a
     * graceful failure (sees no acceleration possible). */
    (void)bo;
    errno = ENOTSUP;
    return -1;
}

int
gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    (void)bo; (void)plane;
    errno = ENOTSUP;
    return -1;
}

union gbm_bo_handle
gbm_bo_get_handle(struct gbm_bo *bo)
{
    /* Xwayland uses this as an opaque cookie; passing the AHB pointer
     * lets tawc-specific glue retrieve it back via gbm_bo_get_handle
     * + cast. Real libgbm returns a uint32_t kernel handle here, but
     * Xwayland never assumes anything about the value. */
    union gbm_bo_handle h = { .ptr = bo->ahb };
    return h;
}

union gbm_bo_handle
gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
    (void)plane;
    return gbm_bo_get_handle(bo);
}

struct gbm_device *
gbm_bo_get_device(struct gbm_bo *bo)
{
    return bo->gbm;
}

void *
gbm_bo_get_ahardwarebuffer(struct gbm_bo *bo)
{
    return bo ? bo->ahb : NULL;
}

void
gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                     void (*destroy)(struct gbm_bo *, void *))
{
    bo->user_data = data;
    bo->user_data_destroy = destroy;
}

void *
gbm_bo_get_user_data(struct gbm_bo *bo)
{
    return bo->user_data;
}

int
gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count)
{
    /* Lock for CPU write, copy, unlock. Used by GLAMOR for cursor /
     * icon uploads. Returns 0 on success, -1 on failure. */
    void *dst = NULL;
    if (AHardwareBuffer_lock(bo->ahb,
                             AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY,
                             -1, NULL, &dst) != 0 || dst == NULL) {
        errno = EIO;
        return -1;
    }
    memcpy(dst, buf, count);
    AHardwareBuffer_unlock(bo->ahb, NULL);
    return 0;
}
