/* tawcroot entry point — argv dispatch + parent flow.
 *
 * `_start` (arch/<arch>_stub.S) tail-calls into `tawcroot_main`. From here
 * the production build parses the CLI and dispatches.
 *
 * Production CLI (must stay tight — anything reachable here is a
 * supported surface):
 *   tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]
 *     The "real" production mode (phase 2d). Opens the rootfs, builds
 *     the bind table, installs handler+filter, and manual-loads CMD
 *     from inside the rootfs view (path translation in effect).
 *   tawcroot --exec-child <fd>
 *     Re-entry from the SIGSYS execve handler dance. Reads exec_state
 *     from the inherited memfd and resumes through the loader. Not
 *     test-scaffolding — the handler in exec_handler.c re-execs into
 *     this branch as part of normal operation.
 *
 * Testhost-only diagnostic argv (gated behind TAWCROOT_TESTHOST so
 * production never exposes them):
 *   tawcroot-testhost --exec PATH [ARGS...]
 *     Loader-only diagnostic — host-fs paths, no translation, no
 *     handler/filter. Lets integration tests exercise manual-load in
 *     isolation.
 *   tawcroot-testhost --exec-via-handler PATH [ARGS...]
 *     Diagnostic for the SIGSYS-handler-side execve interception
 *     (writes a memfd and re-execs ourselves into --exec-child). No
 *     real seccomp trap; the handler function is called directly.
 *
 * The cleat test runner (`build/tawcroot-host/tests`) execs
 * `tawcroot-testhost` for these and for handler-layer smoke. See
 * notes/tawcroot.md "Testing strategy".
 */

#include <stdint.h>

#include "tawcroot.h"
#include "exec_handler.h"
#include "fdtab.h"
#include "filter.h"
#include "handler.h"
#include "io.h"
#include "loader_exec.h"
#include "path.h"
#include "raw_sys.h"
#include "dispatch.h"
#include "usercopy.h"

#ifdef TAWCROOT_TESTHOST
int tawcroot_testhost_main(int argc, char **argv);
#endif

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

/* Locate envp by walking past argv's NULL terminator on the kernel-
 * built initial stack. _start hands us argc/argv but not envp — we
 * recover it here. argv[argc] is NULL; envp = &argv[argc + 1]. */
static char **derive_envp(int argc, char **argv)
{
	return argv + argc + 1;
}

/* Walk the kernel-supplied auxv (which lives just past envp's NULL
 * terminator) and forward the values that matter to the guest. The
 * synthesizer omits zero values, so missing entries are fine.
 *
 * Why this exists: leaving AT_HWCAP=0 caused Firefox to abort early in
 * startup on x86_64 (xul-side CPU-feature gating treats HWCAP=0 as
 * "no SSE/AVX" but its hardcoded SIMD code still runs and faults).
 * AT_SYSINFO_EHDR=0 is also expensive — clock_gettime falls back to
 * a trapped syscall on every call instead of a vDSO call.
 *
 * Match the Linux uapi AT_* values we actually thread through; mirrors
 * `include/loader_stack.h`'s TAWC_AT_*. */
static void capture_host_auxv(int argc, char **argv)
{
	char **envp = derive_envp(argc, argv);
	while (*envp) envp++;
	uint64_t *aux = (uint64_t *)(envp + 1);

	uint64_t hwcap = 0, hwcap2 = 0, clktck = 0, flags = 0;
	uintptr_t sysinfo_ehdr = 0;

	for (size_t i = 0; aux[i] != 0 /* AT_NULL */; i += 2) {
		uint64_t t = aux[i];
		uint64_t v = aux[i + 1];
		switch (t) {
			case 8:  flags        = v; break;            /* AT_FLAGS */
			case 16: hwcap        = v; break;            /* AT_HWCAP */
			case 17: clktck       = v; break;            /* AT_CLKTCK */
			case 26: hwcap2       = v; break;            /* AT_HWCAP2 */
			case 33: sysinfo_ehdr = (uintptr_t)v; break; /* AT_SYSINFO_EHDR */
			default: break;
		}
	}
	tawcroot_loader_set_host_auxv(hwcap, hwcap2, sysinfo_ehdr,
	                              clktck, flags);
}

