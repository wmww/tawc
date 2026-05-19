/* Manual symlink-aware path canonicalization.
 *
 * See `include/path_resolve.h` for the WHY and the HOW-TO-DROP block.
 *
 * Algorithm: walk the suffix component-by-component left-to-right. For
 * each non-leaf component (or all components in FOLLOW mode), ask the
 * oracle "is this prefix a symlink?" via readlink. If yes, splice the
 * target into the path, re-fold to collapse any `..`/`.` from the
 * target, and restart the walk from the beginning. If no, continue.
 *
 * Restarting after each splice is O(N²) in the path-length sense but
 * N is tiny (PATH_MAX = 4096 with usually <10 components), and the
 * well-known-symlink memo skips the typical hot-path libraries before
 * we ever get here. The hop counter caps the total symlink walks at
 * SYMLOOP_MAX, matching Linux's own bound.
 *
 * Freestanding: no libc, no allocation. The readlink target and splice
 * scratch buffers come from path_scratch.c so this resolver can run from
 * a SIGSYS handler on a tiny guest stack.
 */

#include <stddef.h>

#include "errno_neg.h"
#include "path.h"
#include "path_oracle.h"
#include "path_resolve.h"
#include "path_scratch.h"

/* Linux's SYMLOOP_MAX is 40. Match it so an in-rootfs chain of
 * symlinks fails identically to a no-tawcroot equivalent. */
#define TAWC_SYMLOOP_MAX 40

static size_t rstrlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (size_t)(p - s);
}

/* Splice a symlink target into `suf` at component [comp_start..comp_end).
 *
 * Output is written to `tmp` with a leading '/' so the caller can pass
 * it straight to `tawcroot_path_fold_absolute`. Two cases:
 *
 *   - Absolute target (target[0] == '/'): replaces the entire prefix
 *     up through comp_end. Path becomes "/<target><remainder>".
 *
 *   - Relative target: keeps the prefix up to comp_start, replaces the
 *     component, keeps the remainder. Path becomes
 *     "/<prefix>/<target><remainder>".
 *
 * Returns 0 on success, -ENAMETOOLONG on overflow.
 */
static long splice_target(const char *suf, size_t suf_len,
			  size_t comp_start, size_t comp_end,
			  const char *target, size_t target_len,
			  char *tmp, size_t tmp_cap)
{
	if (tmp_cap < 2) return TAWC_ENAMETOOLONG;

	size_t ti = 0;
	int    absolute = (target_len > 0 && target[0] == '/');

	tmp[ti++] = '/';

	if (!absolute) {
		/* Keep the prefix up to (but not including) the component
		 * being replaced. Strip any trailing '/' so we don't double
		 * up below. */
		size_t copy_len = comp_start;
		while (copy_len > 0 && suf[copy_len - 1] == '/') copy_len--;
		for (size_t k = 0; k < copy_len; k++) {
			if (ti >= tmp_cap - 1) return TAWC_ENAMETOOLONG;
			tmp[ti++] = suf[k];
		}
		if (ti > 1) {
			if (ti >= tmp_cap - 1) return TAWC_ENAMETOOLONG;
			tmp[ti++] = '/';
		}
	}

	/* Append target. For an absolute target, skip the leading '/'
	 * since the loop above already wrote one. */
	size_t target_off = absolute ? 1 : 0;
	while (target_off < target_len && target[target_off] == '/') target_off++;
	for (size_t k = target_off; k < target_len; k++) {
		if (ti >= tmp_cap - 1) return TAWC_ENAMETOOLONG;
		tmp[ti++] = target[k];
	}

	/* Append remainder of suf from comp_end. Insert a '/' separator
	 * if the remainder doesn't already start with one. */
	if (comp_end < suf_len) {
		if (suf[comp_end] != '/') {
			if (ti >= tmp_cap - 1) return TAWC_ENAMETOOLONG;
			tmp[ti++] = '/';
		}
		for (size_t k = comp_end; k < suf_len; k++) {
			if (ti >= tmp_cap - 1) return TAWC_ENAMETOOLONG;
			tmp[ti++] = suf[k];
		}
	}

	tmp[ti] = 0;
	return 0;
}

