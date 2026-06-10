/* Integration tests for production rootfs-mode CLI:
 *
 *   tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]
 *
 * This is the full phase-2d/2f path: parse argv, open the rootfs, build
 * the bind table, install handler+filter, translate the guest cmd path,
 * and manual-load. The guest's syscalls trap into our handler and route
 * through tawcroot_path_translate.
 *
 * Coverage today:
 *   - static binary inside the rootfs runs and exits cleanly
 *   - argv0 visible to the guest is the GUEST path (not the host path)
 *   - bind dst routes the guest open to the bind src
 *   - missing -- separator and missing -r both exit with usage code 2
 *   - non-existent guest under -r returns the loader open-failed code
 */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rootfs_helpers.h"

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined by the build"
#endif
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_ARGC_RANDOM_BIN
# error "TAWCROOT_STATIC_ARGC_RANDOM_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_EXECVE_EXIT42_BIN
# error "TAWCROOT_STATIC_EXECVE_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_OPEN_RDONLY_ARGV1_BIN
# error "TAWCROOT_STATIC_OPEN_RDONLY_ARGV1_BIN must be defined"
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define FAKE_ROOTFS  TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-prod"
#define FAKE_BINDSRC TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-prod-bindsrc"

/* Build a minimal fake rootfs:
 *   <root>/bin/static_exit42        copy of the static fixture
 *   <root>/bin/static_argc_random   copy of the argv-checking fixture
 *   <root>/etc/probe                "from-rootfs"
 */
static bool build_rootfs(void)
{
	char p[PATH_MAX];

	if (!rh_mkdir_p(FAKE_ROOTFS, 0755)) return false;
	snprintf(p, sizeof p, "%s/bin", FAKE_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;
	snprintf(p, sizeof p, "%s/etc", FAKE_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;

	snprintf(p, sizeof p, "%s/bin/static_exit42", FAKE_ROOTFS);
	if (!rh_copy_file(TAWCROOT_STATIC_EXIT42_BIN, p, 0755)) return false;

	snprintf(p, sizeof p, "%s/bin/static_argc_random", FAKE_ROOTFS);
	if (!rh_copy_file(TAWCROOT_STATIC_ARGC_RANDOM_BIN, p, 0755)) return false;

	snprintf(p, sizeof p, "%s/bin/static_execve_exit42", FAKE_ROOTFS);
	if (!rh_copy_file(TAWCROOT_STATIC_EXECVE_EXIT42_BIN, p, 0755)) return false;

	snprintf(p, sizeof p, "%s/etc/probe", FAKE_ROOTFS);
	if (!rh_write_text(p, "from-rootfs\n")) return false;

	return true;
}

static bool build_bindsrc(void)
{
	if (!rh_mkdir_p(FAKE_BINDSRC, 0755)) return false;
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/static_exit42", FAKE_BINDSRC);
	return rh_copy_file(TAWCROOT_STATIC_EXIT42_BIN, p, 0755);
}

/* Run TAWCROOT_PROD_BIN with the supplied argv and return the exit
 * status (or negative on signal). Caller passes a NULL-terminated
 * extra_args list, identical shape to test_prod_exec.c::run. */
static int run_with(const char *const *extra_args)
{
	VecStr cmd = c_init(vec_str, {TAWCROOT_PROD_BIN});
	for (const char *const *p = extra_args; *p; p++) {
		vec_str_push(&cmd, *p);
	}
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd, .exit_code = &rc
	});
	failable_result_drop(&res);
	return rc;
}

test(prod_rootfs_static_exit42)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--", "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

test(prod_rootfs_bind_overrides_rootfs)
{
	rh_rmrf(FAKE_ROOTFS);
	rh_rmrf(FAKE_BINDSRC);
	test_true(build_rootfs());
	test_true(build_bindsrc());

	/* Bind <bindsrc> at /opt; running /opt/static_exit42 should resolve
	 * to <bindsrc>/static_exit42 (which the harness placed there) and
	 * exit 42. */
	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", FAKE_BINDSRC ":opt",
		"--", "/opt/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
	rh_rmrf(FAKE_BINDSRC);
}

test(prod_rootfs_no_args_prints_usage)
{
	/* `tawcroot` with no argv tail at all: argc < 2 → usage(2). */
	const char *args[] = { NULL };
	test_int_eq(run_with(args), 2);
}

