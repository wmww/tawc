/* Path-translation orchestration — pure with respect to the OS.
 *
 * Lives in PROD_C_FOR_TESTS so the cleat test orchestrator can call
 * `tawcroot_path_translate_with_ctx` directly (no fork + exec into
 * tawcroot-testhost). The production wrapper in path.c builds a ctx
 * from globals + raw_sys helpers and forwards the rest of the work
 * here.
 *
 * Stage order (review findings B5, D3):
 *
 *   fold → bind → (if no bind) memo → resolver → bind
 *
 * Why bind comes first: the user-supplied bind table is an
 * authoritative replacement for a subtree of the guest view. A
 * rootfs-side symlink memo (e.g. /lib → usr/lib) must not silently
 * defeat a bind on /lib. If the early bind matches, we skip both memo
 * and the rootfs-fd-based resolver — both are properties of the rootfs
 * view, not the bind src.
 *
 * Why bind comes again at the end: a memo may surface a bind that
 * didn't match the literal input. Example: bind on /usr/lib + memo
 * /lib → usr/lib. Input /lib/x doesn't match the bind directly, but
 * after memo it becomes usr/lib/x and the bind on /usr/lib applies on
 * the second pass.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "io.h"
#include "path.h"
#include "path_oracle.h"
#include "path_orchestrate.h"
#include "path_resolve.h"
#include "path_scratch.h"
#include "tawc_string.h"

/* Apply memoization to a folded suffix in place. Returns 1 if the
 * suffix changed (caller may want to re-fold), 0 otherwise.
 *
 * Mode-aware: when the memoized symlink IS the final component of the
 * input (i.e. suf == src exactly, no trailing path), we only rewrite
 * for FOLLOW mode. Under NOFOLLOW / PARENT_CREATE / PARENT_REMOVE the
 * symlink itself is the operation target (lstat'ing it, readlink'ing
 * it, removing it, etc.) and rewriting would silently change which
 * inode the syscall hits. A skipped sole-component match `continue`s
 * (rather than returning) so a later memo entry whose `src` happens
 * to also be a path-prefix can still apply — the memo array does not
 * guarantee unique srcs across entries. */
static int apply_memo(char *suf, size_t cap, tawcroot_path_mode mode,
		      const struct tawcroot_symlink_memo *memos, size_t n_memos)
{
	size_t suf_len = tawc_strlen(suf);
	for (size_t i = 0; i < n_memos; i++) {
		const struct tawcroot_symlink_memo *m = &memos[i];
		if (m->src_len > suf_len) continue;
		if (memcmp(suf, m->src, m->src_len) != 0) continue;
		if (suf_len > m->src_len && suf[m->src_len] != '/') continue;
		if (suf_len == m->src_len && mode != TAWCROOT_PATH_FOLLOW) {
			continue;
		}
		size_t after_src_len = suf_len - m->src_len;
		size_t need = m->target_len + after_src_len + 1;
		/* Doesn't fit: skip this entry (same reasoning as the
		 * sole-component skip above — a later entry may still
		 * apply). */
		if (need > cap) continue;

		/* Shift the trailing remainder (including the terminating
		 * NUL) into its new position. Direction of copy matters:
		 * when target_len > src_len we're growing the prefix and
		 * the destination range overlaps the source from the right
		 * — copy right-to-left. When target_len < src_len we're
		 * shrinking and the destination overlaps from the left —
		 * copy left-to-right. The previous unconditional
		 * right-to-left form would (in the shrink case) read from
		 * positions it had already overwritten, truncating the
		 * path at `target_len` — e.g. memoizing /usr/sbin (-> bin)
		 * over /usr/sbin/bash silently dropped "/bash". */
		if (m->target_len > m->src_len) {
			for (size_t k = 0; k <= after_src_len; k++) {
				suf[m->target_len + after_src_len - k] =
					suf[m->src_len   + after_src_len - k];
			}
		} else if (m->target_len < m->src_len) {
			for (size_t k = 0; k <= after_src_len; k++) {
				suf[m->target_len + k] = suf[m->src_len + k];
			}
		}
		for (size_t k = 0; k < m->target_len; k++) suf[k] = m->target[k];
		return 1;
	}
	return 0;
}

/* Re-route the folded suffix through a bind if it matches the longest
 * `dst` prefix. The folded suffix has NO leading '/'. The bind dst is
 * stored the same way. Match condition:
 *   suffix == dst                    (exact)        OR
 *   suffix starts with dst + '/'    (prefix component boundary)
 * Without the boundary check, "/system_ext" would be matched by a
 * bind with dst "system" (would be misrouted). */
