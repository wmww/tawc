/* libc-free debug-print helpers.
 *
 * Used by everything outside the SIGSYS handler hot path. Inside the
 * handler this is forbidden: even `tawc_write` issues a syscall through
 * the stub, which is fine, but the rest of the design constrains the
 * handler to no allocations / no stdio. Use these freely from main /
 * --exec-child / rootfs init code; do not call them from handlers.
 *
 * Output goes to stderr (fd 2) — see io.c's banner for why. No newline
 * auto-append.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

void tawc_io_str(const char *s);
void tawc_io_hex(unsigned long v);
void tawc_io_dec(long v);
void tawc_io_kv_hex(const char *k, unsigned long v);
void tawc_io_kv_dec(const char *k, long v);

/* Print "[ok ] label\n" or "[FAIL] label\n"; return 0 if ok, 1 otherwise.
 * Used by the smoke drivers; unit tests use the same convention. */
int  tawc_io_step(const char *label, int ok);

/* Print "[skip] label (reason)\n". Used when the kernel lacks a syscall the
 * test exercises (e.g. faccessat2 on <5.8): the case is neither pass nor
 * fail, just N/A on this platform. The cleat parser registers these as
 * passing tests with the reason in the name so the run stays green. */
void tawc_io_skip(const char *label, const char *reason);

/* Tiny string utilities (libc-free). */
size_t tawc_strlen(const char *s);
int    tawc_streq(const char *a, const char *b);
int    tawc_starts_with(const char *s, const char *prefix);
long   tawc_parse_long(const char *s);
int    tawc_int_to_str(char *buf, size_t buflen, int v);   /* returns length */
int    tawc_long_to_str(char *buf, size_t buflen, long v); /* returns length */

/* Bounded string building. `tawc_str_append*` append to `dst` at offset
 * `*pos`, advance `*pos`, and keep `dst` NUL-terminated. On overflow they
 * return TAWC_ENAMETOOLONG and re-terminate at the pre-call `*pos` (the
 * failed append is dropped whole); on success they return 0. */
long tawc_str_append(char *dst, size_t cap, size_t *pos, const char *src);
long tawc_str_append_dec(char *dst, size_t cap, size_t *pos, long v);

/* Copy `src` into `dst` (NUL-terminated). Returns the copied length, or
 * TAWC_ENAMETOOLONG if it didn't fit (dst is left ""). */
long tawc_str_copy(char *dst, size_t cap, const char *src);

/* Render "/proc/self/fd/<fd>" — plus "/<suffix>" when `suffix` is
 * non-NULL and non-empty — into `out`. Returns the length written or
 * TAWC_ENAMETOOLONG. Shared by every site that routes a syscall
 * through an fd-anchored /proc path. */
long tawc_proc_fd_path(char *out, size_t cap, int fd, const char *suffix);
