/* Hosted handler matrix for read-only binds.
 *
 * This is the layer that catches a wrongly-declared READ intent —
 * every path-bearing handler is exercised against an RO bind fixture
 * (writes/creates/removes/renames/metadata → EROFS; linkat source →
 * EXDEV; reads/stats/readlink/exec-shape opens → success), and a
 * subset re-runs against an RW bind asserting nothing changed. Plus
 * the stage-2 fd-metadata residue (fchmod/fchown/futimens/f*xattr on
 * an O_RDONLY fd from an RO bind, the SCM_RIGHTS-passed flavor, and
 * the /proc/self/fd write-mode re-open), and chroot-into-RO-bind
 * root_ro semantics.
 *
 * Fixture layout per test (see th_add_bind_ro):
 *   <bind-src>/probe.txt   "from-bind"      (host-side, pre-existing)
 * plus whatever the test plants host-side before going through the
 * handlers — the bind SRC is host-writable; only the guest view is RO.
 */

#include <cleat/test.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <unistd.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"

#include "errno_neg.h"
#include "fdtab.h"
#include "path.h"
#include "sysnr.h"

#ifndef ST_RDONLY
# define ST_RDONLY 0x0001
#endif

/* ----- write refusals through an RO bind --------------------------- */

test(ro_openat_write_modes_erofs)
{
	th_view v;
	th_setup(&v, "ro-open");
	th_add_bind_ro(&v, "/ro");

	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			   O_WRONLY, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			   O_RDWR, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			   O_RDONLY | O_TRUNC, 0, 0, 0), TAWC_EROFS);
	/* O_CREAT of a MISSING file: EROFS (the create would be real). */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/newfile",
			   O_RDONLY | O_CREAT, 0644, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/newfile",
			   O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0),
		    TAWC_EROFS);

	th_teardown(&v);
}

test(ro_openat_read_shapes_succeed)
{
	th_view v;
	th_setup(&v, "ro-openr");
	const char *src = th_add_bind_ro(&v, "/ro");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	/* O_CREAT-on-EXISTING with a write-free accmode: POSIX allows it
	 * on an RO fs (the create is a no-op) — the retry rule. */
	fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
		    O_RDONLY | O_CREAT, 0644, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	/* O_RDONLY|O_APPEND is legal on an RO fs. */
	fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
		    O_RDONLY | O_APPEND, 0, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	/* O_PATH is a read even with a bogus write accmode. */
	fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
		    O_PATH | O_RDWR, 0, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	/* Directory read (getdents-shape open). */
	fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro",
		    O_RDONLY | O_DIRECTORY, 0, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	(void)src;
	th_teardown(&v);
}

