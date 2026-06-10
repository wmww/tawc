/* Hosted handler-level tests for syscalls_fd.c — reserved-fd
 * protection (close/dup/fcntl lies and rejections) and the getdents64
 * /proc/self/fd filter against REAL kernel getdents64 output. Runs
 * in-process under ASan; see hosted.h.
 *
 * The raw-syscall hook doubles as an observation point here: the
 * close_range trim test must not let a real close_range(0, …) reach
 * the kernel (it would shred the test binary's own fd table), so the
 * hook captures the handler's outgoing call instead. */

#include <cleat/test.h>

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hosted.h"

#include "errno_neg.h"
#include "fdtab.h"
#include "path.h"
#include "sysnr.h"

/* --- reserved-fd EBADF contract ---------------------------------------- */

test(hosted_reserved_fd_close_lies_success_and_fd_survives)
{
	th_view v;
	th_setup(&v, "fd-close");

	int rfd = tawcroot_rootfs_fd;
	test_int_eq(th_sys(TAWC_SYS_close, rfd, 0, 0, 0, 0, 0), 0);

	/* The fd must still be open and usable by the translator. */
	struct stat st;
	test_int_eq(fstat(rfd, &st), 0);
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_close_of_guest_fd_passes_through)
{
	th_view v;
	th_setup(&v, "fd-close2");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(th_sys(TAWC_SYS_close, fd, 0, 0, 0, 0, 0), 0);
	struct stat st;
	test_int_eq(fstat((int)fd, &st), -1);  /* really closed */

	th_teardown(&v);
}

