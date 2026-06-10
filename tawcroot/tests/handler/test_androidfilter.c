/* Handler-layer tests under a synthesized Android-`untrusted_app` seccomp
 * prefilter.
 *
 * Same shape as test_rootfs_syscalls_smoke.c: build a fake rootfs, fork tawcroot-testhost
 * with `-r <rootfs>`, parse [ok ]/[FAIL] lines into cleat tests. The only
 * difference is the wrapper binary (TAWCROOT_ANDROID_FILTER_WRAP) is
 * prepended to the argv, so the testhost runs with an Android-shape stacked
 * filter layered under tawcroot's own filter.
 *
 * What this catches that the bare rootfs syscall suite doesn't:
 *
 *   - openat2 / faccessat2 / clone3 / close_range RET_TRAP from the outer
 *     filter must be caught by tawcroot's SIGSYS handler. Bug shapes:
 *       * tawcroot's openat2 probe runs before install_handler -> killed
 *         (notes/tawcroot.md "Bugs found and fixed during install pipeline
 *         validation"). Reproducer here would be a dead testhost.
 *       * tawcroot's handler issues NR 439 (faccessat2) recursively from
 *         inside SIGSYS -> recursive trap -> killed. Reproducer here would
 *         be a dead testhost on any access() call.
 *       * tawcroot doesn't trap clone3 / close_range / openat2 in its own
 *         dispatch -> Android's TRAP delivers SIGSYS, tawcroot's NULL-
 *         dispatch path returns -ENOSYS, glibc / guests fall back cleanly.
 *
 *   - On x86_64 with --include-legacy-x86_64: every legacy access/open/
 *     chmod/chown/mkdir/rmdir/unlink/symlink/link/rename/readlink/stat/
 *     lstat from the testhost will TRAP via the wrapper. tawcroot's
 *     legacy-x86_64 wrappers must route them through *at variants. Same
 *     shape as the proot-on-Android lp64-access bug (notes/proot.md
 *     "Why upstream proot doesn't work on Android x86_64").
 *
 * Process tree:
 *
 *   cleat orchestrator (host glibc)
 *     -> wrap (host glibc, installs Android-shape seccomp filter)
 *          -> tawcroot-testhost (freestanding, installs its own filter +
 *             SIGSYS handler, runs the rootfs syscall checks)
 *
 * The wrapper does NOT install a SIGSYS handler. Default disposition for
 * SIGSYS is process termination, mimicking Android's behavior pre-tawcroot:
 * any trapped syscall before the testhost installs its handler kills the
 * process. So tawcroot's init order (handler before any trapped syscall)
 * is verified end-to-end by the test surviving startup at all.
 */

#define _GNU_SOURCE

#include <cleat/test.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "steps.h"

#ifndef TAWCROOT_ANDROID_FILTER_WRAP
# error "TAWCROOT_ANDROID_FILTER_WRAP must be passed via -D from tawcroot/Makefile"
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define FAKE_ROOTFS         TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-androidfilter"
#define FAKE_ROOTFS_SIBLING TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-androidfilter-evil"
#define FAKE_BINDSRC        TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-androidfilter-bindsrc"

static bool write_file(const char *path, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	size_t n = strlen(contents);
	bool ok = (write(fd, contents, n) == (ssize_t)n);
	close(fd);
	return ok;
}

static void rmrf(const char *path)
{
	char cmd[PATH_MAX + 32];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
	(void)!system(cmd);
}

static bool build_fake_rootfs(const char *root)
{
	char path[PATH_MAX];

	if (mkdir(root, 0755) < 0 && errno != EEXIST) return false;
#define MKD(sub) do { \
		snprintf(path, sizeof path, "%s/" sub, root); \
		if (mkdir(path, 0755) < 0 && errno != EEXIST) return false; \
	} while (0)
	MKD("etc");
	MKD("usr");
	MKD("usr/lib");
	MKD("run");
#undef MKD

	snprintf(path, sizeof path, "%s/etc/probe", root);
	if (!write_file(path, "hello-from-rootfs\n")) return false;

	snprintf(path, sizeof path, "%s/usr/lib/probe.so", root);
	if (!write_file(path, "libprobe-data\n")) return false;

	snprintf(path, sizeof path, "%s/utime-target", root);
	if (!write_file(path, "utime-target-data\n")) return false;

#define SYMLINK(target, sub) do { \
		snprintf(path, sizeof path, "%s/" sub, root); \
		(void)unlink(path); \
		if (symlink(target, path) < 0) return false; \
	} while (0)
	SYMLINK("usr/lib",     "lib");
	SYMLINK("usr/lib",     "lib64");
	SYMLINK("/etc/passwd", "etc/host-secret");
	SYMLINK("/etc/probe",  "altpath");
	SYMLINK("chain2",      "chain1");
	SYMLINK("chain3",      "chain2");
	SYMLINK("etc/probe",   "chain3");
	SYMLINK("loop",        "loop");
	SYMLINK("utime-target", "utime-link");
	/* Visible AFTER chroot("/usr") for the chroot-symlink-follow test. */
	SYMLINK("lib",         "usr/sublink");
	/* Trailing-slash semantics fixtures — see
	 * test_rootfs_syscalls_smoke.c for the rationale. */
	SYMLINK("etc",         "etcdir-link");
	SYMLINK("/proc",       "procesc");
