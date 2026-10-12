/* Raw host metadata with a pre-4.11 kernel fallback. No guest path
 * translation or ownership decoration; callers supply trusted buffers. */
#pragma once

struct statx;

long tawcroot_statx_compat(int dirfd, const char *path, int flags,
                         unsigned int mask, struct statx *out);
