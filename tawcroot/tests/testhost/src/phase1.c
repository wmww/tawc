/* Phase-1 smoke: real path translation + identity decoration.
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
 * See notes/tawcroot.md "Phase 1 -- MVP path translation".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/stat.h>
#include <sys/stat.h>

#include "phase1.h"
#include "io.h"
#include "raw_sys.h"
#include "filter.h"
#include "fdtab.h"
#include "handler.h"
#include "dispatch.h"
#include "path.h"
#include "sysnr.h"
#include "usercopy.h"

#include "tawc_uapi.h"

/* ----- inline-asm syscall probes (issued from within phase1.c so the
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

int tawcroot_phase1_main_argv(int argc, char **argv, const char *rootfs)
{
	/* Collect `-b src:dst` entries. We need them parsed before
	 * `tawcroot_phase1_main` installs the seccomp filter -- once the
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
			tawc_io_str("[phase1] -b entry missing ':' -- '");
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
			tawc_io_str("[phase1] bind add failed for '");
			tawc_io_str(spec);
			tawc_io_str("' errno=-");
			tawc_io_dec(-br);
			tawc_io_str("\n");
			return 1;
		}
	}
	return tawcroot_phase1_main(rootfs);
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
		char p[512];
		size_t n = 0;
		p[n++] = '/';
		for (size_t j = 0; j < tawcroot_binds[i].dst_len; j++)
			p[n++] = tawcroot_binds[i].dst[j];
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
 * down by build_fake_rootfs in test_phase1.c; using a dedicated
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

/* mknodat: create a FIFO (S_IFIFO doesn't need CAP_MKNOD). The
 * test fixture rootfs lives on tmpfs which supports mkfifo, so a
 * successful return tells us the dispatch is wired up AND the
 * path translation routed to the rootfs view (not the host). */
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
 * which doesn't exist in the fake rootfs -> ENOENT). This must work
 * on every kernel we target; on >=5.6 the kernel's
 * openat2(RESOLVE_IN_ROOT) does the same job in handle_openat too,
 * but the resolver runs unconditionally so all path-bearing handlers
 * (fstatat, readlinkat, ...) get the same clamping. */
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
#endif

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
 * install the guest's filter (it'd stack on top of ours and could
 * KILL_PROCESS our own raw_syscall stub), but returning -EPERM
 * tripped Mozilla's content-sandbox setup into a teardown path
 * that aborted inside libhybris's bionic-Q linker on the
 * `unregister_tls_module` CHECK. So we lie about successful install
 * — the guest thinks its filter is up but the only filter actually
 * in place is ours, which already enforces translation. The crucial
 * thing tested below is that the LIE doesn't break the actual
 * translation: a path syscall after the fake-success still routes
 * through our handler. */
static int test_guest_seccomp_prctl_handling(void)
{
	int fails = 0;
	long rv;
	/* seccomp(SECCOMP_SET_MODE_FILTER, 0, NULL) -- enough to test
	 * the dispatch path without needing a valid filter program. */
	INLINE_SYS6(TAWC_SYS_seccomp, 1 /*SET_MODE_FILTER*/,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("seccomp(SET_MODE_FILTER) -> 0 (faked)",
			      rv == 0);
	tawc_io_kv_dec("    rv", rv);

	/* prctl(PR_SET_SECCOMP) -> 0 (faked, same rationale). */
	INLINE_SYS6(TAWC_SYS_prctl, 22 /*PR_SET_SECCOMP*/,
		    0, 0, 0, 0, 0, rv);
	fails += tawc_io_step("prctl(PR_SET_SECCOMP) -> 0 (faked)",
			      rv == 0);
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

/* ----- main ------------------------------------------------------- */

int tawcroot_phase1_main(const char *rootfs)
{
	int fails = 0;

	tawc_io_str("\ntawcroot phase-1 smoke (path translation)\n");
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

	/* Probe process_vm_readv (against our own pid). Used by the
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

	/* Probe openat2 (kernel ≥ 5.6). Sets `tawcroot_openat2_works` so
	 * the openat handler can route through openat2 with RESOLVE_IN_ROOT
	 * for generic non-final-component symlink resolution.
	 *
	 * Must run AFTER install_handler — Android's untrusted_app filter
	 * RET_TRAPs openat2 (NR 437); the synthesized-Android-filter test
	 * wrapper does the same. With the handler installed, the trap
	 * dispatches to "no slot → -ENOSYS" (since openat2 isn't in our
	 * dispatch table) and the probe interprets that as "openat2
	 * unavailable, fall back to manual canonicalization". Without the
	 * handler, default SIGSYS disposition kills the process. Mirrors
	 * the production fix in main.c (see notes/tawcroot.md "Bugs found
	 * and fixed during install pipeline validation"). */
	tawcroot_path_probe_openat2();
	tawc_io_kv_dec("    openat2_works", (long)tawcroot_openat2_works);

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
	fails += tawc_io_step("install seccomp filter (phase-1 set)",
			      flt == 0);

	if (fails != 0) return 1;

	fails += test_identity_getuid();
	fails += test_identity_geteuid();
	fails += test_identity_getgid();
	fails += test_identity_getegid();
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
	fails += test_fstatat_fake_root_decoration();
	fails += test_escape_attempt_clamps();
	fails += test_relative_path_after_chdir();
	fails += test_readlinkat_dispatch();
	fails += test_faccessat2_known_good();
	fails += test_getcwd_returns_root();
	fails += test_mkdirat_unlinkat();
	fails += test_symlinkat_readlinkat_unlinkat();
	fails += test_openat_opath_nofollow_symlink();
	fails += test_utimensat_symlink_nofollow();
	fails += test_utimensat_follow_regular_file();
	fails += test_mknodat_fifo();
#if defined(__x86_64__)
	fails += test_legacy_mknod_fifo();
#endif
	fails += test_statfs_in_rootfs();
	fails += test_xattr_dispatch();
	fails += test_fchownat_fake_root();
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
#endif
	fails += test_internal_fd_protection();
	fails += test_guest_seccomp_prctl_handling();
	fails += test_sigsys_virtualization();
	fails += test_rt_sigaction_b2_sizing();
	fails += test_proc_self_exe_synthesis();
	fails += test_b4_rootfs_prefix_boundary(rootfs);
	fails += test_b5_bind_over_symlink_memo();
	fails += test_proc_self_maps_reverse_translation();
	fails += test_dev_shm_emulation();

	if (fails == 0) {
		tawc_io_str("PHASE-1 SMOKE: PASS\n");
	} else {
		tawc_io_str("PHASE-1 SMOKE: FAIL (");
		tawc_io_dec(fails);
		tawc_io_str(" failure(s))\n");
	}
	return fails == 0 ? 0 : 1;
}