/* Tiny ASCII-decimal parser for `--exec-child <fd>`. Returns the
 * parsed value or -1 on garbage / overflow / leading sign.
 *
 * Rejecting leading `-`/`+` is intentional: the testhost dispatcher
 * disambiguates `--exec-child <bare-int>` (loader child path,
 * production semantics) from `--exec-child --state-fd=<n>` (smoke
 * child) by trying parse_fd on argv[2] and falling through on -1.
 * "--state-fd=..." starts with `-`, so it never parses as a valid
 * fd here. Don't relax this without auditing the testhost dispatch. */
static long parse_fd(const char *s)
{
	if (!s || !*s) return -1;
	long v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') return -1;
		v = v * 10 + (*p - '0');
		if (v > 0x7fffffff) return -1;
	}
	return v;
}

static __attribute__((noreturn)) void usage(int code)
{
#ifdef TAWCROOT_TESTHOST
	tawc_io_str("tawcroot-testhost: usage:\n"
	            "  tawcroot-testhost                         (foundation smoke)\n"
	            "  tawcroot-testhost --exec PATH [ARGS...]   (loader diagnostic)\n"
	            "  tawcroot-testhost --exec-via-handler PATH [ARGS...]\n"
	            "  tawcroot-testhost --exec-child <fd>       (handler re-exec target)\n"
	            "  tawcroot-testhost -r ROOTFS [-b SRC:DST]...\n");
#else
	tawc_io_str("tawcroot: usage:\n"
	            "  tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]\n"
	            "  tawcroot --exec-child <fd>\n");
#endif
	tawc_exit_group(code);
	__builtin_unreachable();
}

#ifndef TAWCROOT_TESTHOST
/* Linux openat flags. The full open(2) bitfield isn't worth pulling in
 * <fcntl.h> for; pin the values we need. */
#define O_RDONLY    0
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000
#define O_PATH      0x200000
#define AT_FDCWD    -100

/* Parse "src:dst" into a NUL-terminated `src_buf` and a pointer to
 * the dst tail. Returns 0 / -EINVAL. */
static long parse_bind_spec(const char *spec, char *src_buf, size_t cap,
                            const char **dst_out)
{
	size_t i = 0;
	while (spec[i] && spec[i] != ':' && i + 1 < cap) {
		src_buf[i] = spec[i];
		i++;
	}
	if (spec[i] != ':') return -22;
	src_buf[i] = 0;
	*dst_out = spec + i + 1;
	if (**dst_out == 0) return -22;
	return 0;
}

/* Set up rootfs/binds/handler/filter for production "real" mode.
 *
 * Init must complete BEFORE the loader runs because the guest will
 * issue path-bearing syscalls that need our handler installed. Every
 * step here either uses raw_sys helpers (stub-allowlisted, traverse
 * the filter once it's up) or runs before the filter is up.
 *
 * Exit codes 80-99 are reserved for init self-failures so callers can
 * distinguish init-broken from "guest exited with rc". (60..79 are
 * loader self-failures from loader_exec.h.) */