long tawcroot_path_resolve_symlinks(char *suf, size_t cap,
				    tawcroot_path_mode mode,
				    const struct tawcroot_path_oracle *oracle)
{
	if (!oracle || !oracle->readlink) return TAWC_EINVAL;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	for (int hops = 0; hops < TAWC_SYMLOOP_MAX; hops++) {
		size_t suf_len = rstrlen(suf);
		if (suf_len == 0) return 0;        /* root, nothing to resolve */

		size_t i = 0;
		int    walked_symlink = 0;

		while (i < suf_len) {
			while (i < suf_len && suf[i] == '/') i++;
			if (i >= suf_len) break;

			size_t comp_start = i;
			while (i < suf_len && suf[i] != '/') i++;
			size_t comp_end = i;

			int is_leaf = (comp_end >= suf_len);
			if (is_leaf && mode != TAWCROOT_PATH_FOLLOW) {
				/* NOFOLLOW / PARENT_*: don't readlink the leaf;
				 * caller's syscall takes the leaf as-is. */
				break;
			}

			/* Probe: readlink suf[0..comp_end). Temporarily
			 * NUL-terminate at comp_end so we don't allocate. */
			char saved = suf[comp_end];
			suf[comp_end] = 0;

			char *target = scratch->buf[0];
			/* Pass full buffer (not cap-1) so we can detect
			 * truncation: when readlink returns the full cap,
			 * the kernel had more bytes to write but ran out
			 * of room. cap-1 form silently mistakes a truncated
			 * target for an exact-fit one. */
			long n = oracle->readlink(oracle->ctx, suf,
						  target,
						  TAWCROOT_PATH_SCRATCH_SIZE);

			suf[comp_end] = saved;

			if (n == TAWC_EINVAL) {
				/* Not a symlink. Walk into next component. */
				continue;
			}
			if (n == TAWC_ENOENT) {
				/* Component missing: stop with the resolved
				 * prefix; the downstream syscall produces the
				 * kernel-native error. */
				return 0;
			}
			if (n < 0) {
				/* Any other errno (-EACCES, -EIO, ...) is fatal:
				 * propagate so the caller's syscall returns it
				 * to the guest verbatim. Folding these into
				 * "not a symlink, continue" leaks a partially-
				 * translated path the kernel might serve under
				 * different rules than readlinkat used. */
				return n;
			}
			if ((size_t)n == TAWCROOT_PATH_SCRATCH_SIZE) {
				/* Saturation: target is at least sizeof(target)
				 * bytes, so we can neither NUL-terminate it nor
				 * be sure the bytes we have are the whole target.
				 * Refuse rather than silently use a truncated
				 * target the kernel won't agree with. */
				return TAWC_ENAMETOOLONG;
			}
			if (n == 0) {
				/* Zero-length symlink target. Linux refuses to
				 * create empty symlinks; if we see one, treat
				 * it like the kernel does for an empty-target
				 * dereference: -ENOENT. Splicing nothing into
				 * the path would silently delete the component,
				 * causing the syscall to land on the parent
				 * directory or the next sibling. (Review B9.) */
				return TAWC_ENOENT;
			}
			target[n] = 0;

			char *tmp = scratch->buf[1];
			long sr = splice_target(suf, suf_len,
						comp_start, comp_end,
						target, (size_t)n,
						tmp,
						TAWCROOT_PATH_SCRATCH_SIZE);
			if (sr < 0) return sr;

			long fr = tawcroot_path_fold_absolute(tmp, suf, cap);
			if (fr < 0) return fr;

			walked_symlink = 1;
			break;   /* restart the outer loop on the new suf */
		}

		if (!walked_symlink) return 0;
	}

	return TAWC_ELOOP;
}
