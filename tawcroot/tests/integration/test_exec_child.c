/* Integration test for `tawcroot --exec-child <fd>`.
 *
 * Mirrors the back half of the SIGSYS handler's re-exec dance: we
 * (the test harness) play the role the handler will play in the full
 * phase-2.6 flow:
 *
 *   1. memfd_create (non-CLOEXEC)
 *   2. write a serialized exec_state describing the guest binary
 *   3. fork
 *   4. in child: exec the production tawcroot with --exec-child <fd>
 *      (the fd survives execve because !CLOEXEC)
 *   5. in parent: waitpid, assert exit code
 *
 * If this passes, the back half of the dance is correct. The front
 * half (handler-side execve interception, register massaging,
 * triggering the re-exec via execveat) lands separately.
 */

#include <cleat/test.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "exec_state.h"
#include "rootfs_helpers.h"

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_EXIT42_BIN
# error "TAWCROOT_DYNAMIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN
# error "TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_SLEEP_OPEN_CREAT_ARGV1_BIN
# error "TAWCROOT_STATIC_SLEEP_OPEN_CREAT_ARGV1_BIN must be defined"
#endif
#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

/* memfd_create wrapper — glibc has it but pinning to syscall keeps
 * us robust against weird libc configurations. */
static int memfd_create_wrap(const char *name, unsigned flags)
{
	long r = syscall(SYS_memfd_create, name, flags);
	return (int)r;
}

/* Build an exec_state in a memfd and return the open fd (non-CLOEXEC
 * so it survives the upcoming exec). */
static int build_state_memfd(const char *guest_path,
                             int argc, const char *const *argv,
                             const char *const *envp)
{
	int fd = memfd_create_wrap("tawc-exec-state", 0 /* not MFD_CLOEXEC */);
	if (fd < 0) return -1;

	size_t need = tawcroot_exec_state_estimate_bytes(guest_path, argc,
	                                                 argv, envp, NULL);
	uint8_t *buf = malloc(need);
	if (!buf) { close(fd); return -1; }
	long w = tawcroot_exec_state_write(buf, need, guest_path, argc, argv, envp, NULL);
	if (w < 0) { free(buf); close(fd); return -1; }
	if (write(fd, buf, (size_t)w) != w) { free(buf); close(fd); return -1; }
	free(buf);
	if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
	return fd;
}

static int decode_status(int status)
{
	if (WIFEXITED(status))   return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return -WTERMSIG(status);
	return -999;
}

/* Run tawcroot --exec-child <fd> in a forked child, with the memfd
 * preserved across exec. Returns the wait-status decoded by
 * decode_status(). */
static int run_exec_child(int state_fd)
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char fdstr[32];
		snprintf(fdstr, sizeof fdstr, "%d", state_fd);
		execl(TAWCROOT_PROD_BIN, "tawcroot", "--exec-child", fdstr,
		      (char *)NULL);
		_exit(127); /* exec failed */
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) return -1;
	return decode_status(status);
}

test(exec_child_runs_static_exit42)
{
	const char *argv[] = { TAWCROOT_STATIC_EXIT42_BIN, NULL };
	const char *envp[] = { "PATH=/bin", NULL };

	int fd = build_state_memfd(TAWCROOT_STATIC_EXIT42_BIN, 1, argv, envp);
	test_true(fd >= 0);

	test_int_eq(run_exec_child(fd), 42);
	close(fd);
}

test(exec_child_runs_dynamic_exit42)
{
	const char *argv[] = { TAWCROOT_DYNAMIC_EXIT42_BIN, NULL };
	const char *envp[] = { "PATH=/bin", NULL };

	int fd = build_state_memfd(TAWCROOT_DYNAMIC_EXIT42_BIN, 1, argv, envp);
	test_true(fd >= 0);

	test_int_eq(run_exec_child(fd), 42);
	close(fd);
}