static void prod_rootfs_init(const char *rootfs,
                             const char *const *bind_specs, size_t n_binds)
{
	if (!rootfs || rootfs[0] != '/') {
		tawc_io_str("tawcroot: -r ROOTFS must be an absolute path\n");
		tawc_exit_group(80);
	}

	long rfd = tawc_openat(AT_FDCWD, rootfs,
	                       O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	if (rfd < 0) {
		tawc_io_str("tawcroot: open rootfs failed: ");
		tawc_io_str(rootfs);
		tawc_io_str("\n");
		tawc_exit_group(81);
	}

	long resv = tawcroot_fd_reserve((int)rfd);
	if (resv < 0) tawc_exit_group(82);
	tawcroot_rootfs_fd = (int)resv;

	{
		/* Canonicalize the rootfs path via /proc/self/fd/<fd>. The `-r`
		 * argument is whatever the caller passed; the kernel may report
		 * a different canonical path in getcwd / /proc/self/cwd
		 * (e.g. on Android, app dirs accessed via /data/user/0/<pkg>
		 * resolve to /data/data/<pkg> in the underlying mount, and
		 * getcwd reports the mount-side path, not the requested one).
		 * Without canonicalization, the prefix-match in handle_getcwd
		 * fails inside the chroot, glibc's getcwd returns ENOENT, and
		 * everything that depends on cwd (mkdir -p, bash's PWD, the
		 * relative-path resolver) misbehaves. */
		char canon[sizeof tawcroot_rootfs_host_path];
		char fdlink[64];
		size_t fi = 0;
		const char *p = "/proc/self/fd/";
		while (*p) fdlink[fi++] = *p++;
		long fd = tawcroot_rootfs_fd;
		char tmp[24]; int tn = 0;
		unsigned long u = fd > 0 ? (unsigned long)fd : 0;
		if (u == 0) tmp[tn++] = '0';
		while (u && tn < 24) { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; }
		while (tn > 0) fdlink[fi++] = tmp[--tn];
		fdlink[fi] = 0;

		long rl = tawc_readlinkat(AT_FDCWD, fdlink, canon, sizeof canon - 1);
		const char *src;
		size_t srclen;
		if (rl > 0 && canon[0] == '/') {
			canon[rl] = 0;
			src = canon;
			srclen = (size_t)rl;
		} else {
			/* Fall back to the user-supplied path if the readlink
			 * failed (eg /proc not mounted yet, exotic kernels). The
			 * canonicalization-mismatch bug is then visible but the
			 * legacy behaviour is preserved. */
			src = rootfs;
			srclen = 0;
			while (src[srclen]) srclen++;
		}
		size_t k = 0;
		while (k < srclen && k + 1 < sizeof tawcroot_rootfs_host_path) {
			tawcroot_rootfs_host_path[k] = src[k];
			k++;
		}
		/* Strip trailing slashes so the prefix-match in path.c stays
		 * unambiguous (review B4). */
		while (k > 1 && tawcroot_rootfs_host_path[k - 1] == '/') k--;
		tawcroot_rootfs_host_path[k] = 0;
		tawcroot_rootfs_host_path_len = k;
	}

	long up = tawc_usercopy_init();
	if (up < 0) {
		tawc_io_str("tawcroot: usercopy probe failed\n");
		tawc_exit_group(83);
	}

	for (size_t i = 0; i < n_binds; i++) {
		char src[1024];
		const char *dst = 0;
		if (parse_bind_spec(bind_specs[i], src, sizeof src, &dst) < 0) {
			tawc_io_str("tawcroot: malformed -b spec: ");
			tawc_io_str(bind_specs[i]);
			tawc_io_str("\n");
			tawc_exit_group(84);
		}
		long br = tawcroot_path_add_bind(src, dst);
		if (br < 0) {
			tawc_io_str("tawcroot: bind add failed for ");
			tawc_io_str(bind_specs[i]);
			tawc_io_str("\n");
			tawc_exit_group(85);
		}
	}

	tawcroot_path_memoize_well_known();
	tawcroot_dispatch_init();

	long nnp = tawcroot_set_no_new_privs();
	if (nnp != 0) tawc_exit_group(86);
	long inst = tawcroot_install_handler();
	if (inst != 0) tawc_exit_group(87);

	/* Reset the inherited signal mask to empty before we hand control
	 * to the guest binary. The JVM that spawned `/system/bin/sh` (which
	 * then exec'd us via ProcessBuilder) routinely blocks SIGCHLD /
	 * SIGPIPE / SIGUSR1 / SIGSYS / etc., and that mask persists through
	 * fork+execve into us. Two failure modes:
	 *
	 *   1. SIGSYS blocked → trapped syscalls can't reach our handler;
	 *      the kernel kills the process with default action (exit 159).
	 *   2. Other signals blocked → daemons spawned later (gpg-agent
	 *      from pacman-key, anything that select()'s on SIGCHLD) hang
	 *      because their main loop never receives the wake-ups it
	 *      expects.
	 *
	 * Clearing the whole mask here is the cheap fix: a fresh login
	 * starts with an empty mask, the guest binary deserves the same.
	 * Must run BEFORE probe_openat2 since that trips Android's stacked
	 * filter and needs the SIGSYS handler invocable. The runtime
	 * sigprocmask shadow in syscalls_control.c tracks any subsequent
	 * guest manipulation. */
	{
		uint64_t empty = 0;
		(void)TAWC_RAW(TAWC_SYS_rt_sigprocmask, 2 /*SIG_SETMASK*/,
		               (long)&empty, 0, 8, 0, 0);
	}

	/* openat2 probe must run AFTER install_handler AND after the
	 * SIGSYS unblock. Android's `untrusted_app` seccomp filter TRAPs
	 * openat2 (syscall 437) on recent platform versions; with our
	 * handler installed AND SIGSYS deliverable, the trap dispatches to
	 * "no slot → -ENOSYS", which the probe treats as "openat2
	 * unavailable, fall back to manual canonicalization". If SIGSYS is
	 * blocked here, the kernel can't deliver to our handler and the
	 * process dies with default action. */
	tawcroot_path_probe_openat2();

	/* Feed the filter generator from the dispatch table — single
	 * source of truth for the trap set. */
	int trap_nrs[256];
	const size_t trap_cap = sizeof trap_nrs / sizeof trap_nrs[0];
	size_t n_traps = tawcroot_dispatch_trap_list(trap_nrs, trap_cap);
	/* trap_list returns the *true* count even when the buffer was
	 * truncated. Passing that uncapped count to install_filter would
	 * be an OOB read; abort instead so growth past trap_cap is
	 * surfaced loudly rather than silently corrupting the filter. */
	if (n_traps > trap_cap) {
		tawc_io_str("tawcroot: too many trapped syscalls (");
		tawc_io_dec((long)n_traps);
		tawc_io_str("); bump trap_nrs[]\n");
		tawc_exit_group(89);
	}
	long flt = tawcroot_install_filter(trap_nrs, n_traps);
	if (flt != 0) tawc_exit_group(88);
}
#endif  /* !TAWCROOT_TESTHOST */

#if defined(__x86_64__)
# define HOST_PLATFORM "x86_64"
#elif defined(__aarch64__)
# define HOST_PLATFORM "aarch64"
#else
# error "unsupported host arch"
#endif

void tawcroot_main(int argc, char **argv)
{
#ifdef TAWCROOT_TESTHOST
	/* Testhost dispatch. argc==1 (smoke parent) skips capture_host_auxv
	 * since the smoke doesn't go through the loader. The loader-bearing
	 * branches all need auxv populated (HWCAP / SYSINFO_EHDR / etc). */
	if (argc >= 2) {
		capture_host_auxv(argc, argv);

		if (streq(argv[1], "--exec")) {
			if (argc < 3) usage(2);
			struct tawc_loader_exec_args ea = {
				.guest_path = argv[2],
				.argc       = argc - 2,
				.argv       = (const char *const *)(argv + 2),
				.envp       = (const char *const *)derive_envp(argc, argv),
				.platform   = HOST_PLATFORM,
			};
			tawcroot_loader_exec(&ea);
		}

		if (streq(argv[1], "--exec-via-handler")) {
			if (argc < 3) usage(2);
			const char *path = argv[2];
			long rc = tawcroot_exec_handler_perform(
				path,
				argc - 2,
				(const char *const *)(argv + 2),
				(const char *const *)derive_envp(argc, argv));
			tawc_io_str("tawcroot: --exec-via-handler: handler returned ");
			tawc_io_dec(rc);
			tawc_io_str("\n");
			tawc_exit_group(50);
		}

		/* --exec-child has two flavors in testhost: the loader re-exec
		 * target (bare-integer fd, used by --exec-via-handler's
		 * /proc/self/exe re-exec) and the foundation-smoke child
		 * (--state-fd=<n> companion arg). Disambiguate on whether
		 * argv[2] parses as a bare integer; if not, fall through to
		 * the testhost smoke dispatch below. */
		if (streq(argv[1], "--exec-child") && argc >= 3) {
			long fd = parse_fd(argv[2]);
			if (fd >= 0) {
				tawcroot_loader_exec_child((int)fd, HOST_PLATFORM);
			}
		}
	}

	int rc = tawcroot_testhost_main(argc, argv);
	tawc_exit_group(rc);
#else
	if (argc < 2) usage(2);

	/* Capture before any --exec-child dispatch so the loader path has
	 * the host's HWCAP / SYSINFO_EHDR / etc. ready to forward. The
	 * --exec-child path runs this on its own re-exec, so the values
	 * are fresh per tawcroot incarnation. */
	capture_host_auxv(argc, argv);

	if (streq(argv[1], "--exec-child")) {
		if (argc < 3) usage(2);
		long fd = parse_fd(argv[2]);
		if (fd < 0) {
			tawc_io_str("tawcroot: --exec-child: bad fd argument\n");
			tawc_exit_group(2);
		}
		tawcroot_loader_exec_child((int)fd, HOST_PLATFORM);
	}

	/* Production "real" mode: -r ROOTFS [-b SRC:DST]... -- CMD ARGS...
	 *
	 * Init the rootfs+handler+filter, translate the guest cmd, then
	 * hand off to the loader. The loader detects the rootfs-fd-set
	 * state and routes opens through translation. */
	const char *rootfs = 0;
	const char *bind_specs[TAWCROOT_MAX_BINDS];
	size_t      n_binds   = 0;
	int         cmd_start = -1;

	int i = 1;
	while (i < argc) {
		if (streq(argv[i], "--")) { cmd_start = i + 1; break; }
		if (streq(argv[i], "-r")) {
			if (i + 1 >= argc) usage(2);
			rootfs = argv[++i];
			i++;
		} else if (streq(argv[i], "-b")) {
			if (i + 1 >= argc) usage(2);
			if (n_binds >= TAWCROOT_MAX_BINDS) {
				tawc_io_str("tawcroot: too many -b binds\n");
				tawc_exit_group(2);
			}
			bind_specs[n_binds++] = argv[++i];
			i++;
		} else {
			tawc_io_str("tawcroot: unknown option: ");
			tawc_io_str(argv[i]);
			tawc_io_str("\n");
			usage(2);
		}
	}
	if (!rootfs || cmd_start < 0 || cmd_start >= argc) usage(2);

	prod_rootfs_init(rootfs, bind_specs, n_binds);

	/* Phase 2e: stash the guest exe path so /proc/self/exe synthesis
	 * returns it instead of the kernel's libtawcroot view. Set BEFORE
	 * the loader jumps; the readlinkat handler reads it from the SIGSYS
	 * dispatch path. */
	tawcroot_set_guest_exe_path(argv[cmd_start]);

	/* Hand off. The loader internally calls tawcroot_path_translate
	 * for guest_path and PT_INTERP because tawcroot_rootfs_fd is now
	 * set; argv[0] from the guest's perspective stays the path it
	 * asked for (passed unchanged into the synthesized stack). */
	struct tawc_loader_exec_args ea = {
		.guest_path = argv[cmd_start],
		.argc       = argc - cmd_start,
		.argv       = (const char *const *)(argv + cmd_start),
		.envp       = (const char *const *)derive_envp(argc, argv),
		.platform   = HOST_PLATFORM,
	};
	tawcroot_loader_exec(&ea);
#endif
}
