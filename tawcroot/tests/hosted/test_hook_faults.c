/* Hook-based fault injection — failure paths the fork-based layers
 * can't reach. tawcroot_test_raw_hook sits in front of every raw
 * syscall the production code under test issues, so a test can fail
 * exactly one kernel call mid-handler and check the error propagates
 * cleanly (and that no fd/state leaks on the way out — teardown's
 * fd-leak diff covers that for free). See hosted.h. */

#include <cleat/test.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hosted.h"

#include "errno_neg.h"
#include "fdtab.h"
#include "path.h"
#include "sysnr.h"

/* Fail every syscall `nr` with `err` while installed. */
static long fail_nr;
static long fail_err;
static int  fail_hits;
static bool fail_hook(long nr, const long args[6], long *ret)
{
	(void)args;
	if (nr != fail_nr) return false;
	fail_hits++;
	*ret = fail_err;
	return true;
}

static void install_fail(long nr, long err)
{
	fail_nr = nr;
	fail_err = err;
	fail_hits = 0;
	tawcroot_test_raw_hook = fail_hook;
}

test(hosted_inject_enospc_on_create)
{
	th_view v;
	th_setup(&v, "inj-enospc");

	install_fail(TAWC_SYS_openat, TAWC_ENOSPC);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/newfile",
			   O_WRONLY | O_CREAT, 0644, 0, 0), TAWC_ENOSPC);
	test_true(fail_hits > 0);
	tawcroot_test_raw_hook = NULL;

	/* Nothing half-created. */
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/newfile",
			   &st, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_inject_eintr_on_open_propagates)
{
	th_view v;
	th_setup(&v, "inj-eintr");

	/* EINTR from the final openat must reach the guest as EINTR (the
	 * guest's libc owns the retry, not the handler). */
	install_fail(TAWC_SYS_openat, TAWC_EINTR);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			   O_RDONLY, 0, 0, 0), TAWC_EINTR);
	test_true(fail_hits > 0);
	tawcroot_test_raw_hook = NULL;

	/* Hook removed: same call succeeds, view state intact. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_inject_emfile_on_dup_passthrough)
{
	th_view v;
	th_setup(&v, "inj-emfile");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	install_fail(TAWC_SYS_fcntl, TAWC_EMFILE);
	test_int_eq(th_sys(TAWC_SYS_fcntl, fd, F_DUPFD, 0, 0, 0, 0), TAWC_EMFILE);
	test_true(fail_hits > 0);
	tawcroot_test_raw_hook = NULL;

	test_int_eq(close((int)fd), 0);
	th_teardown(&v);
}

test(hosted_inject_chroot_reserve_failure_aborts_cleanly)
{
	th_view v;
	th_setup(&v, "inj-chroot");

	int old_root = tawcroot_rootfs_fd;
	size_t reserved_before = tawcroot_n_reserved_fds;

	/* Fail the F_DUPFD_CLOEXEC reservation inside handle_chroot:
	 * the operation must abort with the fcntl's errno, close the
	 * unreserved fd (teardown's fd-leak diff verifies), and leave
	 * the current view untouched. */
	install_fail(TAWC_SYS_fcntl, TAWC_EMFILE);
	long rv = th_sys(TAWC_SYS_chroot, "/usr", 0, 0, 0, 0, 0);
	tawcroot_test_raw_hook = NULL;
	test_int_eq(rv, TAWC_EMFILE);
	test_true(fail_hits > 0);

	test_int_eq(tawcroot_rootfs_fd, old_root);
	test_int_eq((long)tawcroot_n_reserved_fds, (long)reserved_before);
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

/* Observation: the translated openat the handler actually issues uses
 * the reserved rootfs dirfd + a rootfs-relative suffix — the guest's
 * absolute path never reaches the kernel. */
static long seen_dirfd = -1;
static char seen_path[256];
static bool observe_openat_hook(long nr, const long args[6], long *ret)
{
	(void)ret;
	if (nr != TAWC_SYS_openat) return false;
	seen_dirfd = args[0];
	const char *p = (const char *)args[1];
	if (p) {
		strncpy(seen_path, p, sizeof seen_path - 1);
		seen_path[sizeof seen_path - 1] = 0;
	}
	return false;  /* observe only; let the real syscall run */
}

test(hosted_observe_translated_openat_shape)
{
	th_view v;
	th_setup(&v, "inj-observe");

	seen_dirfd = -1;
	seen_path[0] = 0;
	tawcroot_test_raw_hook = observe_openat_hook;
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	tawcroot_test_raw_hook = NULL;
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	test_int_eq(seen_dirfd, tawcroot_rootfs_fd);
	test_str_eq(seen_path, "etc/probe");

	th_teardown(&v);
}
