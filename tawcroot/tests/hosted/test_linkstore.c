/* Hardlink-emulation fidelity matrix (hosted layer).
 *
 * Every test drives the real dispatch handlers with the raw-hook
 * failing host linkat with EPERM — exactly what the Android
 * untrusted_app SELinux denial does — while all other syscalls hit a
 * real tmpdir rootfs plus a real store dir (sibling of the rootfs).
 * Covers the guest-observable contract from
 * notes/tawcroot/link-emulation.md: shared inode, live st_nlink,
 * any-order unlink, publish/backup idioms, rename-over, same-token
 * rename no-op, RENAME_EXCHANGE, forgery guard, d_type rewrite,
 * version-gate degradation, and legacy-v1 coexistence. */

#include <cleat/test.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dirent.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "linkstore.h"
#include "path.h"
#include "sysnr.h"

/* ------------------------------------------------------------------ */
/* Store helpers                                                       */

#include "linkstore_fixture.h"

/* Fail host linkat with EPERM (the SELinux denial) while installed. */
static bool eperm_linkat_hook(long nr, const long args[6], long *ret)
{
	(void)args;
	if (nr != TAWC_SYS_linkat) return false;
	*ret = TAWC_EPERM;
	return true;
}

static void install_eperm_linkat(void)
{
	tawcroot_test_raw_hook = eperm_linkat_hook;
}

/* Count entries in <store>/link (objects + sidecars). -1 = no dir. */
static int store_link_entries(void)
{
	char p[4400];
	snprintf(p, sizeof p, "%s/link", g_store);
	return rh_count_dir_entries(p);
}

static void write_file(TestCtx *test_ctx, const char *gpath,
		       const char *content)
{
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, gpath,
			 O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
	test_true(fd >= 0);
	test_true(write((int)fd, content, strlen(content)) ==
		  (ssize_t)strlen(content));
	test_int_eq(close((int)fd), 0);
}

static void check_content(TestCtx *test_ctx, const char *gpath,
			  const char *want)
{
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, gpath, O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[128] = {0};
	long n = read((int)fd, buf, sizeof buf - 1);
	test_true(n >= 0);
	test_int_eq(close((int)fd), 0);
	test_str_eq(buf, want);
}

static long lstat_guest(const char *gpath, struct stat *st)
{
	tawcroot_handler_fn fn = tawcroot_dispatch_get(TAWC_SYS_fstatat);
	tawcroot_syscall_args a = {
		.nr = TAWC_SYS_fstatat, .a = AT_FDCWD, .b = (long)gpath,
		.c = (long)st, .d = AT_SYMLINK_NOFOLLOW,
	};
	return fn(&a, NULL);
}

/* Make the emulated pair `src` + `dst` under EPERM injection. */
static void make_pair(TestCtx *test_ctx, const char *src, const char *dst)
{
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, src,
			   AT_FDCWD, dst, 0, 0), 0);
	tawcroot_test_raw_hook = NULL;
}

/* ------------------------------------------------------------------ */
/* Core fidelity                                                       */

