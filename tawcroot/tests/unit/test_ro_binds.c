/* Unit tests for read-only binds — the central enforcement point in
 * tawcroot_path_translate_with_ctx (path_orchestrate.c), the openat
 * flags→intent classifier, the `-b src:dst:ro` spec parser, and the
 * exec_state v6 bind_ro/root_ro ferry.
 *
 * The design contract under test (plans → notes/tawcroot/
 * path-translation.md §"Read-only binds"): handlers only DECLARE
 * intent; the refusal itself lives once in the orchestrator, after
 * the final bind route is known. Zero intent means WRITE (fail
 * closed), and the two mutating modes force write intent regardless
 * of the declaration. The handler-level matrix lives in
 * tests/hosted/test_ro_binds.c; this file pins the pure layers.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include "exec_state.h"
#include "path.h"
#include "path_oracle.h"
#include "path_orchestrate.h"
#include "syscalls_fs.h"
#include "tawcroot.h"

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

/* Mock symlink table — same shape as test_path_orchestrate.c. */
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

static void mk_bind(struct tawcroot_bind *b, int fd, const char *dst, int ro)
{
	memset(b, 0, sizeof *b);
	b->src_fd    = fd;
	b->active    = 1;
	b->read_only = ro;
	size_t n = strlen(dst);
	b->dst_len = n;
	memcpy(b->dst, dst, n);
	b->dst[n]  = 0;
}

/* ----- orchestrator enforcement ----------------------------------- */

test(ro_bind_write_intent_is_erofs)
{
	struct tawcroot_bind b;
	mk_bind(&b, 200, "ro", 1);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/ro/file", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, -30 /* EROFS */);
}

test(ro_bind_read_intent_succeeds_and_reports_ro)
{
	struct tawcroot_bind b;
	mk_bind(&b, 200, "ro", 1);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/ro/file", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_READ);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_int_eq(r.ro, 1);
	test_str_eq(out, "file");
}

test(ro_bind_parent_modes_deny_even_with_read_declared)
{
	/* The fail-closed coupling: PARENT_CREATE / PARENT_REMOVE exist
	 * only for mutations, so they force write intent even when a
	 * (mislabeled) call site declared READ. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "ro", 1);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/ro/newdir", out, sizeof out,
		TAWCROOT_PATH_PARENT_CREATE, TAWCROOT_PATH_INTENT_READ);
	test_int_eq(r.err, -30);
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/ro/file", out, sizeof out,
		TAWCROOT_PATH_PARENT_REMOVE, TAWCROOT_PATH_INTENT_READ);
	test_int_eq(r.err, -30);
}

test(rw_bind_write_intent_succeeds_ro_zero)
{
	struct tawcroot_bind b;
	mk_bind(&b, 200, "rw", 0);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/rw/file", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_int_eq(r.ro, 0);
}

test(rootfs_ro_ctx_behaves_as_ro_root)
{
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.rootfs_ro      = 1,
		.oracle         = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/f", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, -30);
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/f", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_READ);
	test_int_eq(r.err, 0);
	test_int_eq(r.ro, 1);
}

test(rw_bind_under_ro_root_stays_writable)
{
	/* Longest-prefix semantics: an RW bind nested under an RO root
	 * (or an RO bind dst) keeps its own flag — same as an RW mount
	 * under an RO mount. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "opt/rw", 0);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.rootfs_ro      = 1,
		.binds = &b, .n_binds = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/opt/rw/x", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_int_eq(r.ro, 0);
}

test(rw_bind_nested_under_ro_bind_stays_writable)
{
	struct tawcroot_bind b[2];
	mk_bind(&b[0], 200, "opt", 1);
	mk_bind(&b[1], 201, "opt/rw", 0);
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = b, .n_binds = 2,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/opt/rw/x", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 201);
	test_int_eq(r.ro, 0);
	/* ...while the RO parent still refuses. */
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/opt/x", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, -30);
}

