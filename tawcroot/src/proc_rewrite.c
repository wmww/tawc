/* /proc/self/maps reverse-translator (pure).
 *
 * Lives in PROD_C_FOR_TESTS so the cleat unit-test orchestrator can
 * call into it directly under hosted glibc. No syscalls, no globals;
 * the caller hands in a context with the rootfs/bind tables. The
 * syscall-side glue (open the kernel file, allocate buffers, write a
 * memfd) lives in syscalls_fs.c.
 *
 * Match policy mirrors `route_through_binds` in path_orchestrate.c:
 * longest prefix wins, with a component-boundary check on the byte
 * after the prefix so a sibling whose path happens to share the
 * prefix bytes (e.g. rootfs="/foo/rfs" vs. mapping path
 * "/foo/rfs-evil/x") doesn't match. Bind takes precedence over rootfs
 * because a bind is an authoritative replacement for that subtree.
 */

#include <stddef.h>
#include "errno_neg.h"
#include "io.h"
#include "proc_rewrite.h"
#include "tawc_string.h"

/* Test whether `host_path[0..host_len)` starts with `prefix[0..pl)` and
 * the byte after the prefix is either past-the-end or '/'. Returns the
 * length of the prefix on match, 0 on no match. */
static size_t match_host_prefix(const char *host_path, size_t host_len,
				const char *prefix, size_t pl)
{
	if (pl == 0 || pl > host_len) return 0;
	if (memcmp(host_path, prefix, pl) != 0) return 0;
	if (host_len > pl && host_path[pl] != '/') return 0;
	return pl;
}

/* Build "/<dst>/<rest>" into `out`. `dst` and `rest` are bare (no
 * leading slashes). The separator slash is suppressed when either side
 * is empty so we don't emit "//" for the rootfs case (dst empty) or a
 * trailing "/" for an exact-bind-src match (rest empty). Returns bytes
 * written or -ENAMETOOLONG. */
static long emit(char *out, size_t out_cap,
		 const char *dst, size_t dst_len,
		 const char *rest, size_t rest_len)
{
	int    need_sep = (dst_len > 0 && rest_len > 0);
	size_t need     = 1 + dst_len + (need_sep ? 1 : 0) + rest_len;
	if (need + 1 > out_cap) return TAWC_ENAMETOOLONG;

	size_t off = 0;
	out[off++] = '/';
	for (size_t i = 0; i < dst_len; i++) out[off++] = dst[i];
	if (need_sep) out[off++] = '/';
	for (size_t i = 0; i < rest_len; i++) out[off++] = rest[i];
	out[off] = 0;
	return (long)off;
}

long tawcroot_proc_reverse_translate_path(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *host_path, size_t host_len,
	char *out, size_t out_cap)
{
	if (!ctx || !host_path || !out || out_cap == 0) return TAWC_ENOENT;

	/* Bind first, longest-prefix-match. Bind src paths are authoritative
	 * replacements for that subtree of the guest view; if a host mapping
	 * lives under a bind src, the guest sees it via the bind dst, not via
	 * any rootfs-side passthrough. */
	const struct tawcroot_bind *best = 0;
	size_t                       best_pl = 0;
	for (size_t i = 0; i < ctx->n_binds; i++) {
		const struct tawcroot_bind *b = &ctx->binds[i];
		size_t bsl = 0;
		while (b->src[bsl]) bsl++;
		size_t pl = match_host_prefix(host_path, host_len, b->src, bsl);
		if (pl == 0) continue;
		if (!best || pl > best_pl) { best = b; best_pl = pl; }
	}
	if (best) {
		size_t rest_off = best_pl;
		while (rest_off < host_len && host_path[rest_off] == '/') rest_off++;
		return emit(out, out_cap,
			    best->dst, best->dst_len,
			    host_path + rest_off, host_len - rest_off);
	}

	/* Rootfs prefix. */
	size_t rpl = match_host_prefix(host_path, host_len,
				       ctx->rootfs_host_path,
				       ctx->rootfs_host_path_len);
	if (rpl > 0) {
		size_t rest_off = rpl;
		while (rest_off < host_len && host_path[rest_off] == '/') rest_off++;
		return emit(out, out_cap, "", 0,
			    host_path + rest_off, host_len - rest_off);
	}

	return TAWC_ENOENT;
}