static void route_through_binds(tawcroot_path_result *r, char *suf,
				const struct tawcroot_bind *binds, size_t n_binds)
{
	size_t suf_len = tawc_strlen(suf);
	const struct tawcroot_bind *best = 0;
	for (size_t i = 0; i < n_binds; i++) {
		const struct tawcroot_bind *b = &binds[i];
		if (!b->active) continue;
		if (b->dst_len > suf_len) continue;
		if (memcmp(suf, b->dst, b->dst_len) != 0) continue;
		if (suf_len > b->dst_len && suf[b->dst_len] != '/') continue;
		if (!best || b->dst_len > best->dst_len) best = b;
	}
	if (!best) return;

	/* Rewrite: base_fd = best->src_fd, suffix = bytes after best->dst
	 * (skipping any leading '/'). */
	r->base_fd = best->src_fd;
	size_t k = best->dst_len;
	while (suf[k] == '/') k++;
	size_t j = 0;
	while (suf[k]) suf[j++] = suf[k++];
	suf[j] = 0;
}

/* Build the joined absolute path "<cwd_abs>/<rel>" into `out`. Both
 * inputs are NUL-terminated; `cwd_abs` must start with '/'. Returns 0
 * on success, -ENAMETOOLONG on overflow. The result may contain
 * "//" runs if cwd_abs == "/" — the absolute fold collapses those. */
static long join_cwd_rel(const char *cwd_abs, const char *rel,
			 char *out, size_t out_cap)
{
	size_t off = 0;
	long e = tawc_str_append(out, out_cap, &off, cwd_abs);
	if (!e && (off == 0 || out[off - 1] != '/'))
		e = tawc_str_append(out, out_cap, &off, "/");
	if (!e) e = tawc_str_append(out, out_cap, &off, rel);
	return e;
}

long tawcroot_path_binds_reanchor(struct tawcroot_bind *binds, size_t n_binds,
                                  const char *cur_root, size_t cur_len,
                                  const char *new_root, size_t new_len)
{
	if (!cur_root || !new_root || (n_binds && !binds)) return TAWC_EFAULT;

	long worst = 0;
	for (size_t i = 0; i < n_binds; i++) {
		struct tawcroot_bind *b = &binds[i];
		if (!b->active) continue;

		/* Compose old host path = cur_root + "/" + dst. We don't
		 * actually materialise the full string; just compare in
		 * three pieces against new_root and capture the trailing
		 * remainder if it lies inside. */
		size_t total = cur_len + 1 + b->dst_len;

		/* Bind WAS the new root: cur_root + "/" + dst exactly
		 * equals new_root. The new view's "/" is now this bind's
		 * src. The rootfs_fd swap (caller's responsibility) opens
		 * the same underlying directory; the bind is redundant. */
		if (total == new_len &&
		    memcmp(cur_root, new_root, cur_len) == 0 &&
		    new_root[cur_len] == '/' &&
		    memcmp(b->dst, new_root + cur_len + 1, b->dst_len) == 0) {
			b->active  = 0;
			b->dst_len = 0;
			b->dst[0]  = 0;
			continue;
		}

		/* Bind lies inside the new root: prefix-match new_root
		 * against (cur_root "/" dst) with a component-boundary
		 * check on the byte AFTER new_root in the composed path. */
		int outside = 0;
		if (new_len > total) outside = 1;
		else {
			for (size_t k = 0; k < new_len; k++) {
				char composed_k;
				if (k < cur_len)        composed_k = cur_root[k];
				else if (k == cur_len)  composed_k = '/';
				else                    composed_k = b->dst[k - cur_len - 1];
				if (composed_k != new_root[k]) {
					outside = 1;
					break;
				}
			}
			if (!outside) {
				/* new_root is a strict prefix; the next
				 * byte in the composed path must be '/'. */
				char next;
				if (new_len < cur_len)        next = cur_root[new_len];
				else if (new_len == cur_len)  next = '/';
				else                          next = b->dst[new_len - cur_len - 1];
				if (next != '/') outside = 1;
			}
		}
		if (outside) {
			b->active  = 0;
			b->dst_len = 0;
			b->dst[0]  = 0;
			continue;
		}

		/* Remainder = composed[new_len + 1 .. ]. */
		size_t rem_start = new_len + 1;
		size_t rem_len   = total - rem_start;
		if (rem_len + 1 > sizeof b->dst) {
			/* Overflow — drop the bind (matches the dst-too-long
			 * rejection in tawcroot_path_add_bind). Other binds
			 * still get processed; surface the error to caller
			 * for visibility. */
			b->active  = 0;
			b->dst_len = 0;
			b->dst[0]  = 0;
			worst = TAWC_ENAMETOOLONG;
			continue;
		}

		/* Copy out to a temp first — destination overlaps source
		 * when rem_start lands inside b->dst[]. */
		char tmp[sizeof b->dst];
		for (size_t k = 0; k < rem_len; k++) {
			size_t src_idx = rem_start + k;
			if (src_idx < cur_len)        tmp[k] = cur_root[src_idx];
			else if (src_idx == cur_len)  tmp[k] = '/';
			else                          tmp[k] = b->dst[src_idx - cur_len - 1];
		}
		for (size_t k = 0; k < rem_len; k++) b->dst[k] = tmp[k];
		b->dst[rem_len] = 0;
		b->dst_len      = rem_len;
	}
	return worst;
}

