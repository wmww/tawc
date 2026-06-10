/* Unit tests for the path-translation orchestration
 * (tawcroot/src/path_orchestrate.c).
 *
 * The orchestration is pure with respect to the OS: every external
 * dependency (rootfs/bind tables, memo table, readlink oracle, cwd
 * source) is in the context struct passed by the caller. Production
 * builds the ctx from process globals + raw_sys helpers in
 * `tawcroot_path_translate` (path.c); these tests build it inline so
 * we can exercise the inter-stage seams that handler-layer integration
 * tests miss:
 *
 *   - fold → bind → (memo → resolver) → bind ordering (B5/D3)
 *   - bind-vs-memo collisions
 *   - rootfs-escape attempts via .. chains, absolute symlinks, and
 *     prefix-matching siblings
 *   - route_through_binds longest-prefix-match and component-boundary
 *     correctness
 *
 * `tawcroot_path_fold_absolute` (path_fold.c) was previously only
 * covered indirectly via test_path_resolve.c. Direct fold tests for
 * its overflow / clamp / `.`-`..`-edge cases are in this file too.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <string.h>

#include "path.h"
#include "path_oracle.h"
#include "path_orchestrate.h"
#include "path_resolve.h"

/* Sentinel base_fd value. The orchestration only inspects whether the
 * result's base_fd != ctx->rootfs_base_fd to decide "did a bind take
 * over"; the value itself is opaque. We use 100 in the rootfs slot
 * and 200+i for bind slots so test failures show which one matched. */
#define TEST_ROOTFS_FD 100

static long mock_readlink_none(void *ctx, const char *suffix,
			       char *out, size_t out_cap)
{
	(void)ctx; (void)suffix; (void)out; (void)out_cap;
	return -22;  /* EINVAL — every component "is not a symlink" */
}

static const struct tawcroot_path_oracle ora_empty = {
	.ctx = 0, .readlink = mock_readlink_none,
};

/* Mock symlink table — same shape as test_path_resolve.c's mock_fs. */
struct mock_link {
	const char *path;
	const char *target;
};

struct mock_fs {
	const struct mock_link *links;
	size_t                  n_links;
};

static long mock_readlink(void *ctx, const char *suffix,
			  char *out, size_t out_cap)
{
	struct mock_fs *fs = ctx;
	if (suffix[0] == 0) return -22;
	for (size_t i = 0; i < fs->n_links; i++) {
		if (strcmp(fs->links[i].path, suffix) == 0) {
			size_t tl = strlen(fs->links[i].target);
			if (tl > out_cap) return -36;
			memcpy(out, fs->links[i].target, tl);
			return (long)tl;
		}
	}
	return -22;
}

/* Cwd source for relative-path tests. The orchestration calls this
 * with the test-supplied ctx; we hand back a fixed string. */
struct cwd_state {
	const char *value;
	long        ret;     /* -errno to report (0 = success). */
};

static long mock_cwd(void *ctx, char *out, size_t out_cap)
{
	struct cwd_state *cs = ctx;
	if (cs->ret < 0) return cs->ret;
	size_t n = strlen(cs->value);
	if (n + 1 > out_cap) return -36;
	memcpy(out, cs->value, n + 1);
	return 0;
}

/* Helpers to populate a tawcroot_bind by hand. The struct's `src_fd`
 * is opaque to the orchestration; we set it to a distinct value per
 * bind so failures can identify which bind matched. */
static void mk_bind(struct tawcroot_bind *b, int fd, const char *dst)
{
	b->src_fd  = fd;
	b->active  = 1;
	size_t n = strlen(dst);
	b->dst_len = n;
	memcpy(b->dst, dst, n);
	b->dst[n]  = 0;
	b->src[0]  = 0;
	b->src_len = 0;
}

/* ----- Direct fold tests (was: covered only via resolver) ----- */

test(fold_basic_strips_leading_slash)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/foo/bar", out, sizeof out), 0);
	test_str_eq(out, "foo/bar");
}

