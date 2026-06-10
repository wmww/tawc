/* Round-trip tests for the exec_state memfd format
 * (tawcroot/src/exec_state.c).
 *
 * Pure data: writer + reader are libc-free; we exercise them under
 * hosted glibc and assert that what we write comes back identical.
 * The on-device usage path (handler writes → re-exec → child reads)
 * is integration-tested in tests/integration/test_exec_child.c.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "exec_state.h"

test(exec_state_basic_roundtrip)
{
	const char *argv[] = { "/bin/true", "alpha", "beta", NULL };
	const char *envp[] = { "PATH=/bin", "HOME=/root", NULL };

	size_t need = tawcroot_exec_state_estimate_bytes("/bin/true", 3, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);

	long w = tawcroot_exec_state_write(buf, need, "/bin/true", 3, argv, envp, NULL);
	test_int_eq((int)w, (int)need);

	const char *argv_buf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *envp_buf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, argv_buf, envp_buf, &out), 0);

	test_str_eq(out.path, "/bin/true");
	test_int_eq((int)out.argc, 3);
	test_int_eq((int)out.envc, 2);
	test_str_eq(out.argv[0], "/bin/true");
	test_str_eq(out.argv[1], "alpha");
	test_str_eq(out.argv[2], "beta");
	test_ptr_eq(out.argv[3], NULL);
	test_str_eq(out.envp[0], "PATH=/bin");
	test_str_eq(out.envp[1], "HOME=/root");
	test_ptr_eq(out.envp[2], NULL);
	free(buf);
}

test(exec_state_zero_argv_envp)
{
	const char *argv[] = { NULL };
	const char *envp[] = { NULL };
	size_t need = tawcroot_exec_state_estimate_bytes("/x", 0, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/x", 0, argv, envp, NULL),
	            (int)need);

	const char *argv_buf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *envp_buf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, argv_buf, envp_buf, &out), 0);
	test_str_eq(out.path, "/x");
	test_int_eq((int)out.argc, 0);
	test_int_eq((int)out.envc, 0);
	test_ptr_eq(out.argv[0], NULL);
	test_ptr_eq(out.envp[0], NULL);
	free(buf);
}

test(exec_state_buf_too_small)
{
	const char *argv[] = { "long-argument-name", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[16];   /* nowhere near enough */
	test_int_eq((int)tawcroot_exec_state_write(buf, sizeof buf,
	                                           "/usr/bin/some-program",
	                                           1, argv, envp, NULL),
	            -28 /* ENOSPC */);
}

test(exec_state_too_many_args)
{
	const char *argv[TAWCROOT_EXEC_STATE_MAX_ARGS + 2];
	for (int i = 0; i < TAWCROOT_EXEC_STATE_MAX_ARGS + 1; i++) argv[i] = "x";
	argv[TAWCROOT_EXEC_STATE_MAX_ARGS + 1] = NULL;
	const char *envp[] = { NULL };
	uint8_t buf[1024];
	test_int_eq((int)tawcroot_exec_state_write(buf, sizeof buf, "/y",
	            TAWCROOT_EXEC_STATE_MAX_ARGS + 1, argv, envp, NULL),
	            -7 /* E2BIG */);
}

test(exec_state_rejects_bad_magic)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);

	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->magic = 0xdeadbeef;

	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, abuf, ebuf, &out),
	            -22);
}

test(exec_state_rejects_bad_version)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);
	((tawcroot_exec_state_header *)buf)->version = 99;
	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, abuf, ebuf, &out),
	            -22);
}

test(exec_state_rejects_truncated_strings)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	long w = tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);

	/* Truncate to just the header. Reader should refuse — claimed
	 * string_bytes won't fit. */
	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf,
	                                          sizeof(tawcroot_exec_state_header),
	                                          abuf, ebuf, &out),
	            -22);
	(void)w;
}

test(exec_state_rejects_offset_past_strings)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->path_off = h->string_bytes + 100;
	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, abuf, ebuf, &out),
	            -22);
}

