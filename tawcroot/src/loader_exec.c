/* `--exec` driver: parse → map → stack synth → jump for a guest
 * binary, fully in-process. Production loader pipeline end-to-end.
 *
 * See include/loader_exec.h. Lives in PROD_C (uses raw_sys), not
 * compiled into the cleat tests.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_state.h"
#include "identity.h"
#include "io.h"
#include "loader_elf.h"
#include "loader_exec.h"
#include "loader_jump.h"
#include "loader_map.h"
#include "loader_stack.h"
#include "path.h"
#include "raw_sys.h"
#include "supervisor.h"
#include "tawc_uapi.h"

/* Linux uapi mmap flags / prot. Matches loader_map.h's TAWC_MM_* but
 * we re-use those values directly. */
#define PROT_RW   (TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE)
#define MAP_PA    (TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON)

#define TAWC_LDR_PATH_MAX 4096

#define LOADER_FAIL(code) tawc_exit_group(code)

/* Captured-at-startup auxv values. Populated by tawcroot_main_capture_auxv
 * (called from tawcroot_main right after argv lands) and re-populated by
 * --exec-child the same way (re-exec gives us a fresh kernel auxv).
 *
 * Without this, the synthesized guest auxv leaves AT_HWCAP / AT_HWCAP2 /
 * AT_SYSINFO_EHDR / AT_CLKTCK / AT_FLAGS at zero. glibc tolerates that —
 * but Firefox's xul-side CPU/feature detection treats AT_HWCAP=0 as
 * "no SSE/AVX" on x86_64 and aborts when the hardcoded SIMD code-path
 * gets selected anyway. Mozilla's nsTraceRefcnt stack-canary init also
 * reads AT_RANDOM but that we already pass through. */
static uint64_t  g_host_at_hwcap         = 0;
static uint64_t  g_host_at_hwcap2        = 0;
static uintptr_t g_host_at_sysinfo_ehdr  = 0;
static uint64_t  g_host_at_clktck        = 0;
static uint64_t  g_host_at_flags         = 0;
static size_t    g_host_page_size        = 4096;