test(fold_collapses_runs_of_slashes)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("///foo//bar///baz",
						out, sizeof out), 0);
	test_str_eq(out, "foo/bar/baz");
}

test(fold_strips_trailing_slash)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/foo/bar/", out, sizeof out), 0);
	test_str_eq(out, "foo/bar");
}

test(fold_dot_components_drop)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/./foo/./bar/.",
						out, sizeof out), 0);
	test_str_eq(out, "foo/bar");
}

test(fold_dotdot_clamps_at_root)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/foo/../../etc",
						out, sizeof out), 0);
	test_str_eq(out, "etc");
}

test(fold_pure_dotdot_at_root_is_root)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/..", out, sizeof out), 0);
	test_str_eq(out, "");
}

test(fold_root_dot_is_root)
{
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute("/.", out, sizeof out), 0);
	test_str_eq(out, "");
}

test(fold_buffer_overflow_is_enametoolong)
{
	char out[8];
	long r = tawcroot_path_fold_absolute("/aaaaaaaaa/b", out, sizeof out);
	test_int_eq(r, -36);
}

test(fold_deep_dotdot_chain_at_limit)
{
	/* 32 levels of ../ at the root; folds to empty (root). */
	char in[256] = "/";
	for (int i = 0; i < 32; i++) strcat(in, "../");
	strcat(in, "foo");
	char out[256];
	test_int_eq(tawcroot_path_fold_absolute(in, out, sizeof out), 0);
	test_str_eq(out, "foo");
}

/* ----- Orchestration: relative-path cwd source ----- */

test(orch_relative_uses_cwd_to_join)
{
	struct cwd_state cs = { "/srv", 0 };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = TEST_ROOTFS_FD,
		.binds = 0, .n_binds = 0,
		.memos = 0, .n_memos = 0,
		.oracle           = &ora_empty,
		.cwd_to_guest_abs = mock_cwd,
		.cwd_ctx          = &cs,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "data/x", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_str_eq(out, "srv/data/x");
}

test(orch_relative_cwd_at_rootfs_root)
{
	struct cwd_state cs = { "/", 0 };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = TEST_ROOTFS_FD,
		.oracle           = &ora_empty,
		.cwd_to_guest_abs = mock_cwd, .cwd_ctx = &cs,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "etc/passwd", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "etc/passwd");
}

test(orch_relative_cwd_propagates_error)
{
	struct cwd_state cs = { "", -2 };  /* ENOENT — cwd outside rootfs */
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = TEST_ROOTFS_FD,
		.oracle           = &ora_empty,
		.cwd_to_guest_abs = mock_cwd, .cwd_ctx = &cs,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "x", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -2);
}

test(orch_relative_no_cwd_fn_is_enoent)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = TEST_ROOTFS_FD,
		.oracle           = &ora_empty,
		.cwd_to_guest_abs = 0, .cwd_ctx = 0,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "x", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -2);
}

/* ----- Orchestration: route_through_binds ----- */

test(orch_bind_exact_match_empty_suffix)
{
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "tmp");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/tmp", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "");
}

test(orch_bind_prefix_match_strips_dst)
{
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "system");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/system/lib/foo", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "lib/foo");
}

test(orch_bind_component_boundary_required)
{
	/* Bind dst "system" must NOT match "/system_ext/foo" — the next
	 * byte after the dst is "_", not "/" or end-of-string. */
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "system");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/system_ext/foo", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_str_eq(out, "system_ext/foo");
}

test(orch_bind_longest_prefix_wins)
{
	struct tawcroot_bind binds[3];
	mk_bind(&binds[0], 201, "usr");
	mk_bind(&binds[1], 202, "usr/lib");
	mk_bind(&binds[2], 203, "usr/lib/firmware");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 3,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/usr/lib/firmware/blob",
		out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 203);   /* deepest match wins, not table order */
	test_str_eq(out, "blob");
}

test(orch_bind_no_match_keeps_rootfs_fd)
{
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "system");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/passwd", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_str_eq(out, "etc/passwd");
}