test(exec_child_runs_system_bin_true)
{
	if (access("/bin/true", X_OK) != 0) return;
	const char *argv[] = { "/bin/true", NULL };
	extern char **environ;
	int n = 0; while (environ[n]) n++;
	const char *envp[64];
	int copied = n < 63 ? n : 63;
	for (int i = 0; i < copied; i++) envp[i] = environ[i];
	envp[copied] = NULL;

	int fd = build_state_memfd("/bin/true", 1, argv, envp);
	test_true(fd >= 0);
	test_int_eq(run_exec_child(fd), 0);
	close(fd);
}

test(exec_child_bad_fd_fails_clean)
{
	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		execl(TAWCROOT_PROD_BIN, "tawcroot", "--exec-child", "999",
		      (char *)NULL);
		_exit(127);
	}
	int status = 0;
	test_int_eq((int)waitpid(pid, &status, 0), (int)pid);
	/* fd 999 isn't open: lseek returns -EBADF, exec_child reports 80 */
	test_int_eq(decode_status(status), 80);
}

test(exec_child_bad_fd_arg_prints_usage)
{
	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		execl(TAWCROOT_PROD_BIN, "tawcroot", "--exec-child", "abc",
		      (char *)NULL);
		_exit(127);
	}
	int status = 0;
	test_int_eq((int)waitpid(pid, &status, 0), (int)pid);
	test_int_eq(decode_status(status), 2);
}

test(exec_child_too_small_state_fd_rejected)
{
	/* A memfd smaller than the header → loader_exec_child's
	 * lseek-derived size check catches it as code 80. */
	int fd = memfd_create_wrap("garbage", 0);
	test_true(fd >= 0);
	const char garbage[] = "tiny";
	test_int_eq((int)write(fd, garbage, sizeof garbage), (int)sizeof garbage);
	lseek(fd, 0, SEEK_SET);
	test_int_eq(run_exec_child(fd), 80);
	close(fd);
}

test(exec_child_corrupt_magic_rejected)
{
	/* A buffer that's at least header-sized but has the wrong magic.
	 * lseek check passes; exec_state_read fails magic check → code 82. */
	int fd = memfd_create_wrap("corrupt", 0);
	test_true(fd >= 0);
	tawcroot_exec_state_header h;
	memset(&h, 0xff, sizeof h);
	h.magic = 0xdeadbeef;
	h.string_bytes = 0;
	test_int_eq((int)write(fd, &h, sizeof h), (int)sizeof h);
	lseek(fd, 0, SEEK_SET);
	test_int_eq(run_exec_child(fd), 82);
	close(fd);
}

/* The `--exec-child` re-exec must NOT exit early when its parent is
 * init (`getppid() == 1`). This is the gpgme / libgpg-error
 * `posix_spawn` regression: that posix_spawn forks an intermediate
 * that exits immediately, so by the time the actually-spawned worker
 * (running tawcroot's exec interception) re-execs into `--exec-child`,
 * its parent is init. The earlier `bind_to_parent`'s `if (getppid()
 * == 1) exit_group(0)` orphan-check killed every such worker before
 * it could write its first byte, surfacing as `GPGME error: Invalid
 * crypto engine` and `missing required signature` on every Arch
 * package. The fix splits that check off and skips it for
 * `--exec-child`; this test guards against the regression.
 *
 * Mirrors the gpgme pattern: fork an intermediate; intermediate forks
 * a grandchild and immediately _exits. The grandchild waits for the
 * intermediate to exit (using pipe-EOF as the synchronization edge:
 * intermediate holds the write end, grandchild reads — read returns
 * 0 only after all writers close, which is when intermediate exits),
 * then exec's tawcroot `--exec-child` of a side-effect-bearing
 * fixture. We can't waitpid the orphan (we're not its parent, and
 * pidfd-waitid still requires parentage), so the test inverts: poll
 * for the file the fixture creates. Marker appears → orphan-check
 * was correctly skipped and the guest ran; marker absent → the buggy
 * unconditional check killed tawcroot before it reached the loader. */
