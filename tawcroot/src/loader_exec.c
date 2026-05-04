/* `--exec` driver: parse → map → stack synth → jump for a guest
 * binary, fully in-process. Production loader pipeline end-to-end.
 *
 * See include/loader_exec.h. Lives in PROD_C (uses raw_sys), not
 * compiled into the cleat tests.
 */

#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "exec_state.h"
#include "fdtab.h"
#include "filter.h"
#include "handler.h"
#include "io.h"
#include "loader_elf.h"
#include "loader_exec.h"
#include "loader_jump.h"
#include "loader_map.h"
#include "loader_stack.h"
#include "path.h"
#include "raw_sys.h"
#include "shm.h"
#include "usercopy.h"

/* Linux uapi mmap flags / prot. Matches loader_map.h's TAWC_MM_* but
 * we re-use those values directly. */
#define PROT_RW   (TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE)
#define MAP_PA    (TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON)

/* O_DIRECTORY / O_NOFOLLOW differ between aarch64 and x86_64 — pull from
 * the kernel's per-arch header rather than hand-pinning (also defines
 * O_RDONLY / AT_FDCWD). */
#include <linux/fcntl.h>


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

void tawcroot_loader_set_host_auxv(uint64_t hwcap, uint64_t hwcap2,
                                   uintptr_t sysinfo_ehdr,
                                   uint64_t clktck, uint64_t flags)
{
	g_host_at_hwcap        = hwcap;
	g_host_at_hwcap2       = hwcap2;
	g_host_at_sysinfo_ehdr = sysinfo_ehdr;
	g_host_at_clktck       = clktck;
	g_host_at_flags        = flags;
}

/* Open a guest path against the rootfs view (when configured) or the
 * host filesystem (when not). The `--exec` legacy diagnostic path runs
 * before any rootfs is set up, so it falls through to AT_FDCWD/host;
 * production `-r ROOTFS -- CMD` has tawcroot_rootfs_fd set and routes
 * through tawcroot_path_translate. */
static long open_in_view(const char *guest_path, char *suffix_buf,
                         size_t suffix_cap)
{
	if (tawcroot_rootfs_fd < 0) {
		return tawc_openat(AT_FDCWD, guest_path,
		                   O_RDONLY | O_CLOEXEC, 0);
	}
	tawcroot_path_result r = tawcroot_path_translate(
	    guest_path, suffix_buf, suffix_cap, TAWCROOT_PATH_FOLLOW);
	if (r.err) return r.err;
	if (suffix_buf[0] == 0) return -21; /* -EISDIR — exec'ing the rootfs root */
	return tawc_openat(r.base_fd, suffix_buf,
	                   O_RDONLY | O_CLOEXEC, 0);
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
	if (ebuf_cap < sizeof(tawc_elf64_ehdr)) return -22;
	long n = tawc_pread64(fd, ebuf, sizeof(tawc_elf64_ehdr), 0);
	if (n != (long)sizeof(tawc_elf64_ehdr)) return -22;
	long rc = tawc_loader_parse_ehdr(ebuf, sizeof(tawc_elf64_ehdr), img);
	if (rc) return rc;

	*stage_out = PARSE_IMAGE_PHDR;
	size_t pbytes = (size_t)img->e_phnum * img->e_phentsize;
	if (pbytes > pbuf_cap) return -22;
	n = tawc_pread64(fd, pbuf, pbytes, (long)img->e_phoff);
	if (n != (long)pbytes) return -22;
	return tawc_loader_parse_phdrs(pbuf, pbuf_cap, page_size, img);
}

/* Maximum number of `#!` shebang levels we'll resolve before bailing.
 * Linux's binfmt_script.c caps at 4 by default. */
#define TAWC_SHEBANG_MAX_DEPTH 4
/* Linux BINPRM_BUF_SIZE is currently 256 bytes. We can use the same. */
#define TAWC_SHEBANG_BUF 256