/* ----- Orchestration: stage ordering ----- */

test(orch_bind_takes_priority_over_memo)
{
	/* Memo /lib → usr/lib AND a bind on /lib → src=200.
	 *
	 * Bind must come first (B5/D3): the user-supplied bind table is
	 * the authoritative replacement for /lib. If memo ran first the
	 * input would become "usr/lib/x" and the bind on /lib would no
	 * longer apply, silently routing the request to the rootfs view
	 * instead of the user's bind src. */
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "lib");
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/libc.so.6", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);    /* bind won, memo did not run */
	test_str_eq(out, "libc.so.6");
}

test(orch_memo_then_bind_when_no_first_pass_match)
{
	/* Memo /lib → usr/lib AND a bind on /usr/lib → src=200.
	 *
	 * Input /lib/x doesn't match the bind directly, so the first
	 * bind pass falls through. Memo then rewrites to usr/lib/x. The
	 * second bind pass picks up the bind on /usr/lib. */
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "usr/lib");
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/x", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "x");
}

test(orch_memo_with_absolute_target_refolds)
{
	/* Memo /bin → /usr/bin (absolute target). After the rewrite the
	 * suffix is "/usr/bin/x"; the orchestration re-folds, stripping
	 * the leading slash, and the final suffix is "usr/bin/x". */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "bin", .src_len = 3,
		.target = "usr/bin", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/bin/sh", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "usr/bin/sh");
}

test(orch_memo_skipped_under_nofollow_when_sole_component)
{
	/* Memo /lib → usr/lib. Under NOFOLLOW, an op against /lib itself
	 * (lstat, readlink, unlink) must operate on the symlink, not the
	 * link target — skip the rewrite. */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "lib");

	/* But under FOLLOW the rewrite still applies. */
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "usr/lib");
}

test(orch_memo_applies_to_path_with_trailing_components_under_nofollow)
{
	/* The sole-component skip is FOR FOLLOW only when the input is
	 * exactly "lib". /lib/foo under NOFOLLOW still goes through the
	 * rewrite (the leaf is foo, not the symlink). */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/foo", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "usr/lib/foo");
}

/* ----- Orchestration: resolver runs after memo, before final bind ----- */

test(orch_resolver_fires_after_memo_before_final_bind)
{
	/* No memo. A symlink etc/host-secret → /etc/passwd. Bind on
	 * /etc/passwd → src=200. The resolver must rewrite the symlink
	 * (clamping the absolute target inside the rootfs view); the
	 * final bind pass must then catch the resulting path. */
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "etc/passwd");
	const struct mock_link links[] = {
		{ "etc/host-secret", "/etc/passwd" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = { .ctx = &fs, .readlink = mock_readlink };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/host-secret", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "");
}

/* ----- Orchestration: rootfs-escape attempts ----- */

test(orch_dotdot_chain_clamps_at_rootfs_root)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/foo/../../../../../../etc/passwd",
		out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_str_eq(out, "etc/passwd");   /* clamped, not host-relative */
}

test(orch_absolute_symlink_is_clamped_via_resolver)
{
	const struct mock_link links[] = {
		{ "etc/secret", "/etc/passwd" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = { .ctx = &fs, .readlink = mock_readlink };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/secret", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_str_eq(out, "etc/passwd");
}

/* ----- Misuse: NULLs and zero capacity ----- */

test(orch_null_guest_path_is_efault)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD, .oracle = &ora_empty,
	};
	char out[16];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, 0, out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -14);
}

test(orch_null_out_suffix_is_efault)
{
	/* Tightened by the post-review pass — the orchestration's pre-
	 * fold helpers would segfault on a NULL out_suffix; surface it
	 * as -EFAULT instead so misuse fails cleanly. */
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD, .oracle = &ora_empty,
	};
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/foo", 0, 16, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -14);
}

test(orch_zero_capacity_is_efault)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD, .oracle = &ora_empty,
	};
	char out[16];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/foo", out, 0, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -14);
}

