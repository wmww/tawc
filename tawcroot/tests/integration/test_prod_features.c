/* Integration tests for production-tawcroot feature paths that
 * existed only at the handler-test layer (testhost / rootfs smoke) before:
 *
 *   1. AF_UNIX bind() sun_path translation. Untested anywhere prior.
 *      The gpg-agent regression — bind("/run/.gnupg/S.gpg-agent") on
 *      the host fs instead of inside the rootfs (notes/tawcroot/path-translation.md
 *      "More phase-5b bugs"; src/syscalls_socket.c).
 *
 *   2. Reserved fd hidden from /proc/self/fd via getdents64 dirent
 *      filtering. The gpgme closefrom death-spiral regression
 *      (notes/tawcroot/phasing.md "Phase 5c"; src/syscalls_fd.c::handle_getdents64
 *      + src/dirent_filter.c). The rootfs testhost has the same
 *      check (test_internal_fd_protection) but only against the
 *      testhost's own filter install — not under production main.c +
 *      seccomp + supervisor_init.
 *
 *   3. Inherited SIGSYS-blocked sigmask is unblocked by supervisor_init.
 *      The JVM-spawned-shell regression (notes/tawcroot/phasing.md "Phase 4").
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
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN
# error "TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_SMALL_STACK_OPEN_ARGV1_BIN
# error "TAWCROOT_STATIC_SMALL_STACK_OPEN_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_FEXECVE_ARGV1_BIN
# error "TAWCROOT_STATIC_FEXECVE_ARGV1_BIN must be defined"
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
#ifndef TAWCROOT_STATIC_GETDENTS_LEGACY_CHECK_BIN
# error "TAWCROOT_STATIC_GETDENTS_LEGACY_CHECK_BIN must be defined"
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
		{ TAWCROOT_STATIC_EXIT42_BIN,
		  "static_exit42" },
		{ TAWCROOT_STATIC_UNIX_BIND_ARGV1_BIN,
		  "static_unix_bind_argv1" },
		{ TAWCROOT_STATIC_CHECK_PROC_SELF_FD_BIN,
		  "static_check_proc_self_fd" },
		{ TAWCROOT_STATIC_GETDENTS_LEGACY_CHECK_BIN,
		  "static_getdents_legacy_check" },
		{ TAWCROOT_STATIC_FORK_OPEN_ARGV1_BIN,
		  "static_fork_open_argv1" },
		{ TAWCROOT_STATIC_SMALL_STACK_OPEN_ARGV1_BIN,
		  "static_small_stack_open_argv1" },
		{ TAWCROOT_STATIC_SIGALTSTACK_OPEN_ARGV1_BIN,
		  "static_sigaltstack_open_argv1" },
		{ TAWCROOT_STATIC_SIGALTSTACK_SMALL_OPEN_ARGV1_BIN,
		  "static_sigaltstack_small_open_argv1" },
		{ TAWCROOT_STATIC_SIGALTSTACK_SWAP_ARGV1_BIN,
		  "static_sigaltstack_swap_argv1" },
		{ TAWCROOT_STATIC_FEXECVE_ARGV1_BIN,
		  "static_fexecve_argv1" },
		{ TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN,
		  "static_open_creat_argv1" },
		{ TAWCROOT_STATIC_IO_URING_DENY_BIN,
		  "static_io_uring_deny" },
		{ TAWCROOT_STATIC_UNMAPPED_STACK_EXIT_BIN,
		  "static_unmapped_stack_exit" },
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
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd, .exit_code = &rc
	});
	failable_result_drop(&res);
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
#if defined(__ANDROID__)
	printf("    skipping (host-only: Android shell SELinux denies"
	       " filesystem AF_UNIX socket creation under test rootfs)\n");
	return;
#else
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
#endif
}

/* Over-budget sun_path: with a deep install prefix (production:
 * /data/data/me.phie.tawc/distros/<id>/rootfs) the host-absolute
 * rendering overflows sun_path's 108 bytes, so bind must fall back to
 * the /proc/self/fd spellings (tiers 2/3 in syscalls_socket.c). The
 * fixture also getsocknames the bound socket and compares against the
 * guest path, so this exercises the reverse translation of the /proc
 * spellings under production seccomp + SIGSYS. Rootfs lives under a
 * deliberately >107-byte tmpdir. */
test(prod_unix_bind_over_budget_sun_path)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: Android shell SELinux denies"
	       " filesystem AF_UNIX socket creation under test rootfs)\n");
	return;
