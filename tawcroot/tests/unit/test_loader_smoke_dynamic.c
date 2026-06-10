/* Phase 2.5: dynamic-binary smoke. Loads a glibc-linked ET_DYN binary
 * AND its PT_INTERP-pointed ld.so, builds a stack with the right
 * AT_BASE/AT_ENTRY/AT_PHDR for ld.so to take over, and jumps to
 * ld.so. Success = the binary's main() runs to its `return 42`,
 * which means:
 *
 *   - We mapped both binary and ld.so correctly (PT_LOAD geometry).
 *   - We synthesized an auxv ld.so could parse (otherwise it'd
 *     SIGSEGV trying to find program headers, abort because
 *     AT_RANDOM is bad, etc).
 *   - We jumped at the right entry (ld.so's, not the binary's).
 *   - ld.so's mmap calls didn't collide with our existing layout.
 *   - glibc's _start ran main and converted the return into
 *     exit_group(42).
 *
 * If any of those go wrong, the child crashes or exits with a
 * different code. Stack synth + auxv has to be right or ld.so
 * crashes immediately.
 */

#include <cleat/test.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
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

#ifndef TAWCROOT_DYNAMIC_EXIT42_BIN
# error "TAWCROOT_DYNAMIC_EXIT42_BIN must be defined by the build"
#endif

/* libc-forwarding I/O. */
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
	.mmap = io_mmap, .mprotect = io_mprotect,
	.munmap = io_munmap, .pread = io_pread,
};

/* Open + parse a binary's ehdr + phdrs in one go. Returns the open
 * fd on success (caller closes), -1 on any failure. */
static int open_parse(const char *path, struct tawc_loader_image *img)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	if (pread(fd, ebuf, sizeof ebuf, 0) != (ssize_t)sizeof ebuf) {
		close(fd); return -1;
	}
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, img) != 0) {
		close(fd); return -1;
	}
	uint8_t pbuf[64 * 64];
	size_t pbytes = (size_t)img->e_phnum * img->e_phentsize;
	if (pbytes > sizeof pbuf) { close(fd); return -1; }
	if (pread(fd, pbuf, pbytes, (off_t)img->e_phoff) != (ssize_t)pbytes) {
		close(fd); return -1;
	}
	if (tawc_loader_parse_phdrs(pbuf, sizeof pbuf, 4096, img) != 0) {
		close(fd); return -1;
	}
	return fd;
}

/* Walk our own (host) envp and count entries. */
static int count_envp(char *const *envp)
{
	int n = 0;
	while (envp[n]) n++;
	return n;
}

struct dyn_args {
	const char *path;
	int         argc;
	const char *const *argv;
	const char *const *envp;
};

__attribute__((noreturn))
static void child_load_dynamic_and_jump(const struct dyn_args *da)
{
	const char *path = da->path;
	/* --- 1. Open and parse the binary. --- */
	struct tawc_loader_image bin_img;
	int bin_fd = open_parse(path, &bin_img);
	if (bin_fd < 0) _exit(201);
	if (bin_img.e_type != TAWC_ET_DYN) _exit(202);
	if (!bin_img.interp_present) _exit(203);

	/* --- 2. Read PT_INTERP and open ld.so. --- */
	char interp_path[256];
	if (tawc_loader_read_interp(bin_fd, &bin_img, interp_path,
	                            sizeof interp_path, &libc_io) != 0)
		_exit(204);

	struct tawc_loader_image ld_img;
	int ld_fd = open_parse(interp_path, &ld_img);
	if (ld_fd < 0) _exit(205);
	if (ld_img.e_type != TAWC_ET_DYN) _exit(206);
	/* ld.so itself must NOT have a PT_INTERP. */
	if (ld_img.interp_present) _exit(207);

	/* --- 3. Map both. --- */
	struct tawc_loader_placement bin_pl, ld_pl;
	if (tawc_loader_map(&bin_img, bin_fd, 0, 4096, &libc_io, &bin_pl) != 0)
		_exit(208);
	if (tawc_loader_map(&ld_img, ld_fd, 0, 4096, &libc_io, &ld_pl) != 0)
		_exit(209);

	close(bin_fd);
	close(ld_fd);

	/* --- 4. Allocate a fresh stack. --- */
	const size_t stack_sz = 256 * 1024;
	void *stack = mmap(NULL, stack_sz, PROT_READ | PROT_WRITE,
	                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) _exit(210);

	uint8_t random16[16];
	if (getrandom(random16, 16, 0) != 16) _exit(211);

	/* --- 5. Synthesize stack. AT_BASE = ld.so load addr,
	 *       AT_ENTRY = binary's main entry, AT_PHDR = binary phdrs.
	 *       ld.so reads these to take over: it sets itself up, then
	 *       jumps to AT_ENTRY. --- */
	struct tawc_loader_stack_input in = {
		.argc          = da->argc,
		.argv          = da->argv,
		.envp          = da->envp,
		.at_phdr       = bin_pl.phdr_addr,
		.at_phnum      = bin_pl.phnum,
		.at_phent      = bin_pl.phentsize,
		.at_base       = ld_pl.base,
		.at_entry      = bin_pl.entry,
		.at_execfn     = path,
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
		_exit(212);

	/* --- 6. Jump to ld.so's entry point (NOT the binary's). --- */
	tawc_loader_jump(so.sp, ld_pl.entry);
}

static int decode_status(int status)
{
	if (WIFEXITED(status))   return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return -WTERMSIG(status);
	return -999;
}

extern char **environ;

static int run_dynamic(const struct dyn_args *da)
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		child_load_dynamic_and_jump(da);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) return -2;
	return status;
}