test(exec_state_carries_shm_table)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };

	const char *shm_names[] = { "moz-ipc-1", "moz-ipc-2", "media-buf" };
	const int   shm_fds[]   = { 1042, 1099, 1130 };

	tawcroot_exec_state_extras ex = { 0 };
	ex.n_shm    = 3;
	ex.shm_name = shm_names;
	ex.shm_fd   = shm_fds;

	size_t need = tawcroot_exec_state_estimate_bytes("/p", 1, argv, envp,
							 &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/p", 1, argv, envp, &ex);
	test_int_eq((int)w, (int)need);

	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, abuf, ebuf, &out),
		    0);
	test_int_eq((int)out.n_shm, 3);
	test_str_eq(out.shm_name[0], "moz-ipc-1");
	test_str_eq(out.shm_name[1], "moz-ipc-2");
	test_str_eq(out.shm_name[2], "media-buf");
	test_int_eq(out.shm_fd[0], 1042);
	test_int_eq(out.shm_fd[1], 1099);
	test_int_eq(out.shm_fd[2], 1130);
	free(buf);
}

test(exec_state_too_many_shm)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };
	const char *names[TAWCROOT_EXEC_STATE_MAX_SHM + 1];
	int fds[TAWCROOT_EXEC_STATE_MAX_SHM + 1];
	for (int i = 0; i < TAWCROOT_EXEC_STATE_MAX_SHM + 1; i++) {
		names[i] = "n";
		fds[i]   = 1000 + i;
	}
	tawcroot_exec_state_extras ex = { 0 };
	ex.n_shm    = TAWCROOT_EXEC_STATE_MAX_SHM + 1;
	ex.shm_name = names;
	ex.shm_fd   = fds;

	uint8_t buf[16 * 1024];
	test_int_eq((int)tawcroot_exec_state_write(buf, sizeof buf, "/p", 1,
						   argv, envp, &ex),
		    -7 /* E2BIG */);
}

test(exec_state_with_many_args_and_env)
{
	enum { NA = 100, NE = 80 };
	const char *argv[NA + 1];
	const char *envp[NE + 1];
	for (int i = 0; i < NA; i++) argv[i] = "argument-with-some-length";
	argv[NA] = NULL;
	for (int i = 0; i < NE; i++) envp[i] = "VAR=value";
	envp[NE] = NULL;

	size_t need = tawcroot_exec_state_estimate_bytes("/path/to/program",
	                                                 NA, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/path/to/program",
	                                           NA, argv, envp, NULL),
	            (int)need);

	const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, abuf, ebuf, &out), 0);
	test_int_eq((int)out.argc, NA);
	test_int_eq((int)out.envc, NE);
	for (int i = 0; i < NA; i++) test_str_eq(out.argv[i], "argument-with-some-length");
	for (int i = 0; i < NE; i++) test_str_eq(out.envp[i], "VAR=value");
	test_ptr_eq(out.argv[NA], NULL);
	test_ptr_eq(out.envp[NE], NULL);
	free(buf);
}

/* A glob-heavy exec (`rm *` over a big dir, a linker invocation) passes
 * hundreds-to-thousands of args. Pre-fix MAX_ARGS was 256, so each such
 * exec E2BIG'd even though the kernel allows ~2 MB of argv. Serialize a
 * near-cap argv + a large env and confirm it round-trips. */
test(exec_state_near_cap_args_roundtrip)
{
	enum { NA = 2000, NE = 500 };
	static const char *argv[NA + 1];
	static const char *envp[NE + 1];
	for (int i = 0; i < NA; i++) argv[i] = "globbed-filename-12345.txt";
	argv[NA] = NULL;
	for (int i = 0; i < NE; i++) envp[i] = "SOME_EXPORTED_VAR=some-value";
	envp[NE] = NULL;

	size_t need = tawcroot_exec_state_estimate_bytes("/usr/bin/rm",
	                                                 NA, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/usr/bin/rm",
	                                           NA, argv, envp, NULL),
	            (int)need);

	static const char *abuf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	static const char *ebuf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, abuf, ebuf, &out), 0);
	test_int_eq((int)out.argc, NA);
	test_int_eq((int)out.envc, NE);
	test_str_eq(out.argv[0], "globbed-filename-12345.txt");
	test_str_eq(out.argv[NA - 1], "globbed-filename-12345.txt");
	test_str_eq(out.envp[NE - 1], "SOME_EXPORTED_VAR=some-value");
	test_ptr_eq(out.argv[NA], NULL);
	free(buf);
}
