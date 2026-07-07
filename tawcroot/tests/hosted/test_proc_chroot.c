/* Hosted handler-level tests for the /proc shadows (proc_shadow.c) and
 * chroot emulation (chroot.c). Both run their full production pipelines
 * in-process under ASan: the maps shadow's growable mmap read + rewrite
 * + memfd synthesis, and chroot's translate → reserve → bind-reanchor →
 * memo-rebuild sequence. See hosted.h. */

#include <cleat/test.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hosted.h"

#include "errno_neg.h"
#include "fdtab.h"
#include "path.h"
#include "proc_shadow.h"
#include "sysnr.h"

/* --- /proc shadows through the openat handler --------------------------- */

test(hosted_proc_self_maps_shadow_synthesized)
{
	th_view v;
	th_setup(&v, "proc-maps");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/proc/self/maps",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	/* The shadow is a fully-materialized memfd: the whole rewritten
	 * maps content must be readable and look like a maps file. */
	char buf[8192];
	size_t total = 0;
	long n;
	while ((n = read((int)fd, buf, sizeof buf - 1)) > 0) {
		if (total == 0) {
			buf[n] = 0;
			/* First chunk: starts with a hex address range. */
			test_nonnull(strchr(buf, '-'));
			test_nonnull(strstr(buf, " r"));
		}
		total += (size_t)n;
	}
	test_int_eq(n, 0);
	test_true(total > 0);

	/* memfd, not the real procfs file: lseek back works and the size
	 * is stable (procfs maps reads are generated per-read). */
	test_int_eq(lseek((int)fd, 0, SEEK_SET), 0);

	test_int_eq(close((int)fd), 0);
	th_teardown(&v);
}

test(hosted_proc_thread_self_maps_shadow_synthesized)
{
	th_view v;
	th_setup(&v, "proc-tsmaps");

	/* /proc/thread-self/maps is per-mm — identical content class to
	 * /proc/self/maps — and must hit the same shadow. Untranslated,
	 * it leaks raw host paths the guest view doesn't contain. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/proc/thread-self/maps",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);

	/* Discriminate shadow from a passed-through procfs open: the
	 * shadow is a fully-materialized memfd with a real size, while
	 * procfs maps files stat as size 0. */
	struct stat st;
	test_int_eq(fstat((int)fd, &st), 0);
	test_true(st.st_size > 0);

	char buf[512] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_nonnull(strchr(buf, '-'));

	test_int_eq(close((int)fd), 0);
	th_teardown(&v);
}

test(hosted_proc_overflowuid_shadow_content)
{
	th_view v;
	th_setup(&v, "proc-ovfl");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD,
			 "/proc/sys/kernel/overflowuid", O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[16] = {0};
	test_int_eq(read((int)fd, buf, sizeof buf - 1), 6);
	test_str_eq(buf, "65534\n");
	test_int_eq(close((int)fd), 0);

	long gfd = th_sys(TAWC_SYS_openat, AT_FDCWD,
			  "/proc/sys/kernel/overflowgid", O_RDONLY, 0, 0, 0);
	test_true(gfd >= 0);
	memset(buf, 0, sizeof buf);
	test_int_eq(read((int)gfd, buf, sizeof buf - 1), 6);
	test_str_eq(buf, "65534\n");
	test_int_eq(close((int)gfd), 0);

	th_teardown(&v);
}

