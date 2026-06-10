/* Hosted handler-level tests for syscalls_fs.c — the openat/stat/
 * unlink/rename families run in-process under ASan+UBSan against a
 * real tmpdir rootfs. See hosted.h for what this layer covers vs the
 * fork-based layers.
 *
 * Errno expectations mirror kernel semantics; references are the
 * path_oracle tests and `man 2` for each call. */

#include <cleat/test.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hosted.h"

/* No tawc_uapi.h here: it pulls kernel uapi headers that clash with
 * the glibc <fcntl.h> this hosted TU needs. _GNU_SOURCE glibc headers
 * provide every O_ / AT_ / STATX_ constant these tests use. */
#include "errno_neg.h"
#include "path.h"
#include "sysnr.h"

/* A pointer that is guaranteed unmapped (below the kernel's minimum
 * mmap address) — the EFAULT probes use it as a wild guest pointer. */
#define WILD_PTR 0x1000L

/* --- openat: translation basics ------------------------------------- */

test(hosted_openat_absolute_translates_into_rootfs)
{
	th_view v;
	th_setup(&v, "fs-open");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[32] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "from-rootfs\n");
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_openat_missing_file_enoent)
{
	th_view v;
	th_setup(&v, "fs-enoent");

	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/nope",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_openat_dotdot_escape_clamps_at_root)
{
	th_view v;
	th_setup(&v, "fs-clamp");

	/* Clamped to <rootfs>/etc/probe — must read rootfs bytes, not
	 * escape toward the host /etc. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD,
			 "/../../../../etc/probe", O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[32] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "from-rootfs\n");
	test_int_eq(close((int)fd), 0);

	/* The host file /etc/passwd exists; the view's doesn't. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD,
			   "/etc/../../../etc/passwd", O_RDONLY, 0, 0, 0),
		    TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_openat_memoized_symlink_routes_through_target)
{
	th_view v;
	th_setup(&v, "fs-memo");

	/* /lib -> usr/lib is memoized at setup; /lib/probe.so must land
	 * on <rootfs>/usr/lib/probe.so. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/lib/probe.so",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[32] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "fake-lib\n");
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_openat_null_and_wild_path_efault)
{
	th_view v;
	th_setup(&v, "fs-efault");

	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, 0L,
			   O_RDONLY, 0, 0, 0), TAWC_EFAULT);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, WILD_PTR,
			   O_RDONLY, 0, 0, 0), TAWC_EFAULT);

	th_teardown(&v);
}

/* --- trailing-slash kernel semantics --------------------------------- */

test(hosted_trailing_slash_on_regular_file_enotdir)
{
	th_view v;
	th_setup(&v, "fs-tslash");

	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe/",
			   O_RDONLY, 0, 0, 0), TAWC_ENOTDIR);

	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/etc/probe/",
			   &st, 0, 0, 0), TAWC_ENOTDIR);

	/* On a directory a trailing slash is fine. */
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/etc/sub/",
			   &st, 0, 0, 0), 0);
	test_true(S_ISDIR(st.st_mode));

	th_teardown(&v);
}

test(hosted_trailing_slash_missing_leaf_creation)
{
	th_view v;
	th_setup(&v, "fs-tslash2");

	/* O_CREAT with a trailing slash must not create anything. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/new/",
			   O_WRONLY | O_CREAT, 0644, 0, 0), TAWC_EISDIR);
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/new",
			   &st, AT_SYMLINK_NOFOLLOW, 0, 0), TAWC_ENOENT);

	/* mkdirat with a trailing slash is fine (kernel allows it). */
	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/run/newdir/",
			   0755, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/newdir",
			   &st, 0, 0, 0), 0);
	test_true(S_ISDIR(st.st_mode));

	th_teardown(&v);
}

/* --- fake-root stat decoration --------------------------------------- */

