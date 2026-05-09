/* Unit tests for the /proc/self/maps reverse-translator
 * (tawcroot/src/proc_rewrite.c).
 *
 * The rewriter is pure with respect to the OS: the caller passes a
 * tawcroot_proc_rewrite_ctx with the rootfs host prefix and bind table.
 * The handler-side glue (read the kernel file, write a memfd) is in
 * syscalls_fs.c and out of scope here. These tests exercise:
 *
 *   - Single-path reverse-translation: rootfs prefix, bind src,
 *     longest-prefix-match across overlapping binds, component-boundary
 *     correctness (sibling sharing prefix bytes must not match), and
 *     output-buffer overflow.
 *   - Whole-file maps rewrite: typical line, bracketed pseudo-paths
 *     pass through, anonymous (empty path) lines pass through, multi-
 *     line inputs, the "(deleted)" trailing tag is preserved, output
 *     buffer too small returns -ENOSPC, and a partial trailing line
 *     (no '\n') is emitted as-is.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <string.h>

#include "path.h"
#include "proc_rewrite.h"

/* --- helpers ----------------------------------------------------- */

/* Populate a tawcroot_bind for a reverse-translation test. The orchestration
 * tests use mk_bind() with empty src; we need src filled in here. */
static void mk_bind_full(struct tawcroot_bind *b,
			 const char *src, const char *dst)
{
	size_t sn = strlen(src);
	size_t dn = strlen(dst);
	memcpy(b->src, src, sn + 1);
	memcpy(b->dst, dst, dn + 1);
	b->src_len = sn;
	b->dst_len = dn;
	b->src_fd  = 0;
	b->active  = 1;
}

/* --- single-path reverse-translation ----------------------------- */

test(rev_rootfs_prefix_strips)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/usr/lib/libfoo.so";
	char out[256];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/usr/lib/libfoo.so"));
	test_str_eq(out, "/usr/lib/libfoo.so");
}

test(rev_rootfs_exact_match_is_root)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs";
	char out[16];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, 1);
	test_str_eq(out, "/");
}

test(rev_rootfs_with_trailing_slash_is_root)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/";
	char out[16];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, 1);
	test_str_eq(out, "/");
}

test(rev_rootfs_sibling_prefix_does_not_match)
{
	/* "<rootfs>-evil/foo" byte-matches the rootfs prefix but is NOT
	 * inside the view — the byte after the prefix is '-', not '/' or
	 * end-of-string. The component-boundary check must reject. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs-evil/foo";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);  /* -ENOENT */
}

test(rev_no_match_returns_enoent)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/usr/lib/libtawcroot.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);
}

test(rev_bind_src_match)
{
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system_real", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/system_real/lib/libc.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/system/lib/libc.so"));
	test_str_eq(out, "/system/lib/libc.so");
}

test(rev_bind_longest_prefix_wins)
{
	struct tawcroot_bind binds[2];
	mk_bind_full(&binds[0], "/host/a",   "alpha");
	mk_bind_full(&binds[1], "/host/a/b", "beta");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 2,
	};
	const char in[] = "/host/a/b/c";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/beta/c"));
	test_str_eq(out, "/beta/c");
}

test(rev_bind_sibling_prefix_does_not_match)
{
	/* "/system_ext/foo" must not match a bind src "/system". */
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/system_ext/foo";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);
}

test(rev_bind_outranks_rootfs)
{
	/* Bind src "/data/rootfs/lib" sits inside the rootfs prefix. The
	 * mapping points there. Bind takes precedence (matches first
	 * because it's the longer prefix anyway, and conceptually because
	 * binds are authoritative). */
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/data/rootfs/lib", "lib-bound");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/data/rootfs/lib/libc.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/lib-bound/libc.so"));
	test_str_eq(out, "/lib-bound/libc.so");
}

test(rev_output_overflow_returns_nametoolong)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/usr/lib/libfoo.so";
	char small[8];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), small, sizeof small);
	test_int_eq(n, -36);  /* -ENAMETOOLONG */
}

/* --- whole-file maps rewrite ------------------------------------- */

test(maps_rewrites_rootfs_path_in_typical_line)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /data/rootfs/usr/lib/libfoo.so\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /usr/lib/libfoo.so\n");
}

test(maps_passes_bracketed_paths_through)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 [stack]\n"
		"7ffd00021000-7ffd00023000 r--p 00000000 00:00 0 [vvar]\n"
		"7ffd00023000-7ffd00024000 r-xp 00000000 00:00 0 [vdso]\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_passes_anonymous_lines_through)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	/* Anonymous mapping: empty path field after the inode. */
	const char in[] =
		"7f0000000000-7f0000010000 rw-p 00000000 00:00 0 \n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_preserves_deleted_tag)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r--p 00000000 08:01 999 /data/rootfs/tmp/scratch (deleted)\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"7f1234567000-7f1234589000 r--p 00000000 08:01 999 /tmp/scratch (deleted)\n");
}

test(maps_passes_unmapped_host_paths_through)
{
	/* Paths outside the rootfs and any bind src have no guest-visible
	 * counterpart; emit them verbatim rather than dropping the line. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f0000000000-7f0000020000 r-xp 00000000 08:01 1 /usr/lib/libtawcroot.so\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_rewrites_each_line_independently)
{
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system_real", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] =
		"a000-b000 r-xp 00000000 00:00 1 /data/rootfs/usr/lib/libc.so\n"
		"c000-d000 r--p 00000000 00:00 0 [heap]\n"
		"e000-f000 r-xp 00000000 00:00 2 /system_real/lib/libdl.so\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r-xp 00000000 00:00 1 /usr/lib/libc.so\n"
		"c000-d000 r--p 00000000 00:00 0 [heap]\n"
		"e000-f000 r-xp 00000000 00:00 2 /system/lib/libdl.so\n");
}

test(maps_partial_trailing_line_emitted_as_is)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"a000-b000 r--p 00000000 00:00 0 [heap]\n"
		"c000-d000 r-xp 00000000 00:00 1 /data/rootfs/usr/bin/sh";  /* no \n */
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r--p 00000000 00:00 0 [heap]\n"
		"c000-d000 r-xp 00000000 00:00 1 /usr/bin/sh");
}

test(maps_output_too_small_returns_nospc)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /data/rootfs/usr/lib/libfoo.so\n";
	char small[16];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    small, sizeof small);
	test_int_eq(n, -28);  /* -ENOSPC */
}