test(linkstore_first_link_shared_inode_and_nlink)
{
	th_view v;
	th_setup(&v, "ls-first");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "payload\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Both names: NOFOLLOW stats report a regular file, shared
	 * st_ino, st_nlink 2 (gaps 1+2 closed). */
	struct stat a, b;
	test_int_eq(lstat_guest("/run/f", &a), 0);
	test_int_eq(lstat_guest("/run/l1", &b), 0);
	test_true(S_ISREG(a.st_mode));
	test_true(S_ISREG(b.st_mode));
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 2);
	test_int_eq((long)b.st_nlink, 2);

	/* FOLLOW stat agrees. */
	struct stat fa;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/f",
			   &fa, 0, 0, 0), 0);
	test_true(S_ISREG(fa.st_mode));
	test_int_eq((long)fa.st_nlink, 2);
	test_true(fa.st_ino == a.st_ino);

	/* Content readable through both names. */
	check_content(test_ctx, "/run/f", "payload\n");
	check_content(test_ctx, "/run/l1", "payload\n");

	/* readlink says "regular file" (gap 5). */
	char lnk[64];
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/f",
			   lnk, sizeof lnk, 0, 0), TAWC_EINVAL);
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/l1",
			   lnk, sizeof lnk, 0, 0), TAWC_EINVAL);

	/* Intent slot is clean after a completed op. */
	char ip[4400];
	snprintf(ip, sizeof ip, "%s/intent", g_store);
	test_int_eq(access(ip, F_OK), -1);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_shared_write_visible_through_other_name)
{
	th_view v;
	th_setup(&v, "ls-write");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "old\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Writing through one name is visible through the other —
	 * the copy-on-link alternative gets this wrong. */
	write_file(test_ctx, "/run/l1", "new-bytes\n");
	check_content(test_ctx, "/run/f", "new-bytes\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_publish_pattern_git)
{
	th_view v;
	th_setup(&v, "ls-pub");
	store_setup(&v);

	/* git object finalize: link(tmp, final); unlink(tmp). */
	write_file(test_ctx, "/tmp/obj", "objectdata\n");
	make_pair(test_ctx, "/tmp/obj", "/run/final");
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/tmp/obj",
			   0, 0, 0, 0), 0);

	struct stat st;
	test_int_eq(lstat_guest("/tmp/obj", &st), TAWC_ENOENT);
	test_int_eq(lstat_guest("/run/final", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 1);
	check_content(test_ctx, "/run/final", "objectdata\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_any_order_unlink_and_object_reclaim)
{
	th_view v;
	th_setup(&v, "ls-farm");
	store_setup(&v);

	/* cp -al-style farm: one file, three extra names. */
	write_file(test_ctx, "/run/f", "farm\n");
	make_pair(test_ctx, "/run/f", "/run/l1");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/l1",
			   AT_FDCWD, "/run/l2", 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/etc/l3", 0, 0), 0);
	tawcroot_test_raw_hook = NULL;

	struct stat st;
	test_int_eq(lstat_guest("/run/l2", &st), 0);
	test_int_eq((long)st.st_nlink, 4);

	/* Unlink in "wrong" order: the original name first (gap 3). */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/f",
			   0, 0, 0, 0), 0);
	check_content(test_ctx, "/run/l2", "farm\n");
	test_int_eq(lstat_guest("/etc/l3", &st), 0);
	test_int_eq((long)st.st_nlink, 3);

	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l2",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/etc/l3",
			   0, 0, 0, 0), 0);
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_int_eq((long)st.st_nlink, 1);
	check_content(test_ctx, "/run/l1", "farm\n");

	/* Last name gone → object + sidecar reclaimed. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), 0);
	test_int_eq(store_link_entries(), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_link_dst_eexist)
{
	th_view v;
	th_setup(&v, "ls-eexist");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "x\n");
	write_file(test_ctx, "/run/occupied", "y\n");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/occupied", 0, 0), TAWC_EEXIST);
	/* Linking an emulated name onto an occupied dst too. */
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/l1", 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/l1",
			   AT_FDCWD, "/run/occupied", 0, 0), TAWC_EEXIST);
	tawcroot_test_raw_hook = NULL;

	/* Source untouched, still an emulated pair. */
	struct stat st;
	test_int_eq(lstat_guest("/run/f", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 2);
	check_content(test_ctx, "/run/occupied", "y\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_backup_pattern_rename_over)
{
	th_view v;
	th_setup(&v, "ls-backup");
	store_setup(&v);

	/* backup idiom: link(f, f.bak); rename(new, f). */
	write_file(test_ctx, "/run/f", "v1\n");
	make_pair(test_ctx, "/run/f", "/run/f.bak");
	write_file(test_ctx, "/run/new", "v2\n");
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/new",
			   AT_FDCWD, "/run/f", 0, 0), 0);

	/* Old name reads the new bytes; the backup keeps the old bytes
	 * (gap 6). */
	check_content(test_ctx, "/run/f", "v2\n");
	check_content(test_ctx, "/run/f.bak", "v1\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/f", &st), 0);
	test_true(S_ISREG(st.st_mode));  /* plain file now */
	test_int_eq(lstat_guest("/run/f.bak", &st), 0);
	test_int_eq((long)st.st_nlink, 1);

	/* Clobbering the last name reclaims the object. */
	write_file(test_ctx, "/run/new2", "v3\n");
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/new2",
			   AT_FDCWD, "/run/f.bak", 0, 0), 0);
	check_content(test_ctx, "/run/f.bak", "v3\n");
	test_int_eq(store_link_entries(), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_same_token_rename_is_noop)
{
	th_view v;
	th_setup(&v, "ls-samet");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "z\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* POSIX: rename of two names of the same inode is a successful
	 * no-op — both names remain. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/l1", 0, 0), 0);
	struct stat a, b;
	test_int_eq(lstat_guest("/run/f", &a), 0);
	test_int_eq(lstat_guest("/run/l1", &b), 0);
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 2);

	/* NOREPLACE onto an existing (emulated) name: EEXIST. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/l1", RENAME_NOREPLACE, 0),
		    TAWC_EEXIST);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_rename_moves_emulated_name)
{
	th_view v;
	th_setup(&v, "ls-mv");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "m\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Moving an emulated name (and even its whole directory) never
	 * breaks the link — opaque targets are location-independent. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/l1",
			   AT_FDCWD, "/etc/sub/l1-moved", 0, 0), 0);
	check_content(test_ctx, "/etc/sub/l1-moved", "m\n");
	struct stat st;
	test_int_eq(lstat_guest("/etc/sub/l1-moved", &st), 0);
	test_int_eq((long)st.st_nlink, 2);

	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/etc/sub",
			   AT_FDCWD, "/run/subdir", 0, 0), 0);
	check_content(test_ctx, "/run/subdir/l1-moved", "m\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_rename_exchange_passthrough)
{
	th_view v;
	th_setup(&v, "ls-xchg");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "one\n");
	make_pair(test_ctx, "/run/f", "/run/l1");
	write_file(test_ctx, "/run/plain", "two\n");

	/* emulated ↔ plain */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/plain", RENAME_EXCHANGE, 0), 0);
	check_content(test_ctx, "/run/f", "two\n");
	check_content(test_ctx, "/run/plain", "one\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/plain", &st), 0);
	test_int_eq((long)st.st_nlink, 2);

	/* emulated ↔ emulated (two clusters) */
	write_file(test_ctx, "/run/g", "gee\n");
	make_pair(test_ctx, "/run/g", "/run/gl");
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/plain",
			   AT_FDCWD, "/run/gl", RENAME_EXCHANGE, 0), 0);
	check_content(test_ctx, "/run/plain", "gee\n");
	check_content(test_ctx, "/run/gl", "one\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_open_nofollow_semantics)
{
	th_view v;
	th_setup(&v, "ls-nofollow");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "nf\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* O_CREAT|O_EXCL on an emulated name: EEXIST (a regular file is
	 * there). */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			   O_WRONLY | O_CREAT | O_EXCL, 0644, 0, 0),
		    TAWC_EEXIST);

	/* Plain O_CREAT opens the existing "file". */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			 O_RDONLY | O_CREAT, 0644, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_dangling_token_degrades_safely)
{
	th_view v;
	th_setup(&v, "ls-dangle");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "d\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Simulate host-copy data loss: delete the object (and sidecar)
	 * out from under the names. */
	char lp[4400];
	snprintf(lp, sizeof lp, "%s/link", g_store);
	test_true(rh_unlink_by_suffix(lp, ""));  /* all entries */

	/* FOLLOW resolution: ENOENT ("not linked yet"). */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);
	/* O_CREAT must NOT create a fresh uncounted object in link/. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			   O_WRONLY | O_CREAT, 0644, 0, 0), TAWC_ENOENT);
	test_int_eq(store_link_entries(), 0);

	/* The dangling name stays visible (as a symlink) and unlinkable
	 * so cleanup tools still work. */
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), 0);
	test_int_eq(lstat_guest("/run/l1", &st), TAWC_ENOENT);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/f",
			   0, 0, 0, 0), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_forgery_guard)
{
	th_view v;
	th_setup(&v, "ls-forge");
	store_setup(&v);

	/* Guests can never author a target in the tawcroot: namespace. */
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "tawcroot:link:1234",
			   AT_FDCWD, "/run/forged", 0, 0, 0), TAWC_EPERM);
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "tawcroot:future:x",
			   AT_FDCWD, "/run/forged", 0, 0, 0), TAWC_EPERM);
	struct stat st;
	test_int_eq(lstat_guest("/run/forged", &st), TAWC_ENOENT);

	/* Near-misses stay allowed. */
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "tawcrootx:link:1",
			   AT_FDCWD, "/run/ok1", 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/etc/probe",
			   AT_FDCWD, "/run/ok2", 0, 0, 0), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_dirent_dtype_rewrite)
{
	th_view v;
	th_setup(&v, "ls-dtype");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "t\n");
	write_file(test_ctx, "/run/regular", "r\n");
	make_pair(test_ctx, "/run/f", "/run/l1");
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/etc/probe",
			   AT_FDCWD, "/run/plainlink", 0, 0, 0), 0);

	long dfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			  O_RDONLY | O_DIRECTORY, 0, 0, 0);
	test_true(dfd >= 0);
	unsigned char buf[4096];
	long n = th_sys(TAWC_SYS_getdents64, dfd, buf, sizeof buf, 0, 0, 0);
	test_true(n > 0);
	test_int_eq(close((int)dfd), 0);

	/* Every DT_LNK in a rootfs-view dir is rewritten to DT_UNKNOWN —
	 * emulated names must not advertise symlink-ness to type-trusting
	 * walkers, at the accepted cost of degrading plain symlinks too. */
	int saw_l1 = 0, saw_plain = 0;
	long in = 0;
	while (in < n) {
		unsigned short reclen;
		memcpy(&reclen, buf + in + 16, 2);
		test_true(reclen > 19);
		const char *name = (const char *)(buf + in + 19);
		unsigned char d_type = buf[in + 18];
		if (strcmp(name, "l1") == 0)        { saw_l1 = 1;    test_int_eq(d_type, 0); }
		if (strcmp(name, "plainlink") == 0) { saw_plain = 1; test_int_eq(d_type, 0); }
		/* Both names of the pair are emulated (NEW converts the
		 * source too) → flipped. A genuinely regular file keeps
		 * its type. */
		if (strcmp(name, "f") == 0)         test_int_eq(d_type, 0);
		if (strcmp(name, "regular") == 0)   test_int_eq(d_type, DT_REG);
		in += reclen;
	}
	test_true(saw_l1);
	test_true(saw_plain);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_dirent_dtype_rewrite_in_bind)
{
	th_view v;
	th_setup(&v, "ls-dtypebind");
	store_setup(&v);

	/* Same-fs bind dirs need the rewrite too: the NOFOLLOW stat
	 * fixups apply there, so a symlink advertising DT_LNK while
	 * fstatat can say S_IFREG would make type-trusting walkers skip
	 * it. The store must be OPEN for the flip (gate), so mint a pair
	 * first. */
	write_file(test_ctx, "/run/f", "b\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	const char *bind_src = th_add_bind(&v, "/usr/share/tawc");
	char hp[4400];
	snprintf(hp, sizeof hp, "%s/lnk", bind_src);
	test_int_eq(symlink("somewhere", hp), 0);

	long dfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/usr/share/tawc",
			  O_RDONLY | O_DIRECTORY, 0, 0, 0);
	test_true(dfd >= 0);
	unsigned char buf[4096];
	long n = th_sys(TAWC_SYS_getdents64, dfd, buf, sizeof buf, 0, 0, 0);
	test_true(n > 0);
	test_int_eq(close((int)dfd), 0);

	int saw_lnk = 0;
	long in = 0;
	while (in < n) {
		unsigned short reclen;
		memcpy(&reclen, buf + in + 16, 2);
		test_true(reclen > 19);
		const char *name = (const char *)(buf + in + 19);
		unsigned char d_type = buf[in + 18];
		if (strcmp(name, "lnk") == 0) { saw_lnk = 1; test_int_eq(d_type, 0); }
		if (strcmp(name, "probe.txt") == 0) test_int_eq(d_type, DT_REG);
		in += reclen;
	}
	test_true(saw_lnk);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_version_gate_newer_store_goes_degraded)
{
	th_view v;
	th_setup(&v, "ls-vergate");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "vg\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* A newer APK migrated the store under us. */
	char vp[4400];
	snprintf(vp, sizeof vp, "%s/version", g_store);
	test_true(rh_write_text(vp, "2\n"));

	/* Mutations refuse: link is a raw EPERM (no v1 fallback — old
	 * code must not write blind into a newer store), unlink of an
	 * emulated name degrades to a plain unlink (object leaks). */
	write_file(test_ctx, "/run/g", "x\n");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/g",
			   AT_FDCWD, "/run/gl", 0, 0), TAWC_EPERM);
	tawcroot_test_raw_hook = NULL;
	test_int_eq((long)tawcroot_linkstore_state(),
		    (long)TAWCROOT_STORE_DEGRADED);

	/* Detection stays on: reads still work through the store. */
	check_content(test_ctx, "/run/l1", "vg\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_true(S_ISREG(st.st_mode));

	int entries_before = store_link_entries();
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), 0);
	test_int_eq(lstat_guest("/run/l1", &st), TAWC_ENOENT);
	/* Object leaked, count untouched: nothing in the store changed. */
	test_int_eq(store_link_entries(), entries_before);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_v1_artifacts_keep_working)
{
	th_view v;
	th_setup(&v, "ls-v1");
	store_setup(&v);

	/* A legacy v1 fallback symlink (guest-absolute path target)
	 * from an existing rootfs: no token, no misdetection — ordinary
	 * symlink behavior. */
	char p[4400];
	snprintf(p, sizeof p, "%s/run/legacy", v.root);
	test_int_eq(symlink("/etc/probe", p), 0);

	check_content(test_ctx, "/run/legacy", "from-rootfs\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/legacy", &st), 0);
	test_true(S_ISLNK(st.st_mode));
	char lnk[64] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/legacy",
			lnk, sizeof lnk - 1, 0, 0);
	test_true(n > 0);
	test_str_eq(lnk, "/etc/probe");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_no_store_keeps_v1_fallback)
{
	th_view v;
	th_setup(&v, "ls-nostore");
	/* No store configured at all: the EPERM fallback is the v1
	 * rename+symlink emulation, byte-for-byte today's behavior. */
	write_file(test_ctx, "/run/f", "v1beh\n");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/l1", 0, 0), 0);
	tawcroot_test_raw_hook = NULL;

	struct stat st;
	test_int_eq(lstat_guest("/run/f", &st), 0);
	test_true(S_ISLNK(st.st_mode));
	char lnk[64] = {0};
	test_true(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/f",
			 lnk, sizeof lnk - 1, 0, 0) > 0);
	test_str_eq(lnk, "/run/l1");

	th_teardown(&v);
}