test(hosted_proc_bus_pci_devices_shadow_empty)
{
	th_view v;
	th_setup(&v, "proc-pci");

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/proc/bus/pci/devices",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[8];
	test_int_eq(read((int)fd, buf, sizeof buf), 0);  /* empty file */
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_proc_self_exe_classifier)
{
	th_view v;
	th_setup(&v, "proc-exe");

	test_int_eq(tawcroot_is_proc_self_exe("/proc/self/exe"), 1);
	char own[64];
	snprintf(own, sizeof own, "/proc/%d/exe", getpid());
	test_int_eq(tawcroot_is_proc_self_exe(own), 1);
	snprintf(own, sizeof own, "/proc/self/task/%d/exe", getpid());
	test_int_eq(tawcroot_is_proc_self_exe(own), 1);

	test_int_eq(tawcroot_is_proc_self_exe("/proc/self/exec"), 0);
	test_int_eq(tawcroot_is_proc_self_exe("/proc/self"), 0);
	test_int_eq(tawcroot_is_proc_self_exe("/proc/1/exe"), 0);
	test_int_eq(tawcroot_is_proc_self_exe("/etc/exe"), 0);

	/* exe classifier: another process's exe link is OTHER, not NONE —
	 * the readlink handler uses this to suppress the result-equality
	 * substitution for cross-process links. */
	test_int_eq(tawcroot_proc_exe_classify("/proc/self/exe"),
		    TAWCROOT_PROC_EXE_SELF);
	test_int_eq(tawcroot_proc_exe_classify("/proc/1/exe"),
		    TAWCROOT_PROC_EXE_OTHER);
	test_int_eq(tawcroot_proc_exe_classify("/proc/1/task/1/exe"),
		    TAWCROOT_PROC_EXE_OTHER);
	test_int_eq(tawcroot_proc_exe_classify("/proc/self/exec"),
		    TAWCROOT_PROC_EXE_NONE);
	test_int_eq(tawcroot_proc_exe_classify("/etc/exe"),
		    TAWCROOT_PROC_EXE_NONE);

	/* cwd classifier mirrors the exe one (self-only). */
	test_int_eq(tawcroot_is_proc_self_cwd("/proc/self/cwd"), 1);
	snprintf(own, sizeof own, "/proc/%d/cwd", getpid());
	test_int_eq(tawcroot_is_proc_self_cwd(own), 1);
	test_int_eq(tawcroot_is_proc_self_cwd("/proc/1/cwd"), 0);
	test_int_eq(tawcroot_is_proc_self_cwd("/proc/self/cwd2"), 0);
	test_int_eq(tawcroot_is_proc_self_cwd("/proc/self/root"), 0);

	th_teardown(&v);
}

