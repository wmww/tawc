/* Rootfs syscall smoke: path translation + identity decoration.
 *
 * The driver issues a set of guest-perspective syscalls via inline asm
 * (so each one's IP is outside the stub allowlist and traps into our
 * handler). The handler dispatches to the registered fs/identity
 * functions; the smoke checks the result.
 *
 * What's covered today:
 *   - getuid / geteuid / getgid / getegid -> 0 (fake-root).
 *   - openat("/etc/probe", O_RDONLY) -> translates to <rootfs>/etc/probe.
 *
 * The rootfs is provided by the caller; the driver creates the probe
 * file under /etc/probe before installing the filter so we can compare
 * the bytes after.
 *
 * See notes/tawcroot.md "Phase 1 -- MVP path translation" for the
 * historical bring-up label.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/stat.h>
#include <sys/stat.h>

#include "rootfs_smoke.h"
#include "io.h"
#include "raw_sys.h"
#include "filter.h"
#include "fdtab.h"
#include "handler.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "path.h"
#include "sysnr.h"
#include "usercopy.h"

#include "tawc_uapi.h"

/* ----- inline-asm syscall probes (issued from within this file so the
 * IP is outside the stub allowlist and the filter TRAPs into our
 * handler) ---------------------------------------------------------- */

#if defined(__x86_64__)
# define INLINE_SYS6(num, a, b, c, d, e, f, rv)                          \
	do {                                                              \
		register long _a __asm__("rdi") = (long)(a);              \
		register long _b __asm__("rsi") = (long)(b);              \
		register long _c __asm__("rdx") = (long)(c);              \
		register long _d __asm__("r10") = (long)(d);              \
		register long _e __asm__("r8")  = (long)(e);              \
		register long _f __asm__("r9")  = (long)(f);              \
		long _rv;                                                  \
		__asm__ __volatile__ (                                    \
			"syscall"                                          \
			: "=a"(_rv)                                        \
			: "0"((long)(num)),                               \
			  "r"(_a), "r"(_b), "r"(_c),                      \
			  "r"(_d), "r"(_e), "r"(_f)                       \
			: "rcx", "r11", "memory");                        \
		(rv) = _rv;                                               \
	} while (0)
#elif defined(__aarch64__)
# define INLINE_SYS6(num, a, b, c, d, e, f, rv)                          \
	do {                                                              \
		register long _x8 __asm__("x8") = (long)(num);            \
		register long _x0 __asm__("x0") = (long)(a);              \
		register long _x1 __asm__("x1") = (long)(b);              \
		register long _x2 __asm__("x2") = (long)(c);              \
		register long _x3 __asm__("x3") = (long)(d);              \
		register long _x4 __asm__("x4") = (long)(e);              \
		register long _x5 __asm__("x5") = (long)(f);              \
		__asm__ __volatile__ (                                    \
			"svc #0"                                           \
			: "+r"(_x0)                                        \
			: "r"(_x8), "r"(_x1), "r"(_x2), "r"(_x3),         \
			  "r"(_x4), "r"(_x5)                               \
			: "memory");                                       \
		(rv) = _x0;                                                \
	} while (0)
#else
# error "unsupported arch"
#endif

static long inline_getuid(void)
{ long rv; INLINE_SYS6(TAWC_SYS_getuid, 0, 0, 0, 0, 0, 0, rv); return rv; }

static long inline_geteuid(void)
{ long rv; INLINE_SYS6(TAWC_SYS_geteuid, 0, 0, 0, 0, 0, 0, rv); return rv; }

static long inline_getgid(void)
{ long rv; INLINE_SYS6(TAWC_SYS_getgid, 0, 0, 0, 0, 0, 0, rv); return rv; }

static long inline_getegid(void)
{ long rv; INLINE_SYS6(TAWC_SYS_getegid, 0, 0, 0, 0, 0, 0, rv); return rv; }

static long inline_openat(int dirfd, const char *path, int flags, int mode)
{
	long rv;
	INLINE_SYS6(TAWC_SYS_openat, dirfd, path, flags, mode, 0, 0, rv);
	return rv;
}

static long inline_fstatat(int dirfd, const char *path,
			   struct stat *out, int flags)
{
	long rv;
	INLINE_SYS6(TAWC_SYS_fstatat, dirfd, path, out, flags, 0, 0, rv);
	return rv;
}

static long inline_unlinkat(int dirfd, const char *path, int flag)
{
	long rv;
	INLINE_SYS6(TAWC_SYS_unlinkat, dirfd, path, flag, 0, 0, 0, rv);
	return rv;
}

static long inline_faccessat(int dirfd, const char *path, int mode, int flag)
{
	long rv;
	INLINE_SYS6(TAWC_SYS_faccessat, dirfd, path, mode, flag, 0, 0, rv);
	return rv;
}

/* ----- helpers ---------------------------------------------------- */