test(linkstore_store_survives_reconfigure)
{
	th_view v;
	th_setup(&v, "ls-reopen");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "persist\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Session restart: re-configure against the existing store dir
	 * (open path, not create path). Links keep working. */
	tawcroot_linkstore_configure(NULL);
	tawcroot_linkstore_configure(g_store);
	test_int_eq((long)tawcroot_linkstore_state(),
		    (long)TAWCROOT_STORE_READY);

	check_content(test_ctx, "/run/l1", "persist\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_int_eq((long)st.st_nlink, 2);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/f",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), 0);
	test_int_eq(store_link_entries(), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_missing_sidecar_reports_two_never_deletes)
{
	th_view v;
	th_setup(&v, "ls-nocnt");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "sc\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* Torn/partial host copy: sidecar gone. */
	char lp[4400];
	snprintf(lp, sizeof lp, "%s/link", g_store);
	int had = rh_count_dir_entries(lp);
	test_int_eq(had, 2);  /* object + sidecar */
	{
		/* Remove just the .cnt entry. */
		char cmd_p[4400];
		struct stat st;
		snprintf(cmd_p, sizeof cmd_p, "%s/link", g_store);
		/* Find the sidecar by suffix. */
		test_true(rh_unlink_by_suffix(cmd_p, ".cnt"));
		(void)st;
	}

	/* Degraded but data-safe: st_nlink reports 2, unlink never
	 * deletes the object. */
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_int_eq((long)st.st_nlink, 2);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/f",
			   0, 0, 0, 0), 0);
	/* Object survives (leaked, never lost). */
	test_int_eq(store_link_entries(), 1);

	store_teardown();
	th_teardown(&v);
}

