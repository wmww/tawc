/* Handler-layer tests: rootfs syscall translation + runtime invariants.
 *
 * Sets up a minimal fake rootfs, forks `tawcroot-testhost -r <rootfs>`,
 * and registers one cleat test per `[ok ]` / `[FAIL]` line in its output.
 *
 * The testhost installs the dispatch table and issues
 * guest-perspective syscalls via inline asm so each one's IP traps into
 * the SIGSYS handler. The handler dispatches to the registered fs/identity
 * functions; the smoke checks each result via `tawc_io_step`. We surface
 * those checks individually via cleat.
 *
 * Coverage (per `tests/testhost/src/rootfs_smoke.c`, ~50+ checks):
 *   - getuid/geteuid/getgid/getegid -> 0 (fake-root identity)
 *   - openat absolute path translates to <rootfs>/etc/probe
 *   - openat2 RESOLVE_IN_ROOT clamps absolute-symlink escape (host-secret)
 *   - close_range can't kill internal fds (rootfs_fd, bind src_fds)
 *   - guest sigaction(SIGSYS) is shadowed; kernel disposition unchanged
 *   - guest sigprocmask can't actually block SIGSYS
 *   - guest seccomp installation denied with -EPERM
 *   - guest dup/dup2/dup3/fcntl(F_DUPFD) cap below TAWCROOT_RESERVED_FD_BASE
 *   - statx fake-root preserves stx_uid/stx_gid == 0
 *   - linkat falls back to symlink, renameat2/truncate/cwd round-trips, …
 *
 * See notes/tawcroot.md "Phase 1 -- MVP path translation" and
 * "Phase 0.5 -- runtime invariants" for the historical bring-up labels.
 */

#define _GNU_SOURCE  /* symlink(), PATH_MAX in some glibc layouts */

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

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define FAKE_ROOTFS         TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-syscalls"
/* Sibling that shares the rootfs as a byte prefix. Exercises the
 * resolve_relative / handle_getcwd boundary check (review finding B4):
 * "<rootfs>-evil" must NOT be treated as inside the rootfs. */
#define FAKE_ROOTFS_SIBLING TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-syscalls-evil"
/* Bind source for the bind-vs-memo precedence test (review finding B5):
 * binds must win over rootfs-side symlink memos. */
#define FAKE_BINDSRC        TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-syscalls-bindsrc"

static bool write_file(const char *path, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	size_t n = strlen(contents);
	bool ok = (write(fd, contents, n) == (ssize_t)n);
	close(fd);
	return ok;
}

/* Recursive rmrf via shell -- predictable trees, single-digit files,
 * no need for ftw. */
static void rmrf(const char *path)
{
	char cmd[PATH_MAX + 32];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
	(void)!system(cmd);
}

/* Build the minimal rootfs the rootfs syscall smoke expects:
 *   <root>/etc/probe          -- "hello-from-rootfs"
 *   <root>/usr/lib/probe.so   -- "libprobe-data"
 *   <root>/lib                -- symlink -> usr/lib
 *   <root>/etc/host-secret    -- symlink -> /etc/passwd (escape attempt)
 *   <root>/altpath            -- symlink -> /etc/probe (abs target in rootfs)
 *   <root>/chain1             -- symlink -> chain2
 *   <root>/chain2             -- symlink -> chain3
 *   <root>/chain3             -- symlink -> etc/probe (3-hop relative chain)
 *   <root>/loop               -- symlink -> loop (self-loop, must ELOOP)
 *   <root>/run                -- empty dir
 *   <root>/utime-target       -- regular file (utimensat reg-file probe)
 *   <root>/utime-link         -- symlink -> utime-target (lutimes probe;
 *                                exercises AT_SYMLINK_NOFOLLOW path mode —
 *                                pre-fix the resolver followed through and
 *                                the kernel ended up touching the target)
 *   <root>/etcdir-link        -- symlink -> etc (trailing-slash follow probe)
 *   <root>/procesc            -- symlink -> /proc (host-exists/rootfs-missing
 *                                target; trailing-slash containment probe)
 */
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
	/* Second memoized symlink so the bind-vs-memo precedence test
	 * (review finding B5) can use /lib64 without disturbing the /lib
	 * suppression-mode test that uses /lib. */
	SYMLINK("usr/lib",     "lib64");
	/* Symlink visible AFTER chroot("/usr") — the chroot-symlink-follow
	 * test re-chroots through this. Target "lib" is rel-to-dirname,
	 * so from <rootfs>/usr/sublink it resolves to <rootfs>/usr/lib. */
	SYMLINK("lib",         "usr/sublink");
	SYMLINK("/etc/passwd", "etc/host-secret");
	SYMLINK("/etc/probe",  "altpath");
	SYMLINK("chain2",      "chain1");
	SYMLINK("chain3",      "chain2");
	SYMLINK("etc/probe",   "chain3");
	SYMLINK("loop",        "loop");
	SYMLINK("utime-target", "utime-link");
	/* Trailing-slash semantics fixtures: a relative symlink to a dir
	 * (kernel follows it for "etcdir-link/"), and an absolute symlink
	 * whose target exists on the HOST but not in the rootfs — if the
	 * trailing-slash leaf-follow ever bypasses the resolver's clamp,
	 * "procesc/" resolves against the host /proc and the ENOENT
	 * containment check fails. */
	SYMLINK("etc",         "etcdir-link");
	SYMLINK("/proc",       "procesc");