#else
	char root[PATH_MAX];
	int n = snprintf(root, sizeof root, "%s/tawcroot-test-rootfs-afunix-",
	                 TAWCROOT_TEST_TMPDIR);
	test_true(n > 0 && (size_t)n + 91 < sizeof root);
	memset(root + n, 'p', 90);
	root[n + 90] = '\0';
	test_true(strlen(root) > 107);

	char p[PATH_MAX];
	rh_rmrf(root);
	snprintf(p, sizeof p, "%s/bin", root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/run", root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/bin/static_unix_bind_argv1", root);
	test_true(rh_copy_file(TAWCROOT_STATIC_UNIX_BIND_ARGV1_BIN, p, 0755));

	/* Tier 2: short guest suffix. */
	const char *guest_short = "/run/test-agent.sock";
	const char *args_short[] = {
		"-r", root, "--",
		"/bin/static_unix_bind_argv1", guest_short, NULL
	};
	test_int_eq(run_with(args_short), 42);
	snprintf(p, sizeof p, "%s%s", root, guest_short);
	struct stat st = {0};
	test_int_eq(stat(p, &st), 0);
	test_true(S_ISSOCK(st.st_mode));

	/* Tier 3: the suffix itself overflows the tier-2 spelling. */
	char deep[80];
	memset(deep, 'x', 70);
	deep[70] = '\0';
	snprintf(p, sizeof p, "%s/run/%s", root, deep);
	test_true(rh_mkdir_p(p, 0755));
	char guest_deep[128];
	n = snprintf(guest_deep, sizeof guest_deep, "/run/%s/", deep);
	test_true(n > 0);
	memset(guest_deep + n, 'y', 29);
	guest_deep[n + 29] = '\0';    /* guest len 105, suffix 104 */
	const char *args_deep[] = {
		"-r", root, "--",
		"/bin/static_unix_bind_argv1", guest_deep, NULL
	};
	test_int_eq(run_with(args_deep), 42);
	snprintf(p, sizeof p, "%s%s", root, guest_deep);
	memset(&st, 0, sizeof st);
	test_int_eq(stat(p, &st), 0);
	test_true(S_ISSOCK(st.st_mode));

	rh_rmrf(root);
#endif
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

#if defined(__x86_64__)
/* Same contract as prod_proc_self_fd_hides_reserved, but through the
 * LEGACY getdents(2) (NR 78) — end-to-end under the real seccomp
 * filter. This is the test that fails if NR 78 drops out of the trap
 * set: the fixture then reads raw kernel dirents (64-layout, reserved
 * fds visible) and exits 94/95 instead of 42. x86_64-only; the
 * aarch64 fixture twin exits 96 unconditionally. */
test(prod_legacy_getdents_hides_reserved_and_repacks)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", "/proc:proc",
		"--",
		"/bin/static_getdents_legacy_check", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}
#endif

test(prod_execveat_empty_path_execs_fd)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_fexecve_argv1", "/bin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(FAKE_ROOTFS);
}

/* memo_one (path.c) must store relative well-known-symlink targets
 * root-anchored: a relative symlink target is relative to the
 * symlink's PARENT directory. Regression: with rootfs `usr/sbin →
 * bin`, /usr/sbin/x used to be rewritten to bin/x (anchored at the
 * rootfs root) instead of usr/bin/x. The rootfs here deliberately has
 * NO top-level /bin, so the mis-anchored path can't be rescued by a
 * second symlink hop and the exec fails. */
test(prod_memoized_relative_symlink_anchors_at_parent)
{
	const char *root = TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-memo";
	char p[PATH_MAX];

	rh_rmrf(root);
	snprintf(p, sizeof p, "%s/usr/bin", root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/usr/bin/static_exit42", root);
	test_true(rh_copy_file(TAWCROOT_STATIC_EXIT42_BIN, p, 0755));
	snprintf(p, sizeof p, "%s/usr/sbin", root);
	test_int_eq(symlink("bin", p), 0);

	const char *args[] = {
		"-r", root, "--",
		"/usr/sbin/static_exit42", NULL
	};
	test_int_eq(run_with(args), 42);

	rh_rmrf(root);
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

test(prod_path_trap_from_small_stack_thread)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	const char *guest_marker = "/marker-small-stack";
	char host_marker[PATH_MAX];
	snprintf(host_marker, sizeof host_marker,
	         "%s%s", FAKE_ROOTFS, guest_marker);
	(void)unlink(host_marker);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_small_stack_open_argv1", guest_marker, NULL
	};
	test_int_eq(run_with(args), 42);
	test_int_eq(access(host_marker, F_OK), 0);

	(void)unlink(host_marker);
	rh_rmrf(FAKE_ROOTFS);
}

/* Fixture exit codes: 50 = frame hit the thread stack (SA_ONSTACK lost),
 * 51 = altstack (not) written, 52 = sigaltstack readback wrong. */
static int sigaltstack_case(const char *bin, const char *guest_marker)
{
	rh_rmrf(FAKE_ROOTFS);
	if (!build_rootfs()) return -1;

	char host_marker[PATH_MAX];
	snprintf(host_marker, sizeof host_marker,
	         "%s%s", FAKE_ROOTFS, guest_marker);

	const char *args[] = { "-r", FAKE_ROOTFS, "--", bin, guest_marker, NULL };
	int rc = run_with(args);
	if (rc == 42 && access(host_marker, F_OK) != 0) rc = -2;

	rh_rmrf(FAKE_ROOTFS);
	return rc;
}