/* Resolve a #! shebang chain. Mutates `path_out` (replacing with the
 * interpreter path) and prepends one or two argv entries (the interp,
 * optionally a single shebang argument, and the script path) into
 * `argv_storage` / `argv_out`. Returns the resolved fd of the final
 * binary on success (caller takes ownership), or -errno on failure.
 *
 * Each shebang line yields at most ONE argv argument after the
 * interpreter path. Linux splits on the FIRST whitespace in the
 * shebang line and treats the rest (up to BINPRM_BUF_SIZE - 2 bytes)
 * as a single argument, regardless of further whitespace. We match
 * that behaviour. */
static long resolve_shebangs(int initial_fd,
                             char *path_buf, size_t path_cap,
                             const char *path_suffix_storage,
                             const char **argv_out,
                             size_t argv_cap,
                             int *argc_out)
{
	int   bin_fd = (int)initial_fd;
	int   depth  = 0;
	(void)path_suffix_storage;
	(void)path_buf; (void)path_cap;

	for (;;) {
		uint8_t hdr[2];
		long n = tawc_pread64(bin_fd, hdr, 2, 0);
		if (n < 0) return n;
		if (n < 2 || !(hdr[0] == '#' && hdr[1] == '!')) {
			return bin_fd;  /* ELF or unknown — let parse_image classify */
		}
		if (depth >= TAWC_SHEBANG_MAX_DEPTH) return -8 /*ENOEXEC*/;

		static char line[TAWC_SHEBANG_BUF];
		long ln = tawc_pread64(bin_fd, line, sizeof line - 1, 0);
		if (ln < 2) return -8;
		line[ln] = 0;
		/* Find newline; trim. */
		long eol = 2;
		while (eol < ln && line[eol] != '\n') eol++;
		line[eol] = 0;
		/* Skip "#!" and leading whitespace. */
		long i = 2;
		while (i < eol && (line[i] == ' ' || line[i] == '\t')) i++;
		long interp_lo = i;
		while (i < eol && line[i] != ' ' && line[i] != '\t') i++;
		long interp_hi = i;
		while (i < eol && (line[i] == ' ' || line[i] == '\t')) i++;
		long arg_lo = i;
		long arg_hi = eol;
		/* Trim trailing whitespace from argument. */
		while (arg_hi > arg_lo &&
		       (line[arg_hi - 1] == ' ' || line[arg_hi - 1] == '\t'))
			arg_hi--;

		if (interp_hi == interp_lo) return -8;  /* "#!\n" — bad */
		line[interp_hi] = 0;
		const char *interp = &line[interp_lo];

		const char *shebang_arg = (arg_hi > arg_lo) ? &line[arg_lo] : 0;
		if (shebang_arg) line[arg_hi] = 0;

		/* Duplicate the original script path so we can reuse path_buf
		 * for the interpreter. The script-path string was stored in
		 * argv[0] originally (which still points at it); we use that. */
		const char *script_path = argv_out[0];

		/* Prepend: argv = [interp, [shebang_arg,] script_path, oldargv[1..]].
		 * Static storage so we don't return stack pointers; one slot
		 * deeper per shebang level. */
		static char interp_storage[TAWC_SHEBANG_MAX_DEPTH][TAWC_SHEBANG_BUF];
		static char arg_storage   [TAWC_SHEBANG_MAX_DEPTH][TAWC_SHEBANG_BUF];
		size_t interp_len = 0;
		while (interp[interp_len] && interp_len + 1 < sizeof interp_storage[0]) {
			interp_storage[depth][interp_len] = interp[interp_len];
			interp_len++;
		}
		interp_storage[depth][interp_len] = 0;
		const char *new_argv0 = interp_storage[depth];

		const char *new_argv1 = 0;
		if (shebang_arg) {
			size_t al = 0;
			while (shebang_arg[al] && al + 1 < sizeof arg_storage[0]) {
				arg_storage[depth][al] = shebang_arg[al]; al++;
			}
			arg_storage[depth][al] = 0;
			new_argv1 = arg_storage[depth];
		}

		int extra = shebang_arg ? 2 : 1;
		if ((size_t)(*argc_out + extra) > argv_cap) return -7 /*E2BIG*/;

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
		static char interp_suffix[TAWC_LDR_PATH_MAX];
		long new_fd = open_in_view(new_argv0, interp_suffix,
		                           sizeof interp_suffix);
		if (new_fd < 0) {
			tawc_close(bin_fd);
			return new_fd;
		}
		tawc_close(bin_fd);
		bin_fd = (int)new_fd;
		depth++;

		/* DO NOT mutate path_buf here. The caller's path_storage
		 * is shared with argv_out[0]'s ORIGINAL value, and that
		 * pointer is now living in argv_out[1] (our captured
		 * script_path). Overwriting path_buf would silently change
		 * argv[1] to the interpreter's path under us — exactly
		 * what bit pacman-key. AT_EXECFN uses args->guest_path
		 * directly, which is unaffected. */
	}
}

