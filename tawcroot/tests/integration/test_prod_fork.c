/* Integration tests for guest-driven fork() under production tawcroot.
 *
 * The phase-1 testhost stress already covers concurrent CLONE_VM |
 * CLONE_THREAD threads (`test_sigsys_block_shadow_multithread`); that
 * exercises the per-tid signal shadow. What it does NOT exercise is a
 * guest creating a SEPARATE PROCESS via clone(SIGCHLD) — the path
 * where the SIGSYS handler runs against a different kernel pid/tid
 * than the supervisor's parent.
 *
 * Past production regressions all flowed through this gap:
 *
 *   - process_vm_readv must address the *current* thread's tid;
 *     caching the parent's tid silently broke fork-children's path
 *     translation (notes/tawcroot.md "More phase-5b bugs"; comment
 *     in usercopy.c:14-22).
 *
 *   - Bash's fork+execve chain depends on every link working in a
 *     fresh process: handle_execve, exec_handler_perform's memfd
 *     handoff, --exec-child re-init, and the SIGSYS-blocked sigmask
 *     unblock in loader_exec_child (notes/tawcroot.md "phase-5b").
 *
 *   - gpgme's pre-exec closefrom() needs reserved fds to survive
 *     the close()/close_range() barrage and remain re-exportable
 *     through bind.src[256] (notes/tawcroot.md "Phase 5c — full
 *     integration suite").
 *
 * Each test below runs production tawcroot in `-r ROOTFS` mode
 * against a static fork fixture, observes a side-effect (marker file
 * or exit code), and asserts the fixture's exit code matches the
 * expected success encoding.
 *
 * Process tree:
 *
 *   cleat orchestrator (host glibc)
 *     → tawcroot (-r ROOTFS) (manual-loads fixture)
 *           [parent] clone(SIGCHLD) ─────────────────┐
 *           [parent] wait4 → exit_group(forwarded)   │
 *           [child]  ─────────────────────────────►  ┴ openat / execve
 *
 * That second `[child]` is the path under test. Without a real
 * fork fixture the surface only sees a single tawcroot process per
 * test — which is exactly why the regressions above were caught
 * post-deploy on real Arch packages.
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
#ifndef TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN
# error "TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_FORK_EXEC_ARGV1_BIN
# error "TAWCROOT_STATIC_FORK_EXEC_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_FORK_CLOSEFROM_EXEC_ARGV1_BIN
# error "TAWCROOT_STATIC_FORK_CLOSEFROM_EXEC_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_CHECK_PROC_EXE_ARGV0_BIN
# error "TAWCROOT_STATIC_CHECK_PROC_EXE_ARGV0_BIN must be defined"
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define FAKE_ROOTFS  TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-fork"

/* Build a minimal rootfs containing all three fork fixtures plus the
 * static_exit42 child binary used by the fork+exec / closefrom+exec
 * tests. */
static bool build_rootfs(void)
{
	char p[PATH_MAX];

	if (!rh_mkdir_p(FAKE_ROOTFS, 0755)) return false;
	snprintf(p, sizeof p, "%s/bin", FAKE_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;

	struct { const char *src; const char *name; } fixtures[] = {
		{ TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN,
		  "static_fork_open_argv1" },
		{ TAWCROOT_STATIC_FORK_EXEC_ARGV1_BIN,
		  "static_fork_exec_argv1" },
		{ TAWCROOT_STATIC_FORK_CLOSEFROM_EXEC_ARGV1_BIN,
		  "static_fork_closefrom_exec_argv1" },
		{ TAWCROOT_STATIC_EXIT42_BIN,
		  "static_exit42" },
		{ TAWCROOT_STATIC_CHECK_PROC_EXE_ARGV0_BIN,
		  "static_check_proc_exe_argv0" },
	};
	for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
		snprintf(p, sizeof p, "%s/bin/%s",
		         FAKE_ROOTFS, fixtures[i].name);
		if (!rh_copy_file(fixtures[i].src, p, 0755)) return false;
	}
	return true;
}

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

