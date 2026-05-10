/* Integration tests for production-tawcroot feature paths that
 * existed only at the handler-test layer (testhost / phase1) before:
 *
 *   1. AF_UNIX bind() sun_path translation. Untested anywhere prior.
 *      The gpg-agent regression — bind("/run/.gnupg/S.gpg-agent") on
 *      the host fs instead of inside the rootfs (notes/tawcroot.md
 *      "More phase-5b bugs"; src/syscalls_socket.c).
 *
 *   2. Reserved fd hidden from /proc/self/fd via getdents64 dirent
 *      filtering. The gpgme closefrom death-spiral regression
 *      (notes/tawcroot.md "Phase 5c"; src/syscalls_fd.c::handle_getdents64
 *      + src/dirent_filter.c). The phase-1 testhost has the same
 *      check (test_internal_fd_protection) but only against the
 *      testhost's own filter install — not under production main.c +
 *      seccomp + supervisor_init.
 *
 *   3. Inherited SIGSYS-blocked sigmask is unblocked by supervisor_init.
 *      The JVM-spawned-shell regression (notes/tawcroot.md "Phase 4").
 *      Reproduced by blocking SIGSYS in the ORCHESTRATOR (not the guest
 *      — the handler intentionally strips SIGSYS from guest-issued
 *      sigprocmask calls), then forking and execing tawcroot. The
 *      supervisor_init mask reset is what keeps the guest's first
 *      trapping syscall alive.
 *
 * Each test follows the test_prod_rootfs/test_prod_fork pattern:
 * build a minimal rootfs containing only the fixtures we need, run
 * production tawcroot in `-r ROOTFS` mode, observe a side effect
 * (file-on-disk or exit code).
 */

#define _GNU_SOURCE
#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rootfs_helpers.h"

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined by the build"
#endif
#ifndef TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN
# error "TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN
# error "TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_UNIX_BIND_ARGV1_BIN
# error "TAWCROOT_STATIC_UNIX_BIND_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_CHECK_PROC_SELF_FD_BIN
# error "TAWCROOT_STATIC_CHECK_PROC_SELF_FD_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_IO_URING_DENY_BIN
# error "TAWCROOT_STATIC_IO_URING_DENY_BIN must be defined"
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define FAKE_ROOTFS  TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-features"

