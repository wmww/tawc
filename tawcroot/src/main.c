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
 * notes/tawcroot/testing.md "Testing strategy".
 */

#include <stdint.h>

#include <signal.h>
#include <sys/prctl.h>

#include "tawcroot.h"
#include "dispatch.h"
#include "exec_handler.h"
#include "filter.h"
#include "io.h"
#include "loader_exec.h"
#include "path.h"
#include "raw_sys.h"
#include "supervisor.h"

#ifdef TAWCROOT_TESTHOST
int tawcroot_testhost_main(int argc, char **argv);
#endif

/* Bind a freshly-forked top-level tawcroot to its launcher: SIGKILL
 * us when the launcher dies, exit cleanly if we're already orphaned
 * to init (catches the fork→prctl race where the launcher died
 * BEFORE we could install PDEATHSIG, so no signal got queued).
 *
 * **Only correct for top-level entries** — NOT for the `--exec-child`
 * re-exec spawned by our SIGSYS execve handler. The `--exec-child`
 * path runs in the middle of gpgme's posix_spawn flow:
 *
 *   pacman ─fork→ intermediate ─fork→ grand-child ─execve→ gpg
 *                                     ↑ SIGSYS → tawcroot --exec-child
 *
 * where `intermediate` does `_exit(0)` immediately to reparent the
 * grand-child to init (gpgme's standard double-fork-prevent-zombies
 * trick). Two failure modes if we run the top-level binding here:
 *
 *  1. `getppid() == 1`-then-exit: gpgme's reparent makes this state
 *     deliberate, not an orphan-leak. Killing the worker means every
 *     gpgconf/gpg invocation dies during pacman keyring init →
 *     "GPGME error: Invalid crypto engine" → "missing required
 *     signature" on every Arch package. (Surfaced in commit d48faf8.)
 *
 *  2. PDEATHSIG=SIGKILL: PDEATHSIG fires from `forget_original_parent`
 *     during the parent's own `do_exit` (kernel/exit.c), not at the
 *     later waitpid-reap. So the race is between *intermediate's*
 *     `_exit(0)` (the trigger) and the *grand-child's* prctl (the
 *     arm). If TRIGGER wins (usual case — intermediate exits much
 *     faster than the grand-child's exec_handler→memfd→execveat→
 *     supervisor→loader bootstrap), pdeath_signal is still 0 at the
 *     moment forget_original_parent walks, no signal queued, the
 *     later prctl is bound to init (which never exits) and is a
 *     harmless no-op. If ARM wins (rare — the bootstrap raced ahead
 *     of the kernel's exit-side work for `intermediate`), SIGKILL
 *     gets queued and the grand-child dies mid-startup. gpg dies
 *     before writing GOODSIG to its status pipe; gpgme reports
 *     `verify_result->signatures == NULL`; libalpm reports "missing
 *     required signature". A 10 ms nanosleep workaround widened the
 *     bootstrap delay so TRIGGER won more reliably — symptom
 *     mitigation, not root-cause fix. The actual fix is to not arm
 *     PDEATHSIG on this entry shape.
 *
 * Cleanup story for `--exec-child` workers without their own
 * PDEATHSIG: the production caller is the in-app exec broker (see
 * notes/exec-broker.md). When the broker disconnects, the broker's
 * `/proc` BFS walks descendants by `PPid` and SIGKILLs them — but
 * that only catches workers whose ppid still chains back to the
 * broker. Workers reparented to init via the gpgme dance escape that
 * BFS. The actual whole-tree safety net is Android's `am force-stop`
 * (or app death), which SIGKILLs every process under the app's UID
 * regardless of parent. Both the broker's BFS and the per-process
 * PDEATHSIG were always supplementary to that UID-wide kill; the
 * old PDEATHSIG-on-`--exec-child` was already a no-op for the gpgme
 * case (parent was init by the time prctl ran), so dropping it here
 * doesn't regress any path the broker BFS was actually catching.
 *
 * Top-level (ENTRY_PROD) keeps both halves: broker is a stable
 * launcher that doesn't intentionally exit mid-spawn, so PDEATHSIG
 * fires only on real broker death/disconnect — which is what we
 * want. */
