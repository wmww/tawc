/* tawcroot-testhost entry -- replaces main.c's `tawcroot_main` when the
 * binary is built with `-DTAWCROOT_TESTHOST`. Drives the phase-0 / phase-1
 * smoke flows that exercise the SIGSYS handler and BPF filter from inside
 * the same process whose filter is being tested.
 *
 * Lives outside `tawcroot/src/` deliberately: the production binary
 * (`libtawcroot.so` shipped in the APK, plus the host `tawcroot`) must
 * not contain any test-only argv branches or smoke scaffolding. The
 * cleat-driven test runner (`build/tawcroot-host/tests`) execs this
 * binary as a subprocess for handler-layer tests.
 *
 * See notes/tawcroot.md "Testing strategy" -- this is the in-binary half;
 * the cleat half lives under `tests/{unit,handler,integration}/`.
 */

#include <stddef.h>
#include <stdint.h>

#include "tawcroot.h"
#include "io.h"
#include "raw_sys.h"
#include "filter.h"
#include "handler.h"
#include "child.h"
#include "smoke.h"
#include "phase1.h"
#include "tawc_uapi.h"

#ifndef FD_CLOEXEC
# define FD_CLOEXEC 1
#endif

extern char tawcroot_raw_syscall_insn[];
extern char tawcroot_raw_syscall_ret[];

static long open_self_exe(void)
{
	return tawc_openat(AT_FDCWD, "/proc/self/exe", O_RDONLY, 0);
}

static int parent_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	tawc_io_str("tawcroot phase-0 foundation smoke (parent)\n");
	tawc_io_kv_hex("stub_insn_addr",
		       (unsigned long)(uintptr_t)&tawcroot_raw_syscall_insn[0]);
	tawc_io_kv_hex("stub_ret_addr",
		       (unsigned long)(uintptr_t)&tawcroot_raw_syscall_ret[0]);

	int fails = 0;

	long nnp = tawcroot_set_no_new_privs();
	fails += tawc_io_step("PR_SET_NO_NEW_PRIVS", nnp == 0);

	long inst = tawcroot_install_handler();
	fails += tawc_io_step("install SIGSYS handler", inst == 0);

	long flt = tawcroot_install_smoke_filter(TAWC_SYS_getpid);
	fails += tawc_io_step("install seccomp filter (TRAP=getpid)",
			      flt == 0);

	fails += tawcroot_smoke_trap_contract("[parent] ");
	fails += tawcroot_smoke_exercise_raw();

	if (fails != 0) {
		tawc_io_str("PARENT SMOKE: FAIL -- skipping re-exec\n");
		return 1;
	}

	long mfd = tawc_memfd_create("tawcroot-exec-state", MFD_CLOEXEC);
	fails += tawc_io_step("memfd_create exec-state", mfd >= 0);
	if (mfd < 0) { tawc_io_kv_dec("    memfd_create errno", -mfd); return 1; }

	tawcroot_handler_obs ph_obs;
	tawcroot_handler_observe(&ph_obs);
	struct tawcroot_smoke_state st = {
		.magic                = TAWC_SMOKE_MAGIC,
		.parent_pid           = (uint64_t)tawc_getpid(),
		.parent_handler_calls = ph_obs.calls,
		.round                = 0,
	};
	long w = tawc_write((int)mfd, &st, sizeof st);
	fails += tawc_io_step("write exec-state into memfd",
			      w == (long)sizeof st);
	long lr = tawc_lseek((int)mfd, 0, 0 /*SEEK_SET*/);
	fails += tawc_io_step("lseek state-fd to 0", lr == 0);

	long fdflags = tawc_fcntl((int)mfd, F_GETFD, 0);
	fails += tawc_io_step("fcntl(F_GETFD) on state-fd", fdflags >= 0);
	long sfd = tawc_fcntl((int)mfd, F_SETFD, fdflags & ~FD_CLOEXEC);
	fails += tawc_io_step("fcntl(F_SETFD, !CLOEXEC)", sfd == 0);

	if (fails != 0) {
		tawc_io_str("PARENT SMOKE: FAIL before re-exec\n");
		return 1;
	}

	char fd_arg[32];
	{
		const char *prefix = "--state-fd=";
		size_t plen = tawc_strlen(prefix);
		for (size_t i = 0; i < plen; i++) fd_arg[i] = prefix[i];
		tawc_int_to_str(fd_arg + plen, sizeof(fd_arg) - plen,
				(int)mfd);
	}
	char arg0[] = "tawcroot";
	char arg1[] = "--exec-child";
	char *new_argv[] = { arg0, arg1, fd_arg, 0 };

	long exe_fd = open_self_exe();
	fails += tawc_io_step("openat(/proc/self/exe)", exe_fd >= 0);
	if (exe_fd < 0) return 1;

	char *envp_empty[] = { 0 };
	long er = tawc_execveat((int)exe_fd, "", new_argv, envp_empty,
				AT_EMPTY_PATH);

	tawc_io_kv_dec("execveat errno (-rv)", -er);
	tawc_io_str("PARENT SMOKE: FAIL -- execveat returned\n");
	return 1;
}

int tawcroot_testhost_main(int argc, char **argv)
{
	int is_child = 0;
	const char *rootfs = 0;
	for (int i = 1; i < argc; i++) {
		if (tawc_streq(argv[i], "--exec-child")) {
			is_child = 1;
		} else if (tawc_streq(argv[i], "-r") && i + 1 < argc) {
			rootfs = argv[++i];
		}
		/* `-b src:dst` is parsed inside tawcroot_phase1_main so it
		 * has access to argv unmodified -- keep the dispatch loop
		 * here narrow. */
	}

	if (is_child)            return tawcroot_child_main(argc, argv);
	if (rootfs)              return tawcroot_phase1_main_argv(argc, argv, rootfs);
	return parent_main(argc, argv);
}
