/* Lexical absolute-path canonicalizer.
 *
 * `tawcroot_path_fold_absolute` takes a guest-absolute path and produces
 * a rootfs-relative suffix with `..` clamped at root and `.` collapsed.
 * No symlink resolution, no syscalls — just a pure string fold.
 *
 * Lives in its own translation unit so unit tests (cleat orchestrator,
 * hosted glibc) can link this + `path_resolve.c` without dragging in
 * `path.c`'s raw-syscall-using code (memo / probe / bind table).
 *
 * Algorithm: scan components left-to-right and emit kept components
 * directly into `out`. A small stack records the output length before
 * each component was appended, so `..` pops by rewinding `out_len` to
 * the previous component start (clamping at root if empty). `.` is
 * dropped; ordinary components are appended with separators as needed.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "path.h"

#define TAWC_PATH_MAX  4096
#define MAX_COMPONENTS 256

long tawcroot_path_fold_absolute(const char *path, char *out, size_t out_cap)
{
	if (out_cap == 0) return TAWC_ENAMETOOLONG;

	uint16_t starts[MAX_COMPONENTS];
	size_t depth = 0;

	const char *p = path;
	if (*p != '/') return TAWC_ENOENT;
	p++;                           /* skip leading '/' */

	size_t out_len = 0;

	while (*p) {
		const char *q = p;
		while (*q && *q != '/') q++;
		size_t comp_len = (size_t)(q - p);

		if (comp_len == 0) {
			/* run of `/` — skip */
		} else if (comp_len == 1 && p[0] == '.') {
			/* skip */
		} else if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
			if (depth > 0) {
				depth--;
				out_len = starts[depth];
			}
			/* else: clamp at root */
		} else {
			if (depth >= MAX_COMPONENTS)  return TAWC_ENAMETOOLONG;
			size_t need = out_len + (out_len ? 1 : 0) + comp_len + 1;
			if (need > out_cap || need > TAWC_PATH_MAX)
				return TAWC_ENAMETOOLONG;
			starts[depth] = (uint16_t)out_len;
			depth++;
			if (out_len)
				out[out_len++] = '/';
			for (size_t i = 0; i < comp_len; i++)
				out[out_len++] = p[i];
		}

		p = q;
		while (*p == '/') p++;
	}

	if (out_len >= out_cap) return TAWC_ENAMETOOLONG;
	out[out_len] = 0;
	return 0;
}