/* ----- chroot bind re-anchoring (tawcroot_path_binds_reanchor) -----
 *
 * Pure on bind state — exercised here directly. The handler-layer
 * tests in tests/testhost/src/rootfs_smoke.c cover the integration via a
 * real chroot syscall on a fake rootfs; these exercise the corner
 * cases that are awkward to set up there (overflow, exact-match-of-
 * new-root, mixed-state arrays). */

static struct tawcroot_bind mk_bind_with_dst(const char *dst)
{
	struct tawcroot_bind b = {0};
	b.src_fd  = -1;
	b.active  = 1;
	size_t n = strlen(dst);
	b.dst_len = n;
	for (size_t i = 0; i < n; i++) b.dst[i] = dst[i];
	b.dst[n]  = 0;
	b.src_len = 0;
	return b;
}

test(reanchor_identity_keeps_all_binds_intact)
{
	/* chroot to the SAME root: all binds survive with identical
	 * dsts. Models pacman 6.x's chroot("/") path. */
	struct tawcroot_bind binds[2];
	binds[0] = mk_bind_with_dst("dev/shm");
	binds[1] = mk_bind_with_dst("usr/lib");
	long rv = tawcroot_path_binds_reanchor(
		binds, 2,
		"/data/rootfs", 12,
		"/data/rootfs", 12);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 1);
	test_str_eq(binds[0].dst, "dev/shm");
	test_int_eq(binds[1].active, 1);
	test_str_eq(binds[1].dst, "usr/lib");
}

test(reanchor_inside_chroot_strips_prefix)
{
	/* chroot to /data/rootfs/usr — bind originally at usr/lib re-
	 * anchors to lib (host path /data/rootfs/usr/lib stays valid;
	 * the chroot just renames it). */
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("usr/lib");
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 1);
	test_str_eq(binds[0].dst, "lib");
	test_int_eq((int)binds[0].dst_len, 3);
}

test(reanchor_outside_chroot_drops_bind)
{
	/* chroot to /data/rootfs/usr — bind at lib64 (host path
	 * /data/rootfs/lib64) is sibling of /usr, not under it; drop. */
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("lib64");
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);
	test_int_eq((int)binds[0].dst_len, 0);
}

test(reanchor_exact_match_of_new_root_drops_bind)
{
	/* chroot lands EXACTLY on a bind dst's host path. The new
	 * rootfs_fd opens the same dir; the bind would be redundant
	 * (and would route the same paths twice). Drop it. */
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("usr");
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);
}

test(reanchor_component_boundary_required)
{
	/* New root /data/rootfs/usr must NOT swallow a bind whose host
	 * path is /data/rootfs/usrlocal (sibling, shares prefix bytes
	 * but not at a component boundary). The bind is sibling-out,
	 * deactivate. */
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("usrlocal");
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);
}

test(reanchor_inactive_bind_left_alone)
{
	/* An already-inactive bind isn't re-evaluated. */
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("usr/lib");
	binds[0].active = 0;
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);
	test_str_eq(binds[0].dst, "usr/lib");   /* untouched */
}

test(reanchor_mixed_array)
{
	/* Realistic case: chroot to /data/rootfs/usr with three binds
	 * — one inside, one outside, one at the new root. Each should
	 * end up in the right state independently. */
	struct tawcroot_bind binds[3];
	binds[0] = mk_bind_with_dst("usr/lib");      /* inside  → re-anchor */
	binds[1] = mk_bind_with_dst("dev/shm");      /* outside → drop */
	binds[2] = mk_bind_with_dst("usr");          /* exact   → drop */
	long rv = tawcroot_path_binds_reanchor(
		binds, 3,
		"/data/rootfs", 12,
		"/data/rootfs/usr", 16);
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 1);
	test_str_eq(binds[0].dst, "lib");
	test_int_eq(binds[1].active, 0);
	test_int_eq(binds[2].active, 0);
}