test(memo_rewrite_into_ro_bind_denied)
{
	/* Bind on /usr/lib (RO) + memo lib → usr/lib: input /lib/x only
	 * matches the bind AFTER the memo rewrite (second bind pass) —
	 * the RO check must still fire on the final route. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "usr/lib", 1);
	struct tawcroot_symlink_memo memo = {
		.src = "lib", .src_len = 3,
		.target = "usr/lib", .target_len = 7,
	};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.memos = &memo, .n_memos = 1,
		.oracle = &ora_empty,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/x", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, -30);
	r = tawcroot_path_translate_with_ctx(
		&ctx, "/lib/x", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_READ);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, 200);
	test_int_eq(r.ro, 1);
}

test(symlink_into_ro_bind_denied_final_route_wins)
{
	/* A rootfs symlink whose target lands in an RO bind: the RESOLVER
	 * rewrites the path and the third bind pass routes it — final
	 * route wins, write denied. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "ro", 1);
	const struct mock_link links[] = {
		{ "etc/lnk", "/ro/target" },
	};
	struct mock_fs fs = { links, 1 };
	const struct tawcroot_path_oracle ora = {
		.ctx = &fs, .readlink = mock_readlink,
	};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/lnk", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, -30);
}

test(symlink_out_of_ro_territory_allowed_for_write)
{
	/* Inverse of the previous: the input NAME shares a prefix with
	 * nothing RO, the symlink target lands in plain rootfs — write
	 * allowed even though an RO bind exists elsewhere in the table. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "ro", 1);
	const struct mock_link links[] = {
		{ "etc/lnk", "/var/target" },
	};
	struct mock_fs fs = { links, 1 };
	const struct tawcroot_path_oracle ora = {
		.ctx = &fs, .readlink = mock_readlink,
	};
	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd = TEST_ROOTFS_FD,
		.binds = &b, .n_binds = 1,
		.oracle = &ora,
	};
	char out[256];
	tawcroot_path_result r = tawcroot_path_translate_with_ctx(
		&ctx, "/etc/lnk", out, sizeof out, TAWCROOT_PATH_FOLLOW,
		TAWCROOT_PATH_INTENT_WRITE);
	test_int_eq(r.err, 0);
	test_int_eq(r.base_fd, TEST_ROOTFS_FD);
	test_int_eq(r.ro, 0);
	test_str_eq(out, "var/target");
}

test(reanchor_preserves_read_only_flag)
{
	/* chroot re-anchoring edits bind structs in place; the RO flag
	 * must survive a dst rewrite. */
	struct tawcroot_bind b;
	mk_bind(&b, 200, "srv/inner/ro", 1);
	long rv = tawcroot_path_binds_reanchor(
		&b, 1, "/host/root", strlen("/host/root"),
		"/host/root/srv", strlen("/host/root/srv"));
	test_int_eq(rv, 0);
	test_int_eq(b.active, 1);
	test_str_eq(b.dst, "inner/ro");
	test_int_eq(b.read_only, 1);
}

/* ----- openat flags→intent classifier ------------------------------ */

test(openat_intent_classifier_table)
{
	static const struct { int flags; tawcroot_path_intent want; } T[] = {
		{ O_RDONLY,                     TAWCROOT_PATH_INTENT_READ  },
		{ O_WRONLY,                     TAWCROOT_PATH_INTENT_WRITE },
		{ O_RDWR,                       TAWCROOT_PATH_INTENT_WRITE },
		{ O_ACCMODE,                    TAWCROOT_PATH_INTENT_WRITE }, /* accmode 3 fails closed */
		{ O_RDONLY | O_TRUNC,           TAWCROOT_PATH_INTENT_WRITE },
		{ O_RDONLY | O_CREAT,           TAWCROOT_PATH_INTENT_WRITE },
		{ O_RDONLY | O_CREAT | O_EXCL,  TAWCROOT_PATH_INTENT_WRITE },
		{ O_RDONLY | O_APPEND,          TAWCROOT_PATH_INTENT_READ  }, /* kernel allows RDONLY|APPEND on RO fs */
		{ O_RDONLY | O_NOFOLLOW,        TAWCROOT_PATH_INTENT_READ  },
		{ O_RDONLY | O_DIRECTORY,       TAWCROOT_PATH_INTENT_READ  },
		{ O_PATH,                       TAWCROOT_PATH_INTENT_READ  },
		{ O_PATH | O_RDWR,              TAWCROOT_PATH_INTENT_READ  }, /* kernel ignores accmode with O_PATH */
		{ O_PATH | O_CREAT,             TAWCROOT_PATH_INTENT_READ  },
		{ O_WRONLY | O_CREAT | O_TRUNC, TAWCROOT_PATH_INTENT_WRITE },
	};
	for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
		test_int_eq((int)tawcroot_openat_intent(T[i].flags),
			    (int)T[i].want);
	}
}