test(hosted_readlink_proc_self_cwd_returns_guest_path)
{
	th_view v;
	th_setup(&v, "proc-rdcwd");

	char p[4200];
	snprintf(p, sizeof p, "%s/etc/sub", v.root);
	test_int_eq(chdir(p), 0);

	char buf[256] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/cwd",
			buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");

	/* Numeric-pid form. */
	char own[64];
	snprintf(own, sizeof own, "/proc/%d/cwd", getpid());
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, own,
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");

	/* Fd-relative form: readlinkat(<fd of /proc/self>, "cwd"). */
	int pfd = open("/proc/self", O_PATH | O_DIRECTORY | O_CLOEXEC);
	test_true(pfd >= 0);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, pfd, "cwd", buf, sizeof buf - 1,
		   0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");
	test_int_eq(close(pfd), 0);

	/* Truncation contract matches readlink(2): short buffer gets a
	 * partial, non-NUL-padded copy. */
	char tiny[6];
	memset(tiny, 'Z', sizeof tiny);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/cwd",
		   tiny, 4, 0, 0);
	test_int_eq(n, 4);
	test_true(memcmp(tiny, "/etc", 4) == 0);
	test_true(tiny[4] == 'Z');  /* untouched past bufsiz */

	/* Kernel cwd outside the view: refuse (like getcwd), don't leak
	 * the host path. */
	test_int_eq(chdir("/"), 0);
	test_int_eq(th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/cwd",
			   buf, sizeof buf - 1, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

/* /proc/<pid>/fd/<n> magic links resolve to HOST paths; in-view targets
 * must come back reverse-translated as guest paths. Bun's realpath
 * (Claude Code et al.) canonicalizes its cwd via open(dir) +
 * readlink(/proc/self/fd/<n>), then ENOENT'd on the leaked host path.
 * Outside-view targets ("pipe:[...]", app-private files) pass through
 * verbatim — fd links legitimately point outside the view. */
test(hosted_readlink_proc_fd_link_returns_guest_path)
{
	th_view v;
	th_setup(&v, "proc-rdfd");

	/* Classifier shapes. Any pid is accepted (sibling tawcroot
	 * processes share the view; the prefix walk is a no-op for
	 * foreign targets). */
	test_int_eq(tawcroot_is_proc_fd_link("/proc/self/fd/3"), 1);
	test_int_eq(tawcroot_is_proc_fd_link("/proc/1/fd/44"), 1);
	char own[64];
	snprintf(own, sizeof own, "/proc/%d/fd/0", getpid());
	test_int_eq(tawcroot_is_proc_fd_link(own), 1);
	test_int_eq(tawcroot_is_proc_fd_link("/proc/self/fd"), 0);
	test_int_eq(tawcroot_is_proc_fd_link("/proc/self/fd/"), 0);
	test_int_eq(tawcroot_is_proc_fd_link("/proc/self/fd/3x"), 0);
	test_int_eq(tawcroot_is_proc_fd_link("/proc/self/fdinfo/3"), 0);
	test_int_eq(tawcroot_is_proc_fd_link("/etc/fd/3"), 0);

	/* The single-kind helpers are wrappers over the one-pass
	 * classifier; spot-check its raw answers too. */
	test_int_eq(tawcroot_proc_link_classify("/proc/self/fd/3"),
		    TAWCROOT_PROC_LINK_FD);
	test_int_eq(tawcroot_proc_link_classify("/proc/self/cwd"),
		    TAWCROOT_PROC_LINK_CWD);
	test_int_eq(tawcroot_proc_link_classify("/proc/1/cwd"),
		    TAWCROOT_PROC_LINK_NONE);
	test_int_eq(tawcroot_proc_link_classify("/proc/self/exe"),
		    TAWCROOT_PROC_LINK_EXE_SELF);
	test_int_eq(tawcroot_proc_link_classify("/proc/1/exe"),
		    TAWCROOT_PROC_LINK_EXE_OTHER);
	test_int_eq(tawcroot_proc_link_classify("/proc/self/maps"),
		    TAWCROOT_PROC_LINK_NONE);

	/* Route guest /proc to the host's (in production /proc is a
	 * bind) so the translated readlink reaches the real magic link. */
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	/* In-view directory fd: link reads back as the GUEST path. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/sub",
			 O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(fd >= 0);
	char link[64];
	snprintf(link, sizeof link, "/proc/self/fd/%ld", fd);
	char buf[256] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
			buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");

	/* Numeric-pid form. */
	snprintf(link, sizeof link, "/proc/%d/fd/%ld", getpid(), fd);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");

	/* Fd-relative form, as `ls -l /proc/self/fd` issues it:
	 * readlinkat(<fd of /proc/self/fd>, "<n>"). */
	int pfd = open("/proc/self/fd", O_PATH | O_DIRECTORY | O_CLOEXEC);
	test_true(pfd >= 0);
	char leaf[16];
	snprintf(leaf, sizeof leaf, "%ld", fd);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, pfd, leaf, buf, sizeof buf - 1,
		   0, 0);
	test_int_eq(n, (long)strlen("/etc/sub"));
	test_str_eq(buf, "/etc/sub");
	test_int_eq(close(pfd), 0);
	test_int_eq(close((int)fd), 0);

	/* Rootfs root itself reads back as "/". */
	long rfd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/",
			  O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(rfd >= 0);
	snprintf(link, sizeof link, "/proc/self/fd/%ld", rfd);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, 1);
	test_str_eq(buf, "/");
	test_int_eq(close((int)rfd), 0);

	/* Bind-source fd: link reads back as the bind DST. */
	const char *bsrc = th_add_bind(&v, "/system");
	int bfd = open(bsrc, O_PATH | O_DIRECTORY | O_CLOEXEC);
	test_true(bfd >= 0);
	snprintf(link, sizeof link, "/proc/self/fd/%d", bfd);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/system"));
	test_str_eq(buf, "/system");
	test_int_eq(close(bfd), 0);

	/* Outside-view target: kernel bytes pass through verbatim (a
	 * pipe's link text isn't a path at all). */
	int pp[2];
	test_int_eq(pipe(pp), 0);
	snprintf(link, sizeof link, "/proc/self/fd/%d", pp[0]);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
		   buf, sizeof buf - 1, 0, 0);
	test_true(n > 5);
	test_true(memcmp(buf, "pipe:", 5) == 0);
	test_int_eq(close(pp[0]), 0);
	test_int_eq(close(pp[1]), 0);

	th_teardown(&v);
}

