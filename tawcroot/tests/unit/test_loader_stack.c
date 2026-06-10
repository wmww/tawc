/* Unit tests for the initial-stack synthesizer (loader_stack.c).
 *
 * The synth is pure data construction; we test it by:
 *   1. Building a stack into a heap buffer.
 *   2. Walking the result with `tawc_loader_walk_stack` (the same
 *      walker glibc/musl use, in concept).
 *   3. Asserting argc/argv/envp/auxv match what we passed in.
 *   4. Running a host glibc startup-shape check: AT_RANDOM points
 *      at 16 readable bytes, AT_PHDR/PHENT/PHNUM/ENTRY/BASE/EXECFN/
 *      PLATFORM are all present, AT_NULL terminates.
 *
 * The integration test (test_loader_smoke.c, phase 2.4) uses the
 * synth to actually launch a static binary.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "loader_stack.h"

/* Allocate a stack region the test can write to. We use mmap so we
 * can also set guard pages later if needed; a malloc'd buffer would
 * work too. */
static uint8_t *alloc_region(size_t n)
{
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
}

/* Find an auxv entry by type. Returns 1 if found, sets *out. */
static int aux_find(const struct tawc_loader_aux_entry *auxv,
                    uint64_t type, uint64_t *out)
{
	for (const struct tawc_loader_aux_entry *p = auxv;
	     p->a_type != TAWC_AT_NULL; p++) {
		if (p->a_type == type) {
			if (out) *out = p->a_val;
			return 1;
		}
	}
	return 0;
}

static const uint8_t fake_random[16] = {
	0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
	0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
};

static struct tawc_loader_stack_input make_input(int argc,
                                                 const char *const *argv,
                                                 const char *const *envp)
{
	struct tawc_loader_stack_input in = {
		.argc        = argc,
		.argv        = argv,
		.envp        = envp,
		.at_phdr     = 0x40123456,
		.at_phnum    = 11,
		.at_phent    = 56,
		.at_base     = 0x7f0000000000ull,
		.at_entry    = 0x40400000,
		.at_execfn   = "/usr/bin/test-exe",
		.at_platform = "x86_64",
		.at_random16 = fake_random,
		.at_pagesz   = 4096,
		.at_clktck   = 100,
		.at_hwcap    = 0xbfebfbff,
		.at_hwcap2   = 0x2,
		.at_sysinfo_ehdr = 0x7ffff7ffd000ull,
		.at_flags    = 0,
	};
	return in;
}

/* ============================================================ */
/*  basic build + walk                                          */
/* ============================================================ */

test(stack_basic_argc_argv_envp_roundtrip)
{
	const char *argv[] = { "prog", "arg1", "arg2", NULL };
	const char *envp[] = { "PATH=/bin", "HOME=/root", NULL };
	struct tawc_loader_stack_input in = make_input(3, argv, envp);

	uint8_t *region = alloc_region(64 * 1024);
	test_nonnull(region);

	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 64 * 1024, &in, &out), 0);
	test_true((out.sp & 15) == 0);                  /* 16-aligned SP */
	test_true(out.sp >= (uintptr_t)region);
	test_true(out.sp < (uintptr_t)region + 64 * 1024);

	int got_argc;
	char *const *got_argv;
	char *const *got_envp;
	const struct tawc_loader_aux_entry *got_auxv;
	tawc_loader_walk_stack(out.sp, &got_argc, &got_argv, &got_envp, &got_auxv);

	test_int_eq(got_argc, 3);
	test_str_eq(got_argv[0], "prog");
	test_str_eq(got_argv[1], "arg1");
	test_str_eq(got_argv[2], "arg2");
	test_ptr_eq(got_argv[3], NULL);

	test_str_eq(got_envp[0], "PATH=/bin");
	test_str_eq(got_envp[1], "HOME=/root");
	test_ptr_eq(got_envp[2], NULL);

	munmap(region, 64 * 1024);
}

test(stack_zero_argc)
{
	const char *argv[] = { NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(0, argv, envp);

	uint8_t *region = alloc_region(8 * 1024);
	test_nonnull(region);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	int got_argc;
	char *const *got_argv;
	tawc_loader_walk_stack(out.sp, &got_argc, &got_argv, NULL, NULL);
	test_int_eq(got_argc, 0);
	test_ptr_eq(got_argv[0], NULL);
	munmap(region, 8 * 1024);
}

/* ============================================================ */
/*  auxv contents                                               */
/* ============================================================ */

test(stack_auxv_required_entries_present)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);

	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	const struct tawc_loader_aux_entry *auxv;
	tawc_loader_walk_stack(out.sp, NULL, NULL, NULL, &auxv);

	uint64_t v;
	/* Required entries from the input. */
	test_true(aux_find(auxv, TAWC_AT_PHDR, &v));
	test_int_eq((int)v, 0x40123456);
	test_true(aux_find(auxv, TAWC_AT_PHNUM, &v));
	test_int_eq((int)v, 11);
	test_true(aux_find(auxv, TAWC_AT_PHENT, &v));
	test_int_eq((int)v, 56);
	test_true(aux_find(auxv, TAWC_AT_ENTRY, &v));
	test_int_eq((int)v, 0x40400000);
	test_true(aux_find(auxv, TAWC_AT_PAGESZ, &v));
	test_int_eq((int)v, 4096);
	test_true(aux_find(auxv, TAWC_AT_BASE, &v));
	test_true(aux_find(auxv, TAWC_AT_RANDOM, &v));
	test_true(aux_find(auxv, TAWC_AT_EXECFN, &v));
	test_true(aux_find(auxv, TAWC_AT_PLATFORM, &v));

	/* Fake-root identity. */
	test_true(aux_find(auxv, TAWC_AT_UID, &v));
	test_int_eq((int)v, 0);
	test_true(aux_find(auxv, TAWC_AT_EUID, &v));
	test_int_eq((int)v, 0);
	test_true(aux_find(auxv, TAWC_AT_GID, &v));
	test_int_eq((int)v, 0);
	test_true(aux_find(auxv, TAWC_AT_EGID, &v));
	test_int_eq((int)v, 0);
	test_true(aux_find(auxv, TAWC_AT_SECURE, &v));
	test_int_eq((int)v, 0);

	/* Optional entries (set in our input). */
	test_true(aux_find(auxv, TAWC_AT_HWCAP, &v));
	test_true(aux_find(auxv, TAWC_AT_HWCAP2, &v));
	test_true(aux_find(auxv, TAWC_AT_CLKTCK, &v));
	test_true(aux_find(auxv, TAWC_AT_SYSINFO_EHDR, &v));

	munmap(region, 8 * 1024);
}