/* ------------------------------------------------------------------ */
/* Stage 3: symlink objects, NOFOLLOW surfaces, fd-shaped sources      */

test(linkstore_hardlink_of_symlink_absolute)
{
	th_view v;
	th_setup(&v, "ls-hlsym");
	store_setup(&v);

	write_file(test_ctx, "/run/real", "target-data\n");
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/run/real",
			   AT_FDCWD, "/run/s", 0, 0, 0), 0);
	make_pair(test_ctx, "/run/s", "/run/s2");

	/* Both names report the symlink OBJECT: S_IFLNK (real mode, not
	 * hardcoded S_IFREG), shared inode, live count. */
	struct stat a, b;
	test_int_eq(lstat_guest("/run/s", &a), 0);
	test_int_eq(lstat_guest("/run/s2", &b), 0);
	test_true(S_ISLNK(a.st_mode));
	test_true(S_ISLNK(b.st_mode));
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 2);

	/* readlink forwards the object's real target — the token literal
	 * never reaches guest-readable output. */
	char lnk[64] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/s2",
			lnk, sizeof lnk, 0, 0);
	test_int_eq(n, (long)strlen("/run/real"));
	test_true(memcmp(lnk, "/run/real", (size_t)n) == 0);

	/* FOLLOW resolves the spliced target through the GUEST view. */
	check_content(test_ctx, "/run/s2", "target-data\n");
	struct stat f;
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/s2",
			   &f, 0, 0, 0), 0);
	test_true(S_ISREG(f.st_mode));

	/* Any-order unlink + reclaim, same as regular clusters. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/s",
			   0, 0, 0, 0), 0);
	test_int_eq(lstat_guest("/run/s2", &b), 0);
	test_true(S_ISLNK(b.st_mode));
	test_int_eq((long)b.st_nlink, 1);
	check_content(test_ctx, "/run/s2", "target-data\n");
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/s2",
			   0, 0, 0, 0), 0);
	test_int_eq(store_link_entries(), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_hardlink_of_symlink_relative_target)
{
	th_view v;
	th_setup(&v, "ls-hlrel");
	store_setup(&v);

	/* A RELATIVE object target resolves against each NAME's parent
	 * directory — real hardlinked-symlink semantics. */
	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/run/a",
			   0755, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/run/b",
			   0755, 0, 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_mkdirat, AT_FDCWD, "/run/c",
			   0755, 0, 0, 0), 0);
	write_file(test_ctx, "/run/a/t", "A\n");
	write_file(test_ctx, "/run/b/t", "B\n");
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "t",
			   AT_FDCWD, "/run/a/s", 0, 0, 0), 0);
	make_pair(test_ctx, "/run/a/s", "/run/b/s2");

	check_content(test_ctx, "/run/a/s", "A\n");
	check_content(test_ctx, "/run/b/s2", "B\n");

	/* A moved name re-anchors the relative target: /run/c has no t,
	 * so the moved name dangles — exactly like a real hardlinked
	 * symlink. */
	test_int_eq(th_sys(TAWC_SYS_renameat2, AT_FDCWD, "/run/b/s2",
			   AT_FDCWD, "/run/c/s3", 0, 0), 0);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/c/s3",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);
	char lnk[16] = {0};
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/c/s3",
			   lnk, sizeof lnk, 0, 0), 1);
	test_true(lnk[0] == 't');
	struct stat st;
	test_int_eq(lstat_guest("/run/c/s3", &st), 0);
	test_true(S_ISLNK(st.st_mode));
	test_int_eq((long)st.st_nlink, 2);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_nofollow_and_opath_opens_hit_object)
{
	th_view v;
	th_setup(&v, "ls-nfopen");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "nf-data\n");
	make_pair(test_ctx, "/run/f", "/run/l1");
	struct stat want;
	test_int_eq(lstat_guest("/run/f", &want), 0);

	/* Plain O_NOFOLLOW: a real hardlink name is a regular file, so
	 * the open must succeed (reactive ELOOP redirect). */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			 O_RDONLY | O_NOFOLLOW, 0, 0, 0);
	test_true(fd >= 0);
	char buf[32] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "nf-data\n");
	test_int_eq(close((int)fd), 0);

	/* O_PATH|O_NOFOLLOW: pre-probed — the fd must reference the
	 * OBJECT (regular file, shared inode), not the token symlink. */
	long pfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/l1",
			  O_PATH | O_NOFOLLOW, 0, 0, 0);
	test_true(pfd >= 0);
	struct stat pst;
	test_int_eq(th_sys(TAWC_SYS_fstat, pfd, &pst, 0, 0, 0, 0), 0);
	test_true(S_ISREG(pst.st_mode));
	test_true(pst.st_ino == want.st_ino);
	/* fd-based nlink reports the sidecar count — GNU tar CREATE
	 * registers hardlinks from the fstat of the fd it opened, so the
	 * kernel's nlink-1 here would dissolve hardlink structure at
	 * archive time. */
	test_int_eq((long)pst.st_nlink, 2);
	test_int_eq(close((int)pfd), 0);

	/* A symlink-object cluster still ELOOPs plain O_NOFOLLOW. */
	test_int_eq(th_sys(TAWC_SYS_symlinkat, "/run/f",
			   AT_FDCWD, "/run/s", 0, 0, 0), 0);
	make_pair(test_ctx, "/run/s", "/run/s2");
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/s2",
			   O_RDONLY | O_NOFOLLOW, 0, 0, 0), TAWC_ELOOP);
	/* ...but O_PATH|O_NOFOLLOW hands back the symlink object. */
	long sfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/s2",
			  O_PATH | O_NOFOLLOW, 0, 0, 0);
	test_true(sfd >= 0);
	test_int_eq(th_sys(TAWC_SYS_fstat, sfd, &pst, 0, 0, 0, 0), 0);
	test_true(S_ISLNK(pst.st_mode));
	test_int_eq(close((int)sfd), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_nofollow_utimensat_lands_on_object)
{
	th_view v;
	th_setup(&v, "ls-nftime");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "t\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	struct timespec ts[2] = { { 111111, 0 }, { 222222, 0 } };
	test_int_eq(th_sys(TAWC_SYS_utimensat, AT_FDCWD, "/run/f",
			   ts, AT_SYMLINK_NOFOLLOW, 0, 0), 0);

	/* The times must land on the shared OBJECT — visible through the
	 * other name, on both stat modes. */
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_int_eq((long)st.st_mtime, 222222);
	test_int_eq((long)st.st_atime, 111111);
	test_int_eq(th_sys(TAWC_SYS_fstatat, AT_FDCWD, "/run/l1",
			   &st, 0, 0, 0), 0);
	test_int_eq((long)st.st_mtime, 222222);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_proc_fd_spelled_linkat_source)
{
	th_view v;
	th_setup(&v, "ls-procfd");
	store_setup(&v);
	/* Identity /proc bind: the fd-spelling routes through a bind, so
	 * the resolver never sees the token — the O_PATH backstop must. */
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	write_file(test_ctx, "/run/f", "pf\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/f",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	char spell[64];
	snprintf(spell, sizeof spell, "/proc/self/fd/%ld", fd);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, spell,
			   AT_FDCWD, "/run/l2", 0x400 /*AT_SYMLINK_FOLLOW*/,
			   0), 0);
	test_int_eq(close((int)fd), 0);

	struct stat a, b;
	test_int_eq(lstat_guest("/run/f", &a), 0);
	test_int_eq(lstat_guest("/run/l2", &b), 0);
	test_true(S_ISREG(b.st_mode));
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 3);
	check_content(test_ctx, "/run/l2", "pf\n");
	/* One cluster: the backstop must ADD, never NEW (a NEW would have
	 * renamed the object out of link/, dangling the others). */
	test_int_eq(store_link_entries(), 2);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_empty_path_linkat_object_fd_adds)
{
	th_view v;
	th_setup(&v, "ls-emptyadd");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "ea\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/f",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	/* AT_EMPTY_PATH from an open object fd: pre-detected as
	 * store-resident (BEFORE any host attempt) → ADD. */
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/l3", AT_EMPTY_PATH, 0), 0);
	test_int_eq(close((int)fd), 0);

	struct stat st;
	test_int_eq(lstat_guest("/run/l3", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 3);
	check_content(test_ctx, "/run/l3", "ea\n");
	test_int_eq(store_link_entries(), 2);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_empty_path_linkat_named_source_news)
{
	th_view v;
	th_setup(&v, "ls-emptynew");
	store_setup(&v);

	write_file(test_ctx, "/run/g", "en\n");
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/g",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	/* Named in-view source: emulate NEW via the fd's host path. */
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/g2", AT_EMPTY_PATH, 0), 0);
	tawcroot_test_raw_hook = NULL;

	struct stat a, b;
	test_int_eq(lstat_guest("/run/g", &a), 0);
	test_int_eq(lstat_guest("/run/g2", &b), 0);
	test_true(S_ISREG(a.st_mode) && S_ISREG(b.st_mode));
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 2);

	/* The still-open fd references the SAME inode post-NEW (the
	 * rename preserved it): writes land in both names. */
	long wfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/g",
			  O_WRONLY | O_TRUNC, 0, 0, 0);
	test_true(wfd >= 0);
	test_true(write((int)wfd, "post\n", 5) == 5);
	test_int_eq(close((int)wfd), 0);
	check_content(test_ctx, "/run/g2", "post\n");
	struct stat fst;
	test_int_eq(th_sys(TAWC_SYS_fstat, fd, &fst, 0, 0, 0, 0), 0);
	test_true(fst.st_ino == a.st_ino);
	test_int_eq(close((int)fd), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_empty_path_linkat_memfd_exdev)
{
	th_view v;
	th_setup(&v, "ls-memfd");
	store_setup(&v);

	/* A nameless source (memfd: nlink 0) is a deliberate EXDEV until
	 * the tmp/ stage lands. Raw syscall: bionic doesn't declare
	 * memfd_create at our API level. */
	int mfd = (int)syscall(TAWC_SYS_memfd_create, "tawc-test", 0);
	test_true(mfd >= 0);
	test_true(write(mfd, "m\n", 2) == 2);
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, mfd, "",
			   AT_FDCWD, "/run/m", AT_EMPTY_PATH, 0),
		    TAWC_EXDEV);
	tawcroot_test_raw_hook = NULL;
	test_int_eq(close(mfd), 0);

	struct stat st;
	test_int_eq(lstat_guest("/run/m", &st), TAWC_ENOENT);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_latent_session_upgrades_on_token_read)
{
	th_view v;
	th_setup(&v, "ls-latent");
	store_setup(&v);  /* dir absent → LATENT */
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_LATENT);

	write_file(test_ctx, "/run/f", "lat\n");

	/* "Another process" mints the very first link (and the store):
	 * fork so THIS process's linkstore state stays LATENT — the
	 * classic shape is a shell whose `ln` child creates the store,
	 * then the shell itself reads the new name. */
	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		tawcroot_test_raw_hook = eperm_linkat_hook;
		tawcroot_handler_fn fn = tawcroot_dispatch_get(TAWC_SYS_linkat);
		tawcroot_syscall_args a = {
			.nr = TAWC_SYS_linkat, .a = AT_FDCWD, .b = (long)"/run/f",
			.c = AT_FDCWD, .d = (long)"/run/l1", .e = 0,
		};
		_exit(fn && fn(&a, NULL) == 0 ? 0 : 1);
	}
	int wst = -1;
	test_true(waitpid(pid, &wst, 0) == pid);
	test_true(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_LATENT);

	/* First token ENCOUNTER upgrades in place: reads work without a
	 * mutation or restart. */
	check_content(test_ctx, "/run/l1", "lat\n");
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_READY);
	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 2);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_latent_linkat_of_token_name_adds)
{
	th_view v;
	th_setup(&v, "ls-latlink");
	store_setup(&v);  /* dir absent → LATENT */

	write_file(test_ctx, "/run/f", "ll\n");

	/* Sibling process mints the store + first cluster; this process
	 * stays LATENT. */
	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		tawcroot_test_raw_hook = eperm_linkat_hook;
		tawcroot_handler_fn fn = tawcroot_dispatch_get(TAWC_SYS_linkat);
		tawcroot_syscall_args a = {
			.nr = TAWC_SYS_linkat, .a = AT_FDCWD, .b = (long)"/run/f",
			.c = AT_FDCWD, .d = (long)"/run/l1", .e = 0,
		};
		_exit(fn && fn(&a, NULL) == 0 ? 0 : 1);
	}
	int wst = -1;
	test_true(waitpid(pid, &wst, 0) == pid);
	test_true(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_LATENT);

	/* link(2) of the existing emulated name from the LATENT process
	 * must ADD to the same cluster — treating it as a plain symlink
	 * source would hardlink the token symlink itself (phantom
	 * referrer) or nest a token object (both names ENOENT). */
	make_pair(test_ctx, "/run/l1", "/run/l2");
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_READY);

	struct stat a, b;
	test_int_eq(lstat_guest("/run/f", &a), 0);
	test_int_eq(lstat_guest("/run/l2", &b), 0);
	test_true(S_ISREG(b.st_mode));
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 3);
	check_content(test_ctx, "/run/l2", "ll\n");
	/* One cluster: object + sidecar, no nested token object. */
	test_int_eq(store_link_entries(), 2);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_v1_fallback_bind_dst_symlink_target)
{
	th_view v;
	th_setup(&v, "ls-v1bind");
	/* No store configured: pure v1 fallback territory. */
	(void)th_add_bind(&v, "/usr/share/tawc");

	write_file(test_ctx, "/run/f", "v1b\n");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/usr/share/tawc/copy", 0, 0), 0);
	tawcroot_test_raw_hook = NULL;

	/* The back-symlink must carry the GUEST-absolute destination —
	 * "/" + suffix was bind-RELATIVE for a bind dst ("/copy"), and
	 * the original name dangled forever. */
	char lnk[128] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/run/f",
			lnk, sizeof lnk, 0, 0);
	test_true(n > 0);
	test_str_eq(lnk, "/usr/share/tawc/copy");
	check_content(test_ctx, "/run/f", "v1b\n");
	check_content(test_ctx, "/usr/share/tawc/copy", "v1b\n");

	th_teardown(&v);
}