test(ro_namespace_mutations_erofs)
{
	th_view v;
	th_setup(&v, "ro-ns");
	const char *src = th_add_bind_ro(&v, "/ro");
	char hp[4300];
	snprintf(hp, sizeof hp, "%s/sub", src);
	test_true(rh_mkdir_p(hp, 0755));

	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/ro/nd", 0755,
			   0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/ro/probe.txt",
			   0, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/ro/sub",
			   AT_REMOVEDIR, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "target", AT_FDCWD,
			   "/ro/lnk-new", 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_mknodat, AT_FDCWD, "/ro/fifo",
			   S_IFIFO | 0644, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_truncate, "/ro/probe.txt", 0,
			   0, 0, 0, 0), TAWC_EROFS);

	/* renames: out of, into, and within the RO bind — uniform EROFS. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/ro/probe.txt",
			   AT_FDCWD, "/tmp/out", 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/etc/probe",
			   AT_FDCWD, "/ro/in", 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/ro/probe.txt",
			   AT_FDCWD, "/ro/renamed", 0, 0), TAWC_EROFS);

	th_teardown(&v);
}

test(ro_metadata_writes_erofs)
{
	th_view v;
	th_setup(&v, "ro-meta");
	th_add_bind_ro(&v, "/ro");

	test_int_eq(th_sys(TAWC_SYS_fchmodat, AT_FDCWD, "/ro/probe.txt",
			   0600, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_fchownat, AT_FDCWD, "/ro/probe.txt",
			   0, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_utimensat, AT_FDCWD, "/ro/probe.txt",
			   0, 0, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_setxattr, "/ro/probe.txt", "user.x",
			   "v", 1, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_removexattr, "/ro/probe.txt", "user.x",
			   0, 0, 0, 0), TAWC_EROFS);

	th_teardown(&v);
}

test(ro_linkat_source_exdev_dst_erofs)
{
	th_view v;
	th_setup(&v, "ro-link");
	th_add_bind_ro(&v, "/ro");

	/* Source in the RO bind, dst elsewhere: the deliberate EXDEV
	 * divergence (a same-fs host link would mint a writable name for
	 * RO content — the hardlink escape). */
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/ro/probe.txt",
			   AT_FDCWD, "/tmp/hl", 0, 0), TAWC_EXDEV);
	/* Destination in the RO bind: plain EROFS (forced-write dst). */
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/etc/probe",
			   AT_FDCWD, "/ro/hl", 0, 0), TAWC_EROFS);
	/* Both in the RO bind: the dst check wins — EROFS. */
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/ro/probe.txt",
			   AT_FDCWD, "/ro/hl", 0, 0), TAWC_EROFS);

	th_teardown(&v);
}

test(ro_linkat_empty_path_source_exdev)
{
	th_view v;
	th_setup(&v, "ro-linkep");
	th_add_bind_ro(&v, "/ro");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	/* AT_EMPTY_PATH spelling: source judged by the fd's host path. */
	test_int_eq(th_sys(TAWC_SYS_linkat, (int)fd, "", AT_FDCWD,
			   "/tmp/hl", AT_EMPTY_PATH, 0), TAWC_EXDEV);
	close((int)fd);

	th_teardown(&v);
}

/* ----- reads still work; fidelity surfaces -------------------------- */

test(ro_reads_and_access_fidelity)
{
	th_view v;
	th_setup(&v, "ro-read");
	const char *src = th_add_bind_ro(&v, "/ro");
	char hp[4300];
	snprintf(hp, sizeof hp, "%s/lnk", src);
	test_int_eq(symlink("probe.txt", hp), 0);

	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/ro/probe.txt",
			   &st, 0, 0, 0), 0);
	test_true(S_ISREG(st.st_mode));

	char buf[64];
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/ro/lnk",
			buf, sizeof buf, 0, 0);
	test_int_eq(n, (long)strlen("probe.txt"));

	/* access: W_OK probes get the kernel's RO-fs answer, the rest
	 * observe. */
	test_int_eq(th_sys(TAWC_SYS_faccessat, AT_FDCWD, "/ro/probe.txt",
			   R_OK, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_faccessat, AT_FDCWD, "/ro/probe.txt",
			   F_OK, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_faccessat, AT_FDCWD, "/ro/probe.txt",
			   W_OK, 0, 0, 0), TAWC_EROFS);

	th_teardown(&v);
}

test(ro_statfs_shows_st_rdonly)
{
	th_view v;
	th_setup(&v, "ro-statfs");
	th_add_bind_ro(&v, "/ro");
	th_add_bind(&v, "/rw");

	struct statfs sfs;
	memset(&sfs, 0, sizeof sfs);
	test_int_eq(th_sys(TAWC_SYS_statfs, "/ro/probe.txt", &sfs,
			   0, 0, 0, 0), 0);
	test_true(sfs.f_flags & ST_RDONLY);

	memset(&sfs, 0, sizeof sfs);
	test_int_eq(th_sys(TAWC_SYS_statfs, "/rw/probe.txt", &sfs,
			   0, 0, 0, 0), 0);
	test_true(!(sfs.f_flags & ST_RDONLY));

	memset(&sfs, 0, sizeof sfs);
	test_int_eq(th_sys(TAWC_SYS_statfs, "/etc/probe", &sfs,
			   0, 0, 0, 0), 0);
	test_true(!(sfs.f_flags & ST_RDONLY));

	th_teardown(&v);
}