#undef SYMLINK

	return true;
}

/* Build the prefix-boundary sibling: <root>-evil/foo with a known file
 * inside, so a chdir into it (issued by the testhost via raw stub) lands
 * the kernel cwd somewhere whose path-bytes share the rootfs prefix but
 * is not actually inside the rootfs. */
static bool build_sibling_evil(const char *path)
{
	char p[PATH_MAX];
	if (mkdir(path, 0755) < 0 && errno != EEXIST) return false;
	snprintf(p, sizeof p, "%s/foo", path);
	if (mkdir(p, 0755) < 0 && errno != EEXIST) return false;
	snprintf(p, sizeof p, "%s/foo/host-secret", path);
	return write_file(p, "should-never-be-reachable\n");
}

/* Bind source for the bind-vs-memo precedence test. The rootfs's /lib
 * is symlinked to usr/lib (memoized at startup); we bind /lib to this
 * directory so a bind-first ordering hits this `probe.txt` rather than
 * routing through the rootfs symlink. */
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
			c_sv("rootfs_syscalls_smoke"), c_sv("fake_rootfs_setup"),
			cstr_from_fmt("failed to build fake rootfs at %s: %s",
				      FAKE_ROOTFS, strerror(errno)));
		return;
	}
	if (!build_sibling_evil(FAKE_ROOTFS_SIBLING)) {
		register_test_problem(
			c_sv("rootfs_syscalls_smoke"), c_sv("fake_rootfs_sibling_setup"),
			cstr_from_fmt("failed to build sibling at %s: %s",
				      FAKE_ROOTFS_SIBLING, strerror(errno)));
		return;
	}
	if (!build_bindsrc(FAKE_BINDSRC)) {
		register_test_problem(
			c_sv("rootfs_syscalls_smoke"), c_sv("fake_bindsrc_setup"),
			cstr_from_fmt("failed to build bindsrc at %s: %s",
				      FAKE_BINDSRC, strerror(errno)));
		return;
	}

	const char *args[] = {
		"-r", FAKE_ROOTFS,
		/* The lib64 bind is the long-standing B5 test setup
		 * (bind-vs-symlink-memo precedence). The usr/test-bind
		 * bind is consumed by the chroot suite — when chroot
		 * targets /usr, lib64's host path falls outside the new
		 * view (deactivated) while usr/test-bind re-anchors to
		 * "test-bind" inside the new root. Both binds use the
		 * same src dir; that's deliberate, and harmless — they
		 * each open their own O_PATH fd. */
		"-b", FAKE_BINDSRC ":lib64",
		"-b", FAKE_BINDSRC ":usr/test-bind",
		/* Host /dev so test_ioctl_pty_translation can open
		 * /dev/ptmx and exercise TCGETS2/TCSETS2 on a real pty.
		 * On Linux this is the kernel's devpts; the test
		 * allocates its own pty pair and tears it down. */
		"-b", "/dev:dev",
		NULL
	};
	steps_register_from_testhost(c_sv("rootfs_syscalls_smoke"), args);

	/* Best-effort cleanup. The rootfs is small and the test runner is
	 * short-lived; if cleanup fails there's nothing meaningful to do. */
	rmrf(FAKE_ROOTFS);
	rmrf(FAKE_ROOTFS_SIBLING);
	rmrf(FAKE_BINDSRC);
}