/* True iff `v` is a non-zero power of two. */
static int is_pow2(uint64_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

void tawcroot_loader_set_host_auxv(uint64_t hwcap, uint64_t hwcap2,
                                   uintptr_t sysinfo_ehdr,
                                   uint64_t clktck, uint64_t flags,
                                   uint64_t page_size)
{
	g_host_at_hwcap        = hwcap;
	g_host_at_hwcap2       = hwcap2;
	g_host_at_sysinfo_ehdr = sysinfo_ehdr;
	g_host_at_clktck       = clktck;
	g_host_at_flags        = flags;
	/* Fall back to 4 KiB for a missing / nonsensical AT_PAGESZ. */
	g_host_page_size = is_pow2(page_size) ? (size_t)page_size : 4096;
}


/* Stage tag for parse_image so callers can distinguish ehdr vs phdr
 * failure and exit with the correct documented loader code (61 vs 62).
 * Set on failure only; ignored on success. */
enum parse_image_stage {
	PARSE_IMAGE_EHDR = 0,
	PARSE_IMAGE_PHDR = 1,
};

static long parse_image(int fd, struct tawc_loader_image *img,
                        uint8_t *ebuf, size_t ebuf_cap,
                        uint8_t *pbuf, size_t pbuf_cap,
                        size_t page_size,
                        enum parse_image_stage *stage_out)
{
	*stage_out = PARSE_IMAGE_EHDR;
	if (ebuf_cap < sizeof(tawc_elf64_ehdr)) return TAWC_EINVAL;
	long n = tawc_pread64(fd, ebuf, sizeof(tawc_elf64_ehdr), 0);
	if (n != (long)sizeof(tawc_elf64_ehdr)) return TAWC_EINVAL;
	long rc = tawc_loader_parse_ehdr(ebuf, sizeof(tawc_elf64_ehdr), img);
	if (rc) return rc;

	*stage_out = PARSE_IMAGE_PHDR;
	size_t pbytes = (size_t)img->e_phnum * img->e_phentsize;
	if (pbytes > pbuf_cap) return TAWC_EINVAL;
	n = tawc_pread64(fd, pbuf, pbytes, (long)img->e_phoff);
	if (n != (long)pbytes) return TAWC_EINVAL;
	return tawc_loader_parse_phdrs(pbuf, pbuf_cap, page_size, img);
}

long tawcroot_shebang_read(int fd, char *line, size_t cap,
                           const char **interp, const char **arg)
{
	long ln = tawc_pread64(fd, line, cap - 1, 0);
	if (ln < 2) return TAWC_ENOEXEC;
	line[ln] = 0;
	/* Find newline; trim. A full buffer with no newline means the
	 * shebang line exceeds the buffer — the kernel (≥5.1) returns
	 * ENOEXEC rather than truncating the interpreter path mid-token.
	 * (A short read hit EOF, which acts as the line terminator, same
	 * as the kernel.) */
	long eol = 2;
	while (eol < ln && line[eol] != '\n') eol++;
	if (eol == ln && ln == (long)cap - 1) return TAWC_ENOEXEC;
	line[eol] = 0;
	/* Skip "#!" and leading whitespace; the interpreter runs to the
	 * FIRST whitespace. Everything after (trimmed) is a single
	 * argument, regardless of further whitespace — Linux semantics. */
	long i = 2;
	while (i < eol && (line[i] == ' ' || line[i] == '\t')) i++;
	long interp_lo = i;
	while (i < eol && line[i] != ' ' && line[i] != '\t') i++;
	long interp_hi = i;
	if (interp_hi == interp_lo) return TAWC_ENOEXEC;  /* "#!\n" — bad */
	while (i < eol && (line[i] == ' ' || line[i] == '\t')) i++;
	long arg_lo = i;
	long arg_hi = eol;
	while (arg_hi > arg_lo &&
	       (line[arg_hi - 1] == ' ' || line[arg_hi - 1] == '\t'))
		arg_hi--;

	line[interp_hi] = 0;
	*interp = &line[interp_lo];
	if (arg) {
		if (arg_hi > arg_lo) {
			line[arg_hi] = 0;
			*arg = &line[arg_lo];
		} else {
			*arg = 0;
		}
	}
	return 0;
}

/* Resolve a #! shebang chain. Prepends argv entries (the interpreter,
 * optionally a single shebang argument; the original argv[0] becomes
 * the script-path argument) into `argv_out` in place. Returns the
 * resolved fd of the final binary on success (caller takes ownership),
 * or -errno on failure (the working fd is closed).
 *
 * Each shebang line yields at most ONE argv argument after the
 * interpreter path. Linux splits on the FIRST whitespace in the
 * shebang line and treats the rest (up to BINPRM_BUF_SIZE - 2 bytes)
 * as a single argument, regardless of further whitespace. We match
 * that behaviour. */
static long resolve_shebangs(int initial_fd,
                             const char **argv_out,
                             size_t argv_cap,
                             int *argc_out)
{
	int   bin_fd = (int)initial_fd;
	int   depth  = 0;

	for (;;) {
		uint8_t hdr[2];
		long n = tawc_pread64(bin_fd, hdr, 2, 0);
		if (n < 0) { tawc_close(bin_fd); return n; }
		if (n < 2 || !(hdr[0] == '#' && hdr[1] == '!')) {
			return bin_fd;  /* ELF or unknown — let parse_image classify */
		}
		if (depth >= TAWC_SHEBANG_MAX_DEPTH) {
			tawc_close(bin_fd);
			return TAWC_ENOEXEC;
		}

		static char line[TAWC_SHEBANG_BUF];
		const char *interp;
		const char *shebang_arg;
		long pe = tawcroot_shebang_read(bin_fd, line, sizeof line,
		                                &interp, &shebang_arg);
		if (pe < 0) { tawc_close(bin_fd); return pe; }

		/* Duplicate the original script path so we can reuse path_buf
		 * for the interpreter. The script-path string was stored in
		 * argv[0] originally (which still points at it); we use that. */
		const char *script_path = argv_out[0];

		/* Prepend: argv = [interp, [shebang_arg,] script_path, oldargv[1..]].
		 * Static storage so we don't return stack pointers; one slot
		 * deeper per shebang level. */
		static char interp_storage[TAWC_SHEBANG_MAX_DEPTH][TAWC_SHEBANG_BUF];
		static char arg_storage   [TAWC_SHEBANG_MAX_DEPTH][TAWC_SHEBANG_BUF];
		(void)tawc_str_copy(interp_storage[depth],
		                    sizeof interp_storage[0], interp);
		const char *new_argv0 = interp_storage[depth];

		const char *new_argv1 = 0;
		if (shebang_arg) {
			(void)tawc_str_copy(arg_storage[depth],
			                    sizeof arg_storage[0], shebang_arg);
			new_argv1 = arg_storage[depth];
		}

		int extra = shebang_arg ? 2 : 1;
		/* +1: argv_out[*argc_out] gets the NULL terminator below, so
		 * the array needs argc + extra + 1 slots. */
		if ((size_t)(*argc_out + extra + 1) > argv_cap) {
			tawc_close(bin_fd);
			return TAWC_E2BIG;
		}

		/* Shift argv[1..] right by `extra`, place [interp,(arg,)script]
		 * at the front. New argv[0] is interp, then optional shebang
		 * arg, then script path, then original argv[1..]. */
		for (int j = *argc_out; j > 1; j--) {
			argv_out[j + extra - 1] = argv_out[j - 1];
		}
		argv_out[0] = new_argv0;
		if (shebang_arg) {
			argv_out[1] = new_argv1;
			argv_out[2] = script_path;
		} else {
			argv_out[1] = script_path;
		}
		*argc_out += extra;
		argv_out[*argc_out] = 0;

		/* Open the interpreter, close the previous fd, loop. */
		long new_fd = tawcroot_open_in_view(new_argv0);
		if (new_fd < 0) {
			tawc_close(bin_fd);
			return new_fd;
		}
		tawc_close(bin_fd);
		bin_fd = (int)new_fd;
		depth++;
	}
}

void tawcroot_loader_exec(const struct tawc_loader_exec_args *args)
{
	/* Kernel page size (AT_PAGESZ), captured at startup. Drives ELF
	 * segment layout, mmap alignment, the stack guard page, and the
	 * synthesized AT_PAGESZ. Defaults to 4096 when AT_PAGESZ wasn't
	 * captured (legacy --exec path before auxv capture runs). */
	const size_t PAGE = g_host_page_size;

	/* --- 1. Open + parse the guest binary. --- */
	long bin_fd = tawcroot_open_in_view(args->guest_path);
	if (bin_fd < 0) LOADER_FAIL(60);

	/* --- 1.5. Resolve any #! shebang chain into ELF + adjusted argv. ---
	 * Scripts (e.g. pacman-key, gpg wrappers) need the kernel's
	 * binfmt_script behaviour: replace the script path with the
	 * interpreter, prepend the script path as the first arg. We mutate
	 * a working argv array; argv[0] starts as the original guest path
	 * and we rebuild from there. */
	static const char *eff_argv[TAWC_SHEBANG_MAX_DEPTH * 2 + 256];
	int eff_argc = 0;
	if (args->argc > (int)(sizeof eff_argv / sizeof eff_argv[0]) - 1)
		LOADER_FAIL(74);
	/* Seed argv[0] = the original guest path (not the user-provided
	 * argv[0], which can be anything; binfmt_script uses the actual
	 * script path). */
	static char path_storage[TAWC_LDR_PATH_MAX];
	(void)tawc_str_copy(path_storage, sizeof path_storage,
	                    args->guest_path);
	/* argc == 0: synthesize argv[0] from the exec path so a shebang
	 * resolve doesn't lose the script path (the kernel since 5.18
	 * similarly forces argc ≥ 1, with ""). */
	eff_argc = args->argc > 0 ? args->argc : 1;
	eff_argv[0] = path_storage;
	for (int i = 1; i < args->argc; i++) eff_argv[i] = args->argv[i];
	eff_argv[eff_argc] = 0;

	/* NOTE: path_storage must not be mutated after this point. It is
	 * argv[0]'s original value, and after a shebang resolve that
	 * pointer lives on as the script-path argv entry — overwriting it
	 * would silently change the interpreter's argument under us
	 * (exactly what bit pacman-key). AT_EXECFN uses args->guest_path
	 * directly, which is unaffected. */
	long resolved_fd = resolve_shebangs((int)bin_fd, eff_argv,
	                                    sizeof eff_argv / sizeof eff_argv[0],
	                                    &eff_argc);
	if (resolved_fd < 0) LOADER_FAIL(75);
	bin_fd = (int)resolved_fd;

	struct tawc_loader_image bin_img;
	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	uint8_t pbuf[64 * 64];
	enum parse_image_stage stage;
	long rc = parse_image((int)bin_fd, &bin_img, ebuf, sizeof ebuf,
	                      pbuf, sizeof pbuf, PAGE, &stage);
	if (rc) LOADER_FAIL(stage == PARSE_IMAGE_EHDR ? 61 : 62);
	/* Post-commit backstop for the exec handler's pre-probe: never map
	 * and jump into a wrong-machine image (TOCTOU replacement, or a
	 * direct --exec-child invocation that skipped the probe). */
	if (bin_img.e_machine != TAWC_EM_HOST) LOADER_FAIL(63);
	if (bin_img.e_type != TAWC_ET_EXEC && bin_img.e_type != TAWC_ET_DYN)
		LOADER_FAIL(63);

	/* --- 2. Map the binary. --- */
	struct tawc_loader_placement bin_pl;
	if (tawc_loader_map(&bin_img, (int)bin_fd, 0, PAGE,
	                    &tawcroot_loader_io_prod, &bin_pl) != 0)
		LOADER_FAIL(68);
	/* phdr_addr == 0 means compute_phdr_addr couldn't produce a sane
	 * AT_PHDR (no PT_PHDR and phdrs outside the first PT_LOAD) —
	 * refuse to load instead of handing ld.so a NULL AT_PHDR. */
	if (bin_pl.phdr_addr == 0)
		LOADER_FAIL(73);

	/* --- 3. Optional: ld.so for dynamic guests. --- */
	uintptr_t at_base = 0;
	uintptr_t entry   = bin_pl.entry;
	if (bin_img.interp_present) {
		char interp_path[256];
		/* Read PT_INTERP from bin_fd BEFORE closing it. */
		if (tawc_loader_read_interp((int)bin_fd, &bin_img, interp_path,
		                            sizeof interp_path,
		                            &tawcroot_loader_io_prod) != 0)
			LOADER_FAIL(64);
		/* PT_INTERP is a guest-absolute path baked into the ELF
		 * (typically /lib64/ld-linux-x86-64.so.2). In rootfs mode the
		 * file at that host path won't exist (or worse, exists with
		 * a different ABI); route it through the same translator the
		 * binary used. */
		long ld_fd = tawcroot_open_in_view(interp_path);
		if (ld_fd < 0) LOADER_FAIL(65);

		struct tawc_loader_image ld_img;
		enum parse_image_stage ld_stage;
		(void)ld_stage;
		if (parse_image((int)ld_fd, &ld_img, ebuf, sizeof ebuf,
		                pbuf, sizeof pbuf, PAGE, &ld_stage) != 0)
			LOADER_FAIL(66);
		if (ld_img.e_machine != TAWC_EM_HOST ||
		    ld_img.e_type != TAWC_ET_DYN || ld_img.interp_present)
			LOADER_FAIL(67);

		struct tawc_loader_placement ld_pl;
		if (tawc_loader_map(&ld_img, (int)ld_fd, 0, PAGE,
		                    &tawcroot_loader_io_prod, &ld_pl) != 0)
			LOADER_FAIL(69);
		tawc_close((int)ld_fd);

		at_base = ld_pl.base;
		entry   = ld_pl.entry;
	}
	tawc_close((int)bin_fd);

	/* --- 4. Allocate fresh stack. ---
	 *
	 * Match Linux's default RLIMIT_STACK (8 MiB). 256 KiB was enough for
	 * a hello-world but not for real coreutils: GCC's
	 * -fstack-clash-protection has wc_lines/wc_bytes pre-allocate a
	 * 256 KiB read buffer in the stack frame and probe each page on the
	 * way down, which walks straight off the bottom of a 256 KiB region
	 * and SIGSEGVs (SEGV_MAPERR at rsp). MAP_GROWSDOWN is broken on
	 * modern kernels, so we just commit the full 8 MiB up front.
	 * Anonymous pages are demand-zeroed, so the cost is reservation +
	 * page tables — no real RSS until the guest touches them. */
	const size_t STACK_SZ = 8 * 1024 * 1024;
	long stack_rv = tawc_mmap((void *)0, STACK_SZ, PROT_RW, MAP_PA, -1, 0);
	if (tawc_loader_mmap_is_err((uintptr_t)stack_rv)) LOADER_FAIL(70);
	void *stack = (void *)stack_rv;
	/* Guard page at the low end so stack overflow is a deterministic
	 * SIGSEGV instead of a silent walk into whatever mapping the
	 * kernel placed below. */
	if (tawc_mprotect(stack, PAGE, TAWC_MM_PROT_NONE) != 0)
		LOADER_FAIL(70);

	uint8_t random16[16];
	if (tawc_getrandom(random16, 16, 0) != 16) LOADER_FAIL(71);

	struct tawc_loader_stack_input in = {
		/* Use the (possibly shebang-rewritten) effective argv. For
		 * non-shebang ELFs eff_argv == args->argv. */
		.argc          = eff_argc,
		.argv          = eff_argv,
		.envp          = args->envp,
		.at_phdr       = bin_pl.phdr_addr,
		.at_phnum      = bin_pl.phnum,
		.at_phent      = bin_pl.phentsize,
		.at_base       = at_base,
		.at_entry      = bin_pl.entry,
		/* AT_EXECFN is the path the guest "asked to exec" — use the
		 * original guest path, not the shebang-resolved interpreter,
		 * so /proc/self/exe-equivalent semantics match what the guest
		 * intended. */
		.at_execfn     = args->guest_path,
		.at_platform   = args->platform,
		.at_random16   = random16,
		.at_pagesz     = PAGE,
		/* AT_CLKTCK / AT_HWCAP / AT_HWCAP2 / AT_SYSINFO_EHDR /
		 * AT_FLAGS: forwarded from the kernel's auxv to ours.
		 * tawcroot_main_capture_auxv (or the equivalent in
		 * --exec-child) is responsible for stashing them at the
		 * startup of every tawcroot incarnation. Leaving them zero
		 * has bitten us specifically with Firefox: AT_HWCAP=0 on
		 * x86_64 trips xul's CPU-feature gating into a SIMD path
		 * the host can't run, then SIGSEGVs early in startup; and
		 * AT_SYSINFO_EHDR=0 forces clock_gettime through the syscall
		 * trap on every call, which is a measurable perf hit. */
		.at_clktck       = g_host_at_clktck,
		.at_hwcap        = g_host_at_hwcap,
		.at_hwcap2       = g_host_at_hwcap2,
		.at_sysinfo_ehdr = g_host_at_sysinfo_ehdr,
		.at_flags        = g_host_at_flags,
	};

	struct tawc_loader_stack_out so;
	if (tawc_loader_build_stack(stack, STACK_SZ, &in, &so) != 0)
		LOADER_FAIL(72);

	/* --- 5. Jump. --- */
	tawc_loader_jump(so.sp, entry);
}

/* `--exec-child <fd>` driver. Reads a serialized exec_state out of
 * the passed-in fd (typically a memfd opened by the SIGSYS handler
 * before re-execing tawcroot), parses argv/envp/path, then hands off
 * to `tawcroot_loader_exec`. Never returns on success.
 *
 * The state fd is mmap'd rather than read into a buffer so the path
 * / argv / envp pointers we hand to `tawcroot_loader_exec` reference
 * the mmap and stay valid for the lifetime of the load (we never
 * unmap it before the jump). */
void tawcroot_loader_exec_child(int state_fd, const char *platform)
{
	/* lseek SEEK_END is the cheapest "get file size" without dragging
	 * in the whole struct kernel_stat machinery. memfds are seekable. */
	long size = tawc_lseek(state_fd, 0, 2 /* SEEK_END */);
	if (size < 0 || size < (long)sizeof(tawcroot_exec_state_header))
		LOADER_FAIL(80);

	const int PROT_R = TAWC_MM_PROT_READ;
	const int FLAGS  = TAWC_MM_MAP_PRIVATE;
	long mrv = tawc_mmap((void *)0, (size_t)size, PROT_R, FLAGS,
	                     state_fd, 0);
	if (tawc_loader_mmap_is_err((uintptr_t)mrv)) LOADER_FAIL(81);
	const void *buf = (const void *)mrv;

	static const char *argv_buf[TAWCROOT_EXEC_STATE_MAX_ARGS + 1];
	static const char *envp_buf[TAWCROOT_EXEC_STATE_MAX_ENV + 1];
	tawcroot_exec_state st;
	if (tawcroot_exec_state_read(buf, (size_t)size, argv_buf, envp_buf, &st) != 0)
		LOADER_FAIL(82);

	/* Phase 2g: re-establish tawcroot's per-process state in the new
	 * incarnation. The seccomp filter is inherited (kernel state) but
	 * the SIGSYS handler is reset to SIG_DFL by execve; the rootfs
	 * fd and bind table live in our data segment and were wiped by
	 * the exec. Without re-init, the post-jump guest's first path-
	 * bearing syscall would either kill the process (no handler) or
	 * route to the host filesystem (no rootfs view).
	 *
	 * tawcroot_supervisor_init does the bootstrap shared with
	 * prod_rootfs_init; on top of that we just stash the guest exe
	 * path. The seccomp filter and PR_SET_NO_NEW_PRIVS are inherited
	 * via execve from the supervising tawcroot. */
	if (st.rootfs_host) {
		struct tawcroot_supervisor_args sa = {
			.rootfs_host_path = st.rootfs_host,
			.bind_src         = st.bind_src,
			.bind_dst         = st.bind_dst,
			.n_binds          = st.n_binds,
			.store_host_path  = st.store_host,
			.shm_names        = st.shm_name,
			.shm_fds          = st.shm_fd,
			.n_shm            = st.n_shm,
		};
		tawcroot_supervisor_init(&sa);

		if (st.guest_exe) tawcroot_set_guest_exe_path(st.guest_exe);
		else              tawcroot_set_guest_exe_path(st.path);
	}

	/* Restore the pre-exec virtual identity AFTER supervisor_init —
	 * dispatch init inside it resets identity to root defaults, and
	 * a guest that dropped privileges must not resurface as fake
	 * root in the exec'd image. */
	if (st.has_identity) tawcroot_identity_load(&st.identity);

	/* Don't bother closing — we're about to hand control to the guest
	 * and any leftover fd dies on the next execve. */
	(void)tawc_close(state_fd);

	struct tawc_loader_exec_args ea = {
		.guest_path = st.path,
		.argc       = (int)st.argc,
		.argv       = (const char *const *)st.argv,
		.envp       = (const char *const *)st.envp,
		.platform   = platform,
	};
	tawcroot_loader_exec(&ea);
}
