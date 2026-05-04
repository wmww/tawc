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
 * Algorithm: scan components left-to-right; maintain a small stack of
 * (start, length) offsets representing the kept components. `..` pops
 * the top entry (clamping at root if empty); `.` is dropped; otherwise
 * the component is appended. At the end the kept components are
 * re-emitted separated by `/`.
 */

#include <stddef.h>

#include "errno_neg.h"
#include "path.h"

#define TAWC_PATH_MAX  4096
#define MAX_COMPONENTS 256

long tawcroot_path_fold_absolute(const char *path, char *out, size_t out_cap)
{
	if (out_cap == 0) return TAWC_ENAMETOOLONG;

	size_t starts[MAX_COMPONENTS];
	size_t lens[MAX_COMPONENTS];
	size_t depth = 0;

	const char *p = path;
	if (*p != '/') return TAWC_ENOENT;
	p++;                           /* skip leading '/' */

	/* Build component bytes into a scratch buffer, then compact into
	 * `out`. Avoids in-place editing of the partial result. */
	char scratch[TAWC_PATH_MAX];
	size_t scratch_len = 0;

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
				scratch_len = (depth == 0) ? 0
					    : starts[depth - 1] + lens[depth - 1];
			}
			/* else: clamp at root */
		} else {
			if (depth >= MAX_COMPONENTS)  return TAWC_ENAMETOOLONG;
			if (scratch_len + comp_len + 1 > sizeof scratch)
				return TAWC_ENAMETOOLONG;
			starts[depth] = scratch_len;
			lens[depth]   = comp_len;
			depth++;
			for (size_t i = 0; i < comp_len; i++)
				scratch[scratch_len++] = p[i];
		}

		p = q;
		while (*p == '/') p++;
	}

	/* Emit kept components into `out` separated by `/`. */
	size_t total = 0;
	for (size_t i = 0; i < depth; i++) {
		if (i > 0) {
			if (total + 1 >= out_cap) return TAWC_ENAMETOOLONG;
			out[total++] = '/';
		}
		if (total + lens[i] >= out_cap) return TAWC_ENAMETOOLONG;
		for (size_t j = 0; j < lens[i]; j++)
			out[total++] = scratch[starts[i] + j];
	}
	if (total >= out_cap) return TAWC_ENAMETOOLONG;
	out[total] = 0;
	return 0;
}
