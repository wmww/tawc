/* Hosted handler-level tests for the /proc shadows (proc_shadow.c) and
 * chroot emulation (chroot.c). Both run their full production pipelines
 * in-process under ASan: the maps shadow's growable mmap read + rewrite
 * + memfd synthesis, and chroot's translate → reserve → bind-reanchor →
 * memo-rebuild sequence. See hosted.h. */

#include <cleat/test.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