test(reanchor_overflow_drops_bind_returns_enametoolong)
{
	/* Construct a pathological case: cur_root very short, dst as
	 * long as the buffer almost allows, new_root short enough that
	 * stripping a 1-char prefix grows the dst by a lot... actually
	 * it's hard to GROW with this scheme since stripping always
	 * shortens. Try the real overflow case: rem_len > sizeof dst.
	 * That's only possible if the input dst is right at the limit
	 * AND the new_root is shorter than (cur_root + "/"). Wait —
	 * with this routing rem_len = total - new_len - 1
	 *                         = cur_len + 1 + dst_len - new_len - 1
	 *                         = cur_len + dst_len - new_len.
	 * If new_len > cur_len, rem_len < dst_len. If new_len < cur_len
	 * (meaning the new root has fewer bytes than the old, which is
	 * unusual but possible if the chroot target prefix-matches via
	 * a different absolute path), rem_len > dst_len. So construct
	 * cur="/aaaaa" (6), new="/a" (2), bind dst at the buffer limit
	 * minus 4. */
	struct tawcroot_bind binds[1];
	char dst_buf[sizeof binds[0].dst];
	for (size_t i = 0; i < sizeof dst_buf - 1; i++) dst_buf[i] = 'b';
	dst_buf[sizeof dst_buf - 1] = 0;
	binds[0] = mk_bind_with_dst(dst_buf);
	long rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/aaaaa", 6,
		"/a",     2);
	/* This case has new_root /a as a prefix of cur_root /aaaaa
	 * (prefix-match without component boundary on next byte 'a' →
	 * outside), so it's actually deactivated as outside-of-view
	 * rather than overflow. Hits the "no component boundary" path. */
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);

	/* Real overflow: new root is one byte shorter than cur_root and
	 * IS a prefix at a component boundary. Unusual in production,
	 * but easy to construct. cur="/a/bb" (5), new="/a" (2), dst at
	 * the buffer limit. After stripping new_root + "/" = 3 bytes
	 * from "/a/bb/<dst>", remainder is "bb/<dst>" — longer than
	 * the original dst by exactly 3 bytes. Choose dst length so
	 * remainder overflows. */
	for (size_t i = 0; i < sizeof dst_buf - 1; i++) dst_buf[i] = 'c';
	dst_buf[sizeof dst_buf - 1] = 0;
	binds[0] = mk_bind_with_dst(dst_buf);
	rv = tawcroot_path_binds_reanchor(
		binds, 1,
		"/a/bb", 5,
		"/a",    2);
	test_int_eq(rv, -36);              /* -ENAMETOOLONG */
	test_int_eq(binds[0].active, 0);   /* overflowed bind dropped */
}

test(reanchor_chroot_into_bind_src_drops_all_binds)
{
	/* When the guest chroots into a bind dst, the path translator
	 * routes the chroot target through the bind and openat lands
	 * at the bind src's host path. binds_reanchor then sees the
	 * new root (= bind src's host path, e.g. "/tmp/x") has no
	 * shared prefix with the rootfs-anchored composed
	 * `cur_root + "/" + dst` (e.g. "/data/rootfs/usr/test-bind").
	 *
	 * Outcome: every bind drops, including the one we chrooted
	 * into. The chroot still works (the new rootfs_fd points at
	 * the bind src dir); we just lose the bind table. Workloads
	 * that chroot into a bind don't typically need other binds
	 * after, so this is acceptable. The notes document this
	 * limitation in §"chroot emulation". */
	struct tawcroot_bind binds[2];
	binds[0] = mk_bind_with_dst("usr/test-bind");   /* the chrooted-into bind */
	binds[1] = mk_bind_with_dst("usr/test-bind/inner"); /* nested under it */
	long rv = tawcroot_path_binds_reanchor(
		binds, 2,
		"/data/rootfs", 12,
		"/tmp/x",       6);   /* bind src host path */
	test_int_eq(rv, 0);
	test_int_eq(binds[0].active, 0);
	test_int_eq(binds[1].active, 0);   /* nested-under bind also lost */
}

