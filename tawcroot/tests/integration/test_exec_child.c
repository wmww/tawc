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

#define _GNU_SOURCE
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
 * Mirrors the gpgme pattern: fork, in child fork grandchild and have
 * the child exit immediately; grandchild waits for `getppid() == 1`
 * (kernel reparented it to init) and then exec's tawcroot
 * `--exec-child` of `/usr/bin/touch <marker_path>`. We can't waitpid
 * the orphan (we're not its parent and pidfd-waitid still requires
 * parentage), so the test inverts: we run a guest that produces a
 * filesystem side-effect, then poll for the marker. If the marker
 * appears, the orphan-check was correctly skipped and the guest ran.
 * If the marker never appears, the buggy unconditional check killed
 * tawcroot before it could exec the guest. */
static int run_exec_child_orphaned_touching(int state_fd)
{
	pid_t child = fork();
	if (child < 0) return -1;
	if (child == 0) {
		pid_t gc = fork();
		if (gc < 0) _exit(127);
		if (gc == 0) {
			/* Spin until the kernel reparents us to init. The
			 * intermediate (our parent) exits right after writing
			 * to the pipe; in practice this loop terminates within
			 * a few ms. Bound with a hard cap so a test-runner
			 * subreaper that catches the orphan first can't hang. */
			int spins = 0;
			while (getppid() != 1) {
				struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
				nanosleep(&ts, NULL);
				if (++spins > 5000) _exit(125);
			}
			char fdstr[32];
			snprintf(fdstr, sizeof fdstr, "%d", state_fd);
			execl(TAWCROOT_PROD_BIN, "tawcroot",
			      "--exec-child", fdstr, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	/* Reap the intermediate so its grandchild orphans onto init.
	 * The grandchild itself isn't ours to reap; it lives until init
	 * sweeps the zombie. */
	waitpid(child, NULL, 0);
	return 0;
}

test(exec_child_runs_guest_when_orphaned_to_init)
{
	char marker[256];
	snprintf(marker, sizeof marker,
	         "/tmp/tawc-test-orphan-marker-%d-%ld",
	         (int)getpid(), (long)time(NULL));
	unlink(marker);

	const char *argv[] = { TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN, marker, NULL };
	const char *envp[] = { "PATH=/bin", NULL };

	int fd = build_state_memfd(TAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN,
	                           2, argv, envp);
	test_true(fd >= 0);

	test_int_eq(run_exec_child_orphaned_touching(fd), 0);

	/* Poll for the marker. The grandchild is detached so we can't
	 * waitpid; budget a few seconds. With the bug, the marker never
	 * appears (tawcroot exits 0 via exit_if_orphan before reaching
	 * the loader); with the fix, the fixture runs and creates it. */
	int found = 0;
	for (int i = 0; i < 100; i++) {
		if (access(marker, F_OK) == 0) { found = 1; break; }
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 };
		nanosleep(&ts, NULL);
	}
	test_true(found);

	unlink(marker);
	close(fd);
}