test(loader_runs_dynamic_exit42)
{
	const char *argv[] = { TAWCROOT_DYNAMIC_EXIT42_BIN, NULL };
	struct dyn_args da = {
		.path = TAWCROOT_DYNAMIC_EXIT42_BIN,
		.argc = 1, .argv = argv,
		.envp = (const char *const *)environ,
	};
	int status = run_dynamic(&da);
	/* exit code:
	 *   42         the dynamic linking + libc startup + main worked
	 *   201..212   our driver self-failed before the jump
	 *   negative N child died on signal N (loader pipeline broken)
	 *   anything else  glibc startup detected something wrong
	 */
	test_int_eq(decode_status(status), 42);
}

test(loader_runs_system_bin_true)
{
	/* The canonical phase-2 exit gate from notes/tawcroot.md: load
	 * the host's actual /bin/true (a glibc-linked ET_DYN binary built
	 * by coreutils, completely unrelated to our fixtures) and run it.
	 * /bin/true exits 0; if anything in the loader pipeline breaks
	 * the process, we'd see a non-zero status or signal.
	 *
	 * Skip if /bin/true is missing or static (busybox systems). The
	 * fixture-based tests above still cover the dynamic path. */
	if (access("/bin/true", X_OK) != 0) {
		/* Mark passing — this isn't a tawcroot failure, just that
		 * the host doesn't have /bin/true. */
		return;
	}
	const char *argv[] = { "/bin/true", NULL };
	struct dyn_args da = {
		.path = "/bin/true",
		.argc = 1, .argv = argv,
		.envp = (const char *const *)environ,
	};
	int status = run_dynamic(&da);
	test_int_eq(decode_status(status), 0);
}

test(loader_runs_system_bin_ls_dev_null)
{
	/* /bin/ls is a much larger glibc-linked binary than /bin/true:
	 * many DT_NEEDED libs (libc, libcap, libselinux, etc on most
	 * distros), opens directories, formats output. If anything in
	 * our loader synthesis gets a corner wrong that /bin/true
	 * happened to skip, /bin/ls will catch it.
	 *
	 * Run as `ls /dev/null` so we read a stable single-file target
	 * and don't pollute test output with the cwd listing. /dev/null
	 * exists on every Linux system. */
	if (access("/bin/ls", X_OK) != 0) return;
	const char *argv[] = { "/bin/ls", "/dev/null", NULL };
	struct dyn_args da = {
		.path = "/bin/ls",
		.argc = 2, .argv = argv,
		.envp = (const char *const *)environ,
	};
	int status = run_dynamic(&da);
	test_int_eq(decode_status(status), 0);
}

test(loader_runs_dynamic_with_argv_envp_intact)
{
	/* Pass minimal envp — just what dynamic_argv_check looks for plus
	 * the absolute minimum glibc startup needs. Empirically glibc is
	 * happy with no env vars at all (it falls back to defaults), but
	 * keeping a non-empty envp also exercises that envp NULL terminator
	 * placement is correct. */
	const char *argv[] = {
		TAWCROOT_DYNAMIC_ARGV_CHECK_BIN, "alpha", "beta", NULL,
	};
	const char *envp[] = { "TAWC_TEST=ok", NULL };
	struct dyn_args da = {
		.path = TAWCROOT_DYNAMIC_ARGV_CHECK_BIN,
		.argc = 3, .argv = argv, .envp = envp,
	};
	int status = run_dynamic(&da);
	/* exit code:
	 *   42       success — argc, argv[0..2], envp all intact through ld.so
	 *   60..64   guest detected a specific argv/envp mismatch
	 *   201..212 driver self-failed
	 *   negative loader pipeline broken (signal)
	 */
	test_int_eq(decode_status(status), 42);
}
