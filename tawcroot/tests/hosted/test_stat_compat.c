/* Linux 4.9 regression: kernel statx is absent, including calls made
 * from inside the exec handler. Other syscalls hit the real kernel. */
#include <cleat/test.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "hosted.h"
#include "linkstore_fixture.h"
#include "errno_neg.h"
#include "exec_handler.h"
#include "stat_compat.h"
#include "sysnr.h"

#ifndef STATX_MNT_ID
# define STATX_MNT_ID 0x00001000U
#endif

static long statx_error;
static int statx_hits;
static int fstatat_hits;
static bool old_kernel_hook(long nr, const long args[6], long *ret)
{
	(void)args;
	if (nr == TAWC_SYS_fstatat) fstatat_hits++;
	if (nr == TAWC_SYS_statx) {
		statx_hits++;
		*ret = statx_error;
		return true;
	}
	/* Exercise Android's emulated hardlink path as well. */
	if (nr == TAWC_SYS_linkat) {
		*ret = TAWC_EPERM;
		return true;
	}
	return false;
}

static void without_statx(void)
{
	statx_error = TAWC_ENOSYS;
	statx_hits = fstatat_hits = 0;
	tawcroot_test_raw_hook = old_kernel_hook;
}

test(hosted_statx_compat_metadata)
{
	th_view v;
	th_setup(&v, "statx-compat");
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
	                 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	struct stat st;
	test_int_eq(fstat((int)fd, &st), 0);
	without_statx();
	struct statx sx;
	memset(&sx, 0xff, sizeof sx);
	test_int_eq(tawcroot_statx_compat((int)fd, "", AT_EMPTY_PATH,
	                                STATX_BASIC_STATS | STATX_BTIME |
	                                STATX_MNT_ID, &sx), 0);
	test_int_eq(statx_hits, 1);
	test_int_eq(fstatat_hits, 1);
	test_int_eq(sx.stx_mask, STATX_BASIC_STATS);
	test_true(sx.stx_ino == st.st_ino);
	test_true(sx.stx_size == (unsigned long long)st.st_size);
	test_true(sx.stx_blocks == (unsigned long long)st.st_blocks);
	test_int_eq(sx.stx_blksize, st.st_blksize);
	test_int_eq(sx.stx_mode, st.st_mode);
	test_int_eq(sx.stx_nlink, st.st_nlink);
	test_int_eq(sx.stx_uid, st.st_uid);
	test_int_eq(sx.stx_gid, st.st_gid);
	test_int_eq(sx.stx_dev_major, major(st.st_dev));
	test_int_eq(sx.stx_dev_minor, minor(st.st_dev));
	test_int_eq(sx.stx_atime.tv_sec, st.st_atim.tv_sec);
	test_int_eq(sx.stx_atime.tv_nsec, st.st_atim.tv_nsec);
	test_int_eq(sx.stx_mtime.tv_sec, st.st_mtim.tv_sec);
	test_int_eq(sx.stx_mtime.tv_nsec, st.st_mtim.tv_nsec);
	test_int_eq(sx.stx_ctime.tv_sec, st.st_ctim.tv_sec);
	test_int_eq(sx.stx_ctime.tv_nsec, st.st_ctim.tv_nsec);
	test_true(sx.stx_btime.tv_sec == 0 && sx.stx_btime.tv_nsec == 0);
	test_true(sx.stx_attributes == 0 && sx.stx_attributes_mask == 0);
	test_true(sx.stx_mnt_id == 0);

	/* Device nodes exercise st_rdev rather than the filesystem dev. */
	int nullfd = open("/dev/null", O_RDONLY);
	test_true(nullfd >= 0);
	test_int_eq(fstat(nullfd, &st), 0);
	test_int_eq(tawcroot_statx_compat(nullfd, "", AT_EMPTY_PATH, 0, &sx), 0);
	test_int_eq(sx.stx_rdev_major, major(st.st_rdev));
	test_int_eq(sx.stx_rdev_minor, minor(st.st_rdev));
	test_int_eq(close(nullfd), 0);
	test_int_eq(close((int)fd), 0);
	th_teardown(&v);
}