test(reanchor_zero_binds_is_noop)
{
	long rv = tawcroot_path_binds_reanchor(
		0, 0, "/x", 2, "/x/y", 4);
	test_int_eq(rv, 0);
}

test(reanchor_null_paths_efault)
{
	struct tawcroot_bind binds[1];
	binds[0] = mk_bind_with_dst("foo");
	long rv = tawcroot_path_binds_reanchor(binds, 1, 0, 0, "/x", 2);
	test_int_eq(rv, -14);
	rv = tawcroot_path_binds_reanchor(binds, 1, "/x", 2, 0, 0);
	test_int_eq(rv, -14);
}

/* ----- Memo loop bound ----- */

test(orch_memo_loop_terminates_at_eight_hops)
{
	/* Pathological memo set: a → b, b → a (a 2-cycle). Without the
	 * hop bound, the orchestration would loop forever. With it, we
	 * exit after 8 hops with whichever side we land on; the only
	 * thing the test cares about is that the call returns at all
	 * (cleat has no test timeout — an infinite loop hangs the run). */
	struct tawcroot_symlink_memo memos[2] = {
		{ .src = "a", .src_len = 1,
		  .target = "b", .target_len = 1 },
		{ .src = "b", .src_len = 1,
		  .target = "a", .target_len = 1 },
	};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 2,
		.oracle = &ora_empty,
	};
	char out[16];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/a", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	/* After 8 hops alternating a↔b, the suffix is one of "a" or "b". */
	int landed = (out[0] == 'a' || out[0] == 'b') && out[1] == 0;
	test_int_eq(landed, 1);
}

/* ----- Empty guest path ----- */

test(orch_empty_path_is_enoent)
{
	/* Kernel semantics: an empty pathname fails ENOENT on every path
	 * syscall without AT_EMPTY_PATH. Regression: "" used to take the
	 * relative branch and translate to the cwd. */
	struct cwd_state cs = { .value = "/home/user", .ret = 0 };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora_empty,
		.cwd_to_guest_abs = mock_cwd, .cwd_ctx = &cs,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -2);
}

/* ----- Memo re-fold overflow ----- */

test(orch_memo_refold_overflow_is_enametoolong)
{
	/* A memo rewrite that grows the suffix past the re-fold scratch
	 * (4096 incl. the re-attached leading '/') must error out, not
	 * silently truncate to a wrong path. Build "/a/<4080 x's>" and a
	 * memo a → <32-byte prefix> so the rewritten path no longer fits. */
	static char in[4096];
	size_t i = 0;
	in[i++] = '/';
	in[i++] = 'a';
	in[i++] = '/';
	while (i < 4084) in[i++] = 'x';
	in[i] = 0;

	struct tawcroot_symlink_memo memos[1] = {{
		.src = "a", .src_len = 1,
		.target = "0123456789012345678901234567890", .target_len = 31,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	static char out[8192];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, in, out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, -36);
}

/* ----- Root-anchored memo target with multi-component src ----- */

test(orch_memo_multicomponent_src_uses_root_anchored_target)
{
	/* The builder (memo_one in path.c) stores relative symlink
	 * targets root-anchored: rootfs usr/sbin → "bin" is stored as
	 * target "usr/bin". This pins the orchestration side of that
	 * contract: the stored target replaces the full src prefix. */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "usr/sbin", .src_len = 8,
		.target = "usr/bin", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/usr/sbin/pacman", out, sizeof out,
		TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "usr/bin/pacman");
}

test(orch_memo_dotdot_target_refolds)
{
	/* var/run → ../run is stored root-anchored as "var/../run"; the
	 * re-fold collapses it to "run". */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "var/run", .src_len = 7,
		.target = "var/../run", .target_len = 10,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/var/run/dbus", out, sizeof out,
		TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "run/dbus");
}

/* ----- Memo shrink regression (comment in apply_memo describes it,
 * previously untested) ----- */