#undef SYMLINK

	return true;
}

static bool build_sibling_evil(const char *path)
{
	char p[PATH_MAX];
	if (mkdir(path, 0755) < 0 && errno != EEXIST) return false;
	snprintf(p, sizeof p, "%s/foo", path);
	if (mkdir(p, 0755) < 0 && errno != EEXIST) return false;
	snprintf(p, sizeof p, "%s/foo/host-secret", path);
	return write_file(p, "should-never-be-reachable\n");
}

static bool build_bindsrc(const char *path)
{
	char p[PATH_MAX];
	if (mkdir(path, 0755) < 0 && errno != EEXIST) return false;
	snprintf(p, sizeof p, "%s/probe.txt", path);
	return write_file(p, "from-bind-src\n");
}

register_dynamic_tests
{
	rmrf(FAKE_ROOTFS);
	rmrf(FAKE_ROOTFS_SIBLING);
	rmrf(FAKE_BINDSRC);
	if (!build_fake_rootfs(FAKE_ROOTFS)) {
		register_test_problem(
			c_sv("androidfilter"), c_sv("fake_rootfs_setup"),
			cstr_from_fmt("failed to build fake rootfs at %s: %s",
				      FAKE_ROOTFS, strerror(errno)));
		return;
	}
	if (!build_sibling_evil(FAKE_ROOTFS_SIBLING)) {
		register_test_problem(
			c_sv("androidfilter"), c_sv("fake_rootfs_sibling_setup"),
			cstr_from_fmt("failed to build sibling at %s: %s",
				      FAKE_ROOTFS_SIBLING, strerror(errno)));
		return;
	}
	if (!build_bindsrc(FAKE_BINDSRC)) {
		register_test_problem(
			c_sv("androidfilter"), c_sv("fake_bindsrc_setup"),
			cstr_from_fmt("failed to build bindsrc at %s: %s",
				      FAKE_BINDSRC, strerror(errno)));
		return;
	}

	/* Default wrapper config: trap the kernel-version-gated NRs that
	 * appear on every arch (openat2 / faccessat2 / clone3 / close_range).
	 * This is the minimum set that catches all the SIGSYS-recursion
	 * and probe-ordering bugs from notes/tawcroot.md. */
	const char *prefix_default[] = {
		TAWCROOT_ANDROID_FILTER_WRAP,
		"--",
		NULL,
	};
	const char *args[] = {
		"-r", FAKE_ROOTFS,
		"-b", FAKE_BINDSRC ":lib64",
		"-b", FAKE_BINDSRC ":usr/test-bind",
		/* Mirror test_rootfs_syscalls_smoke.c: expose host /dev so
		 * test_ioctl_pty_translation can allocate a real pty. */
		"-b", "/dev:dev",
		NULL
	};
	steps_register_from_testhost_prefixed(c_sv("androidfilter"),
	                                      prefix_default, args);

#if defined(__x86_64__)
	/* On x86_64, also run with the legacy lp64-Android trap set
	 * enabled. tawcroot's legacy wrappers must route every legacy NR
	 * to its *at sibling so the trap is harmless to guests. */
	const char *prefix_legacy[] = {
		TAWCROOT_ANDROID_FILTER_WRAP,
		"--include-legacy-x86_64",
		"--",
		NULL,
	};
	steps_register_from_testhost_prefixed(c_sv("androidfilter_legacy"),
	                                      prefix_legacy, args);
#endif

	/* Same shape, but with Android's TCGETS2 xperm-EACCES gap
	 * simulated. test_ioctl_pty_translation only meaningfully
	 * exercises the bypass under this wrapper — without it, the
	 * host kernel happily fulfils TCGETS2 and the translator's
	 * effort is invisible. The other rootfs syscall tests must still pass
	 * untouched: they don't touch termios2 cmds.
	 *
	 * Both arches: the xperm-EACCES rule is keyed on the (arch-
	 * independent) ioctl cmd number, not the syscall number. */
	const char *prefix_xperm[] = {
		TAWCROOT_ANDROID_FILTER_WRAP,
		"--xperm-tcgets2",
		"--",
		NULL,
	};
	steps_register_from_testhost_prefixed(c_sv("androidfilter_xperm"),
	                                      prefix_xperm, args);

	rmrf(FAKE_ROOTFS);
	rmrf(FAKE_ROOTFS_SIBLING);
	rmrf(FAKE_BINDSRC);
}
