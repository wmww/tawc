/* End-to-end loader smoke: actually load and run a guest binary
 * through parser → mapper → stack synth → trampoline.
 *
 * Strategy:
 *   1. fork() — so the child can be replaced by the guest without
 *      taking down the test orchestrator.
 *   2. In the child: open static_exit42, parse, map at its absolute
 *      vaddr (ET_EXEC), build a stack on a fresh mmap'd region, jump.
 *      The fixture binary calls exit_group(42).
 *   3. In the parent: waitpid, assert exit code == 42.
 *
 * If any of parser/mapper/stack/trampoline are broken, the child will
 * exit with a different code (or signal) and the test reports clearly.
 *
 * This is the phase 2.4 exit gate from notes/tawcroot.md: until this
 * passes, dynamic-binary support (phase 2.5) doesn't land.
 */

#include <cleat/test.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "loader_elf.h"
#include "loader_jump.h"
#include "loader_map.h"
#include "loader_stack.h"

#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined by the build"
#endif

/* libc-forwarding I/O for the mapper. Same pattern as test_loader_map.c
 * — duplicated here rather than shared because cleat tests are
 * intentionally self-contained per-file. */
static uintptr_t io_mmap(void *ctx, void *addr, size_t len, int prot,
                         int flags, int fd, uint64_t offset)
{
	(void)ctx;
	void *r = mmap(addr, len, prot, flags, fd, (off_t)offset);
	if (r == MAP_FAILED) return (uintptr_t)-(intptr_t)errno;
	return (uintptr_t)r;
}
static long io_mprotect(void *ctx, void *addr, size_t len, int prot)
{ (void)ctx; if (mprotect(addr, len, prot) < 0) return -errno; return 0; }
static long io_munmap(void *ctx, void *addr, size_t len)
{ (void)ctx; if (munmap(addr, len) < 0) return -errno; return 0; }
static long io_pread(void *ctx, int fd, void *buf, size_t n, uint64_t off)
{ (void)ctx; ssize_t r = pread(fd, buf, n, (off_t)off);
  if (r < 0) return -errno; return r; }

static struct tawc_loader_io libc_io = {
	.ctx = nullptr,
	.mmap = io_mmap,
	.mprotect = io_mprotect,
	.munmap = io_munmap,
	.pread = io_pread,
};

struct child_args {
	const char *path;
	int         argc;
	const char *const *argv;
	const char *const *envp;
	const uint8_t *random16;   /* 16 bytes; if NULL we call getrandom */
};

/* Load and jump. Called inside the forked child only — never returns
 * (either the guest runs and exits, or we _exit() with a small code
 * (200..209) to signal a specific failure step in this driver, kept
 * disjoint from the fixture's own [96..227] exit-code range). */
__attribute__((noreturn))
static void child_load_and_jump(const struct child_args *ca)
{
	int fd = open(ca->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) _exit(201);

	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	if (pread(fd, ebuf, sizeof ebuf, 0) != (ssize_t)sizeof ebuf)
		_exit(202);

	struct tawc_loader_image img;
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, &img) != 0) _exit(203);

	uint8_t pbuf[64 * 64];
	size_t pbytes = (size_t)img.e_phnum * img.e_phentsize;
	if (pbytes > sizeof pbuf) _exit(204);
	if (pread(fd, pbuf, pbytes, (off_t)img.e_phoff) != (ssize_t)pbytes)
		_exit(204);
	if (tawc_loader_parse_phdrs(pbuf, sizeof pbuf, 4096, &img) != 0)
		_exit(205);

	struct tawc_loader_placement pl;
	if (tawc_loader_map(&img, fd, /*requested_base*/ 0, 4096,
	                    &libc_io, &pl) != 0) _exit(206);

	close(fd);

	const size_t stack_sz = 256 * 1024;
	void *stack = mmap(NULL, stack_sz, PROT_READ | PROT_WRITE,
	                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) _exit(207);

	uint8_t local_random[16];
	const uint8_t *random16 = ca->random16;
	if (!random16) {
		if (getrandom(local_random, 16, 0) != 16) _exit(208);
		random16 = local_random;
	}

	struct tawc_loader_stack_input in = {
		.argc          = ca->argc,
		.argv          = ca->argv,
		.envp          = ca->envp,
		.at_phdr       = pl.phdr_addr,
		.at_phnum      = pl.phnum,
		.at_phent      = pl.phentsize,
		.at_base       = 0,
		.at_entry      = pl.entry,
		.at_execfn     = ca->path,
		.at_platform   = "x86_64",
		.at_random16   = random16,
		.at_pagesz     = 4096,
		.at_clktck     = (uint64_t)getauxval(AT_CLKTCK),
		.at_hwcap      = (uint64_t)getauxval(AT_HWCAP),
		.at_hwcap2     = (uint64_t)getauxval(AT_HWCAP2),
		.at_sysinfo_ehdr = (uintptr_t)getauxval(AT_SYSINFO_EHDR),
		.at_flags      = (uint64_t)getauxval(AT_FLAGS),
	};

	struct tawc_loader_stack_out so;
	if (tawc_loader_build_stack(stack, stack_sz, &in, &so) != 0)
		_exit(209);

	tawc_loader_jump(so.sp, pl.entry);
}