/* Find the start of the path field in a maps line.
 *
 * Format: addr-range perms offset dev inode <whitespace>path
 * Path may be empty, a real path starting with '/', a `[bracketed]`
 * marker, or `(deleted)` etc. seq_path's mangle_path escapes spaces
 * inside the path as '\040', so once we've crossed the inode/whitespace
 * boundary, everything until '\n' is the path field.
 *
 * Returns the byte offset within `line[0..len)` of the first byte of
 * the path field, or `len` if no path is present (line is shorter
 * than the 5 mandatory fields).
 *
 * Strategy: walk to the end of the inode field (5th run of
 * non-whitespace tokens), then skip whitespace.
 */
static size_t find_path_offset(const char *line, size_t len)
{
	size_t i = 0;
	int    fields = 0;
	while (i < len && line[i] != '\n') {
		/* skip leading whitespace */
		while (i < len && line[i] != '\n' &&
		       (line[i] == ' ' || line[i] == '\t')) i++;
		if (i >= len || line[i] == '\n') break;
		/* if we already have 5 fields, the next token IS the path */
		if (fields == 5) return i;
		/* consume one non-whitespace token */
		while (i < len && line[i] != '\n' &&
		       line[i] != ' ' && line[i] != '\t') i++;
		fields++;
	}
	return i;  /* end-of-line: no path field */
}

/* Write `n` bytes; returns -ENOSPC if buffer would overflow. */
static long out_write(char *out, size_t out_cap, size_t *off,
		      const char *src, size_t n)
{
	if (*off + n > out_cap) return TAWC_ENOSPC;
	for (size_t i = 0; i < n; i++) out[*off + i] = src[i];
	*off += n;
	return 0;
}

long tawcroot_proc_maps_rewrite(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *in, size_t in_len,
	char *out, size_t out_cap)
{
	if (!ctx || !in || !out) return TAWC_ENOSPC;

	size_t out_off = 0;
	size_t i       = 0;
	while (i < in_len) {
		/* find end of this line (inclusive of '\n' if present) */
		size_t line_end = i;
		while (line_end < in_len && in[line_end] != '\n') line_end++;
		int has_nl = (line_end < in_len);
		size_t line_len = line_end - i;
		const char *line = in + i;

		size_t path_off = find_path_offset(line, line_len);
		size_t path_len = line_len - path_off;

		/* Decide whether to attempt rewrite:
		 *   - empty path → emit verbatim
		 *   - starts with '[' → bracketed pseudo-path, emit verbatim
		 *   - starts with '/' → real path, try reverse-translate
		 *   - anything else → emit verbatim (defensive) */
		const char *p = line + path_off;
		int do_rewrite = (path_len > 0 && p[0] == '/');

		/* Emit prefix (everything up to the path field) verbatim. */
		long e = out_write(out, out_cap, &out_off, line, path_off);
		if (e < 0) return e;

		if (do_rewrite) {
			/* The path field may have a trailing " (deleted)" or
			 * similar tag the kernel appends. Reverse-translate
			 * only the leading path; the tag, if any, is preserved
			 * verbatim. We define the leading path as bytes up to
			 * the first ' ' that's followed by '(' — covers
			 * "(deleted)" without tripping on '\040'-escaped
			 * spaces inside the path. */
			size_t plen = 0;
			while (plen < path_len) {
				if (plen + 1 < path_len &&
				    p[plen] == ' ' && p[plen + 1] == '(') break;
				plen++;
			}

			char xlated[4096];
			long rl = tawcroot_proc_reverse_translate_path(
				ctx, p, plen, xlated, sizeof xlated);
			if (rl > 0) {
				e = out_write(out, out_cap, &out_off,
					      xlated, (size_t)rl);
				if (e < 0) return e;
				/* trailing tag (e.g. " (deleted)") + rest */
				e = out_write(out, out_cap, &out_off,
					      p + plen, path_len - plen);
				if (e < 0) return e;
			} else {
				/* No prefix matched — emit the path verbatim. */
				e = out_write(out, out_cap, &out_off,
					      p, path_len);
				if (e < 0) return e;
			}
		} else {
			e = out_write(out, out_cap, &out_off, p, path_len);
			if (e < 0) return e;
		}

		if (has_nl) {
			e = out_write(out, out_cap, &out_off, "\n", 1);
			if (e < 0) return e;
			i = line_end + 1;
		} else {
			i = line_end;
		}
	}
	return (long)out_off;
}
