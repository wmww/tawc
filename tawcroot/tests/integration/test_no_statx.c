/* Force the kernel side of statx to ENOSYS across a real guest exec.
 * The inherited filter also covers tawcroot's allowlisted raw stub. */
#include <cleat/test.h>

#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rootfs_helpers.h"
#include "sysnr.h"

test(prod_exec_without_kernel_statx)
{
	char root[] = TAWCROOT_TEST_TMPDIR "/tawcroot-no-statx-XXXXXX";
	test_nonnull(mkdtemp(root));
	char path[4096];
	snprintf(path, sizeof path, "%s/bin", root);
	test_true(rh_mkdir_p(path, 0755));
	snprintf(path, sizeof path, "%s/bin/static_exit42", root);
	test_true(rh_copy_file(TAWCROOT_STATIC_EXIT42_BIN, path, 0755));
	snprintf(path, sizeof path, "%s/bin/start", root);
	test_true(rh_copy_file(TAWCROOT_STATIC_EXECVE_EXIT42_BIN, path, 0755));

	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		struct sock_filter code[] = {
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			         offsetof(struct seccomp_data, nr)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TAWC_SYS_statx, 0, 1),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog prog = {
			.len = sizeof code / sizeof code[0], .filter = code,
		};
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
		    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
			_exit(120);
		execl(TAWCROOT_PROD_BIN, "tawcroot", "-r", root, "--",
		      "/bin/start", (char *)NULL);
		_exit(121);
	}
	int status;
	pid_t waited;
	do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
	rh_rmrf(root);
	test_true(waited == pid);
	test_true(WIFEXITED(status));
	test_int_eq(WEXITSTATUS(status), 42);
}
