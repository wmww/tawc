/* Pure string / memory helpers — no syscalls, no libc. Used by every
 * tawcroot binary (production, testhost) and also pulled directly into
 * the cleat-driven test orchestrator under hosted glibc for unit tests
 * (see tawcroot/tests/unit/test_strings.c). The helpers are declared in io.h.
 *
 * memcpy/memset/memmove/memcmp/strlen are here too because the compiler
 * lowers struct copies and `= {0}` field stores to those names even under
 * `-nostdlib`. Keep them simple, freestanding, and async-signal safe.
 *
 * Hosted (cleat) builds skip these defs and use glibc's instead, so we
 * don't ship two strong copies of `memcpy` etc. into the test orchestrator
 * and rely on link order to pick the right one. tawc_string.h does the
 * matching include-strategy switch so callers see the same names either way.
 */

#include <stddef.h>
#include "errno_neg.h"
#include "io.h"

#if !__STDC_HOSTED__

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	unsigned char  v = (unsigned char)c;
	for (size_t i = 0; i < n; i++) d[i] = v;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	if (d == s || n == 0) return dst;
	if (d < s) {
		for (size_t i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
	}
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = (const unsigned char *)a;
	const unsigned char *q = (const unsigned char *)b;
	for (size_t i = 0; i < n; i++) {
		if (p[i] != q[i]) return (int)p[i] - (int)q[i];
	}
	return 0;
}

size_t strlen(const char *s) { return tawc_strlen(s); }

#endif /* !__STDC_HOSTED__ */

size_t tawc_strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (size_t)(p - s);
}

int tawc_streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b) return 0;
		a++; b++;
	}
	return *a == *b;
}

int tawc_starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s != *prefix) return 0;
		s++; prefix++;
	}
	return 1;
}

long tawc_parse_long(const char *s)
{
	long v = 0;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (long)(*s - '0');
		s++;
	}
	return neg ? -v : v;
}

int tawc_int_to_str(char *buf, size_t buflen, int v)
{
	return tawc_long_to_str(buf, buflen, v);
}

int tawc_long_to_str(char *buf, size_t buflen, long v)
{
	if (buflen == 0) return 0;
	int n = 0;
	if (v == 0) {
		if (n + 1 < (int)buflen) buf[n++] = '0';
		buf[n] = 0;
		return n;
	}
	/* Negate in unsigned space: -v is UB for LONG_MIN. */
	int neg = v < 0;
	unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
	char tmp[20];
	int t = 0;
	while (u && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
	if (neg && n + 1 < (int)buflen) buf[n++] = '-';
	while (t && n + 1 < (int)buflen) buf[n++] = tmp[--t];
	buf[n] = 0;
	return n;
}

long tawc_str_append(char *dst, size_t cap, size_t *pos, const char *src)
{
	size_t i = *pos;
	if (i >= cap) return TAWC_ENAMETOOLONG;
	for (; *src; src++) {
		if (i + 1 >= cap) {
			dst[*pos] = 0;
			return TAWC_ENAMETOOLONG;
		}
		dst[i++] = *src;
	}
	dst[i] = 0;
	*pos = i;
	return 0;
}

long tawc_str_append_dec(char *dst, size_t cap, size_t *pos, long v)
{
	char tmp[24];
	tawc_long_to_str(tmp, sizeof tmp, v);
	return tawc_str_append(dst, cap, pos, tmp);
}

long tawc_str_copy(char *dst, size_t cap, const char *src)
{
	size_t pos = 0;
	long e = tawc_str_append(dst, cap, &pos, src);
	return e ? e : (long)pos;
}

long tawc_proc_fd_path(char *out, size_t cap, int fd, const char *suffix)
{
	size_t pos = 0;
	long e = tawc_str_append(out, cap, &pos, "/proc/self/fd/");
	if (!e) e = tawc_str_append_dec(out, cap, &pos, fd);
	if (!e && suffix && suffix[0]) {
		e = tawc_str_append(out, cap, &pos, "/");
		if (!e) e = tawc_str_append(out, cap, &pos, suffix);
	}
	return e ? e : (long)pos;
}