tawcroot_path_result tawcroot_path_translate_with_ctx(
	const struct tawcroot_path_translate_ctx *ctx,
	const char *guest_path, char *out_suffix, size_t out_cap,
	tawcroot_path_mode mode)
{
	tawcroot_path_result r;
	r.err     = 0;
	r.base_fd = -1;

	if (!ctx || !guest_path || !out_suffix || out_cap == 0) {
		r.err = TAWC_EFAULT;
		return r;
	}
	/* Kernel semantics: an empty pathname fails ENOENT on every path
	 * syscall (AT_EMPTY_PATH callers never reach the translator).
	 * Without this check "" would take the relative branch and
	 * translate to the cwd. */
	if (guest_path[0] == 0) {
		r.err = TAWC_ENOENT;
		return r;
	}
	r.base_fd = ctx->rootfs_base_fd;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	if (guest_path[0] != '/') {
		if (!ctx->cwd_to_guest_abs) { r.err = TAWC_ENOENT; return r; }
		char *cwd_abs = scratch->buf[0];
		long cr = ctx->cwd_to_guest_abs(ctx->cwd_ctx, cwd_abs,
						TAWCROOT_PATH_SCRATCH_SIZE);
		if (cr < 0) { r.err = cr; return r; }
		if (cwd_abs[0] != '/') { r.err = TAWC_ENOENT; return r; }

		char *joined = scratch->buf[1];
		long jr = join_cwd_rel(cwd_abs, guest_path, joined,
				       TAWCROOT_PATH_SCRATCH_SIZE);
		if (jr < 0) { r.err = jr; return r; }
		long fr = tawcroot_path_fold_absolute(joined, out_suffix, out_cap);
		if (fr < 0) { r.err = fr; return r; }
	} else {
		long fr = tawcroot_path_fold_absolute(guest_path, out_suffix, out_cap);
		if (fr < 0) { r.err = fr; return r; }
	}

	/* Bind first. If matched, the bind src takes over — skip memo
	 * and resolver, both of which are rootfs-view-only. */
	route_through_binds(&r, out_suffix, ctx->binds, ctx->n_binds);
	if (r.base_fd != ctx->rootfs_base_fd) return r;

	/* Well-known-symlink rewrite. If the rewrite kicks in, the suffix
	 * may now contain `..`/`.` from the target, so re-fold. Bound
	 * iterations to avoid pathological loops. */
	for (int hop = 0; hop < 8; hop++) {
		if (!apply_memo(out_suffix, out_cap, mode,
				ctx->memos, ctx->n_memos)) break;
		char *tmp = scratch->buf[2];
		size_t i = 0;
		long je = tawc_str_append(tmp, TAWCROOT_PATH_SCRATCH_SIZE,
					  &i, "/");
		if (!je) je = tawc_str_append(tmp, TAWCROOT_PATH_SCRATCH_SIZE,
					      &i, out_suffix);
		if (je) {
			/* Suffix doesn't fit the re-fold scratch — error out
			 * rather than silently truncating to a wrong path. */
			r.err = je;
			return r;
		}
		long rf = tawcroot_path_fold_absolute(tmp, out_suffix, out_cap);
		if (rf < 0) { r.err = rf; return r; }
	}

	/* A memo rewrite may have landed the path inside bind territory
	 * (bind on /usr/lib, memo lib → usr/lib, input /lib/x). Route now,
	 * BEFORE the resolver — otherwise the resolver walks the ROOTFS
	 * oracle for a suffix that belongs to the bind src, and the
	 * rootfs's own shadow of that subtree (conflicting symlinks, or a
	 * file where the bind has a dir) drives resolution against the
	 * wrong tree. */
	route_through_binds(&r, out_suffix, ctx->binds, ctx->n_binds);
	if (r.base_fd != ctx->rootfs_base_fd) return r;

	/* Manual symlink resolver. See path_resolve.h banner. */
	if (ctx->oracle) {
		long er = tawcroot_path_resolve_symlinks(out_suffix, out_cap,
							 mode, ctx->oracle);
		if (er < 0) { r.err = er; return r; }
	}

	/* Final bind pass — memo may have surfaced a match. */
	route_through_binds(&r, out_suffix, ctx->binds, ctx->n_binds);
	return r;
}