/* Status-pipe tags written by the grandchild so the test parent can
 * tell *why* the marker is missing on failure. We can't waitpid the
 * orphan, but a non-CLOEXEC pipe whose write end the grandchild
 * holds DOES survive across the execveat — when tawcroot eventually
 * exits, the pipe closes and the test parent reads EOF. Any tag we
 * wrote first is then visible.
 *
 *   ORPHAN_TAG_GC_RAN   — grandchild reached the spin past the orphan barrier
 *   ORPHAN_TAG_PPID1    — getppid() == 1 confirmed before exec
 *   ORPHAN_TAG_SPIN_TO  — spin-loop timed out (kernel reparenting stalled)
 *   ORPHAN_TAG_FORK_ERR — second fork failed
 *   ORPHAN_TAG_EXEC_ERR — execl returned (path or perms); errno follows
 */
#define ORPHAN_TAG_GC_RAN   'g'
#define ORPHAN_TAG_PPID1    'p'
#define ORPHAN_TAG_SPIN_TO  's'
#define ORPHAN_TAG_FORK_ERR 'f'
#define ORPHAN_TAG_EXEC_ERR 'x'

static void orphan_tag_write(int fd, char tag)
{
	(void)write(fd, &tag, 1);
}

static int run_exec_child_orphaned_touching(int state_fd, int diag_fd)
{
	/* pipefd is the orphan-barrier. Intermediate inherits both ends
	 * and closes neither — so when intermediate exits, the kernel
	 * closes intermediate's write-end copy, and the grandchild's
	 * blocked read returns EOF. O_CLOEXEC keeps it from leaking
	 * across the upcoming execveat into tawcroot. */
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;

	pid_t intermediate = fork();
	if (intermediate < 0) {
		close(pipefd[0]); close(pipefd[1]);
		return -1;
	}
	if (intermediate == 0) {
		pid_t gc = fork();
		if (gc < 0) {
			orphan_tag_write(diag_fd, ORPHAN_TAG_FORK_ERR);
			_exit(127);
		}
		if (gc == 0) {
			/* Grandchild: close write end so only intermediate
			 * holds it; block on read until intermediate exits. */
			close(pipefd[1]);
			char b;
			(void)read(pipefd[0], &b, 1); /* expect EOF (rc=0) */
			close(pipefd[0]);
			orphan_tag_write(diag_fd, ORPHAN_TAG_GC_RAN);

			/* Linux closes fds (waking the read) before exit_notify
			 * actually reparents children, so there's a microscopic
			 * window where read returned but ppid is still the
			 * intermediate's tid. Tight bound on the spin: the gap
			 * is hundreds of nanoseconds in practice. */
			int spins = 0;
			while (getppid() != 1) {
				struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
				nanosleep(&ts, NULL);
				if (++spins > 100) {
					orphan_tag_write(diag_fd, ORPHAN_TAG_SPIN_TO);
					_exit(125); /* ~100ms cap */
				}
			}
			orphan_tag_write(diag_fd, ORPHAN_TAG_PPID1);

			char fdstr[32];
			snprintf(fdstr, sizeof fdstr, "%d", state_fd);
			execl(TAWCROOT_PROD_BIN, "tawcroot",
			      "--exec-child", fdstr, (char *)NULL);
			orphan_tag_write(diag_fd, ORPHAN_TAG_EXEC_ERR);
			_exit(127);
		}
		_exit(0);
	}
	/* Test-parent: close pipe ends so we don't pin the read open;
	 * reap the intermediate. The grandchild is detached and lives
	 * until init sweeps its zombie. */
	close(pipefd[0]);
	close(pipefd[1]);
	int wstatus = 0;
	pid_t reaped = waitpid(intermediate, &wstatus, 0);
	if (reaped != intermediate) return -2;
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) return -3;
	return 0;
}

