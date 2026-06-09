/* End-to-end test of the SIGSYS-handler-side execve interception via
 * the testhost-only `--exec-via-handler` diagnostic.
 *
 * `--exec-via-handler` exercises `tawcroot_exec_handler_perform()`,
 * which builds an exec_state in a memfd, opens /proc/self/exe, and
 * execveats into us with `--exec-child <fd>` — the same dance the
 * real SIGSYS handler will perform when it traps the guest's
 * `execve(2)`. Production gates `--exec-via-handler` off (only the
 * `--exec-child` re-entry is reachable in production); tawcroot-
 * testhost exposes both halves so we can test the round-trip.
 *
 * The handler's `/proc/self/exe` re-exec lands back in testhost main
 * with `--exec-child <bare-int>`, which testhost dispatches to the
 * production loader-child path (see main.c). So this test exercises
 * the same code path production runs at SIGSYS-driven exec time.
 *
 * Success of these tests means front + back halves of the phase-2.6
 * dance are wired correctly. The remaining phase-2.6 work (hooking
 * the handler into the dispatch table for actual SIGSYS-driven
 * traps) is a small wrapper that reads guest memory via usercopy.c
 * and calls into this same code path.
 */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#ifndef TAWCROOT_TESTHOST_BIN
# error "TAWCROOT_TESTHOST_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_EXIT42_BIN
# error "TAWCROOT_DYNAMIC_EXIT42_BIN must be defined"
#endif

static int run(const char *const *extra_args)
{
	VecStr cmd = c_init(vec_str, {TAWCROOT_TESTHOST_BIN});
	for (const char *const *p = extra_args; *p; p++) {
		vec_str_push(&cmd, *p);
	}
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd, .exit_code = &rc
	});
	failable_result_drop(&res);
	return rc;
}

test(exec_via_handler_static_exit42)
{
	const char *args[] = { "--exec-via-handler",
	                       TAWCROOT_STATIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_dynamic_exit42)
{
	const char *args[] = { "--exec-via-handler",
	                       TAWCROOT_DYNAMIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_system_bin_true)
{
	const char *args[] = { "--exec-via-handler", "/bin/true", NULL };
	test_int_eq(run(args), 0);
}

test(exec_via_handler_nonexistent_returns_50)
{
	/* `--exec-via-handler` reports any handler-perform negative
	 * return code via main and exits 50. Probe of nonexistent path
	 * fails at the open() step inside the handler. */
	const char *args[] = {
		"--exec-via-handler",
		"/this/path/does/not/exist/promise", NULL,
	};
	test_int_eq(run(args), 50);
}

test(exec_via_handler_directory_returns_50)
{
	/* execve of a directory must fail cleanly at the probe (EISDIR)
	 * — NOT execveat into the loader, which would destroy the calling
	 * process and exit with a loader code. Regression: the probe's
	 * O_RDONLY open succeeds on directories. */
	const char *args[] = { "--exec-via-handler", "/etc", NULL };
	test_int_eq(run(args), 50);
}

test(exec_via_handler_non_executable_returns_50)
{
	/* Same for a mode-644 regular file: real execve gives EACCES and
	 * the caller survives. /etc/hostname is a stable non-executable
	 * file on every host we run on. */
	const char *args[] = { "--exec-via-handler", "/etc/hostname", NULL };
	test_int_eq(run(args), 50);
}