test(prod_path_trap_lands_on_guest_sigaltstack)
{
	test_int_eq(sigaltstack_case("/bin/static_sigaltstack_open_argv1",
	                             "/marker-sigaltstack"), 42);
}

test(prod_undersized_sigaltstack_is_substituted)
{
	test_int_eq(sigaltstack_case("/bin/static_sigaltstack_small_open_argv1",
	                             "/marker-sigaltstack-small"), 42);
}

/* 60 = replacement not applied (frame missed B), 61 = frame still on A,
 * 62 = disable not read back; a SIGSEGV death = disable not applied. */
test(prod_sigaltstack_replace_and_disable_reach_kernel)
{
	test_int_eq(sigaltstack_case("/bin/static_sigaltstack_swap_argv1",
	                             "/marker-sigaltstack-swap"), 42);
}

/* musl's detached-thread exit: munmap the stack SP is on, then exit(2).
 * SIGSEGV death = the exit trap had no fallback altstack (ChatGPT's
 * bundled codex app-server died this way). */
test(prod_exit_on_unmapped_stack)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());
	const char *args[] = {
		"-r", FAKE_ROOTFS, "--", "/bin/static_unmapped_stack_exit", NULL
	};
	test_int_eq(run_with(args), 42);
	rh_rmrf(FAKE_ROOTFS);
}

test(prod_long_path_over_1024_still_translates)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	char guest_path[PATH_MAX];
	char host_path[PATH_MAX];
	size_t gl = 0;
	size_t hl = 0;

	int n = snprintf(guest_path, sizeof guest_path, "/long");
	test_true(n > 0 && (size_t)n < sizeof guest_path);
	gl = (size_t)n;
	n = snprintf(host_path, sizeof host_path, "%s/long", FAKE_ROOTFS);
	test_true(n > 0 && (size_t)n < sizeof host_path);
	hl = (size_t)n;
	test_true(rh_mkdir_p(host_path, 0755));

	for (int i = 0; i < 24; i++) {
		char comp[48];
		n = snprintf(comp, sizeof comp,
		             "/segment%02d-abcdefghijklmnopqrstuvwxyz0123456789", i);
		test_true(n > 0 && (size_t)n < sizeof comp);
		size_t cl = (size_t)n;
		test_true(gl + cl + 1 < sizeof guest_path);
		memcpy(guest_path + gl, comp, cl + 1);
		gl += cl;
		test_true(hl + cl + 1 < sizeof host_path);
		memcpy(host_path + hl, comp, cl + 1);
		hl += cl;
		test_true(rh_mkdir_p(host_path, 0755));
	}

	const char *leaf = "/leaf";
	size_t leaf_len = strlen(leaf);
	test_true(gl + leaf_len + 1 < sizeof guest_path);
	memcpy(guest_path + gl, leaf, leaf_len + 1);
	test_true(hl + leaf_len + 1 < sizeof host_path);
	memcpy(host_path + hl, leaf, leaf_len + 1);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_open_creat_argv1", guest_path, NULL
	};
	test_true(strlen(guest_path) > 1024);
	test_int_eq(run_with(args), 0);
	test_int_eq(access(host_path, F_OK), 0);

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

/* open(O_CREAT) on an existing absolute symlink leaf must follow the
 * link INSIDE the rootfs view. The /etc/resolv.conf → /run/resolv.conf
 * shape is the canonical real-world case. Pre-fix, the O_CREAT leaf
 * reached the host kernel un-resolved and the absolute target was
 * chased against the HOST root (write landing outside the view, or
 * bogus ENOENT/EACCES). */
test(prod_open_creat_symlink_leaf_stays_in_view)
{
	rh_rmrf(FAKE_ROOTFS);
	test_true(build_rootfs());

	char p[PATH_MAX], lnk[PATH_MAX];
	snprintf(p, sizeof p, "%s/etc", FAKE_ROOTFS);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(lnk, sizeof lnk, "%s/etc/resolv.conf", FAKE_ROOTFS);
	(void)unlink(lnk);
	test_int_eq(symlink("/run/resolv.conf", lnk), 0);

	const char *args[] = {
		"-r", FAKE_ROOTFS, "--",
		"/bin/static_open_creat_argv1", "/etc/resolv.conf", NULL
	};
	test_int_eq(run_with(args), 0);

	/* The write must land at <rootfs>/run/resolv.conf. */
	snprintf(p, sizeof p, "%s/run/resolv.conf", FAKE_ROOTFS);
	test_int_eq(access(p, F_OK), 0);

	rh_rmrf(FAKE_ROOTFS);
}

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