static long open_rootfs(const char *path)
{
	return tawc_openat(AT_FDCWD, path,
			   O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
}

/* Write/probe the rootfs we'll be translating against. The smoke uses
 * /etc/probe inside the rootfs as its open target. We assume the rootfs
 * tree already exists with /etc/probe present -- the caller (test
 * harness) sets this up. */

/* ----- entry ------------------------------------------------------ */

int tawcroot_rootfs_smoke_main_argv(int argc, char **argv, const char *rootfs)
{
	/* Collect `-b src:dst` entries. We need them parsed before
	 * `tawcroot_rootfs_smoke_main` installs the seccomp filter -- once the
	 * filter is up, opening additional bind src dirs becomes a TRAPed
	 * openat, which we'd dispatch through our own translator and route
	 * back into the host filesystem... that works, but doing it before
	 * filter install is simpler. */
	for (int i = 1; i < argc; i++) {
		if (!tawc_streq(argv[i], "-b") || i + 1 >= argc) continue;
		const char *spec = argv[++i];
		/* Find the colon. */
		size_t colon = 0;
		while (spec[colon] && spec[colon] != ':') colon++;
		if (!spec[colon]) {
			tawc_io_str("[rootfs-smoke] -b entry missing ':' -- '");
			tawc_io_str(spec);
			tawc_io_str("'\n");
			return 1;
		}
		char src[1024];
		size_t k;
		for (k = 0; k < colon && k + 1 < sizeof src; k++) src[k] = spec[k];
		src[k] = 0;
		const char *dst = spec + colon + 1;
		long br = tawcroot_path_add_bind(src, dst);
		if (br < 0) {
			tawc_io_str("[rootfs-smoke] bind add failed for '");
			tawc_io_str(spec);
			tawc_io_str("' errno=-");
			tawc_io_dec(-br);
			tawc_io_str("\n");
			return 1;
		}
	}
	return tawcroot_rootfs_smoke_main(rootfs);
}

/* ----- per-scenario test functions -------------------------------- */

/* Identity tests -- must all return 0 regardless of host uid/gid. */
static int test_identity_getuid(void)
{
	int fails = 0;
	long u = inline_getuid();
	fails += tawc_io_step("getuid -> 0", u == 0);
	tawc_io_kv_dec("    rv", u);
	return fails;
}

static int test_identity_geteuid(void)
{
	int fails = 0;
	long u = inline_geteuid();
	fails += tawc_io_step("geteuid -> 0", u == 0);
	tawc_io_kv_dec("    rv", u);
	return fails;
}

static int test_identity_getgid(void)
{
	int fails = 0;
	long g = inline_getgid();
	fails += tawc_io_step("getgid -> 0", g == 0);
	tawc_io_kv_dec("    rv", g);
	return fails;
}

static int test_identity_getegid(void)
{
	int fails = 0;
	long g = inline_getegid();
	fails += tawc_io_step("getegid -> 0", g == 0);
	tawc_io_kv_dec("    rv", g);
	return fails;
}

/* set*id family must fake success: a guest that believes it is root
 * calls setuid(0)/setgroups() (daemons dropping privileges, runuser,
 * maintainer scripts) and aborts on the kernel's real EPERM. */
static int test_identity_setid_family_fakes_success(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_setuid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setuid(0) -> 0 (faked)", rv == 0);
	tawc_io_kv_dec("    rv", rv);
	INLINE_SYS6(TAWC_SYS_setgid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setgid(0) -> 0 (faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setresuid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setresuid(0,0,0) -> 0 (faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setresgid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setresgid(0,0,0) -> 0 (faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setreuid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setreuid(0,0) -> 0 (faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setregid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setregid(0,0) -> 0 (faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setfsuid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setfsuid(0) -> 0 (prev fsuid faked)", rv == 0);
	INLINE_SYS6(TAWC_SYS_setfsgid, 0, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setfsgid(0) -> 0 (prev fsgid faked)", rv == 0);
	/* Non-trivial setgroups: a real kernel call from the app uid would
	 * EPERM; the dropped-groups list content is irrelevant to us. */
	unsigned int one_group[1] = { 0 };
	INLINE_SYS6(TAWC_SYS_setgroups, 1, one_group, 0, 0, 0, 0, rv);
	fails += tawc_io_step("setgroups(1, [0]) -> 0 (faked)", rv == 0);
	/* And the guest still sees uid 0 afterwards. */
	fails += tawc_io_step("getuid still 0 after set*id dance",
			      inline_getuid() == 0);
	return fails;
}

/* openat absolute path -> handler should translate to rootfs-rel. */
static int test_openat_absolute_translates(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/etc/probe\") -> translates inside rootfs",
		fd >= 0);
	tawc_io_kv_dec("    fd", fd);
	if (fd >= 0) {
		char buf[64];
		long n = tawc_read((int)fd, buf, sizeof buf - 1);
		fails += tawc_io_step("read probe contents", n > 0);
		if (n > 0) {
			buf[n] = 0;
			tawc_io_str("    contents = '");
			tawc_io_str(buf);
			tawc_io_str("'\n");
		}
		tawc_close((int)fd);
	} else {
		tawc_io_kv_dec("    errno (-rv)", -fd);
	}
	return fails;
}

/* EFAULT smoke: NULL path and a wild non-NULL pointer must both
 * return -EFAULT, not crash. usercopy via process_vm_readv is what
 * makes this safe. */
static int test_openat_null_efault(void)
{
	int fails = 0;
	long fd;
	INLINE_SYS6(TAWC_SYS_openat, AT_FDCWD, (long)0, O_RDONLY, 0,
		    0, 0, fd);
	fails += tawc_io_step(
		"openat(NULL) -> -EFAULT (no crash)", fd == -14);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

static int test_openat_unmapped_efault(void)
{
	int fails = 0;
	long fd;
	/* A guaranteed-unmapped address: 0x1000 is below the
	 * minimum mmap region on every Linux we care about. */
	INLINE_SYS6(TAWC_SYS_openat, AT_FDCWD, (long)0x1000,
		    O_RDONLY, 0, 0, 0, fd);
	fails += tawc_io_step(
		"openat(<unmapped>) -> -EFAULT (no crash)",
		fd == -14);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Bogus paths should ENOENT, not crash. */
static int test_openat_bogus_enoent(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/does-not-exist",
				O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/etc/does-not-exist\") -> -ENOENT",
		fd == -2);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* EFAULT-safety for OUTPUT buffers (review finding B1+B6).
 * The handlers must not blindly do `*out = local;` against an
 * untrusted guest pointer -- a wild value (0x1000 below, the
 * guaranteed-unmapped "low userspace" region) would deliver
 * SIGSEGV synchronously into the SIGSYS handler and crash
 * tawcroot. tawc_copy_to_guest routes the write through
 * process_vm_writev so the kernel validates the destination
 * and reports -EFAULT cleanly. */
static int test_efault_safety_output_buffers(void)
{
	int fails = 0;
	long rv;
	struct stat st_in;  /* unused, just an input stand-in */
	(void)st_in;

	/* fstatat with valid path, wild output buffer. */
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/etc/probe",
		    (long)0x1000, 0, 0, 0, rv);
	fails += tawc_io_step(
		"fstatat(<good path>, <unmapped *st>) -> -EFAULT (B1)",
		rv == -14);
	tawc_io_kv_dec("    rv", rv);

	/* statx with valid path, wild output buffer. */
	INLINE_SYS6(TAWC_SYS_statx, AT_FDCWD, "/etc/probe",
		    0, 0x7ff /*STATX_BASIC_STATS*/, (long)0x1000, 0, rv);
	fails += tawc_io_step(
		"statx(<good path>, <unmapped *out>) -> -EFAULT (B1)",
		rv == -14);
	tawc_io_kv_dec("    rv", rv);

	/* getcwd with wild output buffer. We chdir into the rootfs
	 * first so the cwd is inside the view -- otherwise
	 * handle_getcwd returns -ENOENT before getting to the
	 * output-buffer copy and the test wouldn't exercise the
	 * EFAULT path. */
	INLINE_SYS6(TAWC_SYS_chdir, "/", 0, 0, 0, 0, 0, rv);
	INLINE_SYS6(TAWC_SYS_getcwd, (long)0x1000, 256, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"getcwd(<unmapped buf>) -> -EFAULT (B1)",
		rv == -14);
	tawc_io_kv_dec("    rv", rv);

#if defined(__x86_64__)
	/* Legacy stat -- same hazard via stat_via_at. */
	INLINE_SYS6(TAWC_SYS_stat, "/etc/probe", (long)0x1000,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy stat(<good path>, <unmapped>) -> -EFAULT (B1)",
		rv == -14);
	INLINE_SYS6(TAWC_SYS_lstat, "/etc/probe", (long)0x1000,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy lstat(<good path>, <unmapped>) -> -EFAULT (B1)",
		rv == -14);
#endif
	return fails;
}

/* EFAULT-safety for the legacy and two-path handlers -- every
 * path-bearing handler must route through process_vm_readv, NOT
 * deref the guest pointer directly. Wild pointers must come back
 * as -EFAULT, not crash the SIGSYS handler. */
static int test_efault_safety_legacy_two_path(void)
{
	int fails = 0;
	long rv;
	struct stat st;
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, (long)0x1000, &st,
		    0, 0, 0, rv);
	fails += tawc_io_step(
		"fstatat(<unmapped>) -> -EFAULT", rv == -14);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_linkat, AT_FDCWD, (long)0x1000,
		    AT_FDCWD, "/etc/captured", 0, 0, rv);
	fails += tawc_io_step(
		"linkat(<unmapped>, ...) -> -EFAULT", rv == -14);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_renameat2, AT_FDCWD, "/etc/probe",
		    AT_FDCWD, (long)0x1000, 0, 0, rv);
	fails += tawc_io_step(
		"renameat2(/etc/probe, <unmapped>) -> -EFAULT", rv == -14);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_truncate, (long)0x1000, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"truncate(<unmapped>) -> -EFAULT", rv == -14);
	tawc_io_kv_dec("    rv", rv);

#if defined(__x86_64__)
	INLINE_SYS6(TAWC_SYS_stat, (long)0x1000, &st, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy stat(<unmapped>) -> -EFAULT", rv == -14);
	INLINE_SYS6(TAWC_SYS_rename, (long)0x1000, "/etc/captured",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy rename(<unmapped>, ...) -> -EFAULT", rv == -14);
#endif
	return fails;
}

/* Symlink memoization: if the rootfs has `/lib` -> `usr/lib` (typical
 * glibc layout), opening `/lib/foo` should resolve via `usr/lib/foo`.
 * The fake rootfs sets this up if `lib_target` is present. */
static int test_symlink_memoization(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/lib/probe.so",
				O_RDONLY, 0);
	const char *result = (fd >= 0)
		? "OK (memoized symlink hit)" : "(skipped or no memo)";
	tawc_io_str("    /lib/probe.so -> ");
	tawc_io_str(result);
	tawc_io_str(" rv=");
	tawc_io_dec(fd);
	tawc_io_str("\n");
	if (fd >= 0) tawc_close((int)fd);
	/* Don't fail if the test rootfs lacks the symlink. */
	return fails;
}

/* Bind-mount path: if the user passed a `-b src:/host_alias`, then
 * opening /host_alias/<file> should resolve via the bind src fd
 * rather than the rootfs fd. We don't know the dst path at compile
 * time, but we can detect "any binds configured" and try each
 * bind's dst with /probe.txt appended. */
static int test_bind_mount_routing(void)
{
	int fails = 0;
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		/* Skip binds whose src isn't part of the test fixture
		 * (FAKE_BINDSRC). The "/dev" bind exists for
		 * test_ioctl_pty_translation to allocate a real pty and
		 * has no probe.txt by design — skipping by dst name keeps
		 * the routing assertion focused on convention-following
		 * binds without baking in the bindsrc layout here. */
		if (b->dst_len == 3 &&
		    b->dst[0] == 'd' && b->dst[1] == 'e' && b->dst[2] == 'v')
			continue;

		char p[512];
		size_t n = 0;
		p[n++] = '/';
		for (size_t j = 0; j < b->dst_len; j++)
			p[n++] = b->dst[j];
		const char *suffix = "/probe.txt";
		for (size_t j = 0; suffix[j] && n + 1 < sizeof p; j++)
			p[n++] = suffix[j];
		p[n] = 0;
		long fd = inline_openat(AT_FDCWD, p, O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(<bind dst>/probe.txt) -> routes to bind src",
			fd >= 0);
		tawc_io_str("    target: ");
		tawc_io_str(p);
		tawc_io_str("\n");
		tawc_io_kv_dec("    rv", fd);
		if (fd >= 0) {
			char buf[64];
			long rd = tawc_read((int)fd, buf, sizeof buf - 1);
			if (rd > 0) {
				buf[rd] = 0;
				tawc_io_str("    contents = '");
				tawc_io_str(buf);
				tawc_io_str("'\n");
			}
			tawc_close((int)fd);
		}
	}
	return fails;
}

/* `..` clamp: an absolute path with `..` walks at the rootfs root
 * must NOT escape into host filesystem. Translates to the same
 * place as /etc/probe. */
static int test_dotdot_clamp_etc_etc_probe(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/../etc/probe",
				O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/etc/../etc/probe\") -> translates",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

static int test_dotdot_clamp_root_relative(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/../../etc/probe",
				O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/../../etc/probe\") -> clamped, translates",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* fd-relative `..` clamp: openat(rootfs_root_fd, "..", O_PATH) must
 * resolve to the rootfs root again, not the host's parent of the
 * rootfs. Real chroot(2) does this in the kernel; tawcroot has to
 * emulate it. systemd's path_is_root_at probe (chase.c) relies on
 * the inodes matching -- without the clamp, chase() returns a
 * relative path where chase() expects an absolute one and aborts
 * with `Assertion 'path_is_absolute(p)' failed`.
 *
 * Also probe an fd-relative `..` from a subdir resolving to a
 * sibling: openat(<rootfs>/etc, "../etc/probe", O_RDONLY) should
 * succeed. Pre-fix this would have walked through the kernel's
 * view of <rootfs>/etc/.. (the host parent of the rootfs); the
 * `etc` component would then ENOENT. */
static int test_dotdot_via_dirfd_clamps_at_rootfs(void)
{
	int fails = 0;

	long fd_root = inline_openat(AT_FDCWD, "/",
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(\"/\") -> fd at rootfs root",
			      fd_root >= 0);
	if (fd_root < 0) return fails;

	struct stat st_root;
	long sr = inline_fstatat((int)fd_root, "", &st_root, AT_EMPTY_PATH);
	fails += tawc_io_step("fstatat(rootfs_fd, \"\", AT_EMPTY_PATH)",
			      sr == 0);

	long fd_dotdot = inline_openat((int)fd_root, "..",
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(rootfs_fd, \"..\") -> fd",
			      fd_dotdot >= 0);
	if (fd_dotdot >= 0) {
		struct stat st_dot;
		long sd = inline_fstatat((int)fd_dotdot, "", &st_dot,
					 AT_EMPTY_PATH);
		fails += tawc_io_step("fstatat(rootfs_fd/.., \"\") ok",
				      sd == 0);
		fails += tawc_io_step(
			"openat(rootfs_fd, \"..\") clamps at rootfs root",
			sd == 0 &&
			st_dot.st_ino == st_root.st_ino &&
			st_dot.st_dev == st_root.st_dev);
		tawc_io_kv_dec("    rootfs ino", (long)st_root.st_ino);
		tawc_io_kv_dec("    dotdot ino", (long)st_dot.st_ino);
		tawc_close((int)fd_dotdot);
	}

	long fd_etc = inline_openat((int)fd_root, "etc",
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(rootfs_fd, \"etc\") -> fd",
			      fd_etc >= 0);
	if (fd_etc >= 0) {
		long fd_probe = inline_openat((int)fd_etc, "../etc/probe",
					      O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(etc_fd, \"../etc/probe\") -> stays inside rootfs",
			fd_probe >= 0);
		if (fd_probe >= 0) tawc_close((int)fd_probe);
		tawc_io_kv_dec("    rv", fd_probe);

		/* `./..` exercises the component-aware detection: `.`
		 * alone is not `..`, but the trailing `..` is, so the
		 * lift must still kick in. Same target as just `..`
		 * → clamp to rootfs root. */
		long fd_dot_dotdot = inline_openat((int)fd_etc, "./..",
			O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
		fails += tawc_io_step(
			"openat(etc_fd, \"./..\") -> rootfs root",
			fd_dot_dotdot >= 0);
		if (fd_dot_dotdot >= 0) {
			struct stat st_dd;
			long sdd = inline_fstatat((int)fd_dot_dotdot, "",
				&st_dd, AT_EMPTY_PATH);
			fails += tawc_io_step(
				"openat(etc_fd, \"./..\") inode == rootfs root",
				sdd == 0 &&
				st_dd.st_ino == st_root.st_ino &&
				st_dd.st_dev == st_root.st_dev);
			tawc_close((int)fd_dot_dotdot);
		}

		/* Slash runs and intermediate `..` chains: `..//../etc`
		 * canonicalises to `/etc` after the rootfs-clamped fold
		 * (etc → up to rootfs → up clamped → etc). */
		long fd_slashes = inline_openat((int)fd_etc,
			"..//../etc/probe", O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(etc_fd, \"..//../etc/probe\") -> rootfs/etc/probe",
			fd_slashes >= 0);
		if (fd_slashes >= 0) tawc_close((int)fd_slashes);
		tawc_io_kv_dec("    rv", fd_slashes);

		tawc_close((int)fd_etc);
	}

	tawc_close((int)fd_root);
	return fails;
}

/* Bind-src dirfd `..` clamps at the bind-dst boundary in the guest
 * view. The rootfs smoke fixture binds <TMPDIR>/...-bindsrc to /lib64, so
 * a dirfd opened through /lib64 walks `..` to the guest rootfs root
 * (not the host's /tmp). dirfd_to_guest_abs reverse-translates the
 * bind-src host path to /<bind.dst>/..., the appended `..` folds
 * to "/", and the second-pass translate routes through rootfs_fd. */
static int test_dotdot_via_bind_dst_dirfd(void)
{
	int fails = 0;
	if (tawcroot_n_binds == 0) {
		tawc_io_str("  [skip] no binds configured\n");
		return 0;
	}

	char path[512];
	size_t n = 0;
	path[n++] = '/';
	for (size_t j = 0; j < tawcroot_binds[0].dst_len; j++)
		path[n++] = tawcroot_binds[0].dst[j];
	path[n] = 0;
	long fd_bind = inline_openat(AT_FDCWD, path,
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(<bind dst>) -> fd", fd_bind >= 0);
	if (fd_bind < 0) return fails;

	long fd_root = inline_openat(AT_FDCWD, "/",
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(\"/\") -> fd at rootfs root",
			      fd_root >= 0);

	long fd_dotdot = inline_openat((int)fd_bind, "..",
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step(
		"openat(<bind dst>, \"..\") -> fd",
		fd_dotdot >= 0);

	if (fd_dotdot >= 0 && fd_root >= 0) {
		struct stat st_dd, st_root;
		long sd = inline_fstatat((int)fd_dotdot, "", &st_dd,
		                         AT_EMPTY_PATH);
		long sr = inline_fstatat((int)fd_root, "", &st_root,
		                         AT_EMPTY_PATH);
		fails += tawc_io_step(
			"openat(<bind dst>, \"..\") clamps at rootfs root",
			sd == 0 && sr == 0 &&
			st_dd.st_ino == st_root.st_ino &&
			st_dd.st_dev == st_root.st_dev);
		tawc_io_kv_dec("    rootfs ino", (long)st_root.st_ino);
		tawc_io_kv_dec("    dotdot ino", (long)st_dd.st_ino);
	}

	if (fd_dotdot >= 0) tawc_close((int)fd_dotdot);
	if (fd_root   >= 0) tawc_close((int)fd_root);
	tawc_close((int)fd_bind);
	return fails;
}

/* Bind-src dirfd `..` must NOT escape into the host. From a dirfd
 * opened through a bind dst, naive kernel passthrough of `..` would
 * walk up the host tree past the bind src; the rootfs smoke fixture plants
 * FAKE_ROOTFS_SIBLING/foo/host-secret at <TMPDIR> (sibling of the bind
 * src) precisely as a probe target the rootfs view never exposes.
 * dirfd_to_guest_abs reverse-translates the bind-src host path to
 * /<bind.dst>/<remainder>, fetch_and_translate_at's `..` lift folds it,
 * and the second-pass translate routes back through the bind so `..`
 * clamps at the bind-dst boundary.
 *
 * We probe via faccessat(F_OK) rather than openat — both go through
 * the same fetch_and_translate_at lift today, but faccessat is the
 * narrower test (it can't accidentally succeed via O_CREAT or any
 * open-time side effect). */
static int test_dotdot_via_bind_dst_does_not_escape_to_host(void)
{
	int fails = 0;
	if (tawcroot_n_binds == 0) {
		tawc_io_str("  [skip] no binds configured\n");
		return 0;
	}

	char path[512];
	size_t n = 0;
	path[n++] = '/';
	for (size_t j = 0; j < tawcroot_binds[0].dst_len; j++)
		path[n++] = tawcroot_binds[0].dst[j];
	path[n] = 0;
	long fd_bind = inline_openat(AT_FDCWD, path,
		O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("openat(<bind dst>) -> fd (escape probe)",
			      fd_bind >= 0);
	if (fd_bind < 0) return fails;

	/* FAKE_BINDSRC ends in "-bindsrc" and FAKE_ROOTFS_SIBLING uses the
	 * same basename with "-evil" instead. Build "../<sibling>/..." from
	 * the runtime bind src so this test works for the plain rootfs-smoke
	 * fixture and the androidfilter fixture.
	 *
	 * faccessat(F_OK, 0): rv==0 would mean "the kernel reached the file",
	 * i.e. the dirfd `..` walked past the bind src and into /tmp on the
	 * host. The fix clamps at the bind-dst boundary, so rv must be < 0. */
	const char *src = tawcroot_binds[0].src;
	size_t src_len = tawcroot_binds[0].src_len;
	size_t base = src_len;
	while (base > 0 && src[base - 1] != '/') base--;
	const char suffix[] = "-bindsrc";
	size_t suffix_len = sizeof suffix - 1;
	size_t name_len = src_len - base;
	int has_suffix = name_len >= suffix_len;
	for (size_t i = 0; has_suffix && i < suffix_len; i++) {
		if (src[src_len - suffix_len + i] != suffix[i])
			has_suffix = 0;
	}
	if (!has_suffix || name_len + 32 > 512) {
		tawc_io_str("  [skip] bind src does not use smoke fixture name\n");
		tawc_close((int)fd_bind);
		return fails;
	}
	char escape[512];
	size_t e = 0;
	escape[e++] = '.';
	escape[e++] = '.';
	escape[e++] = '/';
	for (size_t i = 0; i < name_len - suffix_len; i++)
		escape[e++] = src[base + i];
	const char tail[] = "-evil/foo/host-secret";
	for (size_t i = 0; i < sizeof tail; i++)
		escape[e++] = tail[i];

	long rv = inline_faccessat((int)fd_bind, escape, 0 /* F_OK */, 0);
	fails += tawc_io_step(
		"faccessat(<bind dst dirfd>, \"../<sibling>/host-secret\", F_OK) "
		"clamps at bind-dst boundary (no host escape)",
		rv != 0);
	tawc_io_kv_dec("    rv", rv);
	tawc_close((int)fd_bind);
	return fails;
}

/* fstatat with fake-root decoration: stat the probe and assert
 * uid==0 / gid==0 even though the host file is owned by `shell`. */
static int test_fstatat_fake_root_decoration(void)
{
	int fails = 0;
	struct stat st;
	long fr = inline_fstatat(AT_FDCWD, "/etc/probe", &st, 0);
	fails += tawc_io_step(
		"fstatat(\"/etc/probe\") via dispatch",
		fr == 0);
	if (fr == 0) {
		fails += tawc_io_step(
			"fake-root: st_uid == 0",
			st.st_uid == 0);
		fails += tawc_io_step(
			"fake-root: st_gid == 0",
			st.st_gid == 0);
		tawc_io_kv_dec("    st_mode (octal-ish)",
			       (long)st.st_mode);
		tawc_io_kv_dec("    st_size", (long)st.st_size);
	}
	tawc_io_kv_dec("    rv", fr);
	return fails;
}

/* dirfd-relative symlink escape: a dotdot-free relative path off a
 * guest dirfd must still be lifted through the translator so an
 * absolute-target symlink along it is clamped at the rootfs. Pre-fix,
 * openat(etc_fd, "host-secret") went to the kernel verbatim and the
 * kernel chased /etc/passwd against the HOST root (which exists —
 * silent view escape). Post-fix it clamps to <rootfs>/etc/passwd →
 * ENOENT. */
static int test_dirfd_relative_symlink_escape_clamped(void)
{
	int fails = 0;
	long dfd = inline_openat(AT_FDCWD, "/etc",
				 O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	fails += tawc_io_step("dirfd-escape: open /etc dirfd", dfd >= 0);
	if (dfd < 0) return fails;
	long fd = inline_openat((int)dfd, "host-secret", O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(etc_fd, \"host-secret\") -> ENOENT (clamped, not host /etc/passwd)",
		fd == TAWC_ENOENT);
	tawc_io_kv_dec("    rv", fd);
	if (fd >= 0) tawc_close((int)fd);
	tawc_close((int)dfd);
	return fails;
}

/* O_CREAT (without O_EXCL) on an existing symlink leaf must follow it
 * (kernel semantics) — and the follow must be clamped at the rootfs.
 * Pre-fix the leaf reached the host kernel un-resolved and an absolute
 * target was chased against the HOST root. */
static int test_open_creat_follows_symlink_leaf_clamped(void)
{
	int fails = 0;
	long rv;
	/* Dangling absolute-target symlink: /run/dangling → /run/created-target */
	INLINE_SYS6(TAWC_SYS_symlinkat, "/run/created-target", AT_FDCWD,
		    "/run/dangling", 0, 0, 0, rv);
	fails += tawc_io_step("creat-follow: symlink(/run/dangling) -> 0",
			      rv == 0);
	long fd = inline_openat(AT_FDCWD, "/run/dangling",
				O_WRONLY | O_CREAT, 0644);
	fails += tawc_io_step(
		"open(dangling abs symlink, O_CREAT) -> fd (target created in view)",
		fd >= 0);
	tawc_io_kv_dec("    rv", fd);
	if (fd >= 0) tawc_close((int)fd);
	struct stat st;
	long sr = inline_fstatat(AT_FDCWD, "/run/created-target", &st,
				 AT_SYMLINK_NOFOLLOW);
	fails += tawc_io_step(
		"creat-follow: /run/created-target exists inside rootfs",
		sr == 0 && S_ISREG(st.st_mode));
	tawc_io_kv_dec("    stat rv", sr);

	/* O_CREAT|O_EXCL must NOT follow: existing symlink leaf (even
	 * dangling) is EEXIST. */
	(void)inline_unlinkat(AT_FDCWD, "/run/created-target", 0);
	long xfd = inline_openat(AT_FDCWD, "/run/dangling",
				 O_WRONLY | O_CREAT | O_EXCL, 0644);
	fails += tawc_io_step(
		"open(symlink leaf, O_CREAT|O_EXCL) -> EEXIST (no follow)",
		xfd == TAWC_EEXIST);
	tawc_io_kv_dec("    rv", xfd);
	if (xfd >= 0) tawc_close((int)xfd);

	(void)inline_unlinkat(AT_FDCWD, "/run/dangling", 0);
	return fails;
}

/* fstat must decorate like stat/fstatat/statx do, and reserved fds
 * must answer EBADF (fdtab.h contract). */
static int test_fstat_fake_root_decoration(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step("fstat: open /etc/probe", fd >= 0);
	if (fd >= 0) {
		struct stat st;
		st.st_uid = 99;
		st.st_gid = 99;
		long rv;
		INLINE_SYS6(TAWC_SYS_fstat, fd, &st, 0, 0, 0, 0, rv);
		fails += tawc_io_step("fstat(fd) -> 0", rv == 0);
		fails += tawc_io_step("fstat fake-root: st_uid == 0",
				      st.st_uid == 0);
		fails += tawc_io_step("fstat fake-root: st_gid == 0",
				      st.st_gid == 0);
		tawc_close((int)fd);
	}
	long rv2;
	struct stat st2;
	INLINE_SYS6(TAWC_SYS_fstat, tawcroot_rootfs_fd, &st2, 0, 0, 0, 0, rv2);
	fails += tawc_io_step("fstat(reserved rootfs fd) -> EBADF",
			      rv2 == TAWC_EBADF);
	tawc_io_kv_dec("    rv", rv2);
	return fails;
}

/* openat2 / fchmodat2 must trap to ENOSYS — untrapped they'd resolve
 * guest paths against the HOST view (BPF default is RET_ALLOW). Every
 * caller has a pre-5.6 / pre-6.6 fallback path. */
static int test_openat2_fchmodat2_enosys(void)
{
	int fails = 0;
	unsigned long long how[3] = { 0, 0, 0 };  /* struct open_how */
	long rv;
	INLINE_SYS6(TAWC_SYS_openat2, AT_FDCWD, "/etc/probe",
		    how, sizeof how, 0, 0, rv);
	fails += tawc_io_step("openat2 -> ENOSYS (forces openat fallback)",
			      rv == TAWC_ENOSYS);
	tawc_io_kv_dec("    rv", rv);
	INLINE_SYS6(TAWC_SYS_fchmodat2, AT_FDCWD, "/etc/probe",
		    0644, 0, 0, 0, rv);
	fails += tawc_io_step("fchmodat2 -> ENOSYS (forces fallback)",
			      rv == TAWC_ENOSYS);
	tawc_io_kv_dec("    rv", rv);
	return fails;
}

/* inotify_add_watch carries a path; untrapped it watches HOST paths
 * (GLib/GIO monitors silently break). inotify_init1 is untrapped on
 * purpose — fd-only, nothing to translate. */
static int test_inotify_add_watch_translates(void)
{
	int fails = 0;
	long ifd;
	INLINE_SYS6(TAWC_SYS_inotify_init1, 0x80000 /*IN_CLOEXEC*/,
		    0, 0, 0, 0, 0, ifd);
	fails += tawc_io_step("inotify_init1 -> fd", ifd >= 0);
	tawc_io_kv_dec("    ifd", ifd);
	if (ifd < 0) return fails;
	long wd;
	INLINE_SYS6(TAWC_SYS_inotify_add_watch, ifd, "/etc/probe",
		    0x8 /*IN_CLOSE_WRITE*/, 0, 0, 0, wd);
	fails += tawc_io_step(
		"inotify_add_watch(\"/etc/probe\") -> wd (translated)",
		wd >= 0);
	tawc_io_kv_dec("    wd", wd);
	INLINE_SYS6(TAWC_SYS_inotify_add_watch, ifd, "/no/such/file",
		    0x8, 0, 0, 0, wd);
	fails += tawc_io_step("inotify_add_watch(bogus) -> ENOENT",
			      wd == TAWC_ENOENT);
	tawc_close((int)ifd);
	return fails;
}

/* fchdir: reserved fds must answer EBADF; ordinary guest dir fds keep
 * working. Kernel cwd is restored via the raw stub afterwards. */
static int test_fchdir_dispatch(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_fchdir, tawcroot_rootfs_fd, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("fchdir(reserved rootfs fd) -> EBADF",
			      rv == TAWC_EBADF);
	tawc_io_kv_dec("    rv", rv);

	char saved[512];
	long sl = TAWC_RAW(TAWC_SYS_getcwd, (long)saved, sizeof saved,
			   0, 0, 0, 0);
	long dfd = inline_openat(AT_FDCWD, "/etc",
				 O_RDONLY | O_DIRECTORY, 0);
	fails += tawc_io_step("fchdir: open(\"/etc\") dir fd", dfd >= 0);
	if (dfd >= 0) {
		INLINE_SYS6(TAWC_SYS_fchdir, dfd, 0, 0, 0, 0, 0, rv);
		fails += tawc_io_step("fchdir(guest dir fd) -> 0", rv == 0);
		tawc_io_kv_dec("    rv", rv);
		tawc_close((int)dfd);
	}
	if (sl > 0)
		(void)TAWC_RAW(TAWC_SYS_chdir, (long)saved, 0, 0, 0, 0, 0);
	return fails;
}

/* Escape attempt: a deeply-nested `..` aimed at a known-good host
 * file outside the rootfs. With our fold semantics this should
 * clamp at root and translate to <rootfs>/host-secret.txt -- which
 * doesn't exist in the rootfs, so ENOENT. The host file at
 * /data/local/tmp/host-secret.txt MUST NOT be reachable through
 * tawcroot. */
static int test_escape_attempt_clamps(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD,
		"/../../../../data/local/tmp/host-secret.txt",
		O_RDONLY, 0);
	fails += tawc_io_step(
		"escape attempt -> ENOENT (clamped at rootfs root)",
		fd == -2);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Relative path: we chdir into the rootfs first, then a relative
 * `etc/probe` should reverse-translate through getcwd -> guest `/` ->
 * append `etc/probe`. The chdir itself is a host syscall (not yet
 * trapped). */
static int test_relative_path_after_chdir(void)
{
	int fails = 0;
	/* chdir into the rootfs via guest perspective ("/" =
	 * rootfs root). The trapped chdir handler translates to a
	 * fchdir on the rootfs fd. */
	long cr;
	INLINE_SYS6(TAWC_SYS_chdir, "/", 0, 0, 0, 0, 0, cr);
	fails += tawc_io_step(
		"chdir(\"/\") -> translates via dispatch", cr == 0);
	tawc_io_kv_dec("    rv", cr);
	long fd = inline_openat(AT_FDCWD, "etc/probe",
				O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"etc/probe\") relative -> reverse-translates",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* cwd inside a bind: `cd` into a bind dst leaves the kernel cwd at the
 * bind SRC host path. getcwd and relative-path resolution must walk
 * the bind table, not just the rootfs prefix — pre-fix, `cd /lib64 &&
 * ls` was dead (getcwd ENOENT, every relative syscall ENOENT). */
static int test_cwd_inside_bind(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_chdir, "/lib64", 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("chdir(\"/lib64\") (bind dst) -> 0", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	char buf[256];
	buf[0] = 0;
	INLINE_SYS6(TAWC_SYS_getcwd, buf, sizeof buf, 0, 0, 0, 0, rv);
	int reports_bind_dst = (rv == 7 &&
				buf[0] == '/' && buf[1] == 'l' &&
				buf[2] == 'i' && buf[3] == 'b' &&
				buf[4] == '6' && buf[5] == '4' &&
				buf[6] == 0);
	fails += tawc_io_step("getcwd inside bind -> \"/lib64\"",
			      reports_bind_dst);
	tawc_io_kv_dec("    rv", rv);

	long fd = inline_openat(AT_FDCWD, "probe.txt", O_RDONLY, 0);
	fails += tawc_io_step(
		"relative open inside bind dst -> routes through bind",
		fd >= 0);
	tawc_io_kv_dec("    rv", fd);
	if (fd >= 0) {
		char data[32];
		long n = tawc_read((int)fd, data, sizeof data - 1);
		int from_bind = (n >= 13 &&
				 data[0] == 'f' && data[1] == 'r' &&
				 data[2] == 'o' && data[3] == 'm' &&
				 data[4] == '-');
		fails += tawc_io_step("relative read = bind src contents",
				      from_bind);
		tawc_close((int)fd);
	}

	/* Restore the guest cwd to "/" for downstream tests. */
	INLINE_SYS6(TAWC_SYS_chdir, "/", 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("chdir back to \"/\"", rv == 0);
	return fails;
}

/* readlinkat: create a symlink in the fake rootfs and verify our
 * dispatch routes it through. */
static int test_readlinkat_dispatch(void)
{
	int fails = 0;
	char buf[64];
	long n;
	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD,
		    "/etc/probe-link", buf, sizeof buf - 1, 0, 0, n);
	/* No symlink exists yet; -EINVAL or -ENOENT is fine. We're
	 * just proving the dispatch routes through. */
	fails += tawc_io_step(
		"readlinkat dispatch (no symlink -> -EINVAL/-ENOENT)",
		n < 0);
	tawc_io_kv_dec("    rv", n);
	return fails;
}

/* Readlink on a path that resolves exactly to a bind dst (or the rootfs
 * root) must report EINVAL — those are directories, not symlinks. The
 * naive translation gives the kernel `(reserved_dir_fd, "")` which it
 * answers with ENOENT; glibc realpath then aborts canonicalisation on
 * the very first /proc component and never reaches /proc/self/exe.
 * Repro: kitty (and any other glibc binary calling
 * `realpath("/proc/self/exe")`) bailed with "Failed to read
 * /proc/self/exe" before this was fixed. */
static int test_readlink_on_bind_dst_returns_einval(void)
{
	int fails = 0;
	char buf[64];
	long rv;

	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD, "/lib64",
		    buf, sizeof buf - 1, 0, 0, rv);
	fails += tawc_io_step(
		"readlinkat(\"/lib64\") -> -EINVAL (bind dst is a directory)",
		rv == -22 /*EINVAL*/);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD, "/dev",
		    buf, sizeof buf - 1, 0, 0, rv);
	fails += tawc_io_step(
		"readlinkat(\"/dev\") -> -EINVAL (bind dst is a directory)",
		rv == -22 /*EINVAL*/);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD, "/",
		    buf, sizeof buf - 1, 0, 0, rv);
	fails += tawc_io_step(
		"readlinkat(\"/\") -> -EINVAL (rootfs root is a directory)",
		rv == -22 /*EINVAL*/);
	tawc_io_kv_dec("    rv", rv);

	return fails;
}

/* faccessat2: probe a known-good path.
 * faccessat2 was added in kernel 5.8; on older kernels (e.g. OnePlus 9
 * on 5.4) the handler's raw syscall returns -ENOSYS. We treat that as
 * a skip rather than a failure -- emulating new syscalls with older
 * ones is intentionally out of scope for tawcroot. Real workloads on
 * 5.4 see -ENOSYS without tawcroot too, so we're not changing observable
 * behavior. */
static int test_faccessat2_known_good(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_faccessat2, AT_FDCWD, "/etc/probe",
		    4 /*R_OK*/, 0, 0, 0, rv);
	if (rv == -38 /*ENOSYS*/) {
		tawc_io_skip("faccessat2(\"/etc/probe\", R_OK) -> 0",
			     "kernel <5.8: faccessat2 not available");
	} else {
		fails += tawc_io_step(
			"faccessat2(\"/etc/probe\", R_OK) -> 0", rv == 0);
		tawc_io_kv_dec("    rv", rv);
	}
	return fails;
}

/* getcwd: we just chdir'd into "/" (the rootfs root). The reverse-
 * translated cwd should be "/", not the host path. */
static int test_getcwd_returns_root(void)
{
	int fails = 0;
	char buf[256];
	long rv;
	INLINE_SYS6(TAWC_SYS_getcwd, buf, sizeof buf,
		    0, 0, 0, 0, rv);
	buf[sizeof buf - 1] = 0;
	int looks_right = rv > 0 && buf[0] == '/' && buf[1] == 0;
	fails += tawc_io_step(
		"getcwd -> '/' (rootfs root, not host path)",
		looks_right);
	tawc_io_str("    cwd = '"); tawc_io_str(buf);
	tawc_io_str("'\n");
	tawc_io_kv_dec("    rv (length incl. NUL)", rv);
	return fails;
}

/* mkdirat / unlinkat: create a directory inside the rootfs view
 * (which lives in the host fake-rootfs tree), then remove it. */
static int test_mkdirat_unlinkat(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_mkdirat, AT_FDCWD, "/tawcroot-mkdir-test",
		    0755, 0, 0, 0, rv);
	fails += tawc_io_step(
		"mkdirat(\"/tawcroot-mkdir-test\", 0755) -> 0",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD, "/tawcroot-mkdir-test",
		    AT_REMOVEDIR, 0, 0, 0, rv);
	fails += tawc_io_step(
		"unlinkat(\"/tawcroot-mkdir-test\", AT_REMOVEDIR) -> 0",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);
	return fails;
}

/* symlinkat: create a symlink inside the rootfs view, readlinkat
 * it back, unlink. */
static int test_symlinkat_readlinkat_unlinkat(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_symlinkat, "etc/probe", AT_FDCWD,
		    "/tawcroot-symlink-test", 0, 0, 0, rv);
	fails += tawc_io_step(
		"symlinkat(\"etc/probe\" -> \"/tawcroot-symlink-test\")",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	char buf[64];
	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD,
		    "/tawcroot-symlink-test", buf, sizeof buf - 1,
		    0, 0, rv);
	fails += tawc_io_step(
		"readlinkat(\"/tawcroot-symlink-test\")",
		rv > 0);
	if (rv > 0) {
		buf[rv] = 0;
		tawc_io_str("    target = '"); tawc_io_str(buf);
		tawc_io_str("'\n");
	}

	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD,
		    "/tawcroot-symlink-test", 0, 0, 0, 0, rv);
	fails += tawc_io_step("unlinkat(symlink)", rv == 0);
	return fails;
}

/* openat(O_PATH | O_NOFOLLOW) on a symlink must succeed and hand
 * back an fd referring to the symlink itself. Without this glibc's
 * fchmodat(..., AT_SYMLINK_NOFOLLOW) emulation (open O_PATH|O_NOFOLLOW
 * + chmod /proc/self/fd/N) trips on the very first step, making
 * libarchive's symlink-extraction warn "Can't set permissions". */
static int test_openat_opath_nofollow_symlink(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/utime-link",
				O_PATH | O_NOFOLLOW, 0);
	fails += tawc_io_step(
		"openat(\"/utime-link\", O_PATH|O_NOFOLLOW) -> fd",
		fd >= 0);
	tawc_io_kv_dec("    rv", fd);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

/* utimensat AT_SYMLINK_NOFOLLOW must update the SYMLINK's mtime,
 * not the target's. Pre-fix the handler ignored the flag and used
 * PATH_FOLLOW for translation, so the resolver walked through to
 * the target; the kernel then wrote the target's mtime and the
 * symlink stayed put — exactly libarchive's "Can't restore time"
 * scenario when pacman extracts a symlink whose target hasn't
 * landed yet (PATH_FOLLOW returns ENOENT).
 *
 * Three asserts: (a) call returns 0, (b) the symlink's own
 * st_mtime advanced to the value we set, (c) the target file's
 * st_mtime did NOT move. Each one independently catches the bug.
 *
 * /utime-link is a fresh symlink to /utime-target, both laid
 * down by build_fake_rootfs in test_rootfs_syscalls_smoke.c; using a dedicated
 * pair avoids coupling with /etc/probe (which other tests read). */
static int test_utimensat_symlink_nofollow(void)
{
	int fails = 0;
	struct kernel_timespec { long tv_sec; long tv_nsec; };
	/* Both atime and mtime explicit (no UTIME_OMIT) — keeps the
	 * assertion below focused on the mtime component without
	 * worrying about kernel-flag encoding. */
	struct kernel_timespec ts[2] = {
		{ 1234567890L, 0 },
		{ 1234567890L, 0 },
	};

	struct stat link_before, target_before;
	long sr = inline_fstatat(AT_FDCWD, "/utime-link",
				 &link_before, AT_SYMLINK_NOFOLLOW);
	fails += tawc_io_step(
		"fstatat AT_SYMLINK_NOFOLLOW pre-utimensat (symlink)",
		sr == 0);
	sr = inline_fstatat(AT_FDCWD, "/utime-target",
			    &target_before, 0);
	fails += tawc_io_step(
		"fstatat pre-utimensat (target file)", sr == 0);

	long rv;
	INLINE_SYS6(TAWC_SYS_utimensat, AT_FDCWD, "/utime-link",
		    (long)ts, AT_SYMLINK_NOFOLLOW, 0, 0, rv);
	fails += tawc_io_step(
		"utimensat(\"/utime-link\", AT_SYMLINK_NOFOLLOW) -> 0",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	struct stat link_after, target_after;
	sr = inline_fstatat(AT_FDCWD, "/utime-link",
			    &link_after, AT_SYMLINK_NOFOLLOW);
	fails += tawc_io_step(
		"fstatat AT_SYMLINK_NOFOLLOW post-utimensat (symlink)",
		sr == 0);
	sr = inline_fstatat(AT_FDCWD, "/utime-target",
			    &target_after, 0);
	fails += tawc_io_step(
		"fstatat post-utimensat (target file)", sr == 0);

	fails += tawc_io_step(
		"utimensat AT_SYMLINK_NOFOLLOW updated symlink's "
		"own st_mtime",
		(long)link_after.st_mtime == 1234567890L);
	tawc_io_kv_dec("    link_before.st_mtime",
		       (long)link_before.st_mtime);
	tawc_io_kv_dec("    link_after.st_mtime",
		       (long)link_after.st_mtime);

	fails += tawc_io_step(
		"utimensat AT_SYMLINK_NOFOLLOW left target's "
		"st_mtime alone",
		(long)target_after.st_mtime ==
			(long)target_before.st_mtime);
	tawc_io_kv_dec("    target_before.st_mtime",
		       (long)target_before.st_mtime);
	tawc_io_kv_dec("    target_after.st_mtime",
		       (long)target_after.st_mtime);
	return fails;
}

/* utimensat without AT_SYMLINK_NOFOLLOW on a regular file: still
 * the common-case path (and what libarchive uses for non-symlink
 * extracts via futimens). Confirms the FOLLOW branch + path
 * translation still update mtime correctly. */
static int test_utimensat_follow_regular_file(void)
{
	int fails = 0;
	struct kernel_timespec { long tv_sec; long tv_nsec; };
	struct kernel_timespec ts[2] = {
		{ 1234567891L, 0 },
		{ 1234567891L, 0 },
	};
	long rv;
	INLINE_SYS6(TAWC_SYS_utimensat, AT_FDCWD, "/utime-target",
		    (long)ts, 0, 0, 0, rv);
	fails += tawc_io_step(
		"utimensat(\"/utime-target\", 0) -> 0", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	struct stat st;
	long sr = inline_fstatat(AT_FDCWD, "/utime-target", &st, 0);
	fails += tawc_io_step(
		"fstatat post-utimensat (target file)", sr == 0);
	fails += tawc_io_step(
		"utimensat updated target file's st_mtime",
		(long)st.st_mtime == 1234567891L);
	tawc_io_kv_dec("    target.st_mtime (after)",
		       (long)st.st_mtime);
	return fails;
}

#if !defined(__ANDROID__)
/* mknodat: create a FIFO (S_IFIFO doesn't need CAP_MKNOD). The
 * test fixture rootfs lives on tmpfs which supports mkfifo, so a
 * successful return tells us the dispatch is wired up AND the
 * path translation routed to the rootfs view (not the host).
 *
 * Host-only: Android shell SELinux denies mknod on /data/local/tmp
 * shell_data_file, and the coverage is not worth making device tests
 * require root. */
static int test_mknodat_fifo(void)
{
	int fails = 0;
	long rv;
	/* S_IFIFO (0010000) | 0644 */
	const long mode = 0010000 | 0644;
	INLINE_SYS6(TAWC_SYS_mknodat, AT_FDCWD, "/tawcroot-mknod-fifo",
		    mode, 0, 0, 0, rv);
	fails += tawc_io_step(
		"mknodat(\"/tawcroot-mknod-fifo\", S_IFIFO|0644) -> 0",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* Verify the FIFO landed in the rootfs view. */
	struct stat st;
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/tawcroot-mknod-fifo",
		    &st, 0, 0, 0, rv);
	fails += tawc_io_step("fstatat sees FIFO", rv == 0);
	fails += tawc_io_step("S_ISFIFO on the new node",
			      (st.st_mode & 0170000) == 0010000);

	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD, "/tawcroot-mknod-fifo",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("unlinkat(FIFO) -> 0", rv == 0);
	return fails;
}

#if defined(__x86_64__)
/* Legacy mknod (NR 133): same FIFO test via the legacy syscall.
 * aarch64 has no legacy mknod (only mknodat NR 33). */
static int test_legacy_mknod_fifo(void)
{
	int fails = 0;
	long rv;
	const long mode = 0010000 | 0644;
	INLINE_SYS6(TAWC_SYS_mknod, "/tawcroot-mknod-legacy",
		    mode, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy mknod(\"/tawcroot-mknod-legacy\", S_IFIFO|0644) -> 0",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);
	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD, "/tawcroot-mknod-legacy",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("unlinkat(legacy FIFO) -> 0", rv == 0);
	return fails;
}
#endif
#endif

/* statfs: should route through translation and return 0 against an
 * in-rootfs path. The struct-statfs contents we don't validate
 * here — that depends on the host fs the fixture lives on. The
 * 128-byte stack buffer is comfortably larger than the kernel's
 * statfs64 (88 bytes including padding). */
static int test_statfs_in_rootfs(void)
{
	int fails = 0;
	long rv;
	char sb[128];
	INLINE_SYS6(TAWC_SYS_statfs, "/etc/probe", sb, 0, 0, 0, 0, rv);
	fails += tawc_io_step("statfs(\"/etc/probe\") -> 0", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* Outside-rootfs path must still translate (clamp) and not
	 * leak host paths. /../etc/probe folds to /etc/probe inside
	 * the rootfs view. */
	INLINE_SYS6(TAWC_SYS_statfs, "/../etc/probe", sb,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"statfs(\"/../etc/probe\") -> 0 (clamp)",
		rv == 0);
	return fails;
}

/* getxattr: the test rootfs (tmpfs) doesn't carry user xattrs we
 * set; expect -ENODATA (-61) for an unset key, NOT -ENOSYS. The
 * point is that the dispatch is registered and the kernel saw the
 * call against an in-rootfs path. */
static int test_xattr_dispatch(void)
{
	int fails = 0;
	long rv;
	char buf[64];
	INLINE_SYS6(TAWC_SYS_getxattr, "/etc/probe", "user.tawcroot.test",
		    buf, sizeof buf, 0, 0, rv);
	fails += tawc_io_step(
		"getxattr(\"/etc/probe\", \"user.tawcroot.test\") "
		"-> -ENODATA / -EOPNOTSUPP (not -ENOSYS)",
		rv == -61 /*ENODATA*/ || rv == -95 /*EOPNOTSUPP*/);
	tawc_io_kv_dec("    rv", rv);

	/* lgetxattr: same syscall family, NOFOLLOW path mode. */
	INLINE_SYS6(TAWC_SYS_lgetxattr, "/etc/probe", "user.tawcroot.test",
		    buf, sizeof buf, 0, 0, rv);
	fails += tawc_io_step(
		"lgetxattr(\"/etc/probe\", \"user.tawcroot.test\") "
		"-> -ENODATA / -EOPNOTSUPP",
		rv == -61 || rv == -95);
	tawc_io_kv_dec("    rv", rv);

	/* listxattr: same shape, expect 0-length list or -EOPNOTSUPP. */
	INLINE_SYS6(TAWC_SYS_listxattr, "/etc/probe", buf, sizeof buf,
		    0, 0, 0, rv);
	fails += tawc_io_step(
		"listxattr(\"/etc/probe\") -> >=0 / -EOPNOTSUPP",
		rv >= 0 || rv == -95);
	tawc_io_kv_dec("    rv", rv);

	/* l*xattr against the rootfs root (suffix=="") must short-
	 * circuit to -EOPNOTSUPP. The handler dispatches through
	 * /proc/self/fd/<n>, which IS itself a magic symlink in
	 * procfs — the kernel's NOFOLLOW lsetxattr would target the
	 * procfs entry rather than the dirfd's underlying inode.
	 * The handler returns -EOPNOTSUPP rather than misroute. */
	INLINE_SYS6(TAWC_SYS_lgetxattr, "/", "user.tawcroot.test",
		    buf, sizeof buf, 0, 0, rv);
	fails += tawc_io_step("lgetxattr(\"/\") -> -EOPNOTSUPP", rv == -95);
	tawc_io_kv_dec("    rv", rv);
	INLINE_SYS6(TAWC_SYS_llistxattr, "/", buf, sizeof buf,
		    0, 0, 0, rv);
	fails += tawc_io_step("llistxattr(\"/\") -> -EOPNOTSUPP", rv == -95);
	tawc_io_kv_dec("    rv", rv);

	/* getxattr (FOLLOW variant) on the rootfs root is fine —
	 * the magic symlink resolves to the dirfd's inode and the
	 * call returns -ENODATA / -EOPNOTSUPP from the kernel as
	 * usual. */
	INLINE_SYS6(TAWC_SYS_getxattr, "/", "user.tawcroot.test",
		    buf, sizeof buf, 0, 0, rv);
	fails += tawc_io_step(
		"getxattr(\"/\") -> -ENODATA / -EOPNOTSUPP",
		rv == -61 || rv == -95);
	tawc_io_kv_dec("    rv", rv);
	return fails;
}

/* fchownat: fake-root no-op should succeed even though our host
 * uid can't actually chown anything. */
static int test_fchownat_fake_root(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_fchownat, AT_FDCWD, "/etc/probe",
		    0 /*uid*/, 0 /*gid*/, 0 /*flags*/, 0, rv);
	fails += tawc_io_step(
		"fchownat(\"/etc/probe\", 0, 0) -> 0 (fake-root no-op)",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);
	return fails;
}

/* fd-only fchown: GNU tar uses this while dpkg-deb extracts control
 * files. Same fake-root no-op semantics as fchownat. */
static int test_fchown_fake_root(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step("open /etc/probe for fchown", fd >= 0);
	if (fd < 0) {
		tawc_io_kv_dec("    fd", fd);
		return fails;
	}
	long rv;
	INLINE_SYS6(TAWC_SYS_fchown, fd, 0 /*uid*/, 0 /*gid*/, 0, 0, 0, rv);
	fails += tawc_io_step(
		"fchown(fd, 0, 0) -> 0 (fake-root no-op)",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);
	INLINE_SYS6(TAWC_SYS_close, fd, 0, 0, 0, 0, 0, rv);
	return fails;
}

/* fstatat AT_EMPTY_PATH with NON-NULL empty string ("" not NULL).
 * glibc's fstat() implements `fstat(fd, &st)` as
 * `fstatat(fd, "", &st, AT_EMPTY_PATH)` with a non-NULL pointer to
 * an empty string. An earlier handler version only short-circuited
 * gpath==0 and fell through to translate "" against the kernel cwd,
 * which routed the stat to the wrong inode and made wc segfault.
 * Verify the empty-string case routes through dirfd directly. */
static int test_fstatat_at_empty_path(void)
{
	int fails = 0;
	long rv, fd;
	INLINE_SYS6(TAWC_SYS_openat, AT_FDCWD, "/etc/probe", O_RDONLY,
		    0, 0, 0, fd);
	if (fd >= 0) {
		struct stat st;
		INLINE_SYS6(TAWC_SYS_fstatat, fd, "", &st,
			    AT_EMPTY_PATH, 0, 0, rv);
		fails += tawc_io_step(
			"fstatat(fd, \"\", AT_EMPTY_PATH) -> 0",
			rv == 0);
		fails += tawc_io_step(
			"fstatat(fd, \"\", AT_EMPTY_PATH) decorates uid==0",
			rv == 0 && st.st_uid == 0);
		tawc_io_kv_dec("    st_size", (long)st.st_size);
		tawc_close((int)fd);
	}
	return fails;
}

/* statx with fake-root decoration: stx_uid / stx_gid should both
 * be 0 even though the on-disk file is owned by `shell`. */
static int test_statx_fake_root_decoration(void)
{
	int fails = 0;
	struct statx sx;
	long rv;
	INLINE_SYS6(TAWC_SYS_statx, AT_FDCWD, "/etc/probe",
		    0, 0x7ff /*STATX_BASIC_STATS*/, &sx, 0, rv);
	fails += tawc_io_step("statx(\"/etc/probe\")", rv == 0);
	if (rv == 0) {
		fails += tawc_io_step("statx fake-root: stx_uid == 0",
				      sx.stx_uid == 0);
		fails += tawc_io_step("statx fake-root: stx_gid == 0",
				      sx.stx_gid == 0);
		tawc_io_kv_dec("    stx_size", (long)sx.stx_size);
	}
	return fails;
}

/* linkat -- happy path. /etc/probe -> /etc/probe-link2. We use AT_FDCWD
 * as both dirfds; the handler translates each operand. Hard link
 * should succeed inside the rootfs. */
static int test_linkat_happy_path(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_linkat, AT_FDCWD, "/etc/probe",
		    AT_FDCWD, "/etc/probe-link2", 0, 0, rv);
	fails += tawc_io_step("linkat(\"/etc/probe\" -> "
			      "\"/etc/probe-link2\") -> 0",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);
	/* clean up */
	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD,
		    "/etc/probe-link2", 0, 0, 0, 0, rv);
	return fails;
}

/* renameat2 -- create /tmp-ren-a, rename to /tmp-ren-b, verify, clean up. */
static int test_renameat2_happy_path(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_mkdirat, AT_FDCWD, "/tmp-ren-a",
		    0755, 0, 0, 0, rv);
	fails += tawc_io_step("mkdirat(\"/tmp-ren-a\") setup", rv == 0);

	INLINE_SYS6(TAWC_SYS_renameat2, AT_FDCWD, "/tmp-ren-a",
		    AT_FDCWD, "/tmp-ren-b", 0, 0, rv);
	fails += tawc_io_step("renameat2(\"/tmp-ren-a\" -> \"/tmp-ren-b\") -> 0",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* old name should be gone */
	struct stat st;
	long sr;
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/tmp-ren-a", &st, 0, 0, 0, sr);
	fails += tawc_io_step("fstatat(\"/tmp-ren-a\") -> -ENOENT (gone)",
			      sr == -2);

	/* new name should exist */
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/tmp-ren-b", &st, 0, 0, 0, sr);
	fails += tawc_io_step("fstatat(\"/tmp-ren-b\") -> 0 (exists)",
			      sr == 0);

	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD, "/tmp-ren-b",
		    AT_REMOVEDIR, 0, 0, 0, rv);
	return fails;
}

/* renameat2 -- escape attempt. Source resolves to inside rootfs by
 * `..`-clamp, so even if it doesn't exist we get ENOENT, NOT a host
 * file rename. */
static int test_renameat2_escape_clamped(void)
{
	int fails = 0;
	long rv;
	INLINE_SYS6(TAWC_SYS_renameat2, AT_FDCWD,
		    "/../../etc/passwd", AT_FDCWD,
		    "/etc/captured", 0, 0, rv);
	fails += tawc_io_step("renameat2 escape attempt -> -ENOENT (clamped)",
			      rv == -2);
	tawc_io_kv_dec("    rv", rv);
	return fails;
}

/* truncate -- create a file via openat O_CREAT, truncate to 1 byte,
 * stat to verify, unlink. */
static int test_truncate_create_and_resize(void)
{
	int fails = 0;
	long fd;
	INLINE_SYS6(TAWC_SYS_openat, AT_FDCWD, "/tmp-trunc-test",
		    0x42 /*O_CREAT|O_RDWR*/, 0644, 0, 0, fd);
	fails += tawc_io_step("openat(O_CREAT) /tmp-trunc-test", fd >= 0);
	if (fd >= 0) {
		long w = tawc_write((int)fd, "hello-truncate", 14);
		fails += tawc_io_step("write 14 bytes", w == 14);
		tawc_close((int)fd);
	}

	long rv;
	INLINE_SYS6(TAWC_SYS_truncate, "/tmp-trunc-test", 5, 0, 0, 0, 0, rv);
	fails += tawc_io_step("truncate(\"/tmp-trunc-test\", 5) -> 0",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	struct stat st;
	long sr;
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/tmp-trunc-test",
		    &st, 0, 0, 0, sr);
	fails += tawc_io_step("post-truncate st_size == 5",
			      sr == 0 && st.st_size == 5);
	tawc_io_kv_dec("    st_size", (long)st.st_size);

	INLINE_SYS6(TAWC_SYS_unlinkat, AT_FDCWD, "/tmp-trunc-test",
		    0, 0, 0, 0, rv);
	return fails;
}

/* Generic in-rootfs symlink resolution. The fake rootfs places
 * `/etc/host-secret` as an absolute symlink -> `/etc/passwd`. The
 * manual resolver in path_resolve.c clamps absolute symlink targets
 * at the rootfs (so `/etc/passwd` is re-rooted at <rootfs>/etc/passwd,
 * which doesn't exist in the fake rootfs -> ENOENT). The resolver
 * runs unconditionally during translate, so this clamping is uniform
 * across all path-bearing handlers (openat, fstatat, readlinkat, …). */
static int test_openat_abs_symlink_resolver_clamps(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/etc/host-secret",
				O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/etc/host-secret\" abs-symlink) -> ENOENT (resolver clamps)",
		fd == -2);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Symlink chain (FOLLOW): /chain1 -> chain2, /chain2 -> chain3,
 * /chain3 -> etc/probe. Resolver should walk all three hops. */
static int test_openat_symlink_chain_three_hops(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/chain1", O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/chain1\") through 3-hop relative chain -> probe content",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Self-loop: /loop -> loop. Resolver must terminate with ELOOP
 * rather than spinning. */
static int test_openat_symlink_self_loop_eloop(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/loop", O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/loop\") self-symlink -> ELOOP",
		fd == -40);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Symlink with absolute target inside the rootfs: /altpath -> /etc/probe.
 * Resolver re-roots the absolute target at the rootfs and reads probe. */
static int test_openat_abs_target_in_rootfs(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/altpath", O_RDONLY, 0);
	fails += tawc_io_step(
		"openat(\"/altpath\") abs-target-in-rootfs -> probe content",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* O_PATH | O_NOFOLLOW on the same absolute-target symlink must give
 * back an fd referring to the SYMLINK ITSELF (S_IFLNK), not the
 * target. This is exactly what glibc's lchmod / fchmodat
 * AT_SYMLINK_NOFOLLOW emulation issues; if O_NOFOLLOW gets dropped
 * on the floor (e.g. handler hard-codes the wrong arch's bit value)
 * the resolver follows /altpath through to /etc/probe and the fd
 * comes back as a regular file, which propagates as libarchive's
 * "Can't set permissions" warning during pacman extraction.
 *
 * The arch-correct O_NOFOLLOW comes from <linux/fcntl.h> at file
 * top — pre-fix the in-file fallback hard-coded the x86_64 value
 * (0x20000), wrong on aarch64 (0x8000). */
static int test_openat_opath_nofollow_abs_symlink(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/altpath",
				O_PATH | O_NOFOLLOW, 0);
	fails += tawc_io_step(
		"openat(\"/altpath\", O_PATH|O_NOFOLLOW) -> fd",
		fd >= 0);
	if (fd >= 0) {
		struct stat st;
		long sr;
		INLINE_SYS6(TAWC_SYS_fstatat, (int)fd, "", &st,
			    AT_EMPTY_PATH, 0, 0, sr);
		int is_symlink = sr == 0 && (st.st_mode & 0170000) == 0120000;
		fails += tawc_io_step(
			"fstat on the O_PATH|O_NOFOLLOW fd -> S_IFLNK "
			"(symlink, not target)",
			is_symlink);
		tawc_io_kv_dec("    st_mode", (long)st.st_mode);
		tawc_close((int)fd);
	}
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* NOFOLLOW preserves a leaf symlink. lstat'ing /chain1 should
 * report S_IFLNK, not the target's type (otherwise the resolver
 * is incorrectly walking into NOFOLLOW leaves). */
static int test_lstat_nofollow_preserves_leaf_symlink(void)
{
	int fails = 0;
	struct stat st;
	long rv;
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/chain1", &st,
		    AT_SYMLINK_NOFOLLOW, 0, 0, rv);
	int is_symlink = rv == 0 && (st.st_mode & 0170000) == 0120000;
	fails += tawc_io_step(
		"lstat(\"/chain1\") NOFOLLOW -> S_IFLNK (resolver leaves leaf alone)",
		is_symlink);
	return fails;
}

/* Mode-aware memoization: when /lib is the leaf and the operation
 * is NOFOLLOW (lstat), the memoizer must NOT rewrite /lib -> usr/lib
 * -- otherwise lstat would report the directory's mode, not the
 * symlink's. With FOLLOW (stat), it should rewrite. The fake rootfs
 * has /lib as a symlink to usr/lib. */
static int test_mode_aware_memoization(void)
{
	int fails = 0;
	struct stat st;
	long rv;
	/* lstat: NOFOLLOW. Memo must be suppressed; kernel walks "lib"
	 * as the leaf and sees the symlink. S_IFLNK = 0120000 = 40960. */
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/lib", &st,
		    AT_SYMLINK_NOFOLLOW, 0, 0, rv);
	int is_lnk = rv == 0 && (st.st_mode & 0170000) == 0120000;
	fails += tawc_io_step("lstat(\"/lib\") -> S_IFLNK (memo suppressed)",
			      is_lnk);
	tawc_io_kv_dec("    st_mode", (long)st.st_mode);
	tawc_io_kv_dec("    rv", rv);

	/* stat: FOLLOW. Memo rewrites; kernel sees "usr/lib" which is
	 * a directory. S_IFDIR = 040000 = 16384. */
	INLINE_SYS6(TAWC_SYS_fstatat, AT_FDCWD, "/lib", &st,
		    0, 0, 0, rv);
	int is_dir = rv == 0 && (st.st_mode & 0170000) == 0040000;
	fails += tawc_io_step("stat(\"/lib\") -> S_IFDIR (memo applied)",
			      is_dir);
	tawc_io_kv_dec("    st_mode", (long)st.st_mode);
	return fails;
}

#if defined(__x86_64__)
/* Legacy x86_64 wrappers. The Android-filter side of this is what
 * the design notes call out -- even if our build's filter doesn't
 * trap these, the inherited zygote filter will, and our TRAP wins.
 * Here we just verify our handler dispatches correctly. */
static int test_legacy_x86_64_wrappers(void)
{
	int fails = 0;
	struct stat st;
	long rv;
	INLINE_SYS6(TAWC_SYS_stat, "/etc/probe", &st, 0, 0, 0, 0, rv);
	fails += tawc_io_step("stat(\"/etc/probe\") (legacy) -> 0",
			      rv == 0);
	fails += tawc_io_step("legacy stat fake-root: st_uid == 0",
			      st.st_uid == 0);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_access, "/etc/probe", 4, 0, 0, 0, 0, rv);
	fails += tawc_io_step("access(\"/etc/probe\", R_OK) (legacy) -> 0",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_mkdir, "/legacy-mkdir-test", 0755,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("mkdir(\"/legacy-mkdir-test\") (legacy)",
			      rv == 0);
	INLINE_SYS6(TAWC_SYS_rmdir, "/legacy-mkdir-test",
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("rmdir(\"/legacy-mkdir-test\") (legacy)",
			      rv == 0);

	/* legacy link + rename + symlink. */
	INLINE_SYS6(TAWC_SYS_link, "/etc/probe", "/etc/probe-l3",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("link(\"/etc/probe\" -> \"/etc/probe-l3\") (legacy)",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_rename, "/etc/probe-l3", "/etc/probe-l4",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("rename(\"/etc/probe-l3\" -> \"/etc/probe-l4\") (legacy)",
			      rv == 0);
	INLINE_SYS6(TAWC_SYS_unlink, "/etc/probe-l4",
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("unlink(\"/etc/probe-l4\") (legacy)", rv == 0);

	INLINE_SYS6(TAWC_SYS_symlink, "etc/probe", "/legacy-symlink",
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("symlink(\"etc/probe\" -> \"/legacy-symlink\") (legacy)",
			      rv == 0);
	INLINE_SYS6(TAWC_SYS_unlink, "/legacy-symlink",
		    0, 0, 0, 0, 0, rv);
	return fails;
}

/* Legacy creat / utime / utimes / futimesat (x86_64-only). These were
 * unregistered while open/stat/unlink were trapped — a static binary
 * issuing raw NR 85/132/235/261 resolved against the HOST view. */
static int test_legacy_time_and_creat(void)
{
	int fails = 0;
	long rv;

	INLINE_SYS6(TAWC_SYS_creat, "/run/legacy-creat", 0644,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("creat(\"/run/legacy-creat\") (legacy) -> fd",
			      rv >= 0);
	tawc_io_kv_dec("    rv", rv);
	if (rv >= 0) tawc_close((int)rv);

	/* utime: set mtime to a recognizable value, stat it back. */
	long utb[2] = { 1000, 2000 };  /* actime, modtime */
	INLINE_SYS6(TAWC_SYS_utime, "/run/legacy-creat", utb, 0, 0, 0, 0, rv);
	fails += tawc_io_step("utime(file, {1000,2000}) (legacy) -> 0",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);
	struct stat st;
	if (inline_fstatat(AT_FDCWD, "/run/legacy-creat", &st, 0) == 0) {
		fails += tawc_io_step("utime set mtime == 2000",
				      st.st_mtime == 2000);
		tawc_io_kv_dec("    st_mtime", (long)st.st_mtime);
	}

	long tv_ok[4]  = { 3000, 0, 4000, 0 };       /* {sec,usec} x2 */
	INLINE_SYS6(TAWC_SYS_utimes, "/run/legacy-creat", tv_ok,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("utimes(file, valid) (legacy) -> 0", rv == 0);
	long tv_bad[4] = { 0, 1000000, 0, 0 };       /* usec out of range */
	INLINE_SYS6(TAWC_SYS_utimes, "/run/legacy-creat", tv_bad,
		    0, 0, 0, 0, rv);
	fails += tawc_io_step("utimes(file, usec=1e6) -> EINVAL",
			      rv == TAWC_EINVAL);
	tawc_io_kv_dec("    rv", rv);

	long tv2[4] = { 5000, 0, 6000, 0 };
	INLINE_SYS6(TAWC_SYS_futimesat, AT_FDCWD, "/run/legacy-creat", tv2,
		    0, 0, 0, rv);
	fails += tawc_io_step("futimesat(AT_FDCWD, file) (legacy) -> 0",
			      rv == 0);
	if (inline_fstatat(AT_FDCWD, "/run/legacy-creat", &st, 0) == 0) {
		fails += tawc_io_step("futimesat set mtime == 6000",
				      st.st_mtime == 6000);
		tawc_io_kv_dec("    st_mtime", (long)st.st_mtime);
	}

	INLINE_SYS6(TAWC_SYS_unlink, "/run/legacy-creat", 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("unlink legacy-creat cleanup", rv == 0);
	return fails;
}

/* Legacy epoll_wait → epoll_pwait translation (x86_64-only).
 *
 * Bug pre-fix (wezterm reproducer): Android's untrusted_app filter
 * RET_TRAPs epoll_wait (NR 232) on x86_64; tawcroot had no dispatch
 * entry; empty-slot fallback returned -ENOSYS to the guest. mio's epoll
 * backend treats that as fatal ("polling for events: ENOSYS;
 * terminating") so wezterm exits before drawing a frame.
 *
 * The synthesized Android filter (--include-legacy-x86_64 in the
 * androidfilter wrapper) traps NR 232. With the handler installed,
 * epoll_wait must succeed via internal redirect to epoll_pwait. Without
 * the redirect this would all return -ENOSYS. */
static int test_legacy_epoll_wait(void)
{
	int fails = 0;
	long rv;

	/* epoll_create1(EPOLL_CLOEXEC=0x80000) — kernel call, not trapped. */
	INLINE_SYS6(291 /*epoll_create1*/, 0x80000, 0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("epoll_create1(EPOLL_CLOEXEC) -> fd >= 0",
			      rv >= 0);
	if (rv < 0) {
		tawc_io_kv_dec("    errno (-rv)", -rv);
		return fails;
	}
	int epfd = (int)rv;

	/* No fds registered + timeout=0 must return 0 events. The kernel
	 * doesn't touch `events` (nevents=0) so the buffer's contents and
	 * exact size don't matter; pass enough room for one struct
	 * epoll_event (12 bytes packed on x86_64) just to be defensive
	 * against future maxevents>0 extensions of this test. */
	char events[64];
	INLINE_SYS6(TAWC_SYS_epoll_wait, (long)epfd, (long)events, 1, 0, 0, 0,
		    rv);
	fails += tawc_io_step(
		"legacy epoll_wait(empty, timeout=0) -> 0 (translated to epoll_pwait)",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* Bad fd: must surface kernel's -EBADF, NOT a stale -ENOSYS from
	 * an empty dispatch slot. Distinguishes "translation happened" from
	 * "translation silently dropped". */
	INLINE_SYS6(TAWC_SYS_epoll_wait, -1L, (long)events, 1, 0, 0, 0, rv);
	fails += tawc_io_step(
		"legacy epoll_wait(bad fd) -> -EBADF (not -ENOSYS)",
		rv == -9 /*EBADF*/);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_close, (long)epfd, 0, 0, 0, 0, 0, rv);
	return fails;
}
#endif

/* ioctl trap + TCGETS2 → TCGETS translation (both arches).
 *
 * Bug: Android's untrusted_app_all_devpts ioctl xperm whitelist
 * includes the legacy TCGETS family but not the termios2 variants.
 * Glibc's tcgetattr issues TCGETS2 first and only falls back on
 * -EINVAL, NOT on -EACCES. Bash sees -EACCES from tcgetattr(stdin),
 * decides stdin isn't a tty, and never prints a prompt or starts
 * readline. Same root cause for lxterminal and wezterm rendering an
 * empty pane on the emulator.
 *
 * The handler can't be exercised via the actual EACCES path on a
 * vanilla host kernel (no SELinux gate), so this test instead pins
 * the behaviour we depend on:
 *
 *   - Unknown ioctl numbers pass through unchanged. We use
 *     ioctl(regfile, FIONBIO, &one) to flip O_NONBLOCK and check it
 *     via fcntl(F_GETFL). Pure pass-through; no termios2 in play.
 *   - TCGETS / TCGETS2 / TCSETS2 on a non-tty fd all return -ENOTTY.
 *     This proves the four-cmd termios2 dispatch reaches the kernel
 *     via the legacy ioctl number rather than getting stuck in our
 *     handler.
 *   - TCGETS2 with a NULL arg returns -EFAULT before issuing the
 *     underlying ioctl (defends the user-pointer check). */
static int test_ioctl_translation(void)
{
	int fails = 0;
	long rv;

	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step("open /etc/probe (regular file) for ioctl test",
			      fd >= 0);
	if (fd < 0) {
		tawc_io_kv_dec("    open errno (-rv)", -fd);
		return fails;
	}

	/* FIONBIO = 0x5421: unknown to our handler, must pass through. */
	int nb = 1;
	INLINE_SYS6(TAWC_SYS_ioctl, fd, 0x5421L, (long)&nb, 0, 0, 0, rv);
	fails += tawc_io_step("ioctl(fd, FIONBIO, &1) -> 0 (passthrough)",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* Confirm O_NONBLOCK actually got set (not just that ioctl returned 0). */
	long flags;
	INLINE_SYS6(TAWC_SYS_fcntl, fd, 3 /*F_GETFL*/, 0, 0, 0, 0, flags);
	fails += tawc_io_step("FIONBIO actually set O_NONBLOCK (fcntl F_GETFL)",
			      flags >= 0 && (flags & 04000 /*O_NONBLOCK*/));
	tawc_io_kv_dec("    flags", flags);

	/* TCGETS / TCGETS2 on a regular file: kernel returns -ENOTTY (-25).
	 * The TCGETS2 path goes through our translator and forwards as
	 * TCGETS — same kernel response, proving the dispatch reaches the
	 * kernel via the legacy number. */
	unsigned char tbuf[44]; /* sizeof(struct termios2) */
	INLINE_SYS6(TAWC_SYS_ioctl, fd, 0x5401L /*TCGETS*/,
		    (long)tbuf, 0, 0, 0, rv);
	fails += tawc_io_step("ioctl(regfile, TCGETS) -> -ENOTTY", rv == -25);
	tawc_io_kv_dec("    rv", rv);

	INLINE_SYS6(TAWC_SYS_ioctl, fd, 0x802C542AL /*TCGETS2*/,
		    (long)tbuf, 0, 0, 0, rv);
	fails += tawc_io_step(
		"ioctl(regfile, TCGETS2) -> -ENOTTY (translated to TCGETS)",
		rv == -25);
	tawc_io_kv_dec("    rv", rv);

	/* TCSETS2 on a regfile: -ENOTTY on lenient kernels, -EACCES on
	 * Android policies that gate write-side tty ioctls on regfiles
	 * (OnePlus 9). Both prove the dispatch reached the kernel — the
	 * actual data path is covered by test_ioctl_pty_translation. */
	INLINE_SYS6(TAWC_SYS_ioctl, fd, 0x402C542BL /*TCSETS2*/,
		    (long)tbuf, 0, 0, 0, rv);
	fails += tawc_io_step(
		"ioctl(regfile, TCSETS2) -> -ENOTTY or -EACCES (dispatch reached kernel)",
		rv == -25 || rv == -13);
	tawc_io_kv_dec("    rv", rv);

	/* (NULL-arg behaviour is pinned in test_ioctl_pty_translation —
	 * on a regfile the response diverges between the native and
	 * fallback paths, since the regfile's ioctl handler returns
	 * -ENOTTY before any pointer access while the fallback path's
	 * copy_to_guest faults on NULL. Both are correct for what they
	 * are; testing them here would just couple the assertion to
	 * which path the OS happened to take.) */

	INLINE_SYS6(TAWC_SYS_close, fd, 0, 0, 0, 0, 0, rv);
	return fails;
}

/* TCGETS2 / TCSETS2 round-trip on a real pty.
 *
 * Companion to test_ioctl_translation(): that one verifies the
 * dispatch + EFAULT + passthrough paths against a regfile (where
 * everything ends in -ENOTTY), this one verifies the actual
 * data-bearing paths against a real pty allocated through the
 * `/dev` bind set up in test_rootfs_syscalls_smoke.c.
 *
 * Coverage:
 *   - TCGETS2 on a pty returns success and a populated termios2 whose
 *     first 36 bytes match the legacy TCGETS view byte-for-byte.
 *     This pins the "translator preserves the kernel-ABI prefix"
 *     contract — a regression that, e.g., dropped c_cc[] or shifted
 *     a tcflag_t would land here.
 *   - The trailing 8 speed bytes are zeroed (handler design — see
 *     handle_ioctl in src/syscalls_fd.c). Native TCGETS2 fills these
 *     with the kernel's idea of the baud (38400 for ptys); our
 *     translation drops them because pty workloads ignore the
 *     speed_t fields and BOTHER is never set.
 *   - TCSETS2 round-trips: flip an obvious bit (ECHO off in c_lflag),
 *     set, get back, confirm the bit changed. This proves the
 *     reverse translation copies the user's first 36 bytes through
 *     to TCSETS rather than dropping or scrambling them.
 *   - FIONREAD passes through unchanged on the master fd. */
static int test_ioctl_pty_translation(void)
{
	int fails = 0;
	long rv;

	/* O_RDWR=2, O_NOCTTY=0o400=0x100 (so the pty doesn't become our
	 * controlling tty in the testhost process). */
	long master = inline_openat(AT_FDCWD, "/dev/ptmx",
				    2 /*O_RDWR*/ | 0x100 /*O_NOCTTY*/, 0);
	fails += tawc_io_step("open /dev/ptmx (via /dev bind)", master >= 0);
	if (master < 0) {
		tawc_io_kv_dec("    open errno (-rv)", -master);
		return fails;
	}

	/* TIOCSPTLCK = _IOW('T', 0x31, int): unlock the slave. */
	int unlock = 0;
	INLINE_SYS6(TAWC_SYS_ioctl, master, 0x40045431L /*TIOCSPTLCK*/,
		    (long)&unlock, 0, 0, 0, rv);
	fails += tawc_io_step("TIOCSPTLCK(master, 0) -> 0", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* TIOCGPTPEER = _IO('T', 0x41): allocate slave fd directly. Linux
	 * 4.13+; the host runs newer kernels in CI so safe to depend on. */
	INLINE_SYS6(TAWC_SYS_ioctl, master, 0x5441L /*TIOCGPTPEER*/,
		    (2 /*O_RDWR*/ | 0x100 /*O_NOCTTY*/), 0, 0, 0, rv);
	fails += tawc_io_step("TIOCGPTPEER(master) -> slave fd", rv >= 0);
	if (rv < 0) {
		tawc_io_kv_dec("    errno (-rv)", -rv);
		INLINE_SYS6(TAWC_SYS_close, master, 0, 0, 0, 0, 0, rv);
		return fails;
	}
	long slave = rv;

	/* Probe whether the underlying OS (or simulated outer filter)
	 * actually allows TCGETS2. TAWC_RAW issues from the stub IP,
	 * which our own seccomp filter unconditionally ALLOWs — so the
	 * result reflects ONLY any outer (Android-shape) filter, not our
	 * handler. Lets us assert the right post-condition for whichever
	 * environment we're running in. */
	unsigned char raw_probe[44];
	for (size_t i = 0; i < sizeof raw_probe; i++) raw_probe[i] = 0xCC;
	long raw_rv = TAWC_RAW(TAWC_SYS_ioctl, slave,
			       0x802C542AL /*TCGETS2*/, (long)raw_probe,
			       0, 0, 0);
	int xperm_blocks_tcgets2 = (raw_rv == -13 /*EACCES*/);
	tawc_io_kv_dec("    raw TCGETS2 rv (no handler in path)", raw_rv);

	/* Baseline via legacy TCGETS (36 bytes). */
	unsigned char t1[36];
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x5401L /*TCGETS*/,
		    (long)t1, 0, 0, 0, rv);
	fails += tawc_io_step("TCGETS(slave) -> 0", rv == 0);

	/* Translated TCGETS2 (44 bytes). Initialise the buffer so we
	 * notice if the handler forgets to write any prefix bytes. */
	unsigned char t2[44];
	for (size_t i = 0; i < sizeof t2; i++) t2[i] = 0xAA;
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x802C542AL /*TCGETS2*/,
		    (long)t2, 0, 0, 0, rv);
	fails += tawc_io_step(
		"TCGETS2(slave) -> 0 (handler returns success in both paths)",
		rv == 0);

	/* Bytes 0..35 must be identical to the legacy view regardless
	 * of which path the handler took (native TCGETS2 fills the same
	 * 36-byte prefix as TCGETS; the fallback explicitly copies the
	 * TCGETS result). This is what the entire translation depends
	 * on. */
	int prefix_match = 1;
	for (size_t i = 0; i < sizeof t1; i++) {
		if (t1[i] != t2[i]) { prefix_match = 0; break; }
	}
	fails += tawc_io_step(
		"TCGETS2 first 36 bytes byte-equal to TCGETS",
		prefix_match);

	/* Speed tail (bytes 36..43) is the contract that distinguishes
	 * the two paths:
	 *   - OS allows TCGETS2: handler took the native path; speed
	 *     fields hold the kernel's view (38400 default for ptys).
	 *   - OS blocks with EACCES: handler fell back to TCGETS; speed
	 *     fields are zeroed by handle_tcgets2_fallback().
	 * Either way they must NOT be the 0xAA initialisation sentinel —
	 * that would mean nothing was written.
	 *
	 * Pinning both branches catches "always fall back even when
	 * native works" regressions (would zero the speed fields on
	 * permissive kernels) AND "forgot to fall back at all"
	 * regressions (would leave the EACCES through to the guest). */
	unsigned int ispeed = (unsigned int)t2[36] |
			      ((unsigned int)t2[37] << 8)  |
			      ((unsigned int)t2[38] << 16) |
			      ((unsigned int)t2[39] << 24);
	unsigned int ospeed = (unsigned int)t2[40] |
			      ((unsigned int)t2[41] << 8)  |
			      ((unsigned int)t2[42] << 16) |
			      ((unsigned int)t2[43] << 24);
	tawc_io_kv_dec("    ispeed", (long)ispeed);
	tawc_io_kv_dec("    ospeed", (long)ospeed);
	if (xperm_blocks_tcgets2) {
		fails += tawc_io_step(
			"OS blocks TCGETS2: speed tail zeroed by fallback",
			ispeed == 0 && ospeed == 0);
	} else {
		fails += tawc_io_step(
			"OS allows TCGETS2: speed tail is the kernel's view (non-zero)",
			ispeed > 0 && ispeed == ospeed);
	}

	/* TCSETS2 round-trip. ECHO is in c_lflag (offset 12, bit 0o010=8).
	 * Capture original, flip, write via TCSETS2, read back via TCGETS,
	 * verify the bit changed in the kernel's view (proves TCSETS2
	 * actually reached TCSETS, not just returned 0 spuriously). */
	unsigned int orig_lflag = *(unsigned int *)(t2 + 12);
	*(unsigned int *)(t2 + 12) = orig_lflag & ~0x8U;
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x402C542BL /*TCSETS2*/,
		    (long)t2, 0, 0, 0, rv);
	fails += tawc_io_step(
		"TCSETS2(slave, ECHO cleared) -> 0 (translated to TCSETS)",
		rv == 0);

	unsigned char t3[36];
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x5401L /*TCGETS*/,
		    (long)t3, 0, 0, 0, rv);
	unsigned int new_lflag = *(unsigned int *)(t3 + 12);
	fails += tawc_io_step(
		"TCSETS2 actually cleared ECHO (verified via TCGETS)",
		(new_lflag & 0x8U) == 0);
	tawc_io_kv_dec("    orig_lflag", (long)orig_lflag);
	tawc_io_kv_dec("    new_lflag",  (long)new_lflag);

	/* Restore so we leave the pty as we found it (tear-down would
	 * cover this anyway since we close both fds, but be tidy). */
	*(unsigned int *)(t2 + 12) = orig_lflag;
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x402C542BL /*TCSETS2*/,
		    (long)t2, 0, 0, 0, rv);

	/* TCGETS2 with NULL arg on a real pty: the kernel's pty driver
	 * runs copy_to_user(arg, &kterm, ...) and faults on the bogus
	 * pointer, returning -EFAULT. Both paths must surface that:
	 *   - Native: kernel returns -EFAULT directly.
	 *   - Fallback (xperm-blocked): kernel returns -EACCES first;
	 *     handler falls back to TCGETS, which is also a write-direction
	 *     ioctl and also faults to -EFAULT.
	 * No defensive pre-check in handle_ioctl(); we trust the kernel
	 * to validate user pointers consistently. */
	INLINE_SYS6(TAWC_SYS_ioctl, slave, 0x802C542AL /*TCGETS2*/,
		    0L, 0, 0, 0, rv);
	fails += tawc_io_step("TCGETS2(pty, NULL) -> -EFAULT (kernel-validated)",
			      rv == -14);
	tawc_io_kv_dec("    rv", rv);

	/* FIONREAD on master with no slave-side input → 0 bytes available.
	 * Pure passthrough; no termios2 interaction. */
	int avail = -1;
	INLINE_SYS6(TAWC_SYS_ioctl, master, 0x541BL /*FIONREAD*/,
		    (long)&avail, 0, 0, 0, rv);
	fails += tawc_io_step("FIONREAD(master) -> 0 with empty pty",
			      rv == 0 && avail == 0);
	tawc_io_kv_dec("    avail", (long)avail);

	INLINE_SYS6(TAWC_SYS_close, slave,  0, 0, 0, 0, 0, rv);
	INLINE_SYS6(TAWC_SYS_close, master, 0, 0, 0, 0, 0, rv);
	return fails;
}

/* Internal-fd protection (Phase 0.5). The reserved range starts at
 * TAWCROOT_RESERVED_FD_BASE (1000); rootfs_fd is reserved at init.
 * From the guest's POV, every fd at/above the base does not exist:
 * close/dup/fcntl on it return -EBADF. close_range across the
 * boundary clamps. After the guest's most aggressive close attempt,
 * a path-bearing syscall must still work. */
static int test_internal_fd_protection(void)
{
	int fails = 0;
	long rv;
	/* Trap close on the rootfs fd: returns success, and the
	 * kernel actually closes the fd. (Earlier rev returned
	 * -EBADF without closing, but glibc's `closefrom()`
	 * — used by gpgme/libcurl/python pre-exec hygiene —
	 * polled `/proc/self/fd` until empty, looped forever
	 * because the lying-EBADF kept the fd in the table.
	 * See `notes/tawcroot.md` "Phase 5c — full integration
	 * suite, OnePlus 9".) The next path-bearing syscall lazy-
	 * reopens via `tawcroot_reopen_reserved_fd()` from the
	 * stashed `tawcroot_rootfs_host_path`. */
	INLINE_SYS6(TAWC_SYS_close, (long)tawcroot_rootfs_fd,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"close(rootfs_fd) -> 0 (kernel closes, lazy-reopen on next op)",
		rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* close_range over a wide range that crosses the boundary:
	 * clamps to base-1, kernel closes only guest-visible fds.
	 * We start at fd 3 so stderr stays open and we can keep
	 * printing test results after.
	 * close_range was added in kernel 5.9; on older kernels the
	 * handler's raw syscall returns -ENOSYS. Skip rather than fail
	 * (out-of-scope to polyfill new syscalls with older ones). */
	INLINE_SYS6(TAWC_SYS_close_range, 3, 0xffffffffu,
		    0, 0, 0, 0, rv);
	bool close_range_unsupported = (rv == -38 /*ENOSYS*/);
	if (close_range_unsupported) {
		tawc_io_skip(
			"close_range(3, ~0u) -> 0 (clamped at reserved boundary)",
			"kernel <5.9: close_range not available");
	} else {
		fails += tawc_io_step(
			"close_range(3, ~0u) -> 0 (clamped at reserved boundary)",
			rv == 0);
		tawc_io_kv_dec("    rv", rv);
	}

	/* close_range entirely above the boundary: success no-op. */
	if (close_range_unsupported) {
		tawc_io_skip(
			"close_range(reserved, ~0u) -> 0 (no-op)",
			"kernel <5.9: close_range not available");
	} else {
		INLINE_SYS6(TAWC_SYS_close_range,
			    TAWCROOT_RESERVED_FD_BASE,
			    0xffffffffu, 0, 0, 0, 0, rv);
		fails += tawc_io_step(
			"close_range(reserved, ~0u) -> 0 (no-op)", rv == 0);
	}

	/* fcntl(F_GETFD) on rootfs_fd: -EBADF. */
	INLINE_SYS6(TAWC_SYS_fcntl, (long)tawcroot_rootfs_fd,
		    1 /*F_GETFD*/, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"fcntl(rootfs_fd, F_GETFD) -> -EBADF", rv == -9);

	/* dup of the rootfs fd: -EBADF. */
	INLINE_SYS6(TAWC_SYS_dup, (long)tawcroot_rootfs_fd,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("dup(rootfs_fd) -> -EBADF", rv == -9);

#if defined(__x86_64__)
	/* dup2 onto a reserved newfd: -EBADF (would otherwise clobber). */
	INLINE_SYS6(TAWC_SYS_dup2, 0,
		    (long)tawcroot_rootfs_fd, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"dup2(0, rootfs_fd) -> -EBADF (newfd in reserved range)",
		rv == -9);
#endif

	/* The acid test: after all that, openat() still resolves
	 * inside the rootfs -- proves the reserved-range trick worked. */
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"path syscall after close_range -- openat still works",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	tawc_io_kv_dec("    rv", fd);
	return fails;
}

/* Guest seccomp / prctl(PR_SET_SECCOMP) handling. We can't honestly
 * install the guest's filter: it would stack on top of ours and could
 * kill our raw_syscall stub, return before our trap, or hand SIGSYS to
 * a guest-owned handler. Refuse with -EPERM and verify translation still
 * works after the denial. */
static int test_guest_seccomp_prctl_handling(void)
{
	int fails = 0;
	long rv;
	/* seccomp(SECCOMP_SET_MODE_FILTER, 0, NULL) -- enough to test
	 * the dispatch path without needing a valid filter program. */
	INLINE_SYS6(TAWC_SYS_seccomp, 1 /*SET_MODE_FILTER*/,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("seccomp(SET_MODE_FILTER) -> -EPERM",
			      rv == TAWC_EPERM);
	tawc_io_kv_dec("    rv", rv);

	/* prctl(PR_SET_SECCOMP) -> -EPERM (same rationale). */
	INLINE_SYS6(TAWC_SYS_prctl, 22 /*PR_SET_SECCOMP*/,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("prctl(PR_SET_SECCOMP) -> -EPERM",
			      rv == TAWC_EPERM);
	tawc_io_kv_dec("    rv", rv);

	/* io_uring_setup -> -ENOSYS (review D4). The guest must fall
	 * back to syscall-based I/O so our translator stays in the
	 * loop; pass-through would let the kernel read SQEs with
	 * host-relative paths from app-shared memory, bypassing us
	 * entirely. */
	INLINE_SYS6(TAWC_SYS_io_uring_setup,
		    0 /*entries*/, 0 /*params*/, 0, 0, 0, 0, rv);
	fails += tawc_io_step("io_uring_setup -> -ENOSYS (D4)",
			      rv == -38);
	tawc_io_kv_dec("    rv", rv);

	/* prctl(PR_GET_SECCOMP) passes through. We're under SECCOMP_MODE_FILTER
	 * so the kernel returns 2. */
	INLINE_SYS6(TAWC_SYS_prctl, 21 /*PR_GET_SECCOMP*/,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step(
		"prctl(PR_GET_SECCOMP) passes through (kernel mode 2)",
		rv == 2);
	tawc_io_kv_dec("    rv", rv);

	/* Path syscall still works after the denial. */
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"path syscall after seccomp denial -- still works",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

/* SIGSYS virtualization. The guest must not be able to remove our
 * handler (rt_sigaction(SIGSYS, SIG_DFL)) or block SIGSYS
 * (sigprocmask(SIG_BLOCK, {SIGSYS})). Both must round-trip in the
 * shadow but never reach the kernel. After each, a path syscall
 * proves the kernel disposition is still ours. */
static int test_sigsys_virtualization(void)
{
	int fails = 0;
	long rv;
	/* Field names kept short -- `sa_handler` is a glibc-side macro
	 * that breaks struct definitions when host headers are in
	 * scope. Layout matches the kernel's `struct sigaction` for
	 * each arch; we only round-trip bytes through the shadow. */
	struct {
		void  *h;
		unsigned long f;
#if defined(__x86_64__)
		void *r;
#endif
		uint64_t m;
	} act_set, act_get;
	for (size_t i = 0; i < sizeof act_set; i++)
		((unsigned char *)&act_set)[i] = 0;
	/* SIG_DFL = 0; we leave act_set zeroed. */

	INLINE_SYS6(TAWC_SYS_rt_sigaction, 31 /*SIGSYS*/, &act_set,
		    &act_get, 8 /*sigsetsize*/, 0, 0, rv);
	fails += tawc_io_step(
		"rt_sigaction(SIGSYS, SIG_DFL) -> 0 (shadowed)", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* Read it back: should match what we set. */
	struct { void *h; unsigned long f;
#if defined(__x86_64__)
		void *r;
#endif
		uint64_t m; } act_check;
	for (size_t i = 0; i < sizeof act_check; i++)
		((unsigned char *)&act_check)[i] = 0xff;
	INLINE_SYS6(TAWC_SYS_rt_sigaction, 31, 0, &act_check, 8, 0, 0, rv);
	int round_tripped = rv == 0 &&
			    act_check.h == 0 &&
			    act_check.f == 0 &&
			    act_check.m == 0;
	fails += tawc_io_step(
		"rt_sigaction(SIGSYS, oldact) reads shadow",
		round_tripped);

	/* Other signals pass through. SIGUSR1 = 10. */
	INLINE_SYS6(TAWC_SYS_rt_sigaction, 10 /*SIGUSR1*/, 0, &act_check,
		    8, 0, 0, rv);
	fails += tawc_io_step(
		"rt_sigaction(SIGUSR1, oldact) passes through", rv == 0);

	/* sigprocmask: try to block SIGSYS. The shadow records
	 * blocked, the kernel never sees the SIGSYS bit. */
	uint64_t set = (1ULL << (31 - 1)) | (1ULL << (10 - 1));
	uint64_t oldset = 0;
	INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/,
		    &set, &oldset, 8, 0, 0, rv);
	fails += tawc_io_step(
		"rt_sigprocmask(SIG_BLOCK, {SIGSYS, SIGUSR1}) -> 0",
		rv == 0);

	/* Read it back: shadow should claim SIGSYS is blocked AND
	 * the genuinely-blocked SIGUSR1 must persist. The latter
	 * caught a real bug where the handler was calling the kernel
	 * rt_sigprocmask and the change got rolled back by sigreturn
	 * restoring ucontext.uc_sigmask; the fix updates the
	 * ucontext mask directly. */
	uint64_t cur = 0;
	INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/,
		    0, &cur, 8, 0, 0, rv);
	int sigsys_blocked  = (cur & (1ULL << (31 - 1))) != 0;
	int sigusr1_blocked = (cur & (1ULL << (10 - 1))) != 0;
	fails += tawc_io_step(
		"rt_sigprocmask query: SIGSYS shows blocked (shadow)",
		rv == 0 && sigsys_blocked);
	fails += tawc_io_step(
		"rt_sigprocmask query: SIGUSR1 actually blocked (kernel)",
		rv == 0 && sigusr1_blocked);
	tawc_io_kv_hex("    cur mask", (unsigned long)cur);

	/* Acid test: with the guest THINKING SIGSYS is blocked, our
	 * trap must still fire -- proves the kernel mask is clear. */
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"path syscall after SIGSYS-block -- trap still fires",
		fd >= 0);
	if (fd >= 0) tawc_close((int)fd);

	/* Restore: unblock SIGSYS shadow + whatever we changed. */
	uint64_t unblock = (1ULL << (31 - 1)) | (1ULL << (10 - 1));
	INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 1 /*SIG_UNBLOCK*/,
		    &unblock, 0, 8, 0, 0, rv);
	return fails;
}

/* Review finding B2 -- rt_sigaction must read/write only the
 * arch-correct kernel-sigaction size, NOT the over-sized 64-byte
 * shadow. We mmap a 2-page region, mprotect the second page
 * PROT_NONE, and place the guest sigaction immediately before that
 * page boundary so an over-read of more bytes than the struct
 * spills into the unmapped page and trips EFAULT. With the bug
 * fixed, the call returns 0 (or the kernel value); with the bug,
 * it returns -14 because process_vm_readv refuses the over-read. */
static int test_rt_sigaction_b2_sizing(void)
{
	int fails = 0;
	long rv;
	void *region;
	long region_rv = TAWC_RAW(TAWC_SYS_mmap, 0, 8192,
			   3 /*PROT_R|W*/, 0x22 /*MAP_PRIVATE|ANON*/,
			   -1, 0);
	region = (void *)region_rv;
	fails += tawc_io_step(
		"B2 setup: mmap two-page region",
		region_rv > 0 || region_rv < -4095);
	if (region_rv > 0 || region_rv < -4095) {
		/* PROT_NONE the second page. */
		(void)TAWC_RAW(TAWC_SYS_mprotect,
			       (long)region + 4096, 4096, 0, 0, 0, 0);
		/* Place a minimum-arch-correct sigaction struct
		 * (24 bytes on aarch64, 32 on x86_64) right before
		 * the PROT_NONE boundary. */
#if defined(__x86_64__)
		const size_t sa_size = 32;
#else
		const size_t sa_size = 24;
#endif
		unsigned char *p = (unsigned char *)region + 4096 - sa_size;
		for (size_t i = 0; i < sa_size; i++) p[i] = 0;

		INLINE_SYS6(TAWC_SYS_rt_sigaction, 31 /*SIGSYS*/,
			    p, 0, 8 /*sigsetsize*/, 0, 0, rv);
		fails += tawc_io_step(
			"rt_sigaction(SIGSYS, &act_at_page_boundary) -> 0 (B2 sizing)",
			rv == 0);
		tawc_io_kv_dec("    rv", rv);

		/* Same on the oldact path: shadow write must not
		 * overflow the guest's struct. */
		INLINE_SYS6(TAWC_SYS_rt_sigaction, 31 /*SIGSYS*/,
			    0, p, 8, 0, 0, rv);
		fails += tawc_io_step(
			"rt_sigaction(SIGSYS, NULL, &oldact_at_boundary) -> 0 (B2 sizing)",
			rv == 0);
		tawc_io_kv_dec("    rv", rv);

		(void)TAWC_RAW(TAWC_SYS_munmap, (long)region, 8192,
			       0, 0, 0, 0);
	}
	return fails;
}

/* Multi-thread SIGSYS-blocked shadow, end-to-end through the handler.
 *
 * The unit suite exercises the lock-free primitives directly (16-thread
 * blocked isolation, 4×4 writer/reader seqlock, tombstone probe-chain
 * preservation, slot reclamation). What was missing — and what this
 * test adds — is concurrent coverage with traffic actually flowing
 * through `handle_rt_sigprocmask` / `handle_exit` on each TID. We
 * spawn N kernel threads via raw clone(2); each blocks/unblocks SIGSYS
 * through the guest's normal sigprocmask, runs a trapping path syscall
 * (proves the kernel mask is clear despite shadow saying "blocked"),
 * reads its mask back, and asserts the SIGSYS bit matches what it
 * just set on every iteration. On thread exit, handle_exit's
 * shadow-clear hook fires (exit(2) is in the trapped set).
 *
 * pthreads are unavailable (testhost is freestanding -nostdlib), so we
 * use raw clone(2). clone3 is intercepted with -ENOSYS, but plain
 * clone (NR 56 on x86_64, 220 on aarch64) is not in our trapped set —
 * RET_ALLOWed by the filter. */

/* clone(2) trampoline: parent gets the new TID (or -errno) back; child
 * runs `func(arg)` on the supplied stack and SYS_exits with the return
 * value. NOT exit_group — we want only the calling thread to die so
 * the testhost stays alive to join. */
extern long tawcroot_test_clone(int (*func)(void *), void *stack_top,
				 void *arg, unsigned long flags);

#if defined(__x86_64__)
__asm__(
	".text\n"
	".globl tawcroot_test_clone\n"
	".hidden tawcroot_test_clone\n"
	".type tawcroot_test_clone, @function\n"
	"tawcroot_test_clone:\n"
	/* SysV ABI: rdi=func, rsi=stack_top, rdx=arg, rcx=flags. */
	"	sub	$16, %rsi\n"
	"	mov	%rdi, 0(%rsi)\n"	/* child stack[0] = func */
	"	mov	%rdx, 8(%rsi)\n"	/* child stack[8] = arg  */
	"	mov	%rcx, %rdi\n"		/* clone arg0 = flags */
	"	xor	%rdx, %rdx\n"		/* parent_tidptr = NULL */
	"	xor	%r10, %r10\n"		/* child_tidptr  = NULL */
	"	xor	%r8, %r8\n"		/* tls           = 0    */
	"	mov	$56, %eax\n"		/* NR_clone (x86_64) */
	"	syscall\n"
	"	test	%rax, %rax\n"
	"	jnz	1f\n"			/* parent: tid in rax */
	/* child: rsp = (stack_top - 16); pop func, pop arg, call. */
	"	pop	%rax\n"			/* func -> rax */
	"	pop	%rdi\n"			/* arg  -> rdi */
	"	call	*%rax\n"
	"	mov	%eax, %edi\n"		/* exit code = func rv */
	"	mov	$60, %eax\n"		/* NR_exit (per-thread) */
	"	syscall\n"
	"	ud2\n"
	"1:	ret\n"
	".size tawcroot_test_clone, .-tawcroot_test_clone\n"
);
#elif defined(__aarch64__)
__asm__(
	".text\n"
	".globl tawcroot_test_clone\n"
	".hidden tawcroot_test_clone\n"
	".type tawcroot_test_clone, %function\n"
	"tawcroot_test_clone:\n"
	/* AAPCS64: x0=func, x1=stack_top, x2=arg, x3=flags. */
	"	sub	x1, x1, #16\n"
	"	str	x0, [x1, #0]\n"		/* child stack[0] = func */
	"	str	x2, [x1, #8]\n"		/* child stack[8] = arg  */
	"	mov	x0, x3\n"		/* clone arg0 = flags */
	/* aarch64 clone(flags, stack, parent_tid, tls, child_tid). */
	"	mov	x2, #0\n"
	"	mov	x3, #0\n"
	"	mov	x4, #0\n"
	"	mov	x8, #220\n"		/* NR_clone */
	"	svc	#0\n"
	"	cbz	x0, 1f\n"		/* x0 == 0 means child */
	"	ret\n"				/* parent: x0 = tid */
	"1:\n"
	"	ldr	x1, [sp, #0]\n"		/* func */
	"	ldr	x0, [sp, #8]\n"		/* arg */
	"	blr	x1\n"
	/* func returned in x0; SYS_exit expects status in x0. */
	"	mov	x8, #93\n"		/* NR_exit (aarch64) */
	"	svc	#0\n"
	"	brk	#0\n"
	".size tawcroot_test_clone, .-tawcroot_test_clone\n"
);
#endif

#define MT_THREADS    8
#define MT_ITERS    200
#define MT_STACK   (128 * 1024)

/* clone flags: pthread_create's set sans CLONE_PARENT_SETTID /
 * CLONE_CHILD_CLEARTID / CLONE_SETTLS. Sharing VM/SIGHAND/THREAD is
 * what makes the new thread share our SIGSYS handler installation and
 * get its own kernel TID. */
#define MT_CLONE_FLAGS                                                  \
	(0x00000100UL  /*CLONE_VM      */ |                             \
	 0x00000200UL  /*CLONE_FS      */ |                             \
	 0x00000400UL  /*CLONE_FILES   */ |                             \
	 0x00000800UL  /*CLONE_SIGHAND */ |                             \
	 0x00010000UL  /*CLONE_THREAD  */ |                             \
	 0x00040000UL  /*CLONE_SYSVSEM */)

struct mt_state {
	volatile int  done;             /* 1 once worker is about to exit  */
	volatile int  iters_completed;  /* monotonic from 0..MT_ITERS      */
	int           last_observed;    /* -1 = OK; else error code        */
	int           tid;              /* kernel TID (gettid)             */
};

static struct mt_state g_mt[MT_THREADS];

/* Worker — runs on the cloned thread. The only inter-thread channel is
 * its `mt_state` slot (volatile + atomic store on `done`). Returns the
 * status that the asm trampoline forwards into SYS_exit. */
static int mt_worker(void *p)
{
	struct mt_state *st = (struct mt_state *)p;
	long rv;
	INLINE_SYS6(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0, rv);
	st->tid = (int)rv;

	const uint64_t sigsys_bit = 1ULL << (31 - 1);  /* SIGSYS = 31 */
	int observed_match = 1;

	for (int i = 0; i < MT_ITERS; i++) {
		int want_blocked = i & 1;
		int how = want_blocked ? 0 /*SIG_BLOCK*/ : 1 /*SIG_UNBLOCK*/;
		uint64_t set = sigsys_bit;

		/* Block / unblock SIGSYS through the guest's sigprocmask
		 * (TRAPs into handle_rt_sigprocmask, which updates this
		 * thread's per-tid blocked shadow). */
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, how, &set, 0, 8,
			    0, 0, rv);
		if (rv != 0) { observed_match = 0; st->last_observed = (int)-rv; break; }

		/* Trapping path syscall — the handler runs on this thread
		 * with the shadow update fresh. If the kernel mask actually
		 * had SIGSYS blocked, the trap wouldn't fire and we'd see
		 * a kernel-issued openat with -ENOENT (rootfs not bound
		 * outside the handler). With the contract intact, this
		 * resolves through the rootfs to /etc/probe. */
		long fd;
		INLINE_SYS6(TAWC_SYS_openat, AT_FDCWD, "/etc/probe",
			    O_RDONLY, 0, 0, 0, fd);
		if (fd < 0) { observed_match = 0; st->last_observed = (int)-fd; break; }
		tawc_close((int)fd);

		/* Read the current mask back. The shadow's SIGSYS bit
		 * must match what we asked for. */
		uint64_t cur = 0;
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/,
			    0, &cur, 8, 0, 0, rv);
		if (rv != 0) { observed_match = 0; st->last_observed = (int)-rv; break; }
		int sigsys_blocked = (cur & sigsys_bit) != 0;
		if (sigsys_blocked != want_blocked) {
			observed_match = 0;
			/* 0xb0 = "we asked for unblocked, got blocked"
			 * 0xb1 = "we asked for blocked,   got unblocked"
			 * (high bit makes it unambiguous vs errno values). */
			st->last_observed = sigsys_blocked ? 0xb0 : 0xb1;
			break;
		}
		st->iters_completed = i + 1;
	}
	if (observed_match) st->last_observed = -1;

	/* Best-effort restore: leave SIGSYS unblocked. handle_exit will
	 * clear the slot regardless, but explicit unblock keeps the
	 * single-writer-per-tid invariant clean. */
	{
		uint64_t set = sigsys_bit;
		long rv2;
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 1 /*SIG_UNBLOCK*/,
			    &set, 0, 8, 0, 0, rv2);
		(void)rv2;
	}

	/* Publish done=1 with release ordering — main reads with acquire,
	 * sees a fully-written `last_observed` / `iters_completed` /
	 * `tid`. The subsequent SYS_exit (issued by the trampoline) is
	 * a hard happens-before edge with handle_exit's shadow-clear. */
	__atomic_store_n(&st->done, 1, __ATOMIC_RELEASE);
	return 0;
}

static int test_sigsys_block_shadow_multithread(void)
{
	int fails = 0;

	for (int i = 0; i < MT_THREADS; i++) {
		g_mt[i].done = 0;
		g_mt[i].iters_completed = 0;
		g_mt[i].last_observed = -2;  /* "never ran" sentinel */
		g_mt[i].tid = 0;
	}

	/* Allocate per-thread stacks. We deliberately don't munmap on
	 * teardown: by the time main observes done=1 the worker may
	 * still be executing instructions on its stack en route to
	 * SYS_exit, and we'd race the kernel. The testhost exits within
	 * a second of this test, so leaking 8 × 128 KiB is fine. */
	void *stacks[MT_THREADS];
	for (int i = 0; i < MT_THREADS; i++) {
		long rv = TAWC_RAW(TAWC_SYS_mmap, 0, MT_STACK,
				   3 /*PROT_READ|PROT_WRITE*/,
				   0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/,
				   -1, 0);
		stacks[i] = (rv > 0 || rv < -4095) ? (void *)rv : 0;
		fails += tawc_io_step("mt: mmap thread stack",
				      stacks[i] != 0);
	}
	if (fails) return fails;

	int n_spawned = 0;
	for (int i = 0; i < MT_THREADS; i++) {
		unsigned char *stack_top =
			(unsigned char *)stacks[i] + MT_STACK;
		long tid = tawcroot_test_clone(mt_worker, stack_top,
					       &g_mt[i], MT_CLONE_FLAGS);
		if (tid <= 0) {
			tawc_io_kv_dec("    clone errno (-rv)", -tid);
			fails += tawc_io_step("mt: clone thread", 0);
			break;
		}
		n_spawned++;
	}
	fails += tawc_io_step("mt: spawned all N threads",
			      n_spawned == MT_THREADS);

	/* Spin-wait for every thread to publish done=1. Workers do
	 * MT_ITERS × ~6 trapping syscalls; that's tens of millions of
	 * cycles total across N threads. We don't have access to
	 * clock_gettime here without going through a lot of setup, so
	 * just bound the spin generously (~3s on a 3 GHz host) and
	 * report time-out as a failure if it ever happens. */
	int all_done_final = 0;
	for (long spin = 0; spin < 200000000L; spin++) {
		int all_done = 1;
		for (int i = 0; i < n_spawned; i++) {
			if (__atomic_load_n(&g_mt[i].done,
					    __ATOMIC_ACQUIRE) != 1) {
				all_done = 0;
				break;
			}
		}
		if (all_done) { all_done_final = 1; break; }
#if defined(__x86_64__)
		__asm__ __volatile__ ("pause" ::: "memory");
#elif defined(__aarch64__)
		__asm__ __volatile__ ("yield" ::: "memory");
#endif
	}
	fails += tawc_io_step("mt: all threads finished within bound",
			      all_done_final);

	for (int i = 0; i < n_spawned; i++) {
		int ok = (g_mt[i].last_observed == -1) &&
			 (g_mt[i].iters_completed == MT_ITERS);
		if (!ok) {
			tawc_io_kv_dec("    thread idx", i);
			tawc_io_kv_dec("    tid", g_mt[i].tid);
			tawc_io_kv_dec("    iters", g_mt[i].iters_completed);
			tawc_io_kv_hex("    last_observed",
				       (unsigned long)(unsigned int)
					   g_mt[i].last_observed);
		}
		fails += tawc_io_step(
			"mt: thread mask round-trip consistent across iters",
			ok);
	}

	/* Verify the main thread's own shadow wasn't corrupted by
	 * concurrent writers. (The shadow is per-tid, so it shouldn't
	 * be — this is a belt-and-braces check.) Set + clear once and
	 * confirm the round-trip. */
	{
		long rv;
		uint64_t set = 1ULL << (31 - 1);
		uint64_t cur = 0;
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/,
			    &set, 0, 8, 0, 0, rv);
		fails += tawc_io_step("mt: main thread can still block SIGSYS shadow",
				      rv == 0);
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/,
			    0, &cur, 8, 0, 0, rv);
		fails += tawc_io_step(
			"mt: main thread shadow read-back shows SIGSYS blocked",
			rv == 0 && (cur & (1ULL << (31 - 1))) != 0);
		INLINE_SYS6(TAWC_SYS_rt_sigprocmask, 1 /*SIG_UNBLOCK*/,
			    &set, 0, 8, 0, 0, rv);
		fails += tawc_io_step("mt: main thread can unblock SIGSYS shadow",
				      rv == 0);
	}

	return fails;
}

/* Phase 2e — /proc/self/exe synthesis. The handler stashes the
 * guest's requested exec path; readlinkat against /proc/self/exe
 * (and /proc/<our-pid>/exe) returns that stash, NOT the kernel's
 * libtawcroot view. Set the stash via the public setter from
 * within the testhost (production main does this in real flow). */
static int test_proc_self_exe_synthesis(void)
{
	int fails = 0;
	extern void tawcroot_set_guest_exe_path(const char *);
	tawcroot_set_guest_exe_path("/usr/bin/some-guest");

	char buf[256];
	long rv;
	INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD,
		    "/proc/self/exe", buf, sizeof buf - 1, 0, 0, rv);
	fails += tawc_io_step(
		"readlinkat(\"/proc/self/exe\") -> guest exe path (2e)",
		rv == (long)tawc_strlen("/usr/bin/some-guest"));
	if (rv > 0) {
		buf[rv] = 0;
		int eq = tawc_streq(buf, "/usr/bin/some-guest");
		fails += tawc_io_step(
			"/proc/self/exe content matches stashed guest path",
			eq);
		tawc_io_str("    got = '"); tawc_io_str(buf);
		tawc_io_str("'\n");
	}

#if defined(__x86_64__)
	/* x86_64 also has the legacy `readlink(2)` (NR 89) on top of
	 * `readlinkat(2)` (NR 267). glibc on x86_64 routes `readlink`
	 * calls via the legacy NR for short paths, so the
	 * /proc/self/exe synthesis above doesn't fire unless our
	 * `handle_readlink` does the same lookup. Surfaced when
	 * Firefox's stub binary called `readlink("/proc/self/exe")`,
	 * got back the libtawcroot host path, and bailed with
	 * "Couldn't load XPCOM." — see notes/tawcroot.md "Phase 5c". */
	{
		extern void tawcroot_set_guest_exe_path(const char *);
		tawcroot_set_guest_exe_path("/usr/bin/legacy-guest");

		char buf2[256];
		long rv2;
		INLINE_SYS6(TAWC_SYS_readlink,
			    "/proc/self/exe", buf2, sizeof buf2 - 1,
			    0, 0, 0, rv2);
		fails += tawc_io_step(
			"readlink(\"/proc/self/exe\") -> guest exe path (legacy NR 89)",
			rv2 == (long)tawc_strlen("/usr/bin/legacy-guest"));
		if (rv2 > 0) {
			buf2[rv2] = 0;
			int eq = tawc_streq(buf2, "/usr/bin/legacy-guest");
			fails += tawc_io_step(
				"legacy readlink content matches stashed guest path",
				eq);
			tawc_io_str("    got = '"); tawc_io_str(buf2);
			tawc_io_str("'\n");
		}

		tawcroot_set_guest_exe_path(0);
	}
#endif

	/* Clear so the rest of the suite doesn't see a synthesized
	 * exe path. */
	tawcroot_set_guest_exe_path(0);
	return fails;
}

/* Review finding B4 — rootfs prefix boundary in resolve_relative
 * and handle_getcwd. Without a component-boundary check, a kernel
 * cwd at "<rootfs>-evil/foo" would byte-match the rootfs prefix and
 * be treated as inside the view; getcwd would leak "/-evil/foo" and
 * relative opens would silently route into the rootfs. The harness
 * built FAKE_ROOTFS_SIBLING ("<rootfs>-evil/foo/...") for us; we
 * chdir into it via the raw-syscall stub (IP-allowlisted, bypasses
 * our trapped chdir handler) and assert -ENOENT for both. */
static int test_b4_rootfs_prefix_boundary(const char *rootfs)
{
	int fails = 0;
	char sibling[1024];
	size_t i = 0;
	while (rootfs[i] && i + 5 < sizeof sibling) {
		sibling[i] = rootfs[i]; i++;
	}
	const char *suf = "-evil/foo";
	for (size_t j = 0; suf[j] && i + 1 < sizeof sibling; j++)
		sibling[i++] = suf[j];
	sibling[i] = 0;

	long cr = TAWC_RAW(TAWC_SYS_chdir, (long)sibling, 0, 0, 0, 0, 0);
	if (cr != 0) {
		tawc_io_kv_dec("    sibling chdir failed (skipping), -errno",
			       -cr);
	} else {
		char buf[256];
		long rv;
		INLINE_SYS6(TAWC_SYS_getcwd, buf, sizeof buf,
			    0, 0, 0, 0, rv);
		fails += tawc_io_step(
			"getcwd from <rootfs>-evil/foo -> -ENOENT (B4 boundary)",
			rv == -2);
		if (rv > 0) {
			/* getcwd contract: rv is length INCLUDING NUL.
			 * Buffer is already NUL-terminated by the kernel. */
			tawc_io_str("    leaked cwd = '");
			tawc_io_str(buf);
			tawc_io_str("'\n");
		}
		tawc_io_kv_dec("    rv", rv);

		long fd = inline_openat(AT_FDCWD, "foo/host-secret",
					O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"foo/host-secret\") relative -> -ENOENT (B4)",
			fd == -2);
		if (fd >= 0) tawc_close((int)fd);
		tawc_io_kv_dec("    rv", fd);

		/* Restore cwd to the rootfs root via the trapped
		 * chdir handler so any later additions don't see a
		 * leaked sibling cwd. */
		INLINE_SYS6(TAWC_SYS_chdir, "/", 0, 0, 0, 0, 0, cr);
	}
	return fails;
}

/* Review finding B5+D3 — bind takes precedence over rootfs symlink
 * memo. The fake rootfs has /lib -> usr/lib (memoized at startup);
 * the harness binds <bindsrc>:lib. With the bug, the memo rewrites
 * `/lib/probe.txt` -> `usr/lib/probe.txt` BEFORE route_through_binds
 * gets a chance, so the bind never matches and the open hits the
 * rootfs (where probe.txt under usr/lib doesn't exist) -> ENOENT.
 * With the fix, the bind on /lib wins and we get bind-src content. */
static int test_b5_bind_over_symlink_memo(void)
{
	int fails = 0;
	int has_lib64_bind = 0;
	for (size_t bi = 0; bi < tawcroot_n_binds; bi++) {
		if (tawcroot_binds[bi].dst_len == 5 &&
		    tawcroot_binds[bi].dst[0] == 'l' &&
		    tawcroot_binds[bi].dst[1] == 'i' &&
		    tawcroot_binds[bi].dst[2] == 'b' &&
		    tawcroot_binds[bi].dst[3] == '6' &&
		    tawcroot_binds[bi].dst[4] == '4') {
			has_lib64_bind = 1; break;
		}
	}
	if (!has_lib64_bind) {
		tawc_io_str("    (no /lib64 bind configured -- B5 test skipped)\n");
	} else {
		long fd = inline_openat(AT_FDCWD, "/lib64/probe.txt",
					O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"/lib64/probe.txt\") with /lib64 bind -> bind src wins (B5)",
			fd >= 0);
		if (fd >= 0) {
			char buf[64];
			long n = tawc_read((int)fd, buf, sizeof buf - 1);
			int eq = 0;
			if (n > 0) {
				buf[n] = 0;
				const char *want = "from-bind-src";
				eq = 1;
				for (size_t k = 0; want[k]; k++) {
					if (buf[k] != want[k]) { eq = 0; break; }
				}
			}
			fails += tawc_io_step(
				"/lib64/probe.txt content matches bind src",
				eq);
			if (n > 0) {
				tawc_io_str("    contents = '");
				tawc_io_str(buf);
				tawc_io_str("'\n");
			}
			tawc_close((int)fd);
		}
		tawc_io_kv_dec("    rv", fd);
	}
	return fails;
}

/* /proc/self/maps reverse-translation. mmap a file from inside
 * the rootfs (so its host path appears in /proc/self/maps), then
 * open /proc/self/maps via the guest dispatch and check that the
 * returned bytes show the guest-visible path, not the rootfs host
 * path. The shadow fd is a memfd, so the guest reads NOT the
 * kernel's file but our rewritten copy. */
static int test_proc_self_maps_reverse_translation(void)
{
	int fails = 0;
	long probe_fd = inline_openat(AT_FDCWD, "/etc/probe",
				      O_RDONLY, 0);
	fails += tawc_io_step("maps test: open /etc/probe", probe_fd >= 0);
	if (probe_fd >= 0) {
		long region = TAWC_RAW(TAWC_SYS_mmap, 0, 4096,
				       1 /*PROT_READ*/,
				       2 /*MAP_PRIVATE*/,
				       (long)probe_fd, 0);
		int mmap_ok = !(region <= 0 && region > -4096);
		fails += tawc_io_step("maps test: mmap probe file",
				      mmap_ok);

		long mfd = inline_openat(AT_FDCWD, "/proc/self/maps",
					 O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"/proc/self/maps\") -> shadow fd",
			mfd >= 0);
		if (mfd >= 0) {
			/* Read the whole shadow file. Two pages is
			 * comfortably more than a testhost mapping
			 * count (~30 lines). */
			char buf[8192];
			size_t total = 0;
			for (;;) {
				long n = tawc_read((int)mfd,
					buf + total,
					sizeof buf - 1 - total);
				if (n <= 0) break;
				total += (size_t)n;
				if (total + 1 >= sizeof buf) break;
			}
			buf[total] = 0;

			/* Search for guest path. Substring match is
			 * fine — proves the rewriter saw the line and
			 * emitted the guest form. */
			int saw_guest = 0;
			for (size_t i = 0; i + 10 < total; i++) {
				if (buf[i]   == '/' && buf[i+1] == 'e' &&
				    buf[i+2] == 't' && buf[i+3] == 'c' &&
				    buf[i+4] == '/' && buf[i+5] == 'p' &&
				    buf[i+6] == 'r' && buf[i+7] == 'o' &&
				    buf[i+8] == 'b' && buf[i+9] == 'e') {
					saw_guest = 1; break;
				}
			}
			fails += tawc_io_step(
				"maps shadow contains '/etc/probe' (guest path)",
				saw_guest);

			/* Search for rootfs host prefix; must NOT
			 * appear in the rewritten output for the probe
			 * mmap. The host prefix could legitimately
			 * appear in unrelated lines if the testhost
			 * binary itself were inside the rootfs (it
			 * isn't) — so this assertion is conservative
			 * but valid here. */
			int saw_host = 0;
			size_t pl = tawcroot_rootfs_host_path_len;
			const char *rh = tawcroot_rootfs_host_path;
			if (pl > 0) {
				for (size_t i = 0; i + pl < total; i++) {
					int eq = 1;
					for (size_t j = 0; j < pl; j++) {
						if (buf[i + j] != rh[j]) {
							eq = 0; break;
						}
					}
					if (eq) { saw_host = 1; break; }
				}
			}
			fails += tawc_io_step(
				"maps shadow does NOT leak rootfs host path",
				!saw_host);

			tawc_close((int)mfd);
		}

		if (mmap_ok) {
			(void)TAWC_RAW(TAWC_SYS_munmap, region, 4096,
				       0, 0, 0, 0);
		}
		tawc_close((int)probe_fd);
	}
	return fails;
}

/* Read a (presumed) maps shadow fd into `out` (cap bytes), returning
 * total bytes read. Helper for the per-form synthesis assertions
 * below — the existing `test_proc_self_maps_reverse_translation`
 * already exercises content rewriting against /proc/self/maps; here we
 * just assert that the new path forms route through the same shadow,
 * by checking for the guest probe substring AND the absence of the
 * rootfs host prefix in each output. */
static size_t drain_fd(int fd, char *out, size_t cap)
{
	size_t total = 0;
	for (;;) {
		long n = tawc_read(fd, out + total, cap - 1 - total);
		if (n <= 0) break;
		total += (size_t)n;
		if (total + 1 >= cap) break;
	}
	out[total] = 0;
	return total;
}

static int maps_content_is_rewritten(const char *buf, size_t total)
{
	int saw_guest = 0;
	for (size_t i = 0; i + 10 < total; i++) {
		if (buf[i]   == '/' && buf[i+1] == 'e' &&
		    buf[i+2] == 't' && buf[i+3] == 'c' &&
		    buf[i+4] == '/' && buf[i+5] == 'p' &&
		    buf[i+6] == 'r' && buf[i+7] == 'o' &&
		    buf[i+8] == 'b' && buf[i+9] == 'e') {
			saw_guest = 1; break;
		}
	}
	int saw_host = 0;
	size_t pl = tawcroot_rootfs_host_path_len;
	const char *rh = tawcroot_rootfs_host_path;
	if (pl > 0) {
		for (size_t i = 0; i + pl < total; i++) {
			int eq = 1;
			for (size_t j = 0; j < pl; j++) {
				if (buf[i + j] != rh[j]) { eq = 0; break; }
			}
			if (eq) { saw_host = 1; break; }
		}
	}
	return saw_guest && !saw_host;
}

/* Issue: tawcroot-proc-self-synthesis-fd-relative — strip_proc_self_prefix
 * extended to accept /proc/self/task/<tid>/<x> and /proc/<tid>/<x> for
 * any TID in our process; handle_openat / handle_readlinkat additionally
 * resolve fd-relative dirfds (openat(proc_dir_fd, "self/maps", ...))
 * via /proc/self/fd/<n> and re-classify.
 *
 * The kernel's /proc is opened via the IP-allowlisted raw stub so the
 * resulting dirfd is rooted at the host's /proc, mirroring a real chroot
 * with /proc bind-mounted. The trapped openat then sees a relative
 * "self/maps" against that dirfd. */
static int test_proc_self_synthesis_extensions(void)
{
	int fails = 0;

	/* mmap a probe file from inside the rootfs so its rewritten path
	 * shows up in maps output -- the same trick the absolute-path
	 * test uses. */
	long probe_fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	if (probe_fd < 0) {
		tawc_io_kv_dec("    skipped (probe open failed): -errno",
			       -probe_fd);
		return 0;
	}
	long region = TAWC_RAW(TAWC_SYS_mmap, 0, 4096,
			       1 /*PROT_READ*/, 2 /*MAP_PRIVATE*/,
			       (long)probe_fd, 0);
	int mmap_ok = !(region <= 0 && region > -4096);
	if (!mmap_ok) {
		tawc_close((int)probe_fd);
		tawc_io_str("    skipped (mmap failed)\n");
		return 0;
	}

	long mypid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);

	/* (A) /proc/self/task/<pid>/maps -- new prefix peel. The main-
	 * thread TID equals the TGID, so this works in single-threaded
	 * testhost; the per-TID branch (n != getpid()) is exercised
	 * indirectly via the same is_my_tid path. */
	{
		char path[64];
		const char *pre = "/proc/self/task/";
		size_t i = 0;
		while (pre[i]) { path[i] = pre[i]; i++; }
		i += (size_t)tawc_int_to_str(path + i, sizeof path - i,
					     (int)mypid);
		const char *suf = "/maps";
		for (size_t j = 0; suf[j]; j++) path[i++] = suf[j];
		path[i] = 0;

		long mfd = inline_openat(AT_FDCWD, path, O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(/proc/self/task/<pid>/maps) -> shadow fd",
			mfd >= 0);
		if (mfd >= 0) {
			char buf[8192];
			size_t total = drain_fd((int)mfd, buf, sizeof buf);
			fails += tawc_io_step(
				"task/<pid>/maps shadow content is rewritten",
				maps_content_is_rewritten(buf, total));
			tawc_close((int)mfd);
		}
	}

	/* (B) fd-relative openat against the kernel's /proc -- mirrors a
	 * sandbox that opens /proc once and reuses the dirfd for
	 * subsequent "self/<x>" lookups. */
	{
		long proc_fd = tawc_openat(AT_FDCWD, "/proc",
					   O_PATH | O_DIRECTORY | O_CLOEXEC,
					   0);
		if (proc_fd >= 0) {
			long mfd;
			INLINE_SYS6(TAWC_SYS_openat, (int)proc_fd, "self/maps",
				    O_RDONLY, 0, 0, 0, mfd);
			fails += tawc_io_step(
				"openat(proc_fd, \"self/maps\") -> shadow fd",
				mfd >= 0);
			if (mfd >= 0) {
				char buf[8192];
				size_t total = drain_fd((int)mfd, buf,
							sizeof buf);
				fails += tawc_io_step(
					"fd-relative maps shadow content is rewritten",
					maps_content_is_rewritten(buf, total));
				tawc_close((int)mfd);
			}
			tawc_close((int)proc_fd);
		} else {
			tawc_io_kv_dec("    /proc open via raw stub failed",
				       -proc_fd);
		}
	}

	/* (C) /proc/self/task/<pid>/exe synthesis. */
	extern void tawcroot_set_guest_exe_path(const char *);
	tawcroot_set_guest_exe_path("/usr/bin/some-guest");
	{
		char path[64];
		const char *pre = "/proc/self/task/";
		size_t i = 0;
		while (pre[i]) { path[i] = pre[i]; i++; }
		i += (size_t)tawc_int_to_str(path + i, sizeof path - i,
					     (int)mypid);
		const char *suf = "/exe";
		for (size_t j = 0; suf[j]; j++) path[i++] = suf[j];
		path[i] = 0;

		char buf[256];
		long rv;
		INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD,
			    path, buf, sizeof buf - 1, 0, 0, rv);
		fails += tawc_io_step(
			"readlinkat(/proc/self/task/<pid>/exe) -> guest exe path",
			rv == (long)tawc_strlen("/usr/bin/some-guest"));
		if (rv > 0) {
			buf[rv] = 0;
			fails += tawc_io_step(
				"task/<pid>/exe content matches stash",
				tawc_streq(buf, "/usr/bin/some-guest"));
		}
	}

	/* (D) fd-relative readlinkat: open /proc/self via the raw stub
	 * (kernel-rooted), then readlinkat(proc_self_fd, "exe", ...). */
	{
		long pself_fd = tawc_openat(AT_FDCWD, "/proc/self",
					    O_PATH | O_DIRECTORY | O_CLOEXEC,
					    0);
		if (pself_fd >= 0) {
			char buf[256];
			long rv;
			INLINE_SYS6(TAWC_SYS_readlinkat, (int)pself_fd,
				    "exe", buf, sizeof buf - 1, 0, 0, rv);
			fails += tawc_io_step(
				"readlinkat(proc_self_fd, \"exe\") -> guest exe path",
				rv == (long)tawc_strlen("/usr/bin/some-guest"));
			if (rv > 0) {
				buf[rv] = 0;
				fails += tawc_io_step(
					"fd-relative exe content matches stash",
					tawc_streq(buf, "/usr/bin/some-guest"));
			}
			tawc_close((int)pself_fd);
		} else {
			tawc_io_kv_dec("    /proc/self open via raw stub failed",
				       -pself_fd);
		}
	}
	/* (E) Negative: /proc/<other-pid>/exe must NOT trigger synthesis.
	 * This is the only case in this test that actually exercises
	 * is_my_tid's /proc/<n>/status read path -- (A)-(D) all hit the
	 * `n == getpid()` short-circuit. PID 1 (init) is always present
	 * on Linux and definitionally not us. The synthesis stash is
	 * still set to "/usr/bin/some-guest" here; the assertion is that
	 * the readlinkat result does NOT come back as that string. */
	{
		char buf[256];
		long rv;
		INLINE_SYS6(TAWC_SYS_readlinkat, AT_FDCWD,
			    "/proc/1/exe", buf, sizeof buf - 1, 0, 0, rv);
		int leaked = 0;
		if (rv > 0 && rv == (long)tawc_strlen("/usr/bin/some-guest")) {
			buf[rv] = 0;
			leaked = tawc_streq(buf, "/usr/bin/some-guest");
		}
		fails += tawc_io_step(
			"readlinkat(/proc/1/exe) does NOT leak our stash (negative)",
			!leaked);
	}

	tawcroot_set_guest_exe_path(0);

	(void)TAWC_RAW(TAWC_SYS_munmap, region, 4096, 0, 0, 0, 0);
	tawc_close((int)probe_fd);
	return fails;
}

/* Readlink /proc/self/fd/<fd> and assert the target starts with
 * `expected_link` — which only memfd_create can produce, so this
 * proves a tawcroot synthesizer fired (vs the kernel returning a
 * real fd). On mismatch the actual link target is logged for
 * diagnostics. Shared between the overflow_id, pci/devices and any
 * future memfd-shadow tests. */
static int check_memfd_link(int fd, const char *expected_link)
{
	char fdpath[64];
	size_t i = 0;
	const char *pre = "/proc/self/fd/";
	while (pre[i]) { fdpath[i] = pre[i]; i++; }
	i += (size_t)tawc_int_to_str(fdpath + i, sizeof fdpath - i,
				     (int)fd);
	fdpath[i] = 0;
	char link[128];
	long ln = tawc_readlinkat(AT_FDCWD, fdpath, link, sizeof link - 1);
	int  is_memfd = (ln >= (long)tawc_strlen(expected_link)) &&
		tawc_starts_with(link, expected_link);
	if (!is_memfd && ln > 0) {
		link[ln < (long)sizeof link ? ln : (long)sizeof link - 1] = 0;
		tawc_io_str("    link = '");
		tawc_io_str(link);
		tawc_io_str("'\n");
	}
	return is_memfd;
}

/* /proc/sys/kernel/overflow{uid,gid} synthesis. Android's SELinux
 * denies untrusted_app any read under /proc/sys/kernel, which kicks
 * bwrap out before it gets to the unshare(CLONE_NEWUSER) failure that
 * glycin's autodetect knows how to read. The handler returns a memfd
 * preloaded with the Linux-conventional "65534\n" so bwrap proceeds.
 *
 * On the testhost the host kernel happily returns the same value, so
 * we can't tell synthesis from kernel-passthrough by content alone.
 * Each positive case asserts via check_memfd_link that the fd is
 * memfd-backed. Negative cases check both the readlink (must NOT
 * be a memfd) and, where applicable, the errno path.
 *
 * The two probes are duplicated rather than table-driven because the
 * test labels and INLINE_SYS6 macro want compile-time strings, and the
 * extra repetition is cheaper than the harness needed to stitch labels
 * dynamically. */
static const char  OFL_BYTES[] = "65534\n";
static const char  OFL_LINK_UID[] = "/memfd:tawcroot-overflowuid";
static const char  OFL_LINK_GID[] = "/memfd:tawcroot-overflowgid";

static int ofl_content_ok(int fd)
{
	char buf[16];
	long n = tawc_read(fd, buf, sizeof buf);
	if (n != (long)(sizeof OFL_BYTES - 1)) return 0;
	for (long i = 0; i < n; i++)
		if (buf[i] != OFL_BYTES[i]) return 0;
	return 1;
}

static int test_proc_sys_overflow_id_synthesis(void)
{
	int fails = 0;

	/* (A) Absolute path — overflowuid. */
	{
		long mfd = inline_openat(AT_FDCWD,
			"/proc/sys/kernel/overflowuid", O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"/proc/sys/kernel/overflowuid\") -> fd",
			mfd >= 0);
		if (mfd >= 0) {
			fails += tawc_io_step(
				"absolute overflowuid -> memfd-backed fd",
				check_memfd_link((int)mfd, OFL_LINK_UID));
			fails += tawc_io_step(
				"absolute overflowuid contents == \"65534\\n\"",
				ofl_content_ok((int)mfd));
			tawc_close((int)mfd);
		}
	}

	/* (B) Absolute path — overflowgid (same code path, separate label
	 * so a regression in just one classifier branch is visible). */
	{
		long mfd = inline_openat(AT_FDCWD,
			"/proc/sys/kernel/overflowgid", O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"/proc/sys/kernel/overflowgid\") -> fd",
			mfd >= 0);
		if (mfd >= 0) {
			fails += tawc_io_step(
				"absolute overflowgid -> memfd-backed fd",
				check_memfd_link((int)mfd, OFL_LINK_GID));
			fails += tawc_io_step(
				"absolute overflowgid contents == \"65534\\n\"",
				ofl_content_ok((int)mfd));
			tawc_close((int)mfd);
		}
	}

	/* (C) Fd-relative against the kernel's /proc — mirrors a sandbox
	 * that opens /proc once and reuses the dirfd for both reads. */
	{
		long proc_fd = tawc_openat(AT_FDCWD, "/proc",
			O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
		if (proc_fd >= 0) {
			long mfd;
			INLINE_SYS6(TAWC_SYS_openat, (int)proc_fd,
				    "sys/kernel/overflowuid",
				    O_RDONLY, 0, 0, 0, mfd);
			fails += tawc_io_step(
				"openat(proc_fd, \"sys/kernel/overflowuid\") -> fd",
				mfd >= 0);
			if (mfd >= 0) {
				fails += tawc_io_step(
					"fd-relative overflowuid -> memfd-backed fd",
					check_memfd_link((int)mfd, OFL_LINK_UID));
				fails += tawc_io_step(
					"fd-relative overflowuid contents == \"65534\\n\"",
					ofl_content_ok((int)mfd));
				tawc_close((int)mfd);
			}
			INLINE_SYS6(TAWC_SYS_openat, (int)proc_fd,
				    "sys/kernel/overflowgid",
				    O_RDONLY, 0, 0, 0, mfd);
			fails += tawc_io_step(
				"openat(proc_fd, \"sys/kernel/overflowgid\") -> fd",
				mfd >= 0);
			if (mfd >= 0) {
				fails += tawc_io_step(
					"fd-relative overflowgid -> memfd-backed fd",
					check_memfd_link((int)mfd, OFL_LINK_GID));
				fails += tawc_io_step(
					"fd-relative overflowgid contents == \"65534\\n\"",
					ofl_content_ok((int)mfd));
				tawc_close((int)mfd);
			}
			tawc_close((int)proc_fd);
		} else {
			tawc_io_kv_dec(
				"    /proc open via raw stub failed",
				-proc_fd);
		}
	}

	/* (D) Sibling that shares the prefix bytes must NOT match. A
	 * buggy classifier that prefix-matched would synthesize anyway. */
	{
		long mfd = inline_openat(AT_FDCWD,
			"/proc/sys/kernel/overflowuid-evil",
			O_RDONLY, 0);
		fails += tawc_io_step(
			"sibling-prefix path is not synthesized (errno path)",
			mfd < 0);
		if (mfd >= 0) tawc_close((int)mfd);
	}

	/* (E) O_PATH must NOT route through synthesis — the peek skips
	 * O_PATH so the open falls through to normal translation. The
	 * outcome is environment-dependent (kernel returns a real O_PATH
	 * fd on a Linux host where /proc is readable; tawcroot's
	 * translator may reject the path on a strict rootfs). The
	 * regression we guard against is "synthesis fired despite
	 * O_PATH" — i.e. fd >= 0 AND /proc/self/fd/<fd> resolves to a
	 * /memfd:tawcroot-overflowuid memfd. Either branch (negative fd,
	 * or positive fd that is NOT a memfd) means synthesis was
	 * correctly skipped. */
	{
		long fd = inline_openat(AT_FDCWD,
			"/proc/sys/kernel/overflowuid",
			O_PATH | O_CLOEXEC, 0);
		int synthesised = (fd >= 0) &&
			check_memfd_link((int)fd, OFL_LINK_UID);
		fails += tawc_io_step(
			"O_PATH does not route through synthesis",
			!synthesised);
		if (fd >= 0) tawc_close((int)fd);
	}

	return fails;
}

/* /proc/bus/pci/devices synthesis. Android exposes the file as an
 * unreadable placeholder (`-?????????`); libpci's procfs back-end
 * exits(1) on that, taking down anything that dlopen'd libpci.so.3
 * (Mozilla glxtest -> WebRender disable cascade). The handler returns
 * an empty memfd, which libpci treats as "no PCI devices visible" and
 * proceeds normally.
 *
 * As with the overflow_id pair, on the testhost the host kernel may
 * happily return the real /proc/bus/pci/devices, so we can't tell
 * synthesis from passthrough by content alone — check_memfd_link
 * (above) is what proves the synthesizer fired. The empty-content
 * assertion is a redundant cross-check (and would false-pass if the
 * host's real file happened to be empty), but the link check is
 * load-bearing. */
static const char PCI_LINK[] = "/memfd:tawcroot-pci-devices";

static int pci_content_empty(int fd)
{
	char buf[4];
	long n = tawc_read(fd, buf, sizeof buf);
	return n == 0;
}

static int test_proc_bus_pci_devices_synthesis(void)
{
	int fails = 0;

	/* (A) Absolute path. */
	{
		long mfd = inline_openat(AT_FDCWD,
			"/proc/bus/pci/devices", O_RDONLY, 0);
		fails += tawc_io_step(
			"openat(\"/proc/bus/pci/devices\") -> fd",
			mfd >= 0);
		if (mfd >= 0) {
			fails += tawc_io_step(
				"absolute pci/devices -> memfd-backed fd",
				check_memfd_link((int)mfd, PCI_LINK));
			fails += tawc_io_step(
				"absolute pci/devices is empty",
				pci_content_empty((int)mfd));
			tawc_close((int)mfd);
		}
	}

	/* (B) Fd-relative against the kernel's /proc — same access pattern
	 * libpci uses internally if a sandboxed caller pre-opens /proc. */
	{
		long proc_fd = tawc_openat(AT_FDCWD, "/proc",
			O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
		if (proc_fd >= 0) {
			long mfd;
			INLINE_SYS6(TAWC_SYS_openat, (int)proc_fd,
				    "bus/pci/devices",
				    O_RDONLY, 0, 0, 0, mfd);
			fails += tawc_io_step(
				"openat(proc_fd, \"bus/pci/devices\") -> fd",
				mfd >= 0);
			if (mfd >= 0) {
				fails += tawc_io_step(
					"fd-relative pci/devices -> memfd-backed fd",
					check_memfd_link((int)mfd, PCI_LINK));
				fails += tawc_io_step(
					"fd-relative pci/devices is empty",
					pci_content_empty((int)mfd));
				tawc_close((int)mfd);
			}
			tawc_close((int)proc_fd);
		} else {
			tawc_io_kv_dec(
				"    /proc open via raw stub failed",
				-proc_fd);
		}
	}

	/* (C) Sibling path that shares the prefix bytes must NOT match.
	 * Guards against a buggy classifier that prefix-matched. */
	{
		long mfd = inline_openat(AT_FDCWD,
			"/proc/bus/pci/devices-evil",
			O_RDONLY, 0);
		fails += tawc_io_step(
			"sibling-prefix path is not synthesized (errno path)",
			mfd < 0);
		if (mfd >= 0) tawc_close((int)mfd);
	}

	/* (D) The directory itself — `/proc/bus/pci` — must NOT route
	 * through synthesis. Only the single `devices` file does. */
	{
		long fd = inline_openat(AT_FDCWD, "/proc/bus/pci",
			O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
		int synthesised = (fd >= 0) && check_memfd_link((int)fd, PCI_LINK);
		fails += tawc_io_step(
			"directory /proc/bus/pci is not synthesized",
			!synthesised);
		if (fd >= 0) tawc_close((int)fd);
	}

	return fails;
}

/* /dev/shm in-handler emulation: create + reuse + unlink + stat. */
static int test_dev_shm_emulation(void)
{
	int fails = 0;
	long fd1 = inline_openat(AT_FDCWD, "/dev/shm/tawcroot-smoke",
				 O_RDWR | 0x40 /*O_CREAT*/,
				 0600);
	fails += tawc_io_step(
		"openat(/dev/shm/...,O_CREAT) -> memfd-backed fd",
		fd1 >= 0);
	tawc_io_kv_dec("    rv", fd1);

	if (fd1 >= 0) {
		/* ftruncate isn't trapped: the kernel does it
		 * directly on the dup. Proves the fd refers to a
		 * real, resizable memfd. */
		long tr;
		INLINE_SYS6(TAWC_SYS_ftruncate, fd1, 4096, 0, 0, 0, 0, tr);
		fails += tawc_io_step(
			"ftruncate on shm fd -> 0", tr == 0);

		/* O_EXCL on existing name -> -EEXIST. */
		long fd_excl = inline_openat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke",
			O_RDWR | 0x40 /*O_CREAT*/ | 0x80 /*O_EXCL*/,
			0600);
		fails += tawc_io_step(
			"openat(...,O_CREAT|O_EXCL) on existing -> -EEXIST",
			fd_excl == -17);

		/* Reopen without O_CREAT -> existing entry. */
		long fd2 = inline_openat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke",
			O_RDWR, 0);
		fails += tawc_io_step(
			"reopen existing /dev/shm name -> fd",
			fd2 >= 0);

		/* stat /dev/shm/<name> -> regular file, root-owned. */
		struct stat st;
		long sr = inline_fstatat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke", &st, 0);
		fails += tawc_io_step(
			"fstatat /dev/shm/<name> -> 0", sr == 0);
		fails += tawc_io_step(
			"shm stat: uid=0", st.st_uid == 0);

		/* stat /dev/shm dir -> directory. */
		long sd = inline_fstatat(AT_FDCWD, "/dev/shm",
					 &st, 0);
		fails += tawc_io_step(
			"fstatat /dev/shm -> 0 (synth dir)", sd == 0);
		fails += tawc_io_step(
			"shm dir stat: is dir",
			(st.st_mode & 0170000) == 0040000);

		/* faccessat on the name and dir -> 0. */
		long ad = inline_faccessat(
			AT_FDCWD, "/dev/shm", 0, 0);
		fails += tawc_io_step(
			"faccessat /dev/shm -> 0", ad == 0);
		long an = inline_faccessat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke", 0, 0);
		fails += tawc_io_step(
			"faccessat /dev/shm/<name> -> 0", an == 0);

		/* Drop the name. */
		long ur = inline_unlinkat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke", 0);
		fails += tawc_io_step(
			"unlinkat /dev/shm/<name> -> 0", ur == 0);

		/* Now re-stat -> -ENOENT. */
		long sr2 = inline_fstatat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke",
			&st, 0);
		fails += tawc_io_step(
			"post-unlink stat -> -ENOENT", sr2 == -2);

		/* unlink-of-missing -> -ENOENT. */
		long ur2 = inline_unlinkat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke", 0);
		fails += tawc_io_step(
			"unlink missing -> -ENOENT", ur2 == -2);

		/* Open w/o O_CREAT after unlink -> -ENOENT. */
		long fd3 = inline_openat(
			AT_FDCWD, "/dev/shm/tawcroot-smoke",
			O_RDWR, 0);
		fails += tawc_io_step(
			"open after unlink (no O_CREAT) -> -ENOENT",
			fd3 == -2);

		if (fd2 >= 0) tawc_close((int)fd2);
		tawc_close((int)fd1);
	}
	return fails;
}

/* Runtime-reserved fd survives a guest closefrom over the reserved
 * range. shm_open reserves an INTERNAL fd in [BASE, ∞) that the guest
 * never sees but its blind close loop (gpgme/pacman fd hygiene) walks.
 * Pre-fix the BPF close trap baked in only install-time reserved fds,
 * so a guest close() of a runtime-reserved shm fd reached the kernel
 * and really closed it — the segment vanished (or, worse, the slot got
 * recycled). The range-compare fix traps the whole half-space.
 * Issue: tawcroot-close-fastpath-misses-runtime-reserved-fds.md. */
static int test_shm_survives_guest_closefrom(void)
{
	int fails = 0;
	long fd = inline_openat(AT_FDCWD, "/dev/shm/tawcroot-closefrom",
				O_RDWR | 0x40 /*O_CREAT*/, 0600);
	fails += tawc_io_step("closefrom-shm: create segment", fd >= 0);
	if (fd < 0) return fails;
	long tr;
	INLINE_SYS6(TAWC_SYS_ftruncate, fd, 4096, 0, 0, 0, 0, tr);
	(void)tr;

	/* Blindly close the whole reserved range via inline asm so each
	 * close traverses the BPF filter (the stub path is IP-allowlisted
	 * and wouldn't trap). The internal shm fd lives somewhere in here. */
	for (int f = TAWCROOT_RESERVED_FD_BASE;
	     f < TAWCROOT_RESERVED_FD_BASE + 64; f++) {
		long cr;
		INLINE_SYS6(TAWC_SYS_close, f, 0, 0, 0, 0, 0, cr);
		(void)cr;
	}

	/* Segment must still be alive: reopen without O_CREAT succeeds. */
	long fd2 = inline_openat(AT_FDCWD, "/dev/shm/tawcroot-closefrom",
				 O_RDWR, 0);
	fails += tawc_io_step(
		"shm segment survives guest closefrom over reserved range",
		fd2 >= 0);
	tawc_io_kv_dec("    reopen rv", fd2);
	if (fd2 >= 0) tawc_close((int)fd2);

	/* rootfs_fd is also in the closed range — path translation must
	 * still work (its close was trapped + faked too). */
	long probe = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"rootfs path translation survives closefrom", probe >= 0);
	if (probe >= 0) tawc_close((int)probe);

	(void)inline_unlinkat(AT_FDCWD, "/dev/shm/tawcroot-closefrom", 0);
	tawc_close((int)fd);
	return fails;
}

/* ----- chroot ----------------------------------------------------- */

static long inline_chroot(const char *path)
{
	long rv;
	INLINE_SYS6(TAWC_SYS_chroot, path, 0, 0, 0, 0, 0, rv);
	return rv;
}

/* chroot(NULL) -> -EFAULT, no state change. */
static int test_chroot_efault_on_null(void)
{
	int fails = 0;
	long rv = inline_chroot((const char *)0);
	fails += tawc_io_step("chroot(NULL) -> -EFAULT", rv == -14);
	tawc_io_kv_dec("    rv", rv);

	/* State is still the original rootfs — verify by re-opening
	 * /etc/probe. */
	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot(NULL): /etc/probe still openable", fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

/* chroot to a regular file -> -ENOTDIR. The path translator says yes
 * (file exists, fold is fine); openat with O_DIRECTORY rejects with
 * -ENOTDIR. The error must propagate without mutating the rootfs view. */
static int test_chroot_enotdir_on_regular_file(void)
{
	int fails = 0;
	long rv = inline_chroot("/etc/probe");
	fails += tawc_io_step("chroot(\"/etc/probe\") -> -ENOTDIR", rv == -20);
	tawc_io_kv_dec("    rv", rv);

	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after rejection: /etc/probe still readable", fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

static int test_chroot_enoent_on_missing(void)
{
	int fails = 0;
	long rv = inline_chroot("/does/not/exist");
	fails += tawc_io_step("chroot(\"/does/not/exist\") -> -ENOENT", rv == -2);
	tawc_io_kv_dec("    rv", rv);

	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after -ENOENT: rootfs view intact", fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

/* chroot("/") is the pacman 6.x case: identity transform. The path
 * layer folds to base_fd=rootfs_fd, suffix="". We open ".", reserve a
 * second fd (the old one stays reserved), update host_path to the
 * canonical readlink(/proc/self/fd/<new>) value, re-anchor binds (all
 * survive — every bind dst is now under the same host root), and
 * rebuild memos. Verify: post-call openat still routes correctly. */
static int test_chroot_identity(void)
{
	int fails = 0;
	long rv = inline_chroot("/");
	fails += tawc_io_step("chroot(\"/\") -> 0 (identity)", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot(\"/\"): /etc/probe still readable", fd >= 0);
	if (fd >= 0) {
		char buf[64];
		long n = tawc_read((int)fd, buf, sizeof buf - 1);
		int eq = 0;
		if (n > 0) {
			buf[n] = 0;
			const char *want = "hello-from-rootfs";
			eq = 1;
			for (size_t k = 0; want[k]; k++) {
				if (buf[k] != want[k]) { eq = 0; break; }
			}
		}
		fails += tawc_io_step(
			"after chroot(\"/\"): probe contents intact", eq);
		tawc_close((int)fd);
	}

	/* The /lib symlink memo still works: openat("/lib/probe.so")
	 * resolves to <root>/usr/lib/probe.so. Memos got rebuilt against
	 * the new (== old) rootfs_fd. */
	long fd2 = inline_openat(AT_FDCWD, "/lib/probe.so", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot(\"/\"): /lib symlink memo rebuilt",
		fd2 >= 0);
	if (fd2 >= 0) tawc_close((int)fd2);
	return fails;
}

/* `..` chains in the chroot target fold inside the rootfs view, same
 * as every other path-bearing syscall. chroot("/foo/../..") clamps to
 * "/", same as test_chroot_identity. */
static int test_chroot_dotdot_clamps(void)
{
	int fails = 0;
	long rv = inline_chroot("/foo/../..");
	fails += tawc_io_step(
		"chroot(\"/foo/../..\") -> 0 (clamped to /)", rv == 0);
	tawc_io_kv_dec("    rv", rv);

	long fd = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after clamped chroot: rootfs view intact", fd >= 0);
	if (fd >= 0) tawc_close((int)fd);
	return fails;
}

/* The big one: chroot("/usr") changes the root view to <rootfs>/usr.
 * Mutates state for the rest of the suite — must run LAST.
 *
 * Verify:
 *   - chroot returns 0
 *   - rootfs_fd / host_path got swapped (peek at globals)
 *   - openat("/lib/probe.so") in the new view opens
 *     <rootfs>/usr/lib/probe.so (since /lib in the new chroot is a
 *     real dir, not a symlink — there's no /lib in <rootfs>/usr/)
 *   - openat("/etc/probe") -> -ENOENT (no /etc in /usr)
 *   - the "lib64" bind got deactivated (its host path was
 *     <rootfs>/lib64, outside the new view)
 *   - the "usr/test-bind" bind got re-anchored to "test-bind" — its
 *     probe.txt content is reachable as /test-bind/probe.txt
 *   - chdir("/") + getcwd reports "/"
 *
 * The test precondition is two binds set up by test_rootfs_syscalls_smoke.c args:
 *     -b FAKE_BINDSRC:lib64        (will be dropped by chroot("/usr"))
 *     -b FAKE_BINDSRC:usr/test-bind (will be re-anchored to test-bind)
 */
static int test_chroot_into_subdir(void)
{
	int fails = 0;

	/* Confirm preconditions: both binds exist and are active before
	 * the chroot. If they're not, the test setup is wrong and we
	 * should skip (rather than report a false negative). */
	int has_lib64_pre = 0, has_test_bind_pre = 0;
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (!b->active) continue;
		if (b->dst_len == 5 &&
		    b->dst[0] == 'l' && b->dst[1] == 'i' &&
		    b->dst[2] == 'b' && b->dst[3] == '6' && b->dst[4] == '4')
			has_lib64_pre = 1;
		if (b->dst_len == 13 && tawc_streq(b->dst, "usr/test-bind"))
			has_test_bind_pre = 1;
	}
	if (!has_lib64_pre || !has_test_bind_pre) {
		tawc_io_str("    (test_chroot_into_subdir skipped — "
		            "missing -b lib64 or -b usr/test-bind)\n");
		return 0;
	}

	long rv = inline_chroot("/usr");
	fails += tawc_io_step("chroot(\"/usr\") -> 0", rv == 0);
	tawc_io_kv_dec("    rv", rv);
	if (rv != 0) return fails;  /* nothing else to check */

	/* Path inside the new root resolves through the new fd. */
	long fd = inline_openat(AT_FDCWD, "/lib/probe.so", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot(\"/usr\"): /lib/probe.so opens (new root view)",
		fd >= 0);
	if (fd >= 0) {
		char buf[64];
		long n = tawc_read((int)fd, buf, sizeof buf - 1);
		int eq = 0;
		if (n > 0) {
			buf[n] = 0;
			const char *want = "libprobe-data";
			eq = 1;
			for (size_t k = 0; want[k]; k++) {
				if (buf[k] != want[k]) { eq = 0; break; }
			}
		}
		fails += tawc_io_step(
			"  contents = libprobe-data (right file)", eq);
		tawc_close((int)fd);
	}

	/* Path that lived OUTSIDE the new chroot is gone. */
	long fd_oob = inline_openat(AT_FDCWD, "/etc/probe", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot: /etc/probe -> -ENOENT (outside chroot)",
		fd_oob == -2);
	tawc_io_kv_dec("    rv", fd_oob);

	/* The lib64 bind is gone (host path <rootfs>/lib64 outside view). */
	long fd_dropped = inline_openat(AT_FDCWD, "/lib64/probe.txt",
					O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot: dropped bind /lib64/probe.txt -> -ENOENT",
		fd_dropped < 0);
	tawc_io_kv_dec("    rv", fd_dropped);

	/* The usr/test-bind bind survived, re-anchored to "test-bind". */
	long fd_kept = inline_openat(AT_FDCWD, "/test-bind/probe.txt",
				     O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot: kept bind /test-bind/probe.txt opens",
		fd_kept >= 0);
	if (fd_kept >= 0) {
		char buf[64];
		long n = tawc_read((int)fd_kept, buf, sizeof buf - 1);
		int eq = 0;
		if (n > 0) {
			buf[n] = 0;
			const char *want = "from-bind-src";
			eq = 1;
			for (size_t k = 0; want[k]; k++) {
				if (buf[k] != want[k]) { eq = 0; break; }
			}
		}
		fails += tawc_io_step(
			"  contents = from-bind-src (bind routed)", eq);
		tawc_close((int)fd_kept);
	}

	/* Verify the bind table state directly: lib64 inactive,
	 * usr/test-bind re-anchored to "test-bind" and active. */
	int lib64_inactive = 1, test_bind_anchored = 0;
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (b->active && b->dst_len == 5 &&
		    b->dst[0] == 'l' && b->dst[1] == 'i' &&
		    b->dst[2] == 'b' && b->dst[3] == '6' && b->dst[4] == '4')
			lib64_inactive = 0;
		if (b->active && b->dst_len == 9 &&
		    tawc_streq(b->dst, "test-bind"))
			test_bind_anchored = 1;
	}
	fails += tawc_io_step("bind /lib64 marked inactive", lib64_inactive);
	fails += tawc_io_step(
		"bind usr/test-bind re-anchored to test-bind",
		test_bind_anchored);

	/* getcwd: chdir to "/" then verify it reports "/". The kernel cwd
	 * may still be inside the original rootfs (whatever the previous
	 * test left it at); chdir("/") inside the new view sets it to
	 * <rootfs>/usr, and getcwd should reverse-translate to "/". */
	long cd;
	INLINE_SYS6(TAWC_SYS_chdir, "/", 0, 0, 0, 0, 0, cd);
	fails += tawc_io_step("chdir(\"/\") inside new chroot -> 0",
			      cd == 0);
	char cwd[256];
	long g;
	INLINE_SYS6(TAWC_SYS_getcwd, cwd, sizeof cwd, 0, 0, 0, 0, g);
	cwd[sizeof cwd - 1] = 0;
	int cwd_ok = g > 0 && cwd[0] == '/' && cwd[1] == 0;
	fails += tawc_io_step(
		"getcwd inside new chroot reports \"/\" (post-chroot view)",
		cwd_ok);
	tawc_io_str("    cwd = '"); tawc_io_str(cwd); tawc_io_str("'\n");

	return fails;
}

/* chroot follows symlinks. After test_chroot_into_subdir leaves us at
 * <rootfs>/usr, /sublink is a symlink (target "lib") that the path
 * translator's FOLLOW-mode resolver walks — the chroot lands at
 * <rootfs>/usr/lib. Verify by reading probe.so from the new "/" view.
 *
 * Runs LAST in the chroot suite — after this the rootfs_fd is at
 * <rootfs>/usr/lib, two levels deep. */
static int test_chroot_through_symlink_follows_post(void)
{
	int fails = 0;
	long rv = inline_chroot("/sublink");
	fails += tawc_io_step(
		"chroot(\"/sublink\") -> 0 (symlink → lib)", rv == 0);
	tawc_io_kv_dec("    rv", rv);
	if (rv != 0) return fails;

	/* New root is <rootfs>/usr/lib; probe.so lives directly there. */
	long fd = inline_openat(AT_FDCWD, "/probe.so", O_RDONLY, 0);
	fails += tawc_io_step(
		"after chroot through symlink: /probe.so opens at target",
		fd >= 0);
	if (fd >= 0) {
		char buf[64];
		long n = tawc_read((int)fd, buf, sizeof buf - 1);
		int eq = 0;
		if (n > 0) {
			buf[n] = 0;
			const char *want = "libprobe-data";
			eq = 1;
			for (size_t k = 0; want[k]; k++) {
				if (buf[k] != want[k]) { eq = 0; break; }
			}
		}
		fails += tawc_io_step(
			"  contents = libprobe-data (symlink resolved)", eq);
		tawc_close((int)fd);
	}
	return fails;
}

/* ----- main ------------------------------------------------------- */

int tawcroot_rootfs_smoke_main(const char *rootfs)
{
	int fails = 0;

	tawc_io_str("\ntawcroot rootfs syscall smoke\n");
	tawc_io_str("  rootfs = "); tawc_io_str(rootfs); tawc_io_str("\n");

	long rfd = open_rootfs(rootfs);
	fails += tawc_io_step("open rootfs (O_PATH|O_DIRECTORY)", rfd >= 0);
	if (rfd < 0) {
		tawc_io_kv_dec("    open errno (-rv)", -rfd);
		return 1;
	}
	/* Move it into the reserved range so guest close/dup/fcntl can't
	 * touch it (Phase 0.5). Bind src_fds get the same treatment further
	 * down, after they're parsed. */
	long resv = tawcroot_fd_reserve((int)rfd);
	fails += tawc_io_step("reserve rootfs_fd above guest range",
			      resv >= TAWCROOT_RESERVED_FD_BASE);
	if (resv < 0) {
		tawc_io_kv_dec("    fd_reserve errno (-rv)", -resv);
		return 1;
	}
	tawcroot_rootfs_fd = (int)resv;
	tawc_io_kv_dec("    rootfs_fd (reserved)", (long)tawcroot_rootfs_fd);

	/* Stash the host path of the rootfs for relative-path reverse
	 * translation. The user must give us an absolute path; relative
	 * `-r` is unsupported (would race kernel cwd at startup). */
	if (rootfs[0] != '/') {
		fails += tawc_io_step("rootfs path is absolute", 0);
		return 1;
	}
	{
		size_t i = 0;
		while (rootfs[i] && i + 1 < sizeof tawcroot_rootfs_host_path) {
			tawcroot_rootfs_host_path[i] = rootfs[i];
			i++;
		}
		/* Strip trailing slashes so the prefix-match in path.c is
		 * unambiguous (host cwd of "/x/y/foo" must extend host
		 * prefix "/x/y" with a fresh "/" boundary). */
		while (i > 1 && tawcroot_rootfs_host_path[i - 1] == '/') i--;
		tawcroot_rootfs_host_path[i] = 0;
		tawcroot_rootfs_host_path_len = i;
	}

	/* Probe process_vm_readv (against our own task id). Used by the
	 * guarded guest-pointer copy helpers -- see notes/tawcroot.md
	 * "Guest memory access". Must run before the seccomp filter goes
	 * up because the probe issues a process_vm_readv that's only
	 * RET_ALLOWed when the IP allowlist is in effect from the stub. */
	long up = tawc_usercopy_init();
	tawc_io_kv_dec("    usercopy probe rv", up);
	tawc_io_kv_dec("    usercopy_works", (long)tawc_usercopy_works);

	/* Memoize well-known symlinks (lib -> usr/lib etc.) before the
	 * filter goes up. After install, readlinkat is on the trapped set
	 * and our handler will route to the rootfs fd anyway, so this is
	 * mostly a perf concern -- keep it simple by doing it pre-filter. */
	tawcroot_path_memoize_well_known();

	/* Print bind table for diagnostic visibility. */
	tawc_io_kv_dec("    n_binds", (long)tawcroot_n_binds);
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		tawc_io_str("    bind: ");
		tawc_io_str(tawcroot_binds[i].dst);
		tawc_io_str(" (src_fd=");
		tawc_io_dec(tawcroot_binds[i].src_fd);
		tawc_io_str(")\n");
	}

	tawcroot_dispatch_init();

	long nnp = tawcroot_set_no_new_privs();
	fails += tawc_io_step("PR_SET_NO_NEW_PRIVS", nnp == 0);

	long inst = tawcroot_install_handler();
	fails += tawc_io_step("install SIGSYS handler", inst == 0);

	/* The signal mask we inherit may have SIGSYS blocked (mirroring
	 * the production fix in main.c — JVM-spawned shell chain blocks
	 * various signals). Unblock SIGSYS up front, BEFORE probe_openat2,
	 * since that trips Android's stacked filter (synthesized in our
	 * test wrapper) and needs the handler actually invocable. */
	{
		uint64_t bit_sigsys = 1ULL << (31 - 1);
		(void)TAWC_RAW(TAWC_SYS_rt_sigprocmask, 1 /*SIG_UNBLOCK*/,
		               (long)&bit_sigsys, 0, 8, 0, 0);
	}

	int trap_nrs[256];
	const size_t trap_cap = sizeof trap_nrs / sizeof trap_nrs[0];
	size_t n_traps = tawcroot_dispatch_trap_list(trap_nrs, trap_cap);
	tawc_io_kv_dec("    trapped syscalls", (long)n_traps);
	/* trap_list returns the *true* count even when the buffer was
	 * truncated. Mirror the production guard in main.c so growth past
	 * trap_cap surfaces loudly rather than silently OOB-reading the
	 * truncated array into the filter generator. */
	fails += tawc_io_step("trap count fits trap_nrs[]", n_traps <= trap_cap);
	if (n_traps > trap_cap) return 1;

	long flt = tawcroot_install_filter(trap_nrs, n_traps);
	fails += tawc_io_step("install seccomp filter (rootfs syscall set)",
			      flt == 0);

	if (fails != 0) return 1;

	fails += test_identity_getuid();
	fails += test_identity_geteuid();
	fails += test_identity_getgid();
	fails += test_identity_getegid();
	fails += test_identity_setid_family_fakes_success();
	fails += test_openat_absolute_translates();
	fails += test_openat_null_efault();
	fails += test_openat_unmapped_efault();
	fails += test_openat_bogus_enoent();
	fails += test_efault_safety_output_buffers();
	fails += test_efault_safety_legacy_two_path();
	fails += test_symlink_memoization();
	fails += test_bind_mount_routing();
	fails += test_dotdot_clamp_etc_etc_probe();
	fails += test_dotdot_clamp_root_relative();
	fails += test_dotdot_via_dirfd_clamps_at_rootfs();
	fails += test_dotdot_via_bind_dst_dirfd();
	fails += test_dotdot_via_bind_dst_does_not_escape_to_host();
	fails += test_fstatat_fake_root_decoration();
	fails += test_dirfd_relative_symlink_escape_clamped();
	fails += test_open_creat_follows_symlink_leaf_clamped();
	fails += test_fstat_fake_root_decoration();
	fails += test_openat2_fchmodat2_enosys();
	fails += test_inotify_add_watch_translates();
	fails += test_fchdir_dispatch();
	fails += test_escape_attempt_clamps();
	fails += test_relative_path_after_chdir();
	fails += test_cwd_inside_bind();
	fails += test_readlinkat_dispatch();
	fails += test_readlink_on_bind_dst_returns_einval();
	fails += test_faccessat2_known_good();
	fails += test_getcwd_returns_root();
	fails += test_mkdirat_unlinkat();
	fails += test_symlinkat_readlinkat_unlinkat();
	fails += test_openat_opath_nofollow_symlink();
	fails += test_utimensat_symlink_nofollow();
	fails += test_utimensat_follow_regular_file();
#if defined(__ANDROID__)
	tawc_io_skip("mknodat FIFO host-only",
		     "Android shell SELinux denies mknod on shell_data_file");
# if defined(__x86_64__)
	tawc_io_skip("legacy mknod FIFO host-only",
		     "Android shell SELinux denies mknod on shell_data_file");
# endif
#else
	fails += test_mknodat_fifo();
# if defined(__x86_64__)
	fails += test_legacy_mknod_fifo();
# endif
#endif
	fails += test_statfs_in_rootfs();
	fails += test_xattr_dispatch();
	fails += test_fchownat_fake_root();
	fails += test_fchown_fake_root();
	fails += test_fstatat_at_empty_path();
	fails += test_statx_fake_root_decoration();
	fails += test_linkat_happy_path();
	fails += test_renameat2_happy_path();
	fails += test_renameat2_escape_clamped();
	fails += test_truncate_create_and_resize();
	fails += test_openat_abs_symlink_resolver_clamps();
	fails += test_openat_symlink_chain_three_hops();
	fails += test_openat_symlink_self_loop_eloop();
	fails += test_openat_abs_target_in_rootfs();
	fails += test_openat_opath_nofollow_abs_symlink();
	fails += test_lstat_nofollow_preserves_leaf_symlink();
	fails += test_mode_aware_memoization();
#if defined(__x86_64__)
	fails += test_legacy_x86_64_wrappers();
	fails += test_legacy_time_and_creat();
	fails += test_legacy_epoll_wait();
#endif
	fails += test_ioctl_translation();
	fails += test_ioctl_pty_translation();
	fails += test_internal_fd_protection();
	fails += test_guest_seccomp_prctl_handling();
	fails += test_sigsys_virtualization();
	fails += test_rt_sigaction_b2_sizing();
	fails += test_sigsys_block_shadow_multithread();
	fails += test_proc_self_exe_synthesis();
	fails += test_b4_rootfs_prefix_boundary(rootfs);
	fails += test_b5_bind_over_symlink_memo();
	fails += test_proc_self_maps_reverse_translation();
	fails += test_proc_self_synthesis_extensions();
	fails += test_proc_sys_overflow_id_synthesis();
	fails += test_proc_bus_pci_devices_synthesis();
	fails += test_dev_shm_emulation();
	fails += test_shm_survives_guest_closefrom();

	/* chroot tests. The non-mutating cases run first (NULL, ENOTDIR,
	 * ENOENT, identity, ..-clamp); the mutating "chroot into /usr"
	 * test runs LAST because it permanently swaps the rootfs view —
	 * nothing else can usefully run after it. */
	fails += test_chroot_efault_on_null();
	fails += test_chroot_enotdir_on_regular_file();
	fails += test_chroot_enoent_on_missing();
	fails += test_chroot_identity();
	fails += test_chroot_dotdot_clamps();
	fails += test_chroot_into_subdir();
	fails += test_chroot_through_symlink_follows_post();

	if (fails == 0) {
		tawc_io_str("ROOTFS SYSCALL SMOKE: PASS\n");
	} else {
		tawc_io_str("ROOTFS SYSCALL SMOKE: FAIL (");
		tawc_io_dec(fails);
		tawc_io_str(" failure(s))\n");
	}
	return fails == 0 ? 0 : 1;
}