/* ----- the same matrix against an RW bind: nothing changed ---------- */

test(rw_bind_matrix_unchanged)
{
	th_view v;
	th_setup(&v, "rw-matrix");
	th_add_bind(&v, "/rw");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/rw/newfile",
			 O_WRONLY | O_CREAT, 0644, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/rw/nd", 0755,
			   0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/rw/newfile",
			   AT_FDCWD, "/rw/renamed", 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_fchmodat, AT_FDCWD, "/rw/renamed",
			   0600, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_faccessat, AT_FDCWD, "/rw/renamed",
			   W_OK, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_utimensat, AT_FDCWD, "/rw/renamed",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/rw/renamed",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/rw/nd",
			   AT_REMOVEDIR, 0, 0, 0), 0);

	th_teardown(&v);
}

/* ----- stage 2: fd-based metadata residue --------------------------- */

test(ro_fd_metadata_writes_erofs)
{
	th_view v;
	th_setup(&v, "ro-fdmeta");
	th_add_bind_ro(&v, "/ro");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	test_int_eq(th_sys(TAWC_SYS_fchmod, (int)fd, 0600, 0, 0, 0, 0),
		    TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_fchown, (int)fd, 0, 0, 0, 0, 0),
		    TAWC_EROFS);
	/* futimens spelling: utimensat(fd, NULL, ...). */
	test_int_eq(th_sys(TAWC_SYS_utimensat, (int)fd, 0, 0, 0, 0, 0),
		    TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_fsetxattr, (int)fd, "user.x", "v", 1,
			   0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_fremovexattr, (int)fd, "user.x",
			   0, 0, 0, 0), TAWC_EROFS);

	close((int)fd);
	th_teardown(&v);
}

test(rootfs_fd_metadata_writes_unaffected)
{
	th_view v;
	th_setup(&v, "rw-fdmeta");
	th_add_bind_ro(&v, "/ro");  /* RO elsewhere must not leak */

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	test_int_eq(th_sys(TAWC_SYS_fchmod, (int)fd, 0644, 0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_utimensat, (int)fd, 0, 0, 0, 0, 0), 0);
	/* fchown: virtual euid is 0 at dispatch init → fake success. */
	test_int_eq(th_sys(TAWC_SYS_fchown, (int)fd, 0, 0, 0, 0, 0), 0);

	close((int)fd);
	th_teardown(&v);
}

test(ro_fd_scm_rights_passed_fd_still_refused)
{
	/* The stateless design's signature win over a taint table: an fd
	 * that ARRIVED via SCM_RIGHTS was never seen by any bookkeeping,
	 * but /proc/self/fd ground truth still names the RO bind src. */
	th_view v;
	th_setup(&v, "ro-scm");
	th_add_bind_ro(&v, "/ro");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	int sp[2];
	test_int_eq(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp), 0);

	char cbuf[CMSG_SPACE(sizeof(int))];
	memset(cbuf, 0, sizeof cbuf);
	char dot = '.';
	struct iovec iov = { .iov_base = &dot, .iov_len = 1 };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof cbuf,
	};
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type  = SCM_RIGHTS;
	cm->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &(int){ (int)fd }, sizeof(int));
	test_true(sendmsg(sp[0], &mh, 0) == 1);
	close((int)fd);

	char cbuf2[CMSG_SPACE(sizeof(int))];
	struct msghdr mh2 = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf2, .msg_controllen = sizeof cbuf2,
	};
	test_true(recvmsg(sp[1], &mh2, 0) == 1);
	struct cmsghdr *cm2 = CMSG_FIRSTHDR(&mh2);
	test_nonnull(cm2);
	int passed = -1;
	memcpy(&passed, CMSG_DATA(cm2), sizeof passed);
	test_true(passed >= 0);

	test_int_eq(th_sys(TAWC_SYS_fchmod, passed, 0600, 0, 0, 0, 0),
		    TAWC_EROFS);

	close(passed);
	close(sp[0]);
	close(sp[1]);
	th_teardown(&v);
}