test(linkstore_bind_operands_degrade)
{
	th_view v;
	th_setup(&v, "ls-bindop");
	store_setup(&v);
	(void)th_add_bind(&v, "/usr/share/tawc");

	/* NEW with a bind destination: v1, never a token symlink in the
	 * bind (the resolver skips binds — the data would be marooned). */
	write_file(test_ctx, "/run/f", "bo\n");
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/usr/share/tawc/b", 0, 0), 0);
	tawcroot_test_raw_hook = NULL;
	check_content(test_ctx, "/run/f", "bo\n");
	check_content(test_ctx, "/usr/share/tawc/b", "bo\n");
	test_true(store_link_entries() <= 0);  /* no cluster minted */

	/* ADD (emulated source) with a bind destination: EXDEV — tools
	 * fall back to copy; a token name in the bind would be a dead
	 * name. */
	write_file(test_ctx, "/run/g", "g\n");
	make_pair(test_ctx, "/run/g", "/run/g2");
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/g2",
			   AT_FDCWD, "/usr/share/tawc/c", 0, 0), TAWC_EXDEV);
	struct stat st;
	test_int_eq(lstat_guest("/run/g", &st), 0);
	test_int_eq((long)st.st_nlink, 2);

	store_teardown();
	th_teardown(&v);
}