test(prod_rootfs_missing_double_dash_is_usage)
{
	/* No `--` separator means we never enter the "command + args"
	 * region; tawcroot exits with usage code 2. */
	const char *args[] = {
		"-r", FAKE_ROOTFS, "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 2);
}

test(prod_rootfs_missing_r_is_usage)
{
	/* `-- CMD` with no `-r ROOTFS` falls through to the unknown-flag
	 * branch and exits with usage code 2. */
	const char *args[] = {
		"--", "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 2);
}

test(prod_rootfs_relative_rootfs_is_usage)
{
	/* prod_rootfs_init insists -r be absolute (review B4). */
	const char *args[] = {
		"-r", "tmp/relative", "--", "/bin/x", NULL
	};
	test_int_eq(run_with(args), 80);
}

test(prod_rootfs_nonexistent_guest_returns_loader_code)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--", "/this/does/not/exist", NULL
	};
	/* loader_exec.h: 60 = open guest failed. */
	test_int_eq(run_with(args), 60);

	rh_rmrf(FAKE_ROOTFS);
}

/* Phase 2g: a guest does execve() and the new process runs.
 *
 *   1. tawcroot starts in -r mode, installs handler+filter, manual-loads
 *      /bin/static_execve_exit42.
 *   2. The fixture issues execve("/bin/static_exit42"). Filter traps,
 *      handler dispatches to the exec handler's prepare/commit pair.
 *   3. prepare translates the path against the rootfs, probes that
 *      it's openable, builds an exec_state with rootfs + bind specs +
 *      guest_exe, and writes the memfd; commit execveats
 *      /proc/self/exe with --exec-child <fd>.
 *   4. The new tawcroot incarnation reads exec_state, re-establishes
 *      the rootfs view (open rootfs fd, bind table, handler reinstall),
 *      then manual-loads /bin/static_exit42 inside the rootfs.
 *   5. /bin/static_exit42 runs and exits 42.
 *
 * If any step fails the fixture or the loader exits with a code outside
 * the [42, 42] range. The encoding 90+errno from the fixture and 60..89
 * from the loader is disjoint from 42 so the test pinpoints which
 * link in the chain broke.
 */
test(prod_rootfs_guest_does_execve)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--", "/bin/static_execve_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

test(prod_rootfs_dotdot_clamps_at_root)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	/* /../../../bin/static_exit42 must clamp at the rootfs root and
	 * resolve to <rootfs>/bin/static_exit42 (which exists), exit 42.
	 * Without clamping this would resolve to host /bin/static_exit42,
	 * which doesn't exist on most systems; either way the assertion
	 * is "clamps inside the view, not host fs". */
	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/../../../bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* Cross-bind absolute symlink: a symlink that lives in bind A whose
 * absolute target is a real host path must follow through to that
 * host file. This is the Pixel-10-Pro-Fold libhybris regression:
 * `/system/lib64/libc.so → /apex/com.android.runtime/lib64/bionic/libc.so`
 * with same-path binds for both `/system` and `/apex`. With openat2 +
 * RESOLVE_IN_ROOT the kernel re-roots the absolute target at bind A's
 * src_fd looking for `<bindA>/<absolute>` and returns ENOENT — older
 * kernels without openat2 (e.g. Pixel 4a's 4.14) fall through to plain
 * openat, which chases the absolute target via the host root and
 * works.
 *
 * Same-path binds (this test) match Android's actual /system→/system,
 * /apex→/apex layout. A companion test
 * (prod_rootfs_cross_bind_abs_symlink_different_paths) verifies that
 * the contract isn't "src must equal dst" — it's "target must be a
 * real host path".
 *
 * Layout:
 *   <bind_a>/link.txt → <bind_b>/target.txt   (absolute, real host path)
 *   <bind_b>/target.txt                       (real file)
 *   tawcroot args: -b <bind_a>:<bind_a> -b <bind_b>:<bind_b>
 *
 * The fixture opens its argv[1] read-only and exits 0 on success / 201
 * on open failure. We launch it as `/bin/static_open_rdonly_argv1
 * <bind_a>/link.txt` so the symlink chase happens inside the guest's
 * openat, which is the broken code path. Pre-bug: 201. Post-fix: 0.
 */
test(prod_rootfs_cross_bind_abs_symlink)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	char bind_a[PATH_MAX];
	char bind_b[PATH_MAX];
	snprintf(bind_a, sizeof bind_a,
	         TAWCROOT_TEST_TMPDIR "/tawcroot-test-prod-bind-a");
	snprintf(bind_b, sizeof bind_b,
	         TAWCROOT_TEST_TMPDIR "/tawcroot-test-prod-bind-b");
	rh_rmrf(bind_a);
	rh_rmrf(bind_b);
	test_true(rh_mkdir_p(bind_a, 0755));
	test_true(rh_mkdir_p(bind_b, 0755));

	/* The host fixture binary lives in <FAKE_ROOTFS>/bin/, copied by
	 * build_rootfs above as /bin/static_open_rdonly_argv1. */
	char fixture[PATH_MAX];
	snprintf(fixture, sizeof fixture,
	         "%s/bin/static_open_rdonly_argv1", FAKE_ROOTFS);
	test_true(rh_copy_file(TAWCROOT_STATIC_OPEN_RDONLY_ARGV1_BIN,
	                       fixture, 0755));

	char target[PATH_MAX];
	snprintf(target, sizeof target, "%s/target.txt", bind_b);
	test_true(rh_write_text(target, "ok\n"));

	char link[PATH_MAX];
	snprintf(link, sizeof link, "%s/link.txt", bind_a);
	test_true(symlink(target, link) == 0);

	/* Same-path binds (src == dst), matching Android's /system→/system
	 * and /apex→/apex layout. */
	char ba_spec[PATH_MAX];
	char bb_spec[PATH_MAX];
	snprintf(ba_spec, sizeof ba_spec, "%s:%s", bind_a, bind_a);
	snprintf(bb_spec, sizeof bb_spec, "%s:%s", bind_b, bind_b);

	char guest_link[PATH_MAX];
	snprintf(guest_link, sizeof guest_link, "%s/link.txt", bind_a);

	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", ba_spec,
		"-b", bb_spec,
		"--", "/bin/static_open_rdonly_argv1", guest_link, NULL
	};
	test_int_eq(run_with(args), 0);

	rh_rmrf(bind_a);
	rh_rmrf(bind_b);
	rh_rmrf(FAKE_ROOTFS);
}