test(hosted_statx_compat_paths_and_errors)
{
	th_view v;
	th_setup(&v, "statx-paths");
	without_statx();
	struct statx sx;
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/etc/probe", 0,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &sx, 0), 0);
	test_true(S_ISREG(sx.stx_mode));
	test_int_eq(sx.stx_uid, 0);
	test_int_eq(sx.stx_gid, 0);
	test_true(sx.stx_mask & STATX_MNT_ID);
	test_true(sx.stx_mnt_id != 0);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/lib",
	                  AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx, 0), 0);
	test_true(S_ISLNK(sx.stx_mode));
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/lib", 0,
	                  STATX_BASIC_STATS, &sx, 0), 0);
	test_true(S_ISDIR(sx.stx_mode));

	long dirfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc",
	                    O_PATH | O_DIRECTORY, 0, 0, 0);
	test_true(dirfd >= 0);
	test_int_eq(th_sys(TAWC_SYS_statx, dirfd, "probe", AT_NO_AUTOMOUNT |
	                  AT_STATX_DONT_SYNC, STATX_BASIC_STATS, &sx, 0), 0);
	test_true(S_ISREG(sx.stx_mode));
	test_int_eq(th_sys(TAWC_SYS_statx, dirfd, "", AT_EMPTY_PATH,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &sx, 0), 0);
	test_true(S_ISDIR(sx.stx_mode));
	test_true(sx.stx_mask & STATX_MNT_ID);
	test_int_eq(close((int)dirfd), 0);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/missing", 0,
	                  STATX_BASIC_STATS, &sx, 0), TAWC_ENOENT);
	test_int_eq(th_sys(TAWC_SYS_statx, -1, "", AT_EMPTY_PATH,
	                  STATX_BASIC_STATS, &sx, 0), TAWC_EBADF);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/etc/probe", 0,
	                  STATX_BASIC_STATS, 0x1000L, 0), TAWC_EFAULT);

	/* Invalid flags/masks must not turn into a successful old stat. */
	test_int_eq(tawcroot_statx_compat(AT_FDCWD, ".", 0x40000000,
	                                0, &sx), TAWC_EINVAL);
	test_int_eq(tawcroot_statx_compat(AT_FDCWD, ".", AT_STATX_SYNC_TYPE,
	                                0, &sx), TAWC_EINVAL);
	test_int_eq(tawcroot_statx_compat(AT_FDCWD, ".", 0,
	                                0x80000000U, &sx), TAWC_EINVAL);
	/* Only ENOSYS enables fallback; preserve access/I/O failures. */
	const long errors[] = { TAWC_EACCES, TAWC_EIO, TAWC_EINVAL };
	for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
		statx_error = errors[i];
		fstatat_hits = 0;
		test_int_eq(tawcroot_statx_compat(AT_FDCWD, ".", 0, 0, &sx),
		            errors[i]);
		test_int_eq(fstatat_hits, 0);
	}
	th_teardown(&v);
}

test(hosted_statx_compat_hardlinks)
{
	th_view v;
	th_setup(&v, "statx-links");
	store_setup(&v);
	without_statx();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/etc/probe",
	                  AT_FDCWD, "/run/linked", 0, 0), 0);
	struct statx a, b;
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/etc/probe", 0,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &a, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/run/linked",
	                  AT_SYMLINK_NOFOLLOW,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &b, 0), 0);
	test_true(S_ISREG(b.stx_mode));
	test_true(a.stx_ino == b.stx_ino);
	test_int_eq(a.stx_nlink, 2);
	test_int_eq(b.stx_nlink, 2);
	test_true(a.stx_mask & STATX_MNT_ID);
	test_true(b.stx_mask & STATX_MNT_ID);
	test_true(a.stx_mnt_id == b.stx_mnt_id);
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/linked",
	                 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(th_sys(TAWC_SYS_statx, fd, "", AT_EMPTY_PATH,
	                  STATX_BASIC_STATS, &b, 0), 0);
	test_int_eq(b.stx_nlink, 2);
	test_true(a.stx_ino == b.stx_ino);
	test_int_eq(close((int)fd), 0);
	store_teardown();
	th_teardown(&v);
}

test(hosted_statx_compat_shm_and_proc)
{
	th_view v;
	th_setup(&v, "statx-shadows");
	without_statx();
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/dev/shm/probe",
	                 O_RDWR | O_CREAT, 0600, 0, 0);
	test_true(fd >= 0);
	struct statx dir, file;
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/dev/shm", 0,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &dir, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/dev/shm/probe", 0,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &file, 0), 0);
	test_true(S_ISREG(file.stx_mode));
	test_true(dir.stx_mask & STATX_MNT_ID);
	test_true(file.stx_mask & STATX_MNT_ID);
	test_true(dir.stx_mnt_id == file.stx_mnt_id);
	test_int_eq(close((int)fd), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/dev/shm/probe",
	                  0, 0, 0, 0), 0);
	struct stat proc;
	test_int_eq(stat("/proc", &proc), 0);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/proc/version", 0,
	                  STATX_BASIC_STATS | STATX_MNT_ID, &file, 0), 0);
	test_true(S_ISREG(file.stx_mode));
	test_int_eq(file.stx_dev_major, major(proc.st_dev));
	test_int_eq(file.stx_dev_minor, minor(proc.st_dev));
	test_true(file.stx_mask & STATX_MNT_ID);
	th_teardown(&v);
}

test(hosted_exec_prepare_without_statx)
{
	th_view v;
	th_setup(&v, "exec-no-statx");
	char host[4300];
	snprintf(host, sizeof host, "%s/run/exe", v.root);
	test_true(rh_copy_file(TAWCROOT_STATIC_EXIT42_BIN, host, 0755));
	snprintf(host, sizeof host, "%s/run/script", v.root);
	test_true(rh_write_text(host, "#!/run/exe\n"));
	test_int_eq(chmod(host, 0755), 0);
	without_statx();
	const char *envp[] = { NULL };
	const char *paths[] = { "/run/exe", "/run/script" };
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
		const char *argv[] = { paths[i], NULL };
		long fd = tawcroot_exec_handler_prepare(paths[i], 1, argv, envp);
		test_true(fd >= 0);
		test_int_eq(close((int)fd), 0);
	}
	test_int_eq(statx_hits, 0);
	const char *argv[] = { "/etc/probe", NULL };
	test_int_eq(tawcroot_exec_handler_prepare("/etc/probe", 1, argv, envp),
	            TAWC_EACCES);
	test_int_eq(tawcroot_exec_handler_prepare("/etc", 1, argv, envp),
	            TAWC_EISDIR);
	test_int_eq(tawcroot_exec_handler_prepare("/missing", 1, argv, envp),
	            TAWC_ENOENT);
	th_teardown(&v);
}
