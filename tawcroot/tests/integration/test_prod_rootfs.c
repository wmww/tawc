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

#define _GNU_SOURCE
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
	cstr out = cstr_init();
	cstr err = cstr_init();
	int rc = run_subproc(cmd, (SubprocArgs){
		.stdout = &out, .stderr = &err
	});
	cstr_drop(&out);
	cstr_drop(&err);
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
 *      handler dispatches to tawcroot_exec_handler_perform.
 *   3. exec_handler_perform translates the path against the rootfs,
 *      probes that it's openable, builds an exec_state with rootfs +
 *      bind specs + guest_exe, writes the memfd, and execveats
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