/* Like prod_rootfs_cross_bind_abs_symlink but with *different-path*
 * binds. The contract isn't "bind src must equal bind dst" — it's "the
 * symlink's absolute target must be a real host path the kernel can
 * follow from /". With `-b <bind_a_host>:/dst_a -b <bind_b_host>:/dst_b`
 * and a symlink at `<bind_a_host>/link.txt → <bind_b_host>/target.txt`
 * (absolute, real host path), opening `/dst_a/link.txt` should still
 * succeed: the kernel chases the absolute target through the host
 * filesystem, doesn't care that the path text never appears in the
 * guest view.
 *
 * This pins down the actual contract — see notes/tawcroot.md
 * §"No `openat2(RESOLVE_IN_ROOT)` shortcut". The case that *isn't*
 * supported is a symlink whose target is a guest-view-only path
 * (e.g. /dst_b/...) where the underlying host path differs; that
 * would need the manual resolver to walk leaf symlinks across bind
 * boundaries and we don't currently do that.
 */
test(prod_rootfs_cross_bind_abs_symlink_different_paths)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	char bind_a[PATH_MAX];
	char bind_b[PATH_MAX];
	snprintf(bind_a, sizeof bind_a,
	         TAWCROOT_TEST_TMPDIR "/tawcroot-test-prod-bind-a-diff");
	snprintf(bind_b, sizeof bind_b,
	         TAWCROOT_TEST_TMPDIR "/tawcroot-test-prod-bind-b-diff");
	rh_rmrf(bind_a);
	rh_rmrf(bind_b);
	test_true(rh_mkdir_p(bind_a, 0755));
	test_true(rh_mkdir_p(bind_b, 0755));

	char fixture[PATH_MAX];
	snprintf(fixture, sizeof fixture,
	         "%s/bin/static_open_rdonly_argv1", FAKE_ROOTFS);
	test_true(rh_copy_file(TAWCROOT_STATIC_OPEN_RDONLY_ARGV1_BIN,
	                       fixture, 0755));

	char target[PATH_MAX];
	snprintf(target, sizeof target, "%s/target.txt", bind_b);
	test_true(rh_write_text(target, "ok\n"));

	char link[PATH_MAX];
	snprintf(link, sizeof link, "%s/link.txt", bind_a);
	test_true(symlink(target, link) == 0);

	/* Different-path binds: bind src is the host tmp dir, bind dst is
	 * a synthetic /dst_a, /dst_b inside the rootfs view. The symlink
	 * target text (host path) doesn't appear anywhere in the guest
	 * view — it's resolved by the kernel through the host root. */
	char ba_spec[PATH_MAX];
	char bb_spec[PATH_MAX];
	snprintf(ba_spec, sizeof ba_spec, "%s:/dst_a", bind_a);
	snprintf(bb_spec, sizeof bb_spec, "%s:/dst_b", bind_b);

	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", ba_spec,
		"-b", bb_spec,
		"--", "/bin/static_open_rdonly_argv1", "/dst_a/link.txt", NULL
	};
	test_int_eq(run_with(args), 0);

	rh_rmrf(bind_a);
	rh_rmrf(bind_b);
	rh_rmrf(FAKE_ROOTFS);
}