test(hosted_fstatat_decorates_uid_gid_zero)
{
	th_view v;
	th_setup(&v, "fs-stat");

	struct stat st;
	st.st_uid = 99;
	st.st_gid = 99;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/etc/probe",
			   &st, 0, 0, 0), 0);
	test_int_eq(st.st_uid, 0);
	test_int_eq(st.st_gid, 0);
	test_true(S_ISREG(st.st_mode));

	/* statx decorates the same way. */
	struct statx stx;
	memset(&stx, 0xff, sizeof stx);
	test_int_eq(th_sys(TAWC_SYS_statx, AT_FDCWD, "/etc/probe",
			   0, STATX_BASIC_STATS, &stx, 0), 0);
	test_int_eq(stx.stx_uid, 0);
	test_int_eq(stx.stx_gid, 0);

	/* Output-buffer EFAULT safety: good path, wild *st. */
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/etc/probe",
			   WILD_PTR, 0, 0, 0), TAWC_EFAULT);

	th_teardown(&v);
}

/* --- write-side ops: create, rename, unlink --------------------------- */

test(hosted_create_rename_unlink_cycle)
{
	th_view v;
	th_setup(&v, "fs-cycle");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/a",
			 O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0);
	test_true(fd >= 0);
	test_true(write((int)fd, "x", 1) == 1);
	test_int_eq(close((int)fd), 0);

	/* Create must have landed inside the rootfs tree. */
	char host[4200];
	snprintf(host, sizeof host, "%s/run/a", v.root);
	struct stat st;
	test_int_eq(stat(host, &st), 0);

	/* O_EXCL on the now-existing leaf: EEXIST. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/a",
			   O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0),
		    TAWC_EEXIST);

	/* renameat2 within the view. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/a",
			   AT_FDCWD, "/run/b", 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/a",
			   &st, 0, 0, 0), TAWC_ENOENT);
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/b",
			   &st, 0, 0, 0), 0);

	/* unlinkat the destination; view is clean again. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/b",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/b",
			   &st, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_unlinkat_semantics)
{
	th_view v;
	th_setup(&v, "fs-unlink");

	/* unlinkat on a dir without AT_REMOVEDIR: EISDIR. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/etc/sub",
			   0, 0, 0, 0), TAWC_EISDIR);
	/* With AT_REMOVEDIR on a non-empty dir's parent content: works on
	 * the empty /etc/sub. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/etc/sub",
			   AT_REMOVEDIR, 0, 0, 0), 0);
	/* unlink on a symlink removes the link, not the target. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/lib",
			   0, 0, 0, 0), 0);
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/usr/lib/probe.so",
			   &st, 0, 0, 0), 0);

	th_teardown(&v);
}

test(hosted_symlinkat_and_readlinkat)
{
	th_view v;
	th_setup(&v, "fs-symlink");

	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/etc/probe", AT_FDCWD,
			   "/run/link", 0, 0, 0), 0);

	char buf[64] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/link",
			buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/etc/probe"));
	test_str_eq(buf, "/etc/probe");

	/* NOFOLLOW leaf semantics: lstat sees the link, stat follows. */
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/link",
			   &st, AT_SYMLINK_NOFOLLOW, 0, 0), 0);
	test_true(S_ISLNK(st.st_mode));
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/link",
			   &st, 0, 0, 0), 0);
	test_true(S_ISREG(st.st_mode));

	/* readlinkat on a directory: EINVAL (not a symlink). */
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/etc",
			   buf, sizeof buf - 1, 0, 0), TAWC_EINVAL);
	/* readlinkat on the rootfs root: EINVAL. */
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/",
			   buf, sizeof buf - 1, 0, 0), TAWC_EINVAL);

	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/link",
			   0, 0, 0, 0), 0);
	th_teardown(&v);
}

test(hosted_symlink_absolute_target_clamped_to_view)
{
	th_view v;
	th_setup(&v, "fs-symclamp");

	/* A symlink whose absolute target names a host-existing path must
	 * resolve inside the view (ENOENT), never against the host root. */
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/etc/passwd", AT_FDCWD,
			   "/run/evil", 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/evil",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);

	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/evil",
			   0, 0, 0, 0), 0);
	th_teardown(&v);
}

/* --- dirfd-anchored resolution ---------------------------------------- */