void tawcroot_loader_exec(const struct tawc_loader_exec_args *args)
{
	const size_t PAGE = 4096;

	/* Buffer for the translated path suffix when rootfs mode is active.
	 * Sized for the worst-case canonicalized suffix (PATH_MAX). Static
	 * to avoid bloating the loader's stack frame; this function never
	 * recurses and the buffer is unused after the openat. */
	static char bin_suffix[TAWC_LDR_PATH_MAX];
	static char ld_suffix[TAWC_LDR_PATH_MAX];

	/* --- 1. Open + parse the guest binary. --- */
	long bin_fd = open_in_view(args->guest_path, bin_suffix,
	                           sizeof bin_suffix);
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
	{
		size_t k = 0;
		while (args->guest_path[k] && k + 1 < sizeof path_storage) {
			path_storage[k] = args->guest_path[k]; k++;
		}
		path_storage[k] = 0;
	}
	eff_argv[0] = path_storage;
	for (int i = 1; i < args->argc; i++) eff_argv[i] = args->argv[i];
	eff_argv[args->argc] = 0;
	eff_argc = args->argc;

	long resolved_fd = resolve_shebangs((int)bin_fd, path_storage,
	                                    sizeof path_storage,
	                                    bin_suffix, eff_argv,
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
	if (bin_img.e_type != TAWC_ET_EXEC && bin_img.e_type != TAWC_ET_DYN)
		LOADER_FAIL(63);

	/* --- 2. Map the binary. --- */
	struct tawc_loader_placement bin_pl;
	if (tawc_loader_map(&bin_img, (int)bin_fd, 0, PAGE,
	                    &tawcroot_loader_io_prod, &bin_pl) != 0)
		LOADER_FAIL(68);

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
		long ld_fd = open_in_view(interp_path, ld_suffix,
		                          sizeof ld_suffix);
		if (ld_fd < 0) LOADER_FAIL(65);

		struct tawc_loader_image ld_img;
		enum parse_image_stage ld_stage;
		(void)ld_stage;
		if (parse_image((int)ld_fd, &ld_img, ebuf, sizeof ebuf,
		                pbuf, sizeof pbuf, PAGE, &ld_stage) != 0)
			LOADER_FAIL(66);
		if (ld_img.e_type != TAWC_ET_DYN || ld_img.interp_present)
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
	 * the SIGSYS handler is reset to SIG_DFL by execve; the rootfs fd
	 * and bind table live in our data segment and were wiped by the
	 * exec. Without re-init, the post-jump guest's first path-bearing
	 * syscall would either kill the process (no handler, default SIGSYS
	 * action is termination) or route to the host filesystem
	 * (no rootfs view).
	 *
	 * Bootstrap order (must use only stub-allowlisted raw syscalls
	 * until the handler is reinstalled):
	 *   1. open rootfs O_PATH | O_DIRECTORY, reserve into high range
	 *   2. set rootfs_host_path (for getcwd reverse-translation)
	 *   3. usercopy_init (probes process_vm_readv via stub)
	 *   4. add binds (raw_sys-only)
	 *   5. memoize well-known + probe openat2
	 *   6. dispatch_init
	 *   7. install SIGSYS handler (stub-rt_sigaction)
	 *   8. (filter is already inherited; do NOT install a new one —
	 *      see notes/tawcroot.md "Why non-PIE")
	 *   9. stash guest_exe_path
	 */
	if (st.rootfs_host) {
		long rfd = tawc_openat(-100 /*AT_FDCWD*/, st.rootfs_host,
		                       O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
		if (rfd < 0) LOADER_FAIL(83);
		long resv = tawcroot_fd_reserve((int)rfd);
		if (resv < 0) LOADER_FAIL(84);
		tawcroot_rootfs_fd = (int)resv;

		size_t k = 0;
		while (st.rootfs_host[k] &&
		       k + 1 < sizeof tawcroot_rootfs_host_path) {
			tawcroot_rootfs_host_path[k] = st.rootfs_host[k];
			k++;
		}
		while (k > 1 && tawcroot_rootfs_host_path[k - 1] == '/') k--;
		tawcroot_rootfs_host_path[k] = 0;
		tawcroot_rootfs_host_path_len = k;

		long up = tawc_usercopy_init();
		if (up < 0) LOADER_FAIL(85);

		for (uint32_t i = 0; i < st.n_binds; i++) {
			if (!st.bind_src[i] || !st.bind_dst[i]) continue;
			long br = tawcroot_path_add_bind(st.bind_src[i],
			                                 st.bind_dst[i]);
			if (br < 0) LOADER_FAIL(86);
		}

		/* Re-register the /dev/shm name table. The internal memfds
		 * are non-CLOEXEC and inherited verbatim across the
		 * execveat — fd numbers stay valid, we just rebuild the
		 * (name → fd) map and re-add fds to the reserved list so
		 * close-trapping protects them. */
		tawcroot_shm_reset();
		for (uint32_t i = 0; i < st.n_shm; i++) {
			if (!st.shm_name[i]) continue;
			long sr = tawcroot_shm_register(st.shm_name[i],
							st.shm_fd[i]);
			if (sr < 0) LOADER_FAIL(89);
		}

		tawcroot_path_memoize_well_known();
		tawcroot_dispatch_init();

		/* Install handler BEFORE probe_openat2: Android's untrusted_app
		 * stacked filter RET_TRAPs openat2 (NR 437) on recent platform
		 * versions. Without our handler in place, that trap falls
		 * through to default disposition and kills the post-re-exec
		 * tawcroot before the guest even loads. With the handler
		 * installed, the unknown-trap path returns -ENOSYS and the
		 * probe correctly concludes "openat2 unavailable, fall back
		 * to manual canonicalization". Mirror of the same ordering
		 * fix in prod_rootfs_init (main.c). */
		long inst = tawcroot_install_handler();
		if (inst != 0) LOADER_FAIL(87);

		/* Reset the inherited signal mask to empty. Same rationale as
		 * the SIG_SETMASK in main.c (start the guest with a fresh mask
		 * regardless of caller state), with one extra wrinkle here:
		 * the SIGSYS handler that just re-exec'd us was running with
		 * SIGSYS auto-masked, and the execveat from the handler
		 * inherits that bit into the new incarnation. Bash and other
		 * shells additionally block SIGSYS before fork+execve. With
		 * SIGSYS blocked, the kernel kills the process by default
		 * instead of invoking our handler. Diagnosed empirically: bash
		 * exec dance left mask=0xc0000000 (bit 30 = SIGSYS) in the new
		 * --exec-child process.
		 *
		 * Must run BEFORE probe_openat2 since that probe trips
		 * Android's stacked filter and needs the SIGSYS handler
		 * invocable. The runtime guest-mask shadow in
		 * syscalls_control.c tracks subsequent guest manipulation. */
		{
			uint64_t empty = 0;
			(void)TAWC_RAW(TAWC_SYS_rt_sigprocmask, 2 /*SIG_SETMASK*/,
			               (long)&empty, 0, 8, 0, 0);
		}

		tawcroot_path_probe_openat2();

		if (st.guest_exe) tawcroot_set_guest_exe_path(st.guest_exe);
		else              tawcroot_set_guest_exe_path(st.path);
	}

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