/* Cross-process exe links must NOT be substituted with the caller's
 * guest exe path. Every tawcroot guest's kernel exe is the same
 * libtawcroot.so, so the handler's readlink-result equality check alone
 * matches for another guest's /proc/<pid>/exe too; the request-path
 * classification has to suppress it (observed in the field: `ls`
 * reading bash's exe link got "/usr/bin/ls"). Simulated here with a
 * forked child (same kernel exe as us) and tawcroot_self_host_path set
 * to our real exe. */
test(hosted_readlink_other_process_exe_not_substituted)
{
	th_view v;
	th_setup(&v, "proc-oexe");

	/* Route guest /proc to the host's so /proc/<pid>/exe is reachable
	 * through translation (in production /proc is a bind). */
	test_int_eq(tawcroot_path_add_bind("/proc", "/proc"), 0);

	char real_exe[4096];
	long rn = readlink("/proc/self/exe", real_exe, sizeof real_exe - 1);
	test_true(rn > 0);
	real_exe[rn] = 0;
	memcpy(tawcroot_self_host_path, real_exe, (size_t)rn + 1);
	tawcroot_self_host_path_len = (size_t)rn;
	tawcroot_set_guest_exe_path("/usr/bin/guest-prog");

	pid_t child = fork();
	test_true(child >= 0);
	if (child == 0) {
		pause();
		_exit(0);
	}

	/* Another process's exe link: kernel bytes pass through verbatim
	 * (consistent with cross-process /proc in general), no
	 * substitution. */
	char link[64];
	snprintf(link, sizeof link, "/proc/%d/exe", (int)child);
	char buf[4096] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, link,
			buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, rn);
	test_str_eq(buf, real_exe);

	/* ... while a non-/proc symlink whose target happens to be our
	 * exe still gets the substitution (that's the surface protecting
	 * readlink("/proc/self/fd/<n>") and O_PATH empty-path reads). */
	char lp[4300];
	snprintf(lp, sizeof lp, "%s/etc/exelink", v.root);
	test_int_eq(symlink(real_exe, lp), 0);
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/etc/exelink",
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/usr/bin/guest-prog"));
	test_str_eq(buf, "/usr/bin/guest-prog");

	/* And our own exe link still synthesizes from the stash. */
	memset(buf, 0, sizeof buf);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/exe",
		   buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/usr/bin/guest-prog"));
	test_str_eq(buf, "/usr/bin/guest-prog");

	test_int_eq(kill(child, SIGKILL), 0);
	test_int_eq(waitpid(child, NULL, 0), child);

	tawcroot_self_host_path[0]  = 0;
	tawcroot_self_host_path_len = 0;
	th_teardown(&v);
}

test(hosted_readlink_proc_self_exe_returns_guest_path)
{
	th_view v;
	th_setup(&v, "proc-rdexe");

	tawcroot_set_guest_exe_path("/usr/bin/guest-prog");

	char buf[128] = {0};
	long n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/exe",
			buf, sizeof buf - 1, 0, 0);
	test_int_eq(n, (long)strlen("/usr/bin/guest-prog"));
	test_str_eq(buf, "/usr/bin/guest-prog");

	/* Truncation contract matches readlink(2): short buffer gets a
	 * partial, non-NUL-padded copy. */
	char tiny[6];
	memset(tiny, 'Z', sizeof tiny);
	n = th_sys(TAWC_SYS_readlinkat, AT_FDCWD, "/proc/self/exe",
		   tiny, 4, 0, 0);
	test_int_eq(n, 4);
	test_true(memcmp(tiny, "/usr", 4) == 0);
	test_true(tiny[4] == 'Z');  /* untouched past bufsiz */

	tawcroot_set_guest_exe_path(NULL);
	th_teardown(&v);
}

/* --- chroot emulation ---------------------------------------------------- */