/* Fork, run the loader-driven child, return the status word. */
static int run_under_loader(const struct child_args *ca)
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		child_load_and_jump(ca);
		/* unreachable */
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) return -2;
	return status;
}

/* Decode wait status into a single int for clean test_int_eq:
 *   exit code N            →  N             (0..255)
 *   killed by signal N     →  -N            (negative, distinct space)
 *   anything else          →  -999          (shouldn't happen)
 *
 * Test sites assert on the encoded value directly. The encoding lets
 * us write `test_int_eq(decode(status), 42)` instead of branching on
 * WIFEXITED/WIFSIGNALED, which keeps cleat's expectation messages
 * useful (it prints both expected and actual side by side). */
static int decode_status(int status)
{
	if (WIFEXITED(status))   return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return -WTERMSIG(status);
	return -999;
}

/* ============================================================ */
/*  smoke tests                                                 */
/* ============================================================ */

test(loader_runs_static_exit42)
{
	/* Minimal fixture: doesn't read its stack at all, just exits 42.
	 * Exercises parser → mapper → trampoline-jump only. */
	const char *argv[] = { TAWCROOT_STATIC_EXIT42_BIN, NULL };
	const char *envp[] = { NULL };
	struct child_args ca = {
		.path = TAWCROOT_STATIC_EXIT42_BIN,
		.argc = 1, .argv = argv, .envp = envp,
	};
	int status = run_under_loader(&ca);
	test_int_eq(decode_status(status), 42);
}

/* Fixture failure-mode key (see static_argc_random.S):
 *   99  argc != 3              (argv plumbing broken)
 *   98  argv[0][0] != '/'      (argv string pointer broken)
 *   97  envp[0][0] != 'X'      (envp plumbing broken)
 *   96  AT_RANDOM not found    (auxv plumbing broken)
 *   200..209  loader-driver self-failure (open/parse/map step)
 *   negative N    killed by signal N   (decode_status encoding)
 *   100..227  success: random16[0] & 0x7f + 100   <- the value we assert
 */

test(loader_runs_static_with_argc_argv_envp_and_known_random)
{
	const char *argv[] = {
		TAWCROOT_STATIC_ARGC_RANDOM_BIN, "alpha", "beta", NULL,
	};
	const char *envp[] = { "X=1", NULL };
	uint8_t det_random[16] = {
		0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xff, 0xee,
		0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66,
	};
	int expected = (det_random[0] & 0x7f) + 100;  /* 0x55 = 85 → 185 */

	struct child_args ca = {
		.path = TAWCROOT_STATIC_ARGC_RANDOM_BIN,
		.argc = 3, .argv = argv, .envp = envp,
		.random16 = det_random,
	};
	int status = run_under_loader(&ca);
	test_int_eq(decode_status(status), expected);
}

test(loader_runs_static_with_different_random)
{
	/* Different deterministic random16 → different exit code. Confirms
	 * the exit really did flow through the AT_RANDOM bytes we supplied,
	 * not e.g. a happens-to-be-185 byte at a stale address. */
	const char *argv[] = {
		TAWCROOT_STATIC_ARGC_RANDOM_BIN, "1", "2", NULL,
	};
	const char *envp[] = { "X=y", NULL };
	uint8_t det_random[16] = {
		0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	int expected = (det_random[0] & 0x7f) + 100;  /* 0x01 → 101 */

	struct child_args ca = {
		.path = TAWCROOT_STATIC_ARGC_RANDOM_BIN,
		.argc = 3, .argv = argv, .envp = envp,
		.random16 = det_random,
	};
	int status = run_under_loader(&ca);
	test_int_eq(decode_status(status), expected);
}