/* Runs the fork+open fixture and verifies BOTH the exit code (parent
 * forwards child's success code 42) AND the marker file's existence
 * inside the rootfs.
 *
 * Why both: an exit code of 42 alone proves the child reached the
 * exit syscall; the marker file proves the child's openat actually
 * landed in the rootfs (path translation worked end-to-end against
 * the child's PID/VM, which is the cached-tid regression surface). */
test(prod_fork_child_opens_marker_in_rootfs)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *guest_marker = "/marker-fork-child";
	char host_marker[PATH_MAX];
	snprintf(host_marker, sizeof host_marker,
	         "%s%s", FAKE_ROOTFS, guest_marker);
	(void)unlink(host_marker);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_fork_open_argv1", guest_marker, NULL
	};
	int rc = run_with(args);

	/* Exit 42 — fork worked, child opened, parent reaped. */
	test_int_eq(rc, 42);

	/* Marker exists on disk inside the rootfs — proves the child's
	 * openat path-translated correctly through process_vm_readv
	 * against the child's tid. With the cached-tid bug this would
	 * surface as -EFAULT and the fixture would exit 90+14=104. */
	test_int_eq(access(host_marker, F_OK), 0);

	(void)unlink(host_marker);
	rh_rmrf(FAKE_ROOTFS);
}

test(prod_fork_then_execve_in_child)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_fork_exec_argv1", "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

test(prod_fork_closefrom_then_execve_in_child)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_fork_closefrom_exec_argv1",
		"/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* Same as prod_fork_then_execve_in_child but with bind mounts in
 * play. Validates that bind src_fds (>= RESERVED_FD_BASE+1) survive
 * the fork-child's execve trap and re-export through bind.src[256]
 * — the exec_handler.c:104 "gpgme fork-child closefrom()" comment's
 * regression surface, plus the AT_EXECFN path-canonicalization fix
 * for descendants. */
test(prod_fork_then_execve_with_bind)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	/* Bind /tmp at /opt/host_tmp; fork-exec a binary inside the
	 * rootfs, no use of the bind dst. The bind src_fd is reserved
	 * but unused — its survival across the fork+execve trap is
	 * what we're checking. */
	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", TAWCROOT_TEST_TMPDIR ":opt/host_tmp",
		"--",
		"/bin/static_fork_exec_argv1", "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* /proc/self/exe must resolve to the *current* exec target after a
 * fork+execve, not the original guest binary. The Firefox libxul.so
 * regression (notes/tawcroot.md "Phase 5c"; exec_handler.c:91-101):
 * descendants saw "/bin/bash" through /proc/self/exe instead of their
 * own exec target, breaking $ORIGIN-relative dlopen.
 *
 * The fixture chain:
 *   tawcroot loads static_fork_exec_argv1 → forks → child execve's
 *   static_check_proc_exe_argv0 → handler runs exec_handler_perform
 *   with extras.guest_exe=NULL → --exec-child loader_exec_child sets
 *   guest_exe = st.path (the actual exec target) → readlink in the
 *   final binary returns "/bin/static_check_proc_exe_argv0", which
 *   matches argv[0] → exit 42.
 *
 * If the regression returns (handler ferries the parent's stashed
 * guest_exe through extras), readlink returns the static_fork_exec_argv1
 * path which doesn't match argv[0] → fixture exits 95 → test fails. */
test(prod_fork_exec_proc_self_exe_correct_in_child)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_fork_exec_argv1",
		"/bin/static_check_proc_exe_argv0", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* Same closefrom shape, but with a bind mount: the bind src_fd lives
 * at fd 1001, the fixture's `close(1001)` aims directly at it. After
 * close+execve, --exec-child must re-establish the bind table from
 * the in-memory `bind.src[256]` strings (the original src_fd is
 * gone). This is the precise regression that broke pacman/gpgme
 * scriptlets in production until exec_handler started snapshotting
 * the host paths. */
test(prod_fork_closefrom_then_execve_with_bind)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", TAWCROOT_TEST_TMPDIR ":opt/host_tmp",
		"--",
		"/bin/static_fork_closefrom_exec_argv1",
		"/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}
