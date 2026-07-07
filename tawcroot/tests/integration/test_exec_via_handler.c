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

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loader_elf.h"

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

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

/* Write `contents` to a fresh 0755 temp file; returns the path in a
 * static buffer (single-threaded test, one outstanding at a time). */
static const char *make_exec_file(const char *suffix, const char *contents)
{
	static char path[256];
	snprintf(path, sizeof path, "%s/tawcroot-classify-%s",
	         TAWCROOT_TEST_TMPDIR, suffix);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) return NULL;
	size_t n = strlen(contents);
	if (write(fd, contents, n) != (ssize_t)n) { close(fd); return NULL; }
	close(fd);
	return path;
}

test(exec_via_handler_non_elf_text_returns_50)
{
	/* A `chmod +x`'d text file with no shebang: real execve returns
	 * -ENOEXEC (so a shell falls back to `sh file`). Pre-fix the probe
	 * passed it through to execveat and the loader died post-commit
	 * with exit 61. The classification probe must turn it into a clean
	 * -ENOEXEC, reported as exit 50. */
	const char *p = make_exec_file("text", "this is not a program\n");
	test_true(p != NULL);
	const char *args[] = { "--exec-via-handler", p, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(p);
}

test(exec_via_handler_wrong_arch_elf_returns_50)
{
	/* A structurally valid ELF64 for the OTHER machine: real execve
	 * returns -ENOEXEC. Pre-fix, classify_elf checked only e_type, so
	 * the probe passed a cross-arch binary through to the commit and
	 * the loader mapped and jumped into foreign code (SIGILL — a
	 * destroyed caller instead of a shell's `cannot execute binary
	 * file`). Header fields other than e_machine are all valid so
	 * only the machine check can reject it. */
	unsigned char eh[64] = {
		0x7f, 'E', 'L', 'F',
		2,  /* ELFCLASS64 */
		1,  /* ELFDATA2LSB */
		1,  /* EV_CURRENT */
	};
	uint16_t wrong = (TAWC_EM_HOST == TAWC_EM_X86_64)
		? TAWC_EM_AARCH64 : TAWC_EM_X86_64;
	eh[16] = 2;                          /* e_type = ET_EXEC */
	eh[18] = (unsigned char)(wrong & 0xff);      /* e_machine */
	eh[19] = (unsigned char)(wrong >> 8);
	eh[20] = 1;                          /* e_version = EV_CURRENT */
	eh[32] = 64;                         /* e_phoff = sizeof(ehdr) */
	eh[54] = 56;                         /* e_phentsize = sizeof(phdr) */
	eh[56] = 1;                          /* e_phnum = 1 */

	static char path[256];
	snprintf(path, sizeof path, "%s/tawcroot-classify-xarch",
	         TAWCROOT_TEST_TMPDIR);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	test_true(fd >= 0);
	test_true(write(fd, eh, sizeof eh) == (ssize_t)sizeof eh);
	close(fd);

	const char *args[] = { "--exec-via-handler", path, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(path);
}

test(exec_via_handler_shebang_missing_interp_returns_50)
{
	/* A script whose interpreter doesn't exist: real execve returns
	 * -ENOENT. Pre-fix the loader chased the shebang post-commit and
	 * died with exit 75. The probe resolves the shebang and surfaces
	 * the missing-interpreter errno as exit 50. */
	const char *p = make_exec_file("badinterp",
	                               "#!/no/such/interpreter/here\n"
	                               "echo hi\n");
	test_true(p != NULL);
	const char *args[] = { "--exec-via-handler", p, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(p);
}
