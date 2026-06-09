/* In-process exec driver: opens a guest binary, parses it,
 * (recursively for ld.so), maps everything, builds an initial stack,
 * and jumps. Never returns on success.
 *
 * Entry points: the production `-r ROOTFS -- CMD` initial exec, the
 * SIGSYS-handler-triggered `--exec-child <fd>` re-exec (guest
 * execve(2) re-routing), and the testhost-only `--exec <path>`
 * diagnostic mode.
 *
 * On any failure we exit_group with a small numeric status (60..83
 * range) so callers can distinguish "loader failed" from "guest ran
 * and exited N" without conflating exit codes.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shebang resolution limits, shared with the exec handler's pre-commit
 * classifier so both walk #! chains with the same kernel-matching
 * rules. BUF mirrors Linux's BINPRM_BUF_SIZE (256); DEPTH mirrors
 * binfmt_script's default chain cap (4). */
#define TAWC_SHEBANG_MAX_DEPTH 4
#define TAWC_SHEBANG_BUF       256

/* Read and tokenize the "#!" line of `fd` (caller has already checked
 * the magic). `line` (capacity `cap`, typically TAWC_SHEBANG_BUF) holds
 * the tokens; on success *interp points at the interpreter path and
 * *arg at the single optional shebang argument (NULL when absent; pass
 * arg == NULL when the caller doesn't want it). Returns 0 or -ENOEXEC
 * for malformed / overlong lines, matching binfmt_script (kernels
 * >= 5.1 ENOEXEC a line that overflows BINPRM_BUF_SIZE rather than
 * truncating; EOF terminates the line like a newline). */
long tawcroot_shebang_read(int fd, char *line, size_t cap,
                           const char **interp, const char **arg);

/* Production loader I/O vtable, defined in src/loader_io_prod.c. */
extern const struct tawc_loader_io tawcroot_loader_io_prod;

/* Stash kernel-supplied auxv values for forwarding to the synthesized
 * guest stack. Call once per tawcroot incarnation (production main and
 * --exec-child both, since the kernel rebuilds auxv on each execve).
 * Caller must walk past argv and envp to find the auxv array; missing
 * entries should be passed as 0 and will be omitted from the synth.
 *
 * `page_size` is the kernel's AT_PAGESZ. It drives ELF segment parsing,
 * mmap alignment, the stack guard page, and the synthesized AT_PAGESZ —
 * a 16 KiB-page kernel (Android 15+, Pixel emulator images) rejects
 * 4 KiB-aligned file-backed mmaps and lies to the guest's ld.so/malloc
 * if we hardcode 4096. Passed as 0 (or a non-power-of-two) falls back
 * to 4096. */
void tawcroot_loader_set_host_auxv(uint64_t hwcap, uint64_t hwcap2,
                                   uintptr_t sysinfo_ehdr,
                                   uint64_t clktck, uint64_t flags,
                                   uint64_t page_size);

/* Arguments to forward to the guest as argv/envp. Passed directly to
 * the stack synthesizer.  `argv[argc]` must be NULL; `envp` must be
 * NULL-terminated. */
struct tawc_loader_exec_args {
	const char  *guest_path;       /* host-fs absolute path to guest */
	int          argc;
	const char *const *argv;        /* size argc + 1 (NULL term) */
	const char *const *envp;        /* NULL-terminated */
	const char  *platform;         /* AT_PLATFORM (e.g. "x86_64") */
};

/* Open + load + jump. Never returns on success (guest takes over).
 * On failure, calls tawc_exit_group(60..79) directly:
 *
 *   60   open guest failed
 *   61   ehdr read / parse failed
 *   62   phdrs read / parse failed
 *   63   guest is not ET_EXEC or ET_DYN
 *   64   PT_INTERP path read failed
 *   65   open ld.so failed
 *   66   ld.so ehdr/phdrs parse failed
 *   67   ld.so isn't ET_DYN, has its own PT_INTERP, etc
 *   68   guest binary mmap failed
 *   69   ld.so mmap failed
 *   70   stack region mmap / guard-page mprotect failed
 *   71   getrandom for AT_RANDOM failed
 *   72   stack synth failed
 *   73   no usable AT_PHDR address (no PT_PHDR, phdrs outside the
 *        first PT_LOAD)
 *   74   too many guest args for the effective-argv array
 *   75   shebang resolve failed (depth limit, unreadable interpreter,
 *        malformed #! line)
 *
 * Caller-distinguishable from guest exit codes (which are typically
 * 0..127). The 60..79 range is reserved for loader-driver self-failure.
 */
__attribute__((noreturn))
void tawcroot_loader_exec(const struct tawc_loader_exec_args *args);

/* `--exec-child <fd>` mode driver: read a serialized exec_state from
 * `state_fd` (which must be open and readable, typically a memfd) and
 * call `tawcroot_loader_exec` with the parsed args. Never returns on
 * success.
 *
 * Failure exit codes (disjoint from `tawcroot_loader_exec`'s 60..79):
 *
 *   80   state_fd size lookup (lseek) failed or state too small
 *   81   mmap of state_fd failed
 *   82   exec_state header invalid (magic / version / sizes)
 */
__attribute__((noreturn))
void tawcroot_loader_exec_child(int state_fd, const char *platform);

#ifdef __cplusplus
}
#endif
