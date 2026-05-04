/* Standard memcpy/memset/memmove/memcmp for tawcroot.
 *
 * tawcroot is built two ways:
 *   - Production / testhost: freestanding (`-ffreestanding`, no libc).
 *     `memcpy` etc. are defined by `strings.c` so the compiler can
 *     resolve struct-copy / `= {0}` lowerings without dragging in libc.
 *   - cleat orchestrator: hosted glibc. `memcpy` etc. come from
 *     `<string.h>`.
 *
 * Either way the signatures and behavior are identical, so callers can
 * use the standard names directly. This header just makes them visible
 * by switching include strategy on `__STDC_HOSTED__` (the `-ffreestanding`
 * builds set this to 0; hosted builds leave it at 1).
 *
 * Use this header — and the standard names `memcpy`/`memset`/`memmove`/
 * `memcmp` — instead of hand-rolling a `local_memcpy` / `peq` / etc. in
 * each translation unit. */

#pragma once

#include <stddef.h>

#if __STDC_HOSTED__
# include <string.h>
#else
void  *memcpy (void *dst, const void *src, size_t n);
void  *memset (void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp (const void *a, const void *b, size_t n);
size_t strlen (const char *s);
#endif