static bool build_rootfs(void)
{
	char p[PATH_MAX];

	if (!rh_mkdir_p(FAKE_ROOTFS, 0755)) return false;
	snprintf(p, sizeof p, "%s/bin", FAKE_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;
	snprintf(p, sizeof p, "%s/run", FAKE_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;

	struct { const char *src; const char *name; } fixtures[] = {
		{ TAWCROOT_STATIC_UNIX_BIND_ARGV1_BIN,
		  "static_unix_bind_argv1" },
		{ TAWCROOT_STATIC_CHECK_PROC_SELF_FD_BIN,
		  "static_check_proc_self_fd" },
		{ TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN,
		  "static_fork_open_argv1" },
		{ TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN,
		  "static_open_creat_argv1" },
		{ TAWCROOT_STATIC_IO_URING_DENY_BIN,
		  "static_io_uring_deny" },
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

/* AF_UNIX bind translates a guest sun_path to inside the rootfs.
 * Without sun_path translation the fixture would either bind on the
 * host filesystem (creating /run/agent.sock at host root) or fail
 * with -EACCES/-EROFS depending on permissions; either way the host
 * rootfs/run/agent.sock is empty so the test would fail on the
 * stat() assertion. */
test(prod_unix_bind_translates_sun_path)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *guest_sock = "/run/test-agent.sock";
	char host_sock[PATH_MAX];
	snprintf(host_sock, sizeof host_sock, "%s%s", FAKE_ROOTFS, guest_sock);
	(void)unlink(host_sock);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_unix_bind_argv1", guest_sock, NULL
	};
	test_int_eq(run_with(args), 42);

	/* The bound socket must exist as a socket inode at the
	 * translated host path. */
	struct stat st = {0};
	test_int_eq(stat(host_sock, &st), 0);
	test_true(S_ISSOCK(st.st_mode));

	(void)unlink(host_sock);
	rh_rmrf(FAKE_ROOTFS);
}

test(prod_proc_self_fd_hides_reserved)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	/* Bind host /proc → guest /proc so /proc/self/fd resolves to a
	 * real procfs view. (The test rootfs is empty under /proc; without
	 * the bind, openat /proc/self/fd from the guest would route to
	 * <rootfs>/proc/self/fd and ENOENT.) Production tawcroot rootfs
	 * setups always mount /proc; this mirrors that. */
	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", "/proc:proc",
		"--",
		"/bin/static_check_proc_self_fd", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* Block SIGSYS in the orchestrator child before it execs tawcroot.
 * Tawcroot inherits the kernel signal mask across execve, and
 * supervisor_init must reset it to empty (rt_sigprocmask SIG_SETMASK 0)
 * — otherwise the guest's first trapping syscall is killed by default
 * action when its SIGSYS is blocked-and-pending.
 *
 * We can't reproduce this from inside a guest fixture: handle_rt_sigprocmask
 * intentionally strips SIGSYS from the kernel mask change (so a guest
 * can't ever actually block SIGSYS at the kernel level via the regular
 * sigprocmask path). The block has to happen BEFORE tawcroot installs
 * the handler, i.e. in the parent process. */
test(prod_inherited_sigsys_block_unblocked_by_init)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *guest_marker = "/marker-sigsys-inherit";
	char host_marker[PATH_MAX];
	snprintf(host_marker, sizeof host_marker,
	         "%s%s", FAKE_ROOTFS, guest_marker);
	(void)unlink(host_marker);

	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGSYS);
		sigprocmask(SIG_BLOCK, &set, NULL);
		execl(TAWCROOT_PROD_BIN, "tawcroot",
		      "-r", FAKE_ROOTFS, "--",
		      "/bin/static_fork_open_argv1", guest_marker,
		      (char *)NULL);
		_exit(127);
	}
	int status = 0;
	test_int_eq((int)waitpid(pid, &status, 0), (int)pid);

	/* static_fork_open_argv1 forwards 42 on the success path (parent
	 * waits, child opens marker, both exit cleanly).
	 * With the fix: supervisor_init resets the mask; the guest's
	 * openat traps successfully; marker created; exit 42.
	 * Without it: the openat traps, kernel finds SIGSYS blocked,
	 * kills with default action (WIFSIGNALED, WTERMSIG=SIGSYS=31). */
	if (!WIFEXITED(status)) {
		fprintf(stderr,
		        "tawcroot died on signal %d (expected clean exit; "
		        "supervisor_init likely failed to reset SIGSYS mask)\n",
		        WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	}
	test_true(WIFEXITED(status));
	test_int_eq(WEXITSTATUS(status), 42);
	test_int_eq(access(host_marker, F_OK), 0);

	(void)unlink(host_marker);
	rh_rmrf(FAKE_ROOTFS);
}

#if defined(__x86_64__)
/* Legacy x86_64 open(2) (NR 2) must be path-translated. Glibc on
 * Android always uses openat (NR 257) because Android's stacked
 * filter RET_TRAPs the legacy NRs and glibc falls back; but a
 * static binary that issues the raw legacy syscall directly
 * (or any non-glibc libc that prefers open(2)) would otherwise
 * bypass tawcroot's rootfs view entirely.
 *
 * static_open_creat_argv1's _start does:
 *   open(argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600); exit_group(rv<0?201:0)
 * via NR 2.
 *
 * Expected with the handler registered: trap → handle_open routes
 * through openat(AT_FDCWD,…) → path translation lands at
 * <rootfs>/<argv[1]> → file created → fixture exits 0.
 *
 * Without the handler: filter has no entry for NR 2, kernel runs
 * `open` directly against the host filesystem path "/<argv[1]>",
 * which fails with EACCES/ENOENT for an unprivileged caller, and
 * the fixture exits 201. Marker is never created at the expected
 * rootfs-relative location either way (with the bug, the open never
 * lands inside the rootfs).
 *
 * x86_64-only: aarch64 has no NR 2 syscall. */
test(prod_legacy_open_translates_path)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *guest_marker = "/marker-legacy-open";
	char host_marker[PATH_MAX];
	snprintf(host_marker, sizeof host_marker,
	         "%s%s", FAKE_ROOTFS, guest_marker);
	(void)unlink(host_marker);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_open_creat_argv1", guest_marker, NULL
	};
	test_int_eq(run_with(args), 0);

	test_int_eq(access(host_marker, F_OK), 0);

	(void)unlink(host_marker);
	rh_rmrf(FAKE_ROOTFS);
}
#endif

/* io_uring_setup, io_uring_enter, and io_uring_register all return
 * -ENOSYS regardless of args. The setup deny is the primary
 * correctness barrier (no ring fd → no SQE traffic); the enter and
 * register denies are defense-in-depth so an inherited ring fd from
 * a non-tawcroot parent can't smuggle path-bearing SQEs past us. */
test(prod_io_uring_all_three_deny)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_io_uring_deny", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}