test(hosted_reserved_fd_dup_family_ebadf)
{
	th_view v;
	th_setup(&v, "fd-dup");

	int rfd = tawcroot_rootfs_fd;
	test_int_eq(th_sys(TAWC_SYS_dup, rfd, 0, 0, 0, 0, 0), TAWC_EBADF);
	test_int_eq(th_sys(TAWC_SYS_dup3, rfd, 5, 0, 0, 0, 0), TAWC_EBADF);
	test_int_eq(th_sys(TAWC_SYS_fcntl, rfd, F_GETFD, 0, 0, 0, 0),
		    TAWC_EBADF);
	test_int_eq(th_sys(TAWC_SYS_fchdir, rfd, 0, 0, 0, 0, 0), TAWC_EBADF);

	/* dup3 with a guest oldfd but a newfd inside the reserved range:
	 * the whole range is rejected, not just live slots. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(th_sys(TAWC_SYS_dup3, fd, TAWCROOT_RESERVED_FD_BASE + 7,
			   0, 0, 0, 0), TAWC_EBADF);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_fcntl_dupfd_into_reserved_range_einval)
{
	th_view v;
	th_setup(&v, "fd-dupfd");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	test_int_eq(th_sys(TAWC_SYS_fcntl, fd, F_DUPFD,
			   TAWCROOT_RESERVED_FD_BASE, 0, 0, 0), TAWC_EINVAL);
	test_int_eq(th_sys(TAWC_SYS_fcntl, fd, F_DUPFD_CLOEXEC,
			   TAWCROOT_RESERVED_FD_BASE + 50, 0, 0, 0),
		    TAWC_EINVAL);

	/* Below the boundary it passes through. */
	long dup_fd = th_sys(TAWC_SYS_fcntl, fd, F_DUPFD, 0, 0, 0, 0);
	test_true(dup_fd >= 0 && dup_fd < TAWCROOT_RESERVED_FD_BASE);
	test_int_eq(close((int)dup_fd), 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

/* --- close_range: observed via the hook, never executed ----------------- */

static long cr_seen_nr;
static long cr_seen_args[6];
static bool cr_hook(long nr, const long args[6], long *ret)
{
	if (nr != TAWC_SYS_close_range) return false;
	cr_seen_nr = nr;
	memcpy(cr_seen_args, args, sizeof cr_seen_args);
	*ret = 0;
	return true;
}

test(hosted_close_range_trims_at_reserved_boundary)
{
	th_view v;
	th_setup(&v, "fd-crange");

	/* Entirely above the boundary: success no-op, no kernel call. */
	cr_seen_nr = 0;
	tawcroot_test_raw_hook = cr_hook;
	test_int_eq(th_sys(TAWC_SYS_close_range,
			   TAWCROOT_RESERVED_FD_BASE, ~0U, 0, 0, 0, 0), 0);
	test_int_eq(cr_seen_nr, 0);  /* handler never issued the syscall */

	/* Range crossing the boundary: `last` must be trimmed to
	 * RESERVED_FD_BASE - 1 before the kernel sees it. */
	test_int_eq(th_sys(TAWC_SYS_close_range, 700, ~0U, 0, 0, 0, 0), 0);
	test_int_eq(cr_seen_nr, TAWC_SYS_close_range);
	test_int_eq(cr_seen_args[0], 700);
	test_int_eq(cr_seen_args[1], TAWCROOT_RESERVED_FD_BASE - 1);

	tawcroot_test_raw_hook = NULL;
	th_teardown(&v);
}

/* --- getdents64 /proc/self/fd filter vs real kernel output -------------- */

test(hosted_getdents64_hides_reserved_fds_in_proc_self_fd)
{
	th_view v;
	th_setup(&v, "fd-dents");
	th_add_bind(&v, "/mnt/host");  /* a second reserved fd */

	int pfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	test_true(pfd >= 0);

	/* Drive the handler exactly as the guest's closefrom would. */
	char names[4096];
	size_t names_n = 0;
	unsigned char buf[512];  /* small buffer to force several batches */
	for (;;) {
		long n = th_sys(TAWC_SYS_getdents64, pfd, buf, sizeof buf,
				0, 0, 0);
		test_true(n >= 0);
		if (n == 0) break;
		long i = 0;
		while (i < n) {
			uint16_t reclen;
			memcpy(&reclen, buf + i + 16, 2);
			const char *name = (const char *)(buf + i + 19);
			size_t len = strlen(name);
			test_true(names_n + len + 2 < sizeof names);
			memcpy(names + names_n, name, len);
			names[names_n + len] = '\n';
			names_n += len + 1;
			i += reclen;
		}
	}
	names[names_n] = 0;

	/* Every reserved fd's number must be absent; the dir fd itself
	 * (a low guest-range fd) must be present. */
	for (size_t i = 0; i < tawcroot_n_reserved_fds; i++) {
		char needle[16];
		snprintf(needle, sizeof needle, "%d\n",
			 tawcroot_reserved_fds[i]);
		test_null(strstr(names, needle));
	}
	char self_needle[16];
	snprintf(self_needle, sizeof self_needle, "%d\n", pfd);
	test_nonnull(strstr(names, self_needle));

	test_int_eq(close(pfd), 0);
	th_teardown(&v);
}

test(hosted_getdents64_non_proc_dir_unfiltered)
{
	th_view v;
	th_setup(&v, "fd-dents2");

	/* A dir inside the rootfs containing a file literally named
	 * "1000" (the reserved base) must NOT have it hidden. */
	long mr = th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/run/d", 0755, 0, 0, 0);
	test_int_eq(mr, 0);
	long cfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/d/1000",
			  O_WRONLY | O_CREAT, 0644, 0, 0);
	test_true(cfd >= 0);
	test_int_eq(close((int)cfd), 0);

	long dfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/d",
			  O_RDONLY | O_DIRECTORY, 0, 0, 0);
	test_true(dfd >= 0);

	unsigned char buf[1024];
	bool saw_1000 = false;
	for (;;) {
		long n = th_sys(TAWC_SYS_getdents64, dfd, buf, sizeof buf,
				0, 0, 0);
		test_true(n >= 0);
		if (n == 0) break;
		long i = 0;
		while (i < n) {
			uint16_t reclen;
			memcpy(&reclen, buf + i + 16, 2);
			if (strcmp((const char *)(buf + i + 19), "1000") == 0)
				saw_1000 = true;
			i += reclen;
		}
	}
	test_true(saw_1000);

	test_int_eq(close((int)dfd), 0);
	th_teardown(&v);
}