test(orch_memo_shrinking_target_keeps_tail)
{
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "very/long/prefix", .src_len = 16,
		.target = "p", .target_len = 1,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/very/long/prefix/bash", out, sizeof out,
		TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "p/bash");
}

/* ----- Trailing-slash semantics (kernel fs/namei.c: bytes after the
 * final component — '/' runs and '/.' — force LOOKUP_FOLLOW |
 * LOOKUP_DIRECTORY on the last step; PARENT-mode ops keep the leaf
 * verbatim and the kernel applies the directory rule to it) ----- */

test(orch_trailing_slash_survives_into_suffix)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/probe/", out, sizeof out, TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "etc/probe/");

	/* Slash runs and '/.' tails collapse to one appended slash. */
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/probe//.//", out, sizeof out,
		TAWCROOT_PATH_FOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "etc/probe/");
}

test(orch_trailing_slash_follows_leaf_symlink_under_nofollow)
{
	const struct mock_link links[] = { { "lnk", "real" } };
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = { .ctx = &fs, .readlink = mock_readlink };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lnk/", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "real/");

	/* '/.' tail forces the same follow. */
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/lnk/.", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "real/");

	/* Without the tail, NOFOLLOW still operates on the symlink. */
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/lnk", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "lnk");
}

test(orch_trailing_slash_keeps_leaf_verbatim_for_parent_modes)
{
	/* unlink("lnk/") / mkdir("lnk/") do NOT follow the leaf symlink on
	 * a real kernel — the verbatim leaf plus the appended slash reach
	 * the kernel, which then produces its native ENOTDIR/EEXIST. */
	const struct mock_link links[] = { { "lnk", "real" } };
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = { .ctx = &fs, .readlink = mock_readlink };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lnk/", out, sizeof out, TAWCROOT_PATH_PARENT_REMOVE);
	test_int_eq(r.err, 0);
	test_str_eq(out, "lnk/");

	r = tawcroot_path_translate_with_ctx(
		&ctx, "/lnk/", out, sizeof out, TAWCROOT_PATH_PARENT_CREATE);
	test_int_eq(r.err, 0);
	test_str_eq(out, "lnk/");
}

test(orch_trailing_slash_carries_through_bind)
{
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "proc");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/proc/self/", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "self/");
}

test(orch_trailing_slash_on_root_stays_empty_suffix)
{
	/* "/", "//", "/." resolve to the rootfs root (or a bind dst) —
	 * the empty suffix is the is_root signal and must stay empty. */
	struct tawcroot_bind binds[1];
	mk_bind(&binds[0], 200, "proc");
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = binds, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	const char *roots[] = { "/", "//", "/." };
	for (size_t i = 0; i < 3; i++) {
		tawcroot_path_result r = tawcroot_path_translate_with_ctx(
			&ctx, roots[i], out, sizeof out,
			TAWCROOT_PATH_NOFOLLOW);
		test_int_eq(r.err, 0);
		test_str_eq(out, "");
	}
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/proc/", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_str_eq(out, "");
}

test(orch_trailing_slash_applies_sole_component_memo_under_nofollow)
{
	/* lstat("/lib/") with memoized /lib → usr/lib: the kernel would
	 * follow the symlink (trailing slash), so the memo applies even
	 * under NOFOLLOW. */
	struct tawcroot_symlink_memo memos[1] = {{
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	}};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.memos = memos, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "usr/lib/");
}

test(orch_trailing_slash_relative_path)
{
	struct cwd_state cwd = { .value = "/srv", .ret = 0 };
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora_empty,
		.cwd_to_guest_abs = mock_cwd, .cwd_ctx = &cwd,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "sub/", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "srv/sub/");
}

test(orch_dot_heavy_names_are_not_trailing_markers)
{
	/* "name.", "name..", "..." are ordinary component names — no
	 * trailing-slash semantics. */
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/name.", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "etc/name.");

	r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/...", out, sizeof out, TAWCROOT_PATH_NOFOLLOW);
	test_int_eq(r.err, 0);
	test_str_eq(out, "etc/...");
}