test(exec_child_runs_guest_when_orphaned_to_init)
{
	char marker[256];
	snprintf(marker, sizeof marker,
	         TAWCROOT_TEST_TMPDIR "/tawc-test-orphan-marker-%d-%ld",
	         (int)getpid(), (long)time(NULL));
	unlink(marker);

	const char *argv[] = { TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN, marker, NULL };
	const char *envp[] = { "PATH=/bin", NULL };

	int fd = build_state_memfd(TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN,
	                           2, argv, envp);
	test_true(fd >= 0);

	/* Diagnostic pipe — non-CLOEXEC so it survives across the
	 * orphan grandchild's execveat into tawcroot. We never write
	 * to the read end ourselves; only the grandchild writes, then
	 * (on success) tawcroot inherits the open fd, and the close-
	 * on-exit at guest termination releases our final reader EOF. */
	int diag[2];
	test_int_eq(pipe2(diag, 0 /* deliberately not CLOEXEC */), 0);

	test_int_eq(run_exec_child_orphaned_touching(fd, diag[1]), 0);

	/* Close our copy of the write end so the only writers are the
	 * (eventually-execve'd-into-tawcroot) grandchild's inherited fd
	 * and any of its further descendants. Once they all exit/close,
	 * a read on diag[0] returns 0 (EOF) and we know nothing more
	 * will write. We don't need that for the success path (marker
	 * appears) but it bounds the diagnostic read on failure. */
	close(diag[1]);

	/* Poll for the marker with generous headroom. Slow Android arm
	 * cores plus tawcroot's full bring-up (memfd read, signal-shadow
	 * init, openat2 probe, loader trampoline) can take a beat under
	 * load. With the bug, the marker never appears (tawcroot exits 0
	 * via exit_if_orphan before reaching the loader); with the fix,
	 * the fixture runs and creates it. */
	bool found = rh_poll_for_path(marker, 200);  /* 10 s budget */
	if (!found) {
		/* Drain the diagnostic pipe so the failure log shows where
		 * we stopped. The most recent tag is the most informative.
		 * Non-blocking read with a small bound — the pipe is at
		 * most a handful of bytes deep. */
		char tags[64] = {0};
		ssize_t n = read(diag[0], tags, sizeof tags - 1);
		fprintf(stderr,
		        "exec_child orphan marker missing; diag tags=\"%.*s\"\n",
		        (int)(n > 0 ? n : 0), tags);
	}
	test_true(found);

	unlink(marker);
	close(diag[0]);
	close(fd);
}

/* The `--exec-child` re-exec must NOT call prctl(PR_SET_PDEATHSIG,
 * SIGKILL). This is the *other* half of the gpgme posix_spawn
 * regression: an earlier fix split bind_to_parent into arm_pdeathsig
 * + exit_if_orphan and scoped only the latter to top-level entries,
 * leaving arm_pdeathsig running on every entry. The orphan-to-init
 * test above (which mirrors the gpgme dance with intermediate exiting
 * BEFORE grandchild reaches arm_pdeathsig) doesn't catch this — by
 * the time prctl runs in that ordering, the parent is init, and
 * pdeath_signal=SIGKILL bound to init is a no-op forever.
 *
 * The bug fires under the *opposite* ordering: intermediate stays
 * alive until grandchild has already armed pdeath_signal, then exits.
 * The kernel's `forget_original_parent` walk during intermediate's
 * `do_exit` (kernel/exit.c) checks each child's pdeath_signal and
 * sends the signal — so the grandchild gets SIGKILL'd mid-execution.
 * In production this fired during a 150-pkg pacman -Sw on roughly
 * 1 % of gpg verifies, surfacing as "missing required signature".
 *
 * Test sequence:
 *   1. parent forks intermediate
 *   2. intermediate forks grandchild
 *   3. grandchild execve's tawcroot --exec-child of the
 *      static_sleep_open_creat_argv1 fixture (immediately — no sync
 *      with intermediate's exit)
 *   4. fixture starts: hits its 1 s nanosleep
 *   5. intermediate sleeps 100 ms (gives grandchild time to bootstrap
 *      through tawcroot --exec-child and reach the fixture's sleep —
 *      the post-execveat path is microseconds; 100 ms is generous),
 *      then _exit(0)
 *   6. intermediate's do_exit walks its children: grandchild's
 *      pdeath_signal is SIGKILL with the bug, 0 with the fix
 *      - bug: grandchild SIGKILL'd mid-nanosleep; never reaches
 *        the open(); marker absent
 *      - fix: pdeath_signal stays 0; nanosleep returns; open creates
 *        the marker
 *   7. parent reaps intermediate, polls for marker with 5 s budget
 *
 * This is timing-dependent in the *bug* case (we need intermediate to
 * exit AFTER grandchild armed its pdeath_signal). 100 ms is many
 * orders of magnitude longer than the post-execveat → tawcroot_main
 * → arm_pdeathsig path takes (microseconds), so the ordering is
 * reliable. With the fix, the test is deterministic — pdeath_signal
 * is never armed regardless of timing. */