test(ro_proc_fd_write_reopen_erofs)
{
	/* /proc/<pid>/fd/<n> write-mode re-open: the kernel would re-open
	 * the RO-bind inode writable (host mount is RW). The openat
	 * handler readlinks the magic link and refuses. Needs a /proc
	 * bind so the path routes there. */
	th_view v;
	th_setup(&v, "ro-procfd");
	th_add_bind_ro(&v, "/ro");
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc", 0), 0);

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/ro/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	char p[64];
	snprintf(p, sizeof p, "/proc/self/fd/%d", (int)fd);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, p, O_RDWR, 0, 0, 0),
		    TAWC_EROFS);
	/* Read-mode re-open stays legal. */
	long rfd = th_sys(TAWC_SYS_openat, AT_FDCWD, p, O_RDONLY, 0, 0, 0);
	test_true(rfd >= 0);
	close((int)rfd);

	/* No false positive: a rootfs fd re-opens writable fine. */
	long efd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			  O_RDONLY, 0, 0, 0);
	test_true(efd >= 0);
	snprintf(p, sizeof p, "/proc/self/fd/%d", (int)efd);
	long wfd = th_sys(TAWC_SYS_openat, AT_FDCWD, p, O_RDWR, 0, 0, 0);
	test_true(wfd >= 0);
	close((int)wfd);
	close((int)efd);

	close((int)fd);
	th_teardown(&v);
}

/* ----- chroot interaction ------------------------------------------- */

test(ro_chroot_into_ro_bind_sets_root_ro)
{
	th_view v;
	th_setup(&v, "ro-chroot");
	const char *src = th_add_bind_ro(&v, "/ro");
	char hp[4300];
	snprintf(hp, sizeof hp, "%s/sub", src);
	test_true(rh_mkdir_p(hp, 0755));

	test_int_eq(th_sys(TAWC_SYS_chroot, "/ro", 0, 0, 0, 0, 0), 0);
	test_int_eq(tawcroot_root_ro, 1);

	/* Whole view is now RO: writes at "/" refuse, reads work. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/newfile",
			   O_WRONLY | O_CREAT, 0644, 0, 0), TAWC_EROFS);
	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/nd", 0755,
			   0, 0, 0), TAWC_EROFS);
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	th_teardown(&v);
}

test(ro_chroot_into_rw_bind_unsets_root_ro)
{
	th_view v;
	th_setup(&v, "rw-chroot");
	th_add_bind(&v, "/rw");

	/* Simulate an RO root (as if we'd chrooted into an RO bind
	 * earlier), then chroot into an RW bind: the flag must clear. */
	tawcroot_root_ro = 1;
	test_int_eq(th_sys(TAWC_SYS_chroot, "/rw", 0, 0, 0, 0, 0), 0);
	test_int_eq(tawcroot_root_ro, 0);

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/newfile",
			 O_WRONLY | O_CREAT, 0644, 0, 0);
	test_true(fd >= 0);
	close((int)fd);

	th_teardown(&v);
}

/* ----- sockets ------------------------------------------------------ */

test(ro_unix_bind_erofs)
{
	/* Refused before any host bind — no sock_file creation needed,
	 * so this registers unconditionally. */
	th_view v;
	th_setup(&v, "ro-sockb");
	th_add_bind_ro(&v, "/ro");

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	test_true(fd >= 0);
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, "/ro/sock");
	test_int_eq(th_sys(TAWC_SYS_bind, fd, &sa, sizeof sa, 0, 0, 0),
		    TAWC_EROFS);
	close(fd);

	th_teardown(&v);
}

/* Connect-side tests need a real socket FILE on the host side; same
 * environment gate as test_socket_handlers.c. */