/* Fail the sidecar WRITE-side open (EMFILE) while letting reads pass. */
static bool emfile_cnt_write_hook(long nr, const long args[6], long *ret)
{
	if (nr == TAWC_SYS_linkat) { *ret = TAWC_EPERM; return true; }
	if (nr == TAWC_SYS_openat) {
		const char *p = (const char *)args[1];
		size_t l = p ? strlen(p) : 0;
		if (l > 4 && strcmp(p + l - 4, ".cnt") == 0 &&
		    (args[2] & O_ACCMODE) == O_WRONLY) {
			*ret = TAWC_EMFILE;
			return true;
		}
	}
	return false;
}

test(linkstore_add_fails_when_count_cannot_land)
{
	th_view v;
	th_setup(&v, "ls-cntfail");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "cf\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	/* ADD whose count increment cannot be written must FAIL, not
	 * publish: count 2 with 3 live names is the undercount that
	 * deletes objects under live names two unlinks later. */
	tawcroot_test_raw_hook = emfile_cnt_write_hook;
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			   AT_FDCWD, "/run/l3", 0, 0), TAWC_EMFILE);
	tawcroot_test_raw_hook = NULL;

	struct stat st;
	test_int_eq(lstat_guest("/run/l3", &st), TAWC_ENOENT);
	test_int_eq(lstat_guest("/run/f", &st), 0);
	test_int_eq((long)st.st_nlink, 2);

	/* Store healthy afterwards: the same ADD succeeds. */
	make_pair(test_ctx, "/run/f", "/run/l3");
	test_int_eq(lstat_guest("/run/l3", &st), 0);
	test_int_eq((long)st.st_nlink, 3);

	store_teardown();
	th_teardown(&v);
}

