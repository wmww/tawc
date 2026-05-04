/* See include/dirent_filter.h for the contract.
 *
 * Lives in PROD_C_FOR_TESTS — the cleat unit suite calls these
 * functions directly under hosted glibc; production tawcroot links
 * them in the freestanding handler path. No syscalls, no allocations,
 * no globals. */

#include <stddef.h>

#include "dirent_filter.h"

#define DIRENT64_RECLEN_OFF  16
#define DIRENT64_NAME_OFF    19

int tawcroot_dirent_filter_is_proc_fd_link(const char *link, long n)
{
	/* Shortest match is "/proc/1/fd" (10 chars). */
	if (n < 10) return 0;
	if (link[0] != '/' || link[1] != 'p' || link[2] != 'r' ||
	    link[3] != 'o' || link[4] != 'c' || link[5] != '/') return 0;
	long i = 6;
	/* "/proc/self/fd" exact. */
	if (i + 7 == n && link[i] == 's' &&
	    link[i+1] == 'e' && link[i+2] == 'l' && link[i+3] == 'f' &&
	    link[i+4] == '/' && link[i+5] == 'f' && link[i+6] == 'd')
		return 1;
	/* "/proc/<digits>/fd" — at least one digit, exactly "/fd" trailing. */
	while (i < n && link[i] >= '0' && link[i] <= '9') i++;
	if (i == 6) return 0;
	if (i + 3 != n) return 0;
	return link[i] == '/' && link[i+1] == 'f' && link[i+2] == 'd';
}

int tawcroot_dirent_filter_dname_is_reserved(const char *name,
					     const int *reserved_fds,
					     size_t n_reserved)
{
	if (!name || !reserved_fds || n_reserved == 0) return 0;
	/* Accumulate in unsigned long so a long numeric d_name can't
	 * trigger signed-overflow UB during the *10 multiply (the .o
	 * for the post-multiply check is allowed to elide it under
	 * -O2). Cap before any int comparison. */
	unsigned long v = 0;
	const char *p = name;
	if (*p == 0) return 0;
	while (*p) {
		if (*p < '0' || *p > '9') return 0;
		v = v * 10 + (unsigned long)(*p - '0');
		/* Reserved fds are int — anything past INT_MAX cannot match. */
		if (v > 0x7fffffffUL) return 0;
		p++;
	}
	for (size_t i = 0; i < n_reserved; i++) {
		if ((unsigned long)reserved_fds[i] == v) return 1;
	}
	return 0;
}

long tawcroot_dirent_filter_compact(void *buf, long n,
				    const int *reserved_fds,
				    size_t n_reserved)
{
	if (!buf || n <= 0 || !reserved_fds || n_reserved == 0) return n;
	unsigned char *p = (unsigned char *)buf;
	long out = 0;
	long in = 0;
	while (in < n) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, p + in + DIRENT64_RECLEN_OFF, 2);
		if (reclen == 0 || in + (long)reclen > n) {
			/* Malformed; bail without filtering further. */
			return n;
		}
		const char *name = (const char *)(p + in + DIRENT64_NAME_OFF);
		if (!tawcroot_dirent_filter_dname_is_reserved(
		        name, reserved_fds, n_reserved)) {
			if (out != in)
				__builtin_memmove(p + out, p + in, reclen);
			out += reclen;
		}
		in += reclen;
	}
	return out;
}