static void bind_top_level_to_parent(void)
{
	(void)tawc_prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
	if (tawc_getppid() == 1) tawc_exit_group(0);
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

	uint64_t hwcap = 0, hwcap2 = 0, clktck = 0, flags = 0, pagesz = 0;
	uintptr_t sysinfo_ehdr = 0;

	for (size_t i = 0; aux[i] != 0 /* AT_NULL */; i += 2) {
		uint64_t t = aux[i];
		uint64_t v = aux[i + 1];
		switch (t) {
			case 6:  pagesz       = v; break;            /* AT_PAGESZ */
			case 8:  flags        = v; break;            /* AT_FLAGS */
			case 16: hwcap        = v; break;            /* AT_HWCAP */
			case 17: clktck       = v; break;            /* AT_CLKTCK */
			case 26: hwcap2       = v; break;            /* AT_HWCAP2 */
			case 33: sysinfo_ehdr = (uintptr_t)v; break; /* AT_SYSINFO_EHDR */
			default: break;
		}
	}
	tawcroot_loader_set_host_auxv(hwcap, hwcap2, sysinfo_ehdr,
	                              clktck, flags, pagesz);
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
 * issue path-bearing syscalls that need our handler installed.
 * Most of the bootstrap (rootfs view, binds, handler, signal-mask
 * reset, /proc/self/exe stash) is shared with the
 * --exec-child re-entry and lives in tawcroot_supervisor_init. The
 * pieces still owned by the production path are:
 *   - validating and parsing the CLI (-r, -b)
 *   - PR_SET_NO_NEW_PRIVS (sticky; child inherits via execve)
 *   - the seccomp filter install (one-shot; child inherits)
 *
 * Exit codes for prod-only failures: 80 (path), 84 (bind parse),
 * 86 (no_new_privs), 88 (filter install), 89 (too many traps).
 * Supervisor-internal failures use 90..95 (see supervisor.h). */
static void prod_rootfs_init(const char *rootfs,
                             const char *const *bind_specs, size_t n_binds)
{
	if (!rootfs || rootfs[0] != '/') {
		tawc_io_str("tawcroot: -r ROOTFS must be an absolute path\n");
		tawc_exit_group(80);
	}

	/* Parse CLI bind specs into parallel src/dst arrays before
	 * handing off to supervisor_init. The buffers live for the rest
	 * of tawcroot_main, which outlives every consumer of the bind
	 * table. */
	static char         bind_src_buf[TAWCROOT_MAX_BINDS][1024];
	static const char  *bind_src[TAWCROOT_MAX_BINDS];
	static const char  *bind_dst[TAWCROOT_MAX_BINDS];
	for (size_t i = 0; i < n_binds; i++) {
		const char *dst = 0;
		if (parse_bind_spec(bind_specs[i], bind_src_buf[i],
		                    sizeof bind_src_buf[i], &dst) < 0) {
			tawc_io_str("tawcroot: malformed -b spec: ");
			tawc_io_str(bind_specs[i]);
			tawc_io_str("\n");
			tawc_exit_group(84);
		}
		bind_src[i] = bind_src_buf[i];
		bind_dst[i] = dst;
	}

	struct tawcroot_supervisor_args sa = {
		.rootfs_host_path = rootfs,
		.bind_src         = bind_src,
		.bind_dst         = bind_dst,
		.n_binds          = n_binds,
		/* No inherited shm at top-level entry. */
		.shm_names        = 0,
		.shm_fds          = 0,
		.n_shm            = 0,
	};
	tawcroot_supervisor_init(&sa);

	/* PR_SET_NO_NEW_PRIVS is sticky: set once, inherited by every
	 * fork+execve descendant including the --exec-child re-exec.
	 * Filter install needs it. */
	long nnp = tawcroot_set_no_new_privs();
	if (nnp != 0) tawc_exit_group(86);

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

/* Entry-point classification.
 *
 * tawcroot has several distinct entry shapes that share `_start`. Each
 * has different requirements for setup (e.g. orphan-detect, auxv
 * capture) and different dispatch destinations. We classify once up
 * front so the policy table is in one place — adding a new entry
 * shape means adding one enum value and one switch arm, not auditing
 * every check scattered through main. */
enum tawcroot_entry {
	ENTRY_PROD,             /* production: -r ROOTFS [-b ...] -- CMD     */
	ENTRY_EXEC_CHILD,       /* --exec-child <fd> (same-process re-exec)  */
#ifdef TAWCROOT_TESTHOST
	ENTRY_EXEC,             /* --exec PATH ARGS (loader-only diagnostic) */
	ENTRY_EXEC_VIA_HANDLER, /* --exec-via-handler PATH ARGS (handler dx) */
	ENTRY_TESTHOST,         /* smoke driver / phase suites               */
#endif
	ENTRY_USAGE_ERROR,      /* malformed CLI                             */
};

static enum tawcroot_entry classify_entry(int argc, char **argv)
{
	if (argc < 2) {
#ifdef TAWCROOT_TESTHOST
		return ENTRY_TESTHOST;       /* smoke parent — no argv flags */
#else
		return ENTRY_USAGE_ERROR;
#endif
	}

	if (tawc_streq(argv[1], "--exec-child")) {
		/* --exec-child has two flavors. Production has only one
		 * (loader re-exec target, bare-integer fd); testhost adds a
		 * smoke-child variant (--state-fd=<n> companion arg). The
		 * disambiguator is: parse argv[2] as a bare integer; if it
		 * parses, this is the loader path; if not (or argc < 3),
		 * fall through. parse_fd rejects leading `-`/`+` so
		 * `--state-fd=...` never accidentally parses. */
		if (argc >= 3 && parse_fd(argv[2]) >= 0) return ENTRY_EXEC_CHILD;
#ifdef TAWCROOT_TESTHOST
		return ENTRY_TESTHOST;
#else
		return ENTRY_USAGE_ERROR;
#endif
	}

#ifdef TAWCROOT_TESTHOST
	if (tawc_streq(argv[1], "--exec"))             return ENTRY_EXEC;
	if (tawc_streq(argv[1], "--exec-via-handler")) return ENTRY_EXEC_VIA_HANDLER;
	return ENTRY_TESTHOST;
#else
	/* Anything else is taken as the start of -r/-b/-- parsing.
	 * prod_main handles malformed flag combinations and emits
	 * usage(2); we don't pre-validate here. */
	return ENTRY_PROD;
#endif
}

#ifndef TAWCROOT_TESTHOST
__attribute__((noreturn)) static void prod_main(int argc, char **argv)
{
	const char *rootfs = 0;
	const char *bind_specs[TAWCROOT_MAX_BINDS];
	size_t      n_binds   = 0;
	int         cmd_start = -1;

	int i = 1;
	while (i < argc) {
		if (tawc_streq(argv[i], "--")) { cmd_start = i + 1; break; }
		if (tawc_streq(argv[i], "-r")) {
			if (i + 1 >= argc) usage(2);
			rootfs = argv[++i];
			i++;
		} else if (tawc_streq(argv[i], "-b")) {
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
	 * dispatch path. (init_self_host_path runs inside supervisor_init.) */
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
}
#endif

void tawcroot_main(int argc, char **argv)
{
	enum tawcroot_entry entry = classify_entry(argc, argv);

	/* Top-level binding (PDEATHSIG + orphan-detect) is unsafe on the
	 * --exec-child re-exec; see bind_top_level_to_parent's docstring
	 * for the gpgme posix_spawn rationale. */
	if (entry != ENTRY_EXEC_CHILD) bind_top_level_to_parent();

	switch (entry) {
		case ENTRY_USAGE_ERROR:
			usage(2);

		case ENTRY_EXEC_CHILD:
			/* --exec-child runs capture_host_auxv on its own re-exec
			 * path so values are fresh per tawcroot incarnation. */
			capture_host_auxv(argc, argv);
			tawcroot_loader_exec_child((int)parse_fd(argv[2]),
			                           HOST_PLATFORM);

#ifdef TAWCROOT_TESTHOST
		case ENTRY_EXEC: {
			if (argc < 3) usage(2);
			capture_host_auxv(argc, argv);
			struct tawc_loader_exec_args ea = {
				.guest_path = argv[2],
				.argc       = argc - 2,
				.argv       = (const char *const *)(argv + 2),
				.envp       = (const char *const *)derive_envp(argc, argv),
				.platform   = HOST_PLATFORM,
			};
			tawcroot_loader_exec(&ea);
		}

		case ENTRY_EXEC_VIA_HANDLER: {
			if (argc < 3) usage(2);
			capture_host_auxv(argc, argv);
			long rc = tawcroot_exec_handler_perform(
				argv[2],
				argc - 2,
				(const char *const *)(argv + 2),
				(const char *const *)derive_envp(argc, argv));
			tawc_io_str("tawcroot: --exec-via-handler: handler returned ");
			tawc_io_dec(rc);
			tawc_io_str("\n");
			tawc_exit_group(50);
		}

		case ENTRY_TESTHOST: {
			/* argc==1 (smoke parent) skips capture_host_auxv since the
			 * smoke driver doesn't go through the loader. argc>=2 means
			 * we landed here from a fall-through (e.g. --exec-child
			 * with --state-fd=<n>) and the loader-bearing smoke phases
			 * may need auxv. */
			if (argc >= 2) capture_host_auxv(argc, argv);
			int rc = tawcroot_testhost_main(argc, argv);
			tawc_exit_group(rc);
		}
#endif

		case ENTRY_PROD:
#ifdef TAWCROOT_TESTHOST
			/* Unreachable in testhost — classify_entry never returns
			 * this branch. The switch arm exists only because the
			 * enum value is shared. */
			tawc_exit_group(2);
#else
			capture_host_auxv(argc, argv);
			prod_main(argc, argv);
#endif
	}
	/* All cases above are noreturn-equivalent. */
	__builtin_unreachable();
}