/* Simulate a lock-free rename racing DEL: swap a DIRECTORY over the
 * name between the handler's probe and the park (the park happens
 * under the lock, so the hook fires the swap on the lock fcntl). */
static char g_swap_name[4700];
static char g_swap_aside[4700];
static int  g_swapped;
static bool dirswap_on_lock_hook(long nr, const long args[6], long *ret)
{
	(void)args; (void)ret;
	if (nr == TAWC_SYS_fcntl && args[1] == 7 /*F_SETLKW*/ &&
	    !g_swapped) {
		g_swapped = 1;
		(void)rename(g_swap_name, g_swap_aside);
		(void)mkdir(g_swap_name, 0755);
	}
	return false;
}

test(linkstore_unlink_racing_dir_swap_returns_eisdir)
{
	th_view v;
	th_setup(&v, "ls-dirswap");
	store_setup(&v);

	write_file(test_ctx, "/run/f", "ds\n");
	make_pair(test_ctx, "/run/f", "/run/l1");

	snprintf(g_swap_name, sizeof g_swap_name, "%s/run/l1", v.root);
	snprintf(g_swap_aside, sizeof g_swap_aside, "%s/run/l1.aside", v.root);
	g_swapped = 0;
	tawcroot_test_raw_hook = dirswap_on_lock_hook;
	/* The kernel's unlink of a directory is EISDIR and leaves it
	 * intact — the old code vanished the directory into work/. */
	test_int_eq(th_sys(TAWC_SYS_unlinkat, AT_FDCWD, "/run/l1",
			   0, 0, 0, 0), TAWC_EISDIR);
	tawcroot_test_raw_hook = NULL;

	struct stat st;
	test_int_eq(lstat_guest("/run/l1", &st), 0);
	test_true(S_ISDIR(st.st_mode));
	/* The moved token name still references the cluster; no
	 * decrement happened. */
	test_int_eq(lstat_guest("/run/l1.aside", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 2);
	/* Nothing stranded in work/. */
	char wp[4400];
	snprintf(wp, sizeof wp, "%s/work", g_store);
	test_int_eq(rh_count_dir_entries(wp), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_store_open_reserve_failure_leaks_no_fds)
{
	th_view v;
	th_setup(&v, "ls-resvfail");
	store_setup(&v);  /* LATENT */

	write_file(test_ctx, "/run/f", "rf\n");

	/* Force every tawcroot_fd_reserve to fail (table "full"): the
	 * first mutation's store_open must close all four dirfds it
	 * opened — the inverted cleanup gates leaked them into guest-
	 * reachable numbers. th_teardown's fd-leak check is the assert. */
	size_t saved = tawcroot_n_reserved_fds;
	tawcroot_n_reserved_fds = TAWCROOT_MAX_RESERVED_FDS;
	install_eperm_linkat();
	long rv = th_sys(TAWC_SYS_linkat, AT_FDCWD, "/run/f",
			 AT_FDCWD, "/run/l1", 0, 0);
	tawcroot_test_raw_hook = NULL;
	tawcroot_n_reserved_fds = saved;

	/* Store creation failed → v1 fallback kept the link working. */
	test_int_eq(rv, 0);
	check_content(test_ctx, "/run/l1", "rf\n");
	test_int_eq(tawcroot_linkstore_state(), TAWCROOT_STORE_OFF);

	store_teardown();
	th_teardown(&v);
}

/* ------------------------------------------------------------------ */
/* Stage 4: linkable O_TMPFILE objects (tmp/) + publish                */

#ifndef O_TMPFILE
# define O_TMPFILE (020000000 | O_DIRECTORY)
#endif

static int store_tmp_entries(void)
{
	char p[4400];
	snprintf(p, sizeof p, "%s/tmp", g_store);
	return rh_count_dir_entries(p);
}

test(linkstore_tmpfile_excl_passthrough)
{
	th_view v;
	th_setup(&v, "ls-tmpexcl");
	store_setup(&v);
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	/* O_EXCL = "will never link": pure passthrough, nlink 0 exact. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			 O_TMPFILE | O_EXCL | O_RDWR, 0600, 0, 0);
	test_true(fd >= 0);
	struct stat st;
	test_int_eq(th_sys(TAWC_SYS_fstat, fd, &st, 0, 0, 0, 0), 0);
	test_int_eq((long)st.st_nlink, 0);
	/* No store entry was created (the store may not even exist). */
	test_true(store_tmp_entries() <= 0);

	/* The magic-link publish spelling fails in-kernel like the real
	 * thing (nlink 0, not linkable → ENOENT). */
	char spell[64];
	snprintf(spell, sizeof spell, "/proc/self/fd/%ld", fd);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, spell,
			   AT_FDCWD, "/run/out", 0x400, 0), TAWC_ENOENT);
	/* AT_EMPTY_PATH lands on the documented anonymous-source EXDEV. */
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/out", AT_EMPTY_PATH, 0),
		    TAWC_EXDEV);
	test_int_eq(close((int)fd), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_tmpfile_publish)
{
	th_view v;
	th_setup(&v, "ls-tmppub");
	store_setup(&v);
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			 O_TMPFILE | O_RDWR, 0600, 0, 0);
	test_true(fd >= 0);
	test_true(write((int)fd, "pub\n", 4) == 4);
	test_int_eq(store_tmp_entries(), 1);

	/* Publish onto an existing name: linkat's EEXIST, temp intact. */
	write_file(test_ctx, "/run/existing", "x\n");
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/existing", AT_EMPTY_PATH, 0),
		    TAWC_EEXIST);
	test_int_eq(store_tmp_entries(), 1);

	/* Publish (AT_EMPTY_PATH spelling). */
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/out", AT_EMPTY_PATH, 0), 0);
	test_int_eq(store_tmp_entries(), 0);
	check_content(test_ctx, "/run/out", "pub\n");
	struct stat st;
	test_int_eq(lstat_guest("/run/out", &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)st.st_nlink, 1);

	/* The still-open fd references the same inode at its new home:
	 * post-publish writes land at the destination (copy-based
	 * emulation gets this wrong). */
	test_true(write((int)fd, "more\n", 5) == 5);
	check_content(test_ctx, "/run/out", "pub\nmore\n");

	/* A second link AFTER publish sees a named in-view source →
	 * normal emulation (NEW). */
	install_eperm_linkat();
	test_int_eq(th_sys(TAWC_SYS_linkat, fd, "",
			   AT_FDCWD, "/run/out2", AT_EMPTY_PATH, 0), 0);
	tawcroot_test_raw_hook = NULL;
	struct stat a, b;
	test_int_eq(lstat_guest("/run/out", &a), 0);
	test_int_eq(lstat_guest("/run/out2", &b), 0);
	test_true(a.st_ino == b.st_ino);
	test_int_eq((long)a.st_nlink, 2);
	test_int_eq(close((int)fd), 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_tmpfile_publish_magic_link_spelling)
{
	th_view v;
	th_setup(&v, "ls-tmpmag");
	store_setup(&v);
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			 O_TMPFILE | O_WRONLY, 0600, 0, 0);
	test_true(fd >= 0);
	test_true(write((int)fd, "mag\n", 4) == 4);

	char spell[64];
	snprintf(spell, sizeof spell, "/proc/self/fd/%ld", fd);
	test_int_eq(th_sys(TAWC_SYS_linkat, AT_FDCWD, spell,
			   AT_FDCWD, "/run/out", 0x400, 0), 0);
	test_int_eq(close((int)fd), 0);
	test_int_eq(store_tmp_entries(), 0);
	check_content(test_ctx, "/run/out", "mag\n");

	store_teardown();
	th_teardown(&v);
}

