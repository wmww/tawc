/* Integration tests for the testhost-only `--exec` diagnostic.
 *
 * `--exec PATH` is the loader-only entry point: no path translation,
 * no SIGSYS handler, no seccomp filter — just the manual ELF loader
 * against host-fs paths. Production gates this off (see main.c); only
 * `tawcroot-testhost` exposes it. We exercise it here because it
 * proves the raw_sys-backed loader I/O vtable (loader_io_prod.c)
 * works against real binaries — the unit tests exercise the same
 * loader logic but with a libc-forwarding I/O impl, which doesn't
 * catch raw_sys wiring bugs.
 *
 * The testhost binary uses the same PROD_C source set as production
 * plus the smoke scaffolding (testhost_main.c, child.c, etc.), so the
 * loader paths under test are byte-identical to production.
 *
 * Test fixtures live in tests/integration/programs/ (built by the
 * Makefile; see TAWCROOT_*_BIN defines).
 */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#ifndef TAWCROOT_TESTHOST_BIN
# error "TAWCROOT_TESTHOST_BIN must be defined by the build"
#endif
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_EXIT42_BIN
# error "TAWCROOT_DYNAMIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_BIG_STACK_BIN
# error "TAWCROOT_DYNAMIC_BIG_STACK_BIN must be defined"
#endif

/* Run TAWCROOT_TESTHOST_BIN with the given extra args (NULL-terminated).
 * Returns the wait-status code (exit code on normal exit, negative
 * on signal or framework failure). run_subproc takes ownership of
 * `cmd`, so the local needn't be dropped. */
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

test(diag_exec_static_exit42)
{
	const char *args[] = { "--exec", TAWCROOT_STATIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(diag_exec_dynamic_exit42)
{
	const char *args[] = { "--exec", TAWCROOT_DYNAMIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

/* Regression: GCC `-fstack-clash-protection` page-probes a function's
 * frame as the prologue grows %rsp. coreutils 9.11 wc compiles its
 * read buffer (256 KiB) onto the stack frame, so the probe loop walks
 * 64+ pages before main work begins. Earlier the loader mmap'd a 256
 * KiB guest stack and the probe stepped one page past the bottom →
 * SEGV_MAPERR at exactly %rsp. The bin allocates 380 KiB > the old
 * 256 KiB stack, < the current 8 MiB stack, and touches every page;
 * exits 42 on success. If the loader stack is bumped down again, this
 * test reports the wait-status as -11 (or similar) instead of 42. */
test(diag_exec_dynamic_big_stack)
{
	const char *args[] = { "--exec", TAWCROOT_DYNAMIC_BIG_STACK_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(diag_exec_system_bin_true)
{
	const char *args[] = { "--exec", "/bin/true", NULL };
	test_int_eq(run(args), 0);
}

test(diag_exec_missing_path_prints_usage)
{
	/* Only `--exec` with no path → usage */
	const char *args[] = { "--exec", NULL };
	test_int_eq(run(args), 2);
}

test(diag_exec_nonexistent_guest_fails_clean)
{
	const char *args[] = {
		"--exec", "/this/path/does/not/exist/promise", NULL,
	};
	/* loader_exec.h: code 60 = open guest failed. */
	test_int_eq(run(args), 60);
}