test(hosted_chroot_swaps_root_view)
{
	th_view v;
	th_setup(&v, "chroot");

	size_t reserved_before = tawcroot_n_reserved_fds;
	int old_root = tawcroot_rootfs_fd;

	test_int_eq(th_sys(TAWC_SYS_chroot, "/usr", 0, 0, 0, 0, 0), 0);

	/* New view: /usr is the root, so /lib/probe.so is now the real
	 * directory <rootfs>/usr/lib (not the memoized symlink). */
	test_true(tawcroot_rootfs_fd != old_root);
	test_int_eq((long)tawcroot_n_reserved_fds, (long)reserved_before + 1);

	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/lib/probe.so",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[16] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "fake-lib\n");
	test_int_eq(close((int)fd), 0);

	/* The old view's /etc is gone. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_chroot_rebuilds_memo_for_new_root)
{
	th_view v;
	th_setup(&v, "chroot-memo");

	/* Give the new root (/usr) its own well-known symlink:
	 * <rootfs>/usr/lib64 -> lib. Created host-side, pre-chroot. */
	char p[4200];
	snprintf(p, sizeof p, "%s/usr/lib64", v.root);
	test_int_eq(symlink("lib", p), 0);

	test_int_eq(th_sys(TAWC_SYS_chroot, "/usr", 0, 0, 0, 0, 0), 0);

	/* /lib64/probe.so must route through the rebuilt memo to
	 * /lib/probe.so inside the new root. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/lib64/probe.so",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[16] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "fake-lib\n");
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}

test(hosted_chroot_reanchors_binds)
{
	th_view v;
	th_setup(&v, "chroot-bind");
	th_add_bind(&v, "/usr/host");

	/* Pre-chroot: routed under /usr/host. */
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/usr/host/probe.txt",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	test_int_eq(th_sys(TAWC_SYS_chroot, "/usr", 0, 0, 0, 0, 0), 0);

	/* Post-chroot the bind dst is re-anchored to /host. */
	fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/host/probe.txt",
		    O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	char buf[16] = {0};
	test_true(read((int)fd, buf, sizeof buf - 1) > 0);
	test_str_eq(buf, "from-bind\n");
	test_int_eq(close((int)fd), 0);

	/* The old dst path no longer routes anywhere. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/usr/host/probe.txt",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);

	th_teardown(&v);
}

test(hosted_chroot_bind_outside_new_root_deactivated)
{
	th_view v;
	th_setup(&v, "chroot-bind2");
	th_add_bind(&v, "/mnt/outside");  /* not under /usr */

	test_int_eq(th_sys(TAWC_SYS_chroot, "/usr", 0, 0, 0, 0, 0), 0);

	/* The bind sat outside the new view: deactivated, not routed. */
	test_int_eq(th_sys(TAWC_SYS_openat, AT_FDCWD, "/mnt/outside/probe.txt",
			   O_RDONLY, 0, 0, 0), TAWC_ENOENT);
	test_int_eq(tawcroot_binds[0].active, 0);

	th_teardown(&v);
}

test(hosted_chroot_failure_leaves_view_untouched)
{
	th_view v;
	th_setup(&v, "chroot-fail");

	int old_root = tawcroot_rootfs_fd;
	size_t reserved_before = tawcroot_n_reserved_fds;

	test_int_eq(th_sys(TAWC_SYS_chroot, "/no/such/dir", 0, 0, 0, 0, 0),
		    TAWC_ENOENT);
	test_int_eq(th_sys(TAWC_SYS_chroot, "/etc/probe", 0, 0, 0, 0, 0),
		    TAWC_ENOTDIR);
	test_int_eq(th_sys(TAWC_SYS_chroot, 0x1000L, 0, 0, 0, 0, 0),
		    TAWC_EFAULT);

	test_int_eq(tawcroot_rootfs_fd, old_root);
	test_int_eq((long)tawcroot_n_reserved_fds, (long)reserved_before);
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			 O_RDONLY, 0, 0, 0);
	test_true(fd >= 0);
	test_int_eq(close((int)fd), 0);

	th_teardown(&v);
}
