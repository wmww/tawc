/*
 * tawc Android-EGL libgbm shim — header.
 *
 * Subset of upstream gbm.h sufficient for Xwayland-24.1 to compile and
 * link. Each gbm_bo is backed by an `AHardwareBuffer` (allocated by
 * libnativewindow.so) so the buffer is something the bionic-side EGL
 * driver can wrap as an EGLImage via `EGL_ANDROID_get_native_client_buffer`.
 *
 * What is and isn't supported:
 *   - alloc/destroy: works (AHardwareBuffer_allocate / _release).
 *   - geometry / format / stride / handle: works (AHardwareBuffer_describe).
 *   - dmabuf fd export (`gbm_bo_get_fd*`): NOT supported. AHB doesn't
 *     expose the underlying gralloc fd publicly. Returns -1; callers
 *     that try to use this path will fail gracefully (Xwayland's
 *     dmabuf-protocol export path) — buffer transport to the
 *     compositor goes through a tawc-specific Wayland glue instead
 *     (see notes/xwayland.md).
 *   - modifiers: always DRM_FORMAT_MOD_INVALID. Linear is the only
 *     thing we promise; gralloc may pick a tiled layout under the
 *     hood, but we never expose it.
 *
 * Format constants are the standard fourcc codes (matching libdrm's
 * `<drm/drm_fourcc.h>` so X-server depth-to-format mapping in
 * `gbm_format_for_depth` is unchanged).
 */

#ifndef TAWC_GBM_SHIM_H
#define TAWC_GBM_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define __GBM__ 1

/* gbm 21.0 — Xwayland 24.1's pkg-config dep is `gbm >= 21.1.0` per
 * upstream meson.build. Bumping our pkg-config version to match
 * keeps `dependency('gbm', version: gbm_req)` happy. */
#define GBM_MAJOR_VERSION 21
#define GBM_MINOR_VERSION 0

/* Subset of the format fourccs Xwayland actually checks. Values match
 * libdrm's drm_fourcc.h so the equality compares in
 * gbm_format_for_depth() Just Work. */
#define GBM_FORMAT_ARGB8888 0x34325241  /* 'AR24' */
#define GBM_FORMAT_XRGB8888 0x34325258  /* 'XR24' */
#define GBM_FORMAT_ABGR8888 0x34324241  /* 'AB24' */
#define GBM_FORMAT_XBGR8888 0x34324258  /* 'XB24' */
#define GBM_FORMAT_RGB565   0x36314752  /* 'RG16' */
#define GBM_FORMAT_BGR888   0x34324742  /* 'BG24' — gralloc rarely backs this */
#define GBM_FORMAT_ARGB2101010 0x30335241
#define GBM_FORMAT_XRGB2101010 0x30335258

/* Usage flags. Xwayland passes SCANOUT|RENDERING; gralloc maps both
 * to GPU-accessible storage. Values are Mesa-compatible bit positions
 * so anything that builds against the real libgbm sees the same
 * meanings. */
#define GBM_BO_USE_SCANOUT      (1 << 0)
#define GBM_BO_USE_CURSOR       (1 << 1)
#define GBM_BO_USE_RENDERING    (1 << 2)
#define GBM_BO_USE_WRITE        (1 << 3)
#define GBM_BO_USE_LINEAR       (1 << 4)
#define GBM_BO_USE_PROTECTED    (1 << 5)

/* DRM_FORMAT_MOD_INVALID — we never advertise modifiers. */
#define GBM_FORMAT_MOD_INVALID  ((((uint64_t)0) << 56) | ((1ULL << 56) - 1))
#define GBM_FORMAT_MOD_LINEAR   0ULL

struct gbm_device;
struct gbm_bo;
struct gbm_surface;

union gbm_bo_handle {
    void     *ptr;
    int32_t  s32;
    uint32_t u32;
    int64_t  s64;
    uint64_t u64;
};

enum gbm_bo_transfer_flags {
    GBM_BO_TRANSFER_READ = (1 << 0),
    GBM_BO_TRANSFER_WRITE = (1 << 1),
    GBM_BO_TRANSFER_READ_WRITE = (GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE),
};

enum gbm_bo_format {
    GBM_BO_FORMAT_XRGB8888,
    GBM_BO_FORMAT_ARGB8888,
};

enum gbm_bo_import_type {
    GBM_BO_IMPORT_WL_BUFFER  = 0x5501,
    GBM_BO_IMPORT_EGL_IMAGE  = 0x5502,
    GBM_BO_IMPORT_FD         = 0x5503,
    GBM_BO_IMPORT_FD_MODIFIER = 0x5504,
};

struct gbm_import_fd_data {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

#define GBM_MAX_PLANES 4

struct gbm_import_fd_modifier_data {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t num_fds;
    int fds[GBM_MAX_PLANES];
    int strides[GBM_MAX_PLANES];
    int offsets[GBM_MAX_PLANES];
    uint64_t modifier;
};

/* ── lifecycle ── */
struct gbm_device *gbm_create_device(int fd);
void               gbm_device_destroy(struct gbm_device *gbm);
int                gbm_device_get_fd(struct gbm_device *gbm);
const char        *gbm_device_get_backend_name(struct gbm_device *gbm);
int                gbm_device_is_format_supported(struct gbm_device *gbm,
                                                  uint32_t format,
                                                  uint32_t usage);

/* ── allocation ── */
struct gbm_bo *gbm_bo_create(struct gbm_device *gbm,
                             uint32_t width, uint32_t height,
                             uint32_t format, uint32_t usage);
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm,
                                            uint32_t width, uint32_t height,
                                            uint32_t format,
                                            const uint64_t *modifiers,
                                            const unsigned int count);
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
                                             uint32_t width, uint32_t height,
                                             uint32_t format,
                                             const uint64_t *modifiers,
                                             const unsigned int count,
                                             uint32_t usage);
struct gbm_bo *gbm_bo_import(struct gbm_device *gbm,
                             uint32_t type, void *buffer,
                             uint32_t usage);
void           gbm_bo_destroy(struct gbm_bo *bo);

/* ── accessors ── */
uint32_t       gbm_bo_get_width(struct gbm_bo *bo);
uint32_t       gbm_bo_get_height(struct gbm_bo *bo);
uint32_t       gbm_bo_get_stride(struct gbm_bo *bo);
uint32_t       gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane);
uint32_t       gbm_bo_get_format(struct gbm_bo *bo);
uint32_t       gbm_bo_get_bpp(struct gbm_bo *bo);
uint32_t       gbm_bo_get_offset(struct gbm_bo *bo, int plane);
uint64_t       gbm_bo_get_modifier(struct gbm_bo *bo);
int            gbm_bo_get_plane_count(struct gbm_bo *bo);
int            gbm_bo_get_fd(struct gbm_bo *bo);
int            gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane);
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo);
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane);
struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo);

/* tawc-specific accessor: returns the underlying AHardwareBuffer*
 * (or NULL if the bo wasn't allocated this way). Callers that know
 * they're on tawc/Android can use this to wrap as an EGLImage via
 * `EGL_ANDROID_get_native_client_buffer`. */
void          *gbm_bo_get_ahardwarebuffer(struct gbm_bo *bo);

void           gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                                    void (*destroy)(struct gbm_bo *, void *));
void          *gbm_bo_get_user_data(struct gbm_bo *bo);
int            gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* TAWC_GBM_SHIM_H */