#define socket_test(name) \
	static void test_##name([[maybe_unused]] TestCtx *test_ctx, \
				[[maybe_unused]] void const *_null_test_data)

static int ro_socket_files_creatable(void)
{
	char dir[512];
	snprintf(dir, sizeof dir, "%s/tawcroot-ro-sockprobe-%d",
		 TAWCROOT_TEST_TMPDIR, getpid());
	mkdir(dir, 0755);
	char sp[600];
	snprintf(sp, sizeof sp, "%s/s", dir);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, sp, strlen(sp));
	int ok = fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
	if (fd >= 0) close(fd);
	unlink(sp);
	rmdir(dir);
	return ok;
}

socket_test(ro_unix_connect_into_ro_bind_works)
{
	/* Connecting to a socket that LIVES on an RO fs is legal (read
	 * intent); only bind (creation) refuses. The listener is planted
	 * host-side — the bind src is host-writable. */
	th_view v;
	th_setup(&v, "ro-sockc");
	const char *src = th_add_bind_ro(&v, "/ro");

	char hp[4300];
	snprintf(hp, sizeof hp, "%s/lsock", src);
	int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	test_true(lfd >= 0);
	struct sockaddr_un lsa;
	memset(&lsa, 0, sizeof lsa);
	lsa.sun_family = AF_UNIX;
	memcpy(lsa.sun_path, hp, strlen(hp));
	test_int_eq(bind(lfd, (struct sockaddr *)&lsa, sizeof lsa), 0);
	test_int_eq(listen(lfd, 1), 0);

	int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
	test_true(cfd >= 0);
	struct sockaddr_un csa;
	memset(&csa, 0, sizeof csa);
	csa.sun_family = AF_UNIX;
	strcpy(csa.sun_path, "/ro/lsock");
	test_int_eq(th_sys(TAWC_SYS_connect, cfd, &csa, sizeof csa,
			   0, 0, 0), 0);

	close(cfd);
	close(lfd);
	unlink(hp);
	th_teardown(&v);
}

socket_test(ro_unix_connect_through_leaf_symlink)
{
	/* The bind→connect mode split moved connect to FOLLOW: a leaf
	 * symlink to a socket must now be chased like the kernel does
	 * (PARENT_CREATE never followed it). */
	th_view v;
	th_setup(&v, "ro-sockl");
	char hp[4300];
	snprintf(hp, sizeof hp, "%s/run/realsock", v.root);
	int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	test_true(lfd >= 0);
	struct sockaddr_un lsa;
	memset(&lsa, 0, sizeof lsa);
	lsa.sun_family = AF_UNIX;
	memcpy(lsa.sun_path, hp, strlen(hp));
	test_int_eq(bind(lfd, (struct sockaddr *)&lsa, sizeof lsa), 0);
	test_int_eq(listen(lfd, 1), 0);

	char lp[4300];
	snprintf(lp, sizeof lp, "%s/etc/socklnk", v.root);
	test_int_eq(symlink("/run/realsock", lp), 0);

	int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
	test_true(cfd >= 0);
	struct sockaddr_un csa;
	memset(&csa, 0, sizeof csa);
	csa.sun_family = AF_UNIX;
	strcpy(csa.sun_path, "/etc/socklnk");
	test_int_eq(th_sys(TAWC_SYS_connect, cfd, &csa, sizeof csa,
			   0, 0, 0), 0);

	close(cfd);
	close(lfd);
	th_teardown(&v);
}

register_dynamic_tests
{
	if (!ro_socket_files_creatable()) return;
	csview mod = test_module_from_file(__FILE__);
	register_test(mod, c_sv("ro_unix_connect_into_ro_bind_works"),
		      test_ro_unix_connect_into_ro_bind_works,
		      nullptr, nullptr);
	register_test(mod, c_sv("ro_unix_connect_through_leaf_symlink"),
		      test_ro_unix_connect_through_leaf_symlink,
		      nullptr, nullptr);
}