static int run_pdeathsig_intermediate(int state_fd, int diag_fd)
{
	pid_t intermediate = fork();
	if (intermediate < 0) return -1;
	if (intermediate == 0) {
		pid_t gc = fork();
		if (gc < 0) {
			orphan_tag_write(diag_fd, ORPHAN_TAG_FORK_ERR);
			_exit(127);
		}
		if (gc == 0) {
			/* Grandchild: immediately execve tawcroot --exec-child
			 * (no waiting on intermediate). The fixture's nanosleep
			 * is what holds us alive while intermediate exits. */
			char fdstr[32];
			snprintf(fdstr, sizeof fdstr, "%d", state_fd);
			execl(TAWCROOT_PROD_BIN, "tawcroot",
			      "--exec-child", fdstr, (char *)NULL);
			orphan_tag_write(diag_fd, ORPHAN_TAG_EXEC_ERR);
			_exit(127);
		}
		/* Intermediate: sleep enough for grandchild to be deep
		 * inside the fixture's nanosleep, then exit. 100 ms covers
		 * the supervisor bring-up + loader_jump + start of sleep. */
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000 };
		nanosleep(&ts, NULL);
		_exit(0);
	}
	int wstatus = 0;
	pid_t reaped = waitpid(intermediate, &wstatus, 0);
	if (reaped != intermediate) return -2;
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) return -3;
	return 0;
}

test(exec_child_survives_pdeathsig_after_intermediate_exit)
{
	char marker[256];
	snprintf(marker, sizeof marker,
	         TAWCROOT_TEST_TMPDIR "/tawc-test-pdeathsig-marker-%d-%ld",
	         (int)getpid(), (long)time(NULL));
	unlink(marker);

	const char *argv[] = { TAWCROOT_STATIC_SLEEP_OPEN_CREAT_ARGV1_BIN,
	                       marker, NULL };
	const char *envp[] = { "PATH=/bin", NULL };

	int fd = build_state_memfd(TAWCROOT_STATIC_SLEEP_OPEN_CREAT_ARGV1_BIN,
	                           2, argv, envp);
	test_true(fd >= 0);

	/* Diagnostic pipe — same plumbing as the orphan test. */
	int diag[2];
	test_int_eq(pipe2(diag, 0 /* not CLOEXEC */), 0);

	test_int_eq(run_pdeathsig_intermediate(fd, diag[1]), 0);
	close(diag[1]);

	/* Marker appears at t=~1 s after grandchild's loader_jump under the
	 * fix. Budget 5 s for slow Android cores + startup overhead. With
	 * the bug, the fixture is SIGKILL'd at t=~100 ms (when intermediate
	 * exits) and the marker never appears. */
	bool found = rh_poll_for_path(marker, 100);  /* 5 s budget */
	if (!found) {
		char tags[64] = {0};
		ssize_t n = read(diag[0], tags, sizeof tags - 1);
		fprintf(stderr,
		        "exec_child pdeathsig marker missing; diag tags=\"%.*s\"\n",
		        (int)(n > 0 ? n : 0), tags);
	}
	test_true(found);

	unlink(marker);
	close(diag[0]);
	close(fd);
}