test(stack_auxv_optional_omitted_when_zero)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);
	in.at_clktck = 0;
	in.at_hwcap  = 0;
	in.at_hwcap2 = 0;
	in.at_sysinfo_ehdr = 0;

	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	const struct tawc_loader_aux_entry *auxv;
	tawc_loader_walk_stack(out.sp, NULL, NULL, NULL, &auxv);

	test_false(aux_find(auxv, TAWC_AT_CLKTCK, NULL));
	test_false(aux_find(auxv, TAWC_AT_HWCAP, NULL));
	test_false(aux_find(auxv, TAWC_AT_HWCAP2, NULL));
	test_false(aux_find(auxv, TAWC_AT_SYSINFO_EHDR, NULL));

	munmap(region, 8 * 1024);
}

test(stack_at_random_points_at_16_bytes_we_supplied)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);

	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	const struct tawc_loader_aux_entry *auxv;
	tawc_loader_walk_stack(out.sp, NULL, NULL, NULL, &auxv);

	uint64_t random_addr;
	test_true(aux_find(auxv, TAWC_AT_RANDOM, &random_addr));
	const uint8_t *p = (const uint8_t *)(uintptr_t)random_addr;
	for (int i = 0; i < 16; i++)
		test_int_eq(p[i], fake_random[i]);

	munmap(region, 8 * 1024);
}

test(stack_at_execfn_and_platform_are_strings_we_supplied)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);

	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	const struct tawc_loader_aux_entry *auxv;
	tawc_loader_walk_stack(out.sp, NULL, NULL, NULL, &auxv);

	uint64_t v;
	test_true(aux_find(auxv, TAWC_AT_EXECFN, &v));
	test_str_eq((const char *)(uintptr_t)v, "/usr/bin/test-exe");
	test_true(aux_find(auxv, TAWC_AT_PLATFORM, &v));
	test_str_eq((const char *)(uintptr_t)v, "x86_64");

	munmap(region, 8 * 1024);
}

test(stack_auxv_terminator_is_at_null_zero)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);

	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), 0);

	const struct tawc_loader_aux_entry *auxv;
	tawc_loader_walk_stack(out.sp, NULL, NULL, NULL, &auxv);

	/* Walk to the terminator and confirm it's {AT_NULL, 0}. */
	const struct tawc_loader_aux_entry *p = auxv;
	while (p->a_type != TAWC_AT_NULL) p++;
	test_int_eq((int)p->a_val, 0);

	munmap(region, 8 * 1024);
}

/* ============================================================ */
/*  errors                                                      */
/* ============================================================ */

test(stack_rejects_null_inputs)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);
	struct tawc_loader_stack_out out;

	test_int_eq((int)tawc_loader_build_stack(NULL, 1024, &in, &out), -22);
	test_int_eq((int)tawc_loader_build_stack((void *)0x1000, 1024, NULL, &out), -22);
	test_int_eq((int)tawc_loader_build_stack((void *)0x1000, 1024, &in, NULL), -22);
}

test(stack_rejects_missing_at_random)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);
	in.at_random16 = NULL;
	uint8_t *region = alloc_region(8 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 8 * 1024, &in, &out), -22);
	munmap(region, 8 * 1024);
}

test(stack_returns_enomem_on_tiny_region)
{
	const char *argv[] = { "longishargumentname", NULL };
	const char *envp[] = { "PATH=lots of long environment data", NULL };
	struct tawc_loader_stack_input in = make_input(1, argv, envp);

	uint8_t *region = alloc_region(64);   /* tiny */
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 64, &in, &out), -12);
	munmap(region, 64);
}

test(stack_with_many_args)
{
	/* Stress: 200 args, 200 envs. Confirms layout + alignment hold up. */
	enum { N = 200 };
	const char *argv[N + 1];
	const char *envp[N + 1];
	for (int i = 0; i < N; i++) {
		argv[i] = "argument-with-some-length";
		envp[i] = "VAR=value";
	}
	argv[N] = NULL;
	envp[N] = NULL;

	struct tawc_loader_stack_input in = make_input(N, argv, envp);

	uint8_t *region = alloc_region(64 * 1024);
	struct tawc_loader_stack_out out;
	test_int_eq((int)tawc_loader_build_stack(region, 64 * 1024, &in, &out), 0);
	test_true((out.sp & 15) == 0);

	int got_argc;
	char *const *got_argv;
	char *const *got_envp;
	tawc_loader_walk_stack(out.sp, &got_argc, &got_argv, &got_envp, NULL);
	test_int_eq(got_argc, N);
	for (int i = 0; i < N; i++) {
		test_str_eq(got_argv[i], "argument-with-some-length");
		test_str_eq(got_envp[i], "VAR=value");
	}
	test_ptr_eq(got_argv[N], NULL);
	test_ptr_eq(got_envp[N], NULL);

	munmap(region, 64 * 1024);
}