test(linkstore_tmpfile_open_error_shape)
{
	th_view v;
	th_setup(&v, "ls-tmperr");
	store_setup(&v);

	/* Kernel contract: O_TMPFILE needs write access... */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			   O_TMPFILE | O_RDONLY, 0600, 0, 0), TAWC_EINVAL);
	/* ...an existing directory operand... */
	write_file(test_ctx, "/run/f", "x\n");
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/f",
			   O_TMPFILE | O_RDWR, 0600, 0, 0), TAWC_ENOTDIR);
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/run/nope",
			   O_TMPFILE | O_RDWR, 0600, 0, 0), TAWC_ENOENT);
	/* No stray store entries from the failures. */
	test_true(store_tmp_entries() <= 0);

	store_teardown();
	th_teardown(&v);
}

test(linkstore_tmpfile_sweep_age_gated)
{
	th_view v;
	th_setup(&v, "ls-tmpsweep");
	store_setup(&v);

	/* A stray from a "previous session": entry aged out. */
	long stray = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			    O_TMPFILE | O_RDWR, 0600, 0, 0);
	test_true(stray >= 0);
	test_int_eq(close((int)stray), 0);
	{
		char tp[4400];
		snprintf(tp, sizeof tp, "%s/tmp", g_store);
		DIR *d = opendir(tp);
		test_nonnull(d);
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char ep[4500];
			snprintf(ep, sizeof ep, "%s/%s", tp, e->d_name);
			struct timespec old[2] = { { 1000, 0 }, { 1000, 0 } };
			test_int_eq(utimensat(AT_FDCWD, ep, old, 0), 0);
		}
		closedir(d);
	}

	/* A live temp (fresh mtime) must survive the sweep. */
	long live = th_sys(TAWC_SYS_openat, AT_FDCWD, "/run",
			   O_TMPFILE | O_RDWR, 0600, 0, 0);
	test_true(live >= 0);
	test_int_eq(store_tmp_entries(), 2);

	tawcroot_linkstore_tmp_sweep();
	test_int_eq(store_tmp_entries(), 1);

	/* The survivor is the live one: its publish still works. */
	test_int_eq(th_sys(TAWC_SYS_linkat, live, "",
			   AT_FDCWD, "/run/kept", AT_EMPTY_PATH, 0), 0);
	test_int_eq(close((int)live), 0);

	store_teardown();
	th_teardown(&v);
}