test(hosted_dirfd_relative_resolution_and_dotdot_clamp)
{
	th_view v;
	th_setup(&v, "fs-dirfd");

	long dfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc",
			  O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(dfd >= 0);

	/* Plain relative leaf through a guest dirfd. */
	long fd = th_sys(TAWC_SYS_openat, dfd, "probe", O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	/* `..` through the dirfd must clamp at the rootfs root, not walk
	 * into the host parent of the tmpdir. */
	long up = th_sys(TAWC_SYS_openat, dfd, "..",
			 O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(up >= 0);
	struct stat st_up, st_root;
	test_int_eq(fstat((int)up, &st_up), 0);
	test_int_eq(stat(v.root, &st_root), 0);
	test_true(st_up.st_ino == st_root.st_ino &&
		  st_up.st_dev == st_root.st_dev);
	test_int_eq(close((int)up), 0);

	test_int_eq(close((int)dfd), 0);
	th_teardown(&v);
}

test(hosted_relative_path_resolves_through_cwd)
{
	th_view v;
	th_setup(&v, "fs-cwd");

	/* Guest chdir("/etc") → kernel cwd lands inside the rootfs; a
	 * relative open then reverse-translates through the cwd. */
	test_int_eq(th_sys(TAWC_SYS_chdir, "/etc", 0, 0, 0, 0, 0), 0);

	char cwd[256] = {0};
	long n = th_sys(TAWC_SYS_getcwd, cwd, sizeof cwd, 0, 0, 0, 0);
	test_int_eq(n, 5);  /* "/etc" + NUL, getcwd counts the NUL */
	test_str_eq(cwd, "/etc");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

/* --- two-path handlers: EFAULT on either side -------------------------- */

test(hosted_two_path_handlers_efault_either_side)
{
	th_view v;
	th_setup(&v, "fs-efault2");

	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, WILD_PTR,
			   AT_FDCWD, "/run/x", 0, 0), TAWC_EFAULT);
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/etc/probe",
			   AT_FDCWD, WILD_PTR, 0, 0), TAWC_EFAULT);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, WILD_PTR,
			   AT_FDCWD, "/run/x", 0, 0), TAWC_EFAULT);
	test_int_eq(th_sys(TAWC_SYS_symlinkat, WILD_PTR, AT_FDCWD,
			   "/run/x", 0, 0, 0), TAWC_EFAULT);

	/* And no side effects from the half-good args. */
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/x",
			   &st, AT_SYMLINK_NOFOLLOW, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

/* --- bind routing ------------------------------------------------------ */

test(hosted_bind_dst_routes_to_bind_src)
{
	th_view v;
	th_setup(&v, "fs-bind");
	th_add_bind(&v, "/mnt/host");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/mnt/host/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[32] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "from-bind\n");
	test_int_eq(close((int)fd), 0);

	/* Longest-prefix match: /mnt alone still resolves in the rootfs
	 * (which has no /mnt) → ENOENT, not the bind. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/mnt",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_bind_dirfd_dotdot_clamps_at_bind_boundary)
{
	th_view v;
	th_setup(&v, "fs-bindup");
	th_add_bind(&v, "/mnt/host");

	long dfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/mnt/host",
			  O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(dfd >= 0);

	/* `..` from the bind dst goes to guest /mnt → rootfs (no such
	 * dir) — definitely NOT the host tmpdir parent, where the rootfs
	 * tree itself ("tawcroot-hosted-fs-bindup-<pid>") is visible. */
	long rv = th_sys(TAWC_SYS_faccessat, dfd, "../../etc/probe",
			 0 /*F_OK*/, 0, 0, 0);
	test_int_eq(rv, 0);  /* clamps to rootfs /etc/probe */

	char evil[4200];
	snprintf(evil, sizeof evil, "../%s", "..");
	long esc = th_sys(TAWC_SYS_openat, dfd, evil,
			  O_PATH | O_DIRECTORY, 0, 0, 0);
	if (esc >= 0) {
		/* If it opened, it must be the rootfs root, not the host
		 * parent of the bind src. */
		struct stat st_got, st_root;
		test_int_eq(fstat((int)esc, &st_got), 0);
		test_int_eq(stat(v.root, &st_root), 0);
		test_true(st_got.st_ino == st_root.st_ino &&
			  st_got.st_dev == st_root.st_dev);
		test_int_eq(close((int)esc), 0);
	}

	test_int_eq(close((int)dfd), 0);
	th_teardown(&v);
}