/* ----- -b spec parser ---------------------------------------------- */

test(parse_bind_spec_two_fields_is_rw)
{
	char src[64], dst[64];
	int ro = -1;
	test_int_eq(tawcroot_parse_bind_spec("/a:/b", src, sizeof src,
					     dst, sizeof dst, &ro), 0);
	test_str_eq(src, "/a");
	test_str_eq(dst, "/b");
	test_int_eq(ro, 0);
}

test(parse_bind_spec_ro_suffix)
{
	char src[64], dst[64];
	int ro = -1;
	test_int_eq(tawcroot_parse_bind_spec("/a:/b:ro", src, sizeof src,
					     dst, sizeof dst, &ro), 0);
	test_str_eq(src, "/a");
	test_str_eq(dst, "/b");
	test_int_eq(ro, 1);
}

test(parse_bind_spec_bad_third_field_rejected)
{
	char src[64], dst[64];
	int ro = -1;
	test_int_eq(tawcroot_parse_bind_spec("/a:/b:rw", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
	test_int_eq(tawcroot_parse_bind_spec("/a:/b:", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
	test_int_eq(tawcroot_parse_bind_spec("/a:/b:ro:x", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
	test_int_eq(tawcroot_parse_bind_spec("/a:/b:RO", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
}

test(parse_bind_spec_missing_dst_rejected)
{
	char src[64], dst[64];
	int ro = -1;
	test_int_eq(tawcroot_parse_bind_spec("/a", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
	test_int_eq(tawcroot_parse_bind_spec("/a:", src, sizeof src,
					     dst, sizeof dst, &ro), -22);
}

/* ----- exec_state v6 ferry ----------------------------------------- */

test(exec_state_carries_bind_ro_and_root_ro)
{
	const char *argv[] = { "/bin/x", NULL };
	const char *envp[] = { NULL };
	const char *bind_src[] = { "/host/a", "/host/b" };
	const char *bind_dst[] = { "/a", "/b" };
	const unsigned char bind_ro[] = { 1, 0 };
	tawcroot_exec_state_extras ex = {
		.rootfs_host = "/host/root",
		.n_binds  = 2,
		.bind_src = bind_src,
		.bind_dst = bind_dst,
		.bind_ro  = bind_ro,
		.root_ro  = 1,
	};

	size_t need = tawcroot_exec_state_estimate_bytes("/bin/x", 1, argv,
							 envp, &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/bin/x",
					   1, argv, envp, &ex);
	test_true(w > 0);

	const char *argv_buf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *envp_buf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, (size_t)w, argv_buf,
						  envp_buf, &out), 0);
	test_int_eq((int)out.n_binds, 2);
	test_int_eq(out.bind_ro[0], 1);
	test_int_eq(out.bind_ro[1], 0);
	test_int_eq((int)out.root_ro, 1);
	free(buf);
}

test(exec_state_defaults_rw_without_flags)
{
	const char *argv[] = { "/bin/x", NULL };
	const char *envp[] = { NULL };
	const char *bind_src[] = { "/host/a" };
	const char *bind_dst[] = { "/a" };
	tawcroot_exec_state_extras ex = {
		.rootfs_host = "/host/root",
		.n_binds  = 1,
		.bind_src = bind_src,
		.bind_dst = bind_dst,
		/* bind_ro NULL, root_ro 0 */
	};

	size_t need = tawcroot_exec_state_estimate_bytes("/bin/x", 1, argv,
							 envp, &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/bin/x",
					   1, argv, envp, &ex);
	test_true(w > 0);

	const char *argv_buf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *envp_buf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, (size_t)w, argv_buf,
						  envp_buf, &out), 0);
	test_int_eq(out.bind_ro[0], 0);
	test_int_eq((int)out.root_ro, 0);
	free(buf);
}
