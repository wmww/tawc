/* See hosted.h. No test() blocks here — pure harness, compiled into
 * the orchestrator alongside the test files. */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"

#include "dispatch.h"
#include "fdtab.h"
#include "linkstore.h"
#include "path.h"
#include "shm.h"
#include "syscalls_socket.h"
#include "usercopy.h"

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

static int count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d) return -1;
	int n = 0;
	while (readdir(d)) n++;
	closedir(d);
	return n;
}

void th_view_setup_impl(TestCtx *test_ctx, th_view *v, const char *tag)
{
	memset(v, 0, sizeof *v);
	v->fds_before = count_fds();
	test_nonnull(getcwd(v->saved_cwd, sizeof v->saved_cwd));

	/* Leftover global view state means a previous test's teardown was
	 * skipped (or two setups nested) — fail loudly instead of letting
	 * binds/reserved fds accumulate across tests. */
	test_int_eq((long)tawcroot_n_binds, 0);
	test_int_eq((long)tawcroot_n_reserved_fds, 0);
	test_int_eq(tawcroot_rootfs_fd, -1);

	snprintf(v->root, sizeof v->root, "%s/tawcroot-hosted-%s-%d",
		 TAWCROOT_TEST_TMPDIR, tag, getpid());
	rh_rmrf(v->root);

	char p[4200];
	snprintf(p, sizeof p, "%s/etc/sub", v->root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/usr/lib", v->root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/run", v->root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/tmp", v->root);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/etc/probe", v->root);
	test_true(rh_write_text(p, "from-rootfs\n"));
	snprintf(p, sizeof p, "%s/usr/lib/probe.so", v->root);
	test_true(rh_write_text(p, "fake-lib\n"));
	snprintf(p, sizeof p, "%s/lib", v->root);
	test_int_eq(symlink("usr/lib", p), 0);

	/* supervisor_init steps 1-3 + 6, hosted edition. */
	long rfd = open(v->root, O_PATH | O_DIRECTORY | O_CLOEXEC);
	test_true(rfd >= 0);
	long resv = tawcroot_fd_reserve((int)rfd);
	test_true(resv >= TAWCROOT_RESERVED_FD_BASE);
	tawcroot_rootfs_fd = (int)resv;

	long rl = tawcroot_proc_fd_to_host_path(tawcroot_rootfs_fd,
						tawcroot_rootfs_host_path,
						sizeof tawcroot_rootfs_host_path);
	test_true(rl > 0);
	tawcroot_rootfs_host_path_len = (size_t)rl;

	long up = tawc_usercopy_init();
	if (up < 0) {
		/* Without working process_vm_* every guarded copy EFAULTs and
		 * all hosted tests fail confusingly. -EPERM on a self-targeted
		 * process_vm_readv means a seccomp errno filter (container
		 * sandbox without CAP_SYS_PTRACE); abort the run with one
		 * clear message instead. */
		fprintf(stderr, "hosted: usercopy probe rv=%ld — host blocks "
			"process_vm_readv on self (sandbox seccomp?); "
			"hosted tests cannot run here\n", up);
		exit(70);
	}
	tawcroot_path_memoize_well_known();
	tawcroot_dispatch_init();
}

const char *th_view_add_bind_impl(TestCtx *test_ctx, th_view *v,
				  const char *dst, int ro)
{
	static char src[4200];
	snprintf(src, sizeof src, "%s-bind-%zu", v->root, tawcroot_n_binds);
	test_true(rh_mkdir_p(src, 0755));
	char p[4300];
	snprintf(p, sizeof p, "%s/probe.txt", src);
	test_true(rh_write_text(p, "from-bind\n"));
	test_int_eq(tawcroot_path_add_bind(src, dst, ro), 0);
	return src;
}

void th_view_teardown_impl(TestCtx *test_ctx, th_view *v)
{
	tawcroot_test_raw_hook = NULL;

	/* Snapshot the bind count before clearing so the bind src trees
	 * can be removed below. Close every fd the view reserved (rootfs +
	 * binds + anything a handler reserved mid-test, e.g. a chroot swap
	 * or shm fd). */
	size_t n_binds = tawcroot_n_binds;
	for (size_t i = 0; i < tawcroot_n_reserved_fds; i++)
		close(tawcroot_reserved_fds[i]);
	tawcroot_n_reserved_fds = 0;
	tawcroot_n_binds = 0;
	tawcroot_rootfs_fd = -1;
	tawcroot_root_ro = 0;
	tawcroot_rootfs_host_path[0] = 0;
	tawcroot_rootfs_host_path_len = 0;
	tawcroot_set_guest_exe_path(NULL);
	tawcroot_shm_reset();
	/* Tier-3 socket parent fds were closed by the reserved loop
	 * above; drop the table entries that pointed at them. */
	tawcroot_socket_reset();
	/* Forget any linkstore state (fds were closed by the reserved
	 * loop above). Tests that opened a store also rm -rf their store
	 * dir themselves. */
	tawcroot_linkstore_configure(NULL);

	test_int_eq(chdir(v->saved_cwd), 0);

	rh_rmrf(v->root);
	for (size_t i = 0; i < n_binds; i++) {
		char src[4300];
		snprintf(src, sizeof src, "%s-bind-%zu", v->root, i);
		rh_rmrf(src);
	}

	/* fd-leak check: the fd table is the product's main leakable
	 * resource (production code has no heap) and neither sanitizer
	 * tracks it. Anything a handler opened and the test didn't close
	 * shows up here. */
	test_int_eq(count_fds(), v->fds_before);
}

long th_sys_impl(TestCtx *test_ctx, long nr, long a, long b, long c,
		 long d, long e, long f)
{
	tawcroot_handler_fn fn = tawcroot_dispatch_get((int)nr);
	test_nonnull(fn);
	tawcroot_syscall_args args = {
		.nr = nr, .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
	};
	return fn(&args, NULL);
}
