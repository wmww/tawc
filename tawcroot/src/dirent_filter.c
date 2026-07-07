/* See include/dirent_filter.h for the contract.
 *
 * Lives in PROD_C_FOR_TESTS — the cleat unit suite calls these
 * functions directly under hosted glibc; production tawcroot links
 * them in the freestanding handler path. No syscalls, no allocations,
 * no globals. */

#include <stddef.h>

#include "dirent_filter.h"

#define DIRENT64_RECLEN_OFF  16
#define DIRENT64_TYPE_OFF    18
#define DIRENT64_NAME_OFF    19

#define TAWC_DT_UNKNOWN 0
#define TAWC_DT_LNK     10

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
		/* A record needs at least the fixed header plus a NUL name
		 * byte; anything smaller (zero included) is malformed and
		 * would put `name` outside the record. */
		if (reclen < DIRENT64_NAME_OFF + 1 || in + (long)reclen > n)
			goto malformed;
		const char *name = (const char *)(p + in + DIRENT64_NAME_OFF);
		/* The buffer is guest memory — another guest thread can race
		 * the kernel's writes. Require the name's NUL inside the
		 * record so the digit scan below can't walk out of bounds. */
		int has_nul = 0;
		for (long k = DIRENT64_NAME_OFF; k < (long)reclen; k++) {
			if (p[in + k] == 0) { has_nul = 1; break; }
		}
		if (!has_nul)
			goto malformed;
		if (!tawcroot_dirent_filter_dname_is_reserved(
		        name, reserved_fds, n_reserved)) {
			if (out != in)
				__builtin_memmove(p + out, p + in, reclen);
			out += reclen;
		}
		in += reclen;
	}
	return out;

malformed:
	/* If nothing was dropped yet the buffer is byte-identical to the
	 * kernel's output — hand it back unfiltered. Once entries have
	 * been compacted away, [out, in) is stale bytes mid-record, so
	 * returning the original length would hand the guest a corrupted
	 * stream; drop the malformed tail instead. */
	return out == in ? n : out;
}

long tawcroot_dirent_filter_delink_types(void *buf, long n)
{
	if (!buf || n <= 0) return n;
	unsigned char *p = (unsigned char *)buf;
	long in = 0;
	while (in < n) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, p + in + DIRENT64_RECLEN_OFF, 2);
		/* Guest memory: bail on a malformed record. Flips already
		 * made stay (they're valid records); the tail is left
		 * exactly as the kernel wrote it. */
		if (reclen < DIRENT64_NAME_OFF + 1 || in + (long)reclen > n)
			break;
		if (p[in + DIRENT64_TYPE_OFF] == TAWC_DT_LNK)
			p[in + DIRENT64_TYPE_OFF] = TAWC_DT_UNKNOWN;
		in += reclen;
	}
	return n;
}
