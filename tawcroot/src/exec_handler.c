/* SIGSYS-handler-side execve interception. See include/exec_handler.h.
 *
 * Async-signal-safe; uses raw_sys.h for every syscall. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "errno_neg.h"
#include "exec_handler.h"
#include "exec_state.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "loader_elf.h"
#include "loader_exec.h"
#include "loader_map.h"
#include "path.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "shm.h"
#include "tawc_uapi.h"


/* Proctitle staging buffer: path (16 KB) + space-joined argv (64 KB) +
 * shebang-expansion slack. Matches proctitle.c's bounce sizing. */
#define EXEC_TITLE_BUF_SIZE  ((16 + 64 + 4) * 1024)

/* Cap on serialized exec_state size we'll write into a memfd. Sized to
 * hold the full header (offset arrays for MAX_ARGS args + MAX_ENV envs)
 * plus everything the collection layer (syscalls_exec.c) accepts: 16 KB
 * path + 64 KB argv + 256 KB envp, with slack, plus the proctitle
 * string. Previously this was 64 KB
 * total, so any argv+envp over ~61 KB made the write -ENOSPC, which the
 * guest saw as a nonsensical execve()==ENOSPC for exactly the busy
 * environments the collection layer was sized for. BSS, not stack. */
#define EXEC_STATE_BUF_SIZE                                            \
	(sizeof(tawcroot_exec_state_header) +                         \
	 (16 * 1024) + (64 * 1024) + (256 * 1024) +                    \
	 EXEC_TITLE_BUF_SIZE + 8192)

/* Validate an exec-probe fd the way execve(2) would validate the file:
 * directories are EISDIR, non-regular files and files with no execute
 * bit at all are EACCES. The O_RDONLY open alone passes for mode-644
 * files and directories, and by the time the loader discovers the
 * problem (post-execveat) the calling program is already gone — `bash
 * -c /etc` must get a clean error, not a destroyed shell. */
static long probe_check_executable(int fd)
{
	/* Only mode is needed; statx would require Linux 4.11. */
	struct stat st;
	long sr = TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0);
	if (sr < 0) return sr;
	if (S_ISDIR(st.st_mode)) return TAWC_EISDIR;
	if (!S_ISREG(st.st_mode)) return TAWC_EACCES;
	if ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
		return TAWC_EACCES;
	return 0;
}


/* Require the file at `fd` to parse as an ET_EXEC/ET_DYN ELF for this
 * machine. A wrong-arch / malformed / too-short file is -ENOEXEC, which
 * is what the kernel returns and what makes a shell fall back to `sh
 * file`. */
static long classify_elf(int fd)
{
	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	long n = tawc_pread64(fd, ebuf, sizeof ebuf, 0);
	if (n != (long)sizeof ebuf) return TAWC_ENOEXEC;
	/* Static: ~900 bytes, and this sits at the bottom of the recursive
	 * shebang chain on the guest stack. Caller holds exec_lock. */
	static struct tawc_loader_image img;
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, &img) != 0)
		return TAWC_ENOEXEC;  /* bad magic / class / unknown machine */
	/* The parser accepts both supported machines (its unit tests are
	 * cross-arch); the RUN decision is ours. Without this, an x86_64
	 * binary in an aarch64 rootfs passes the probe and the loader
	 * jumps into foreign code post-commit — SIGILL instead of the
	 * kernel-shaped ENOEXEC a shell needs for its fallback. */
	if (img.e_machine != TAWC_EM_HOST) return TAWC_ENOEXEC;
	if (img.e_type != TAWC_ET_EXEC && img.e_type != TAWC_ET_DYN)
		return TAWC_ENOEXEC;
	return 0;
}

/* Classify whether the open file is something the loader can actually
 * run, BEFORE the execveat commit point. Without this, problems the
 * loader only discovers post-commit (a `chmod +x`'d text file, a
 * shebang whose interpreter is missing, a wrong-arch ELF) kill the
 * calling program with a loader exit code instead of returning a clean
 * execve errno. Resolves the shebang chain the same way the kernel and
 * loader_exec.c do; the final file must be a valid ELF.
 *
 * Note: a narrow TOCTOU window remains (the file could be replaced
 * between this probe and the child's open), same as for the existing
 * executable-bit probe — but that window is far narrower than the
 * always-dies-post-commit behaviour it replaces.
 *
 * `title_extra` accumulates the bytes the loader's shebang resolution
 * will PREPEND to argv (interpreter path + optional shebang arg, per
 * level), so the proctitle can reserve arg-region space for them. */
static long classify_loadable(int fd, int depth, size_t *title_extra)
{
	if (depth > TAWC_SHEBANG_MAX_DEPTH) return TAWC_ELOOP;

	uint8_t magic[2];
	long n = tawc_pread64(fd, magic, 2, 0);
	if (n < 0) return n;
	if (n < 2 || !(magic[0] == '#' && magic[1] == '!'))
		return classify_elf(fd);

	/* Shebang: parse the line, open the interpreter through the view,
	 * require it executable, and recurse. */
	char line[TAWC_SHEBANG_BUF];
	const char *interp;
	const char *sarg;
	long pe = tawcroot_shebang_read(fd, line, sizeof line, &interp, &sarg);
	if (pe < 0) return pe;
	*title_extra += tawc_strlen(interp) + 1;
	if (sarg) *title_extra += tawc_strlen(sarg) + 1;

	long ifd = tawcroot_open_in_view(interp);
	if (ifd < 0) return ifd;  /* missing interpreter → ENOENT, etc. */
	long ck = probe_check_executable((int)ifd);
	if (ck == 0) ck = classify_loadable((int)ifd, depth + 1, title_extra);
	tawc_close((int)ifd);
	return ck;
}

long tawcroot_exec_handler_prepare(const char *path, int argc,
                                   const char *const *argv,
                                   const char *const *envp)
{
	if (!path || !argv || !envp || argc < 0) return TAWC_EINVAL;

	/* execve("/proc/self/exe", …): the kernel-level link points at
	 * libtawcroot.so, which the loader would then try to map over the
	 * running tawcroot image (LOADER_FAIL 68). Substitute the stashed
	 * guest exe path — the same synthesis readlinkat performs — so the
	 * guest re-execs the binary it believes it is running. Load-bearing
	 * for busybox's standalone-shell mode (Debian's config), where ash
	 * runs nearly every applet via a /proc/self/exe re-exec. */
	if (tawcroot_guest_exe_path_len > 0 && tawcroot_is_proc_self_exe(path))
		path = tawcroot_guest_exe_path;

	/* (1) Probe the guest binary so failures surface as a clean -errno
	 * to the guest rather than a phantom-process exit later. In rootfs
	 * mode (tawcroot_rootfs_fd set) the path is guest-relative; route
	 * the probe through the translator so we open inside the view.
	 * In legacy --exec-via-handler mode (no rootfs) the path is a
	 * host-fs path, opened directly. */
	size_t title_extra = 0;
	{
		long probe = tawcroot_open_in_view(path);
		if (probe < 0) return probe;
		long ck = probe_check_executable((int)probe);
		/* Classify the binary (ELF / shebang chain) before the commit
		 * so a non-ELF non-script, a missing shebang interpreter, or a
		 * wrong-arch ELF returns a clean errno to the guest instead of
		 * killing it with a loader exit code post-execveat. */
		if (ck == 0) ck = classify_loadable((int)probe, 0, &title_extra);
		tawc_close((int)probe);
		if (ck < 0) return ck;
	}

	/* (2) Create a non-CLOEXEC memfd. !CLOEXEC is required so the fd
	 * survives the upcoming execveat into our own re-exec. */
	long mfd = tawc_memfd_create("tawcroot-exec-state", 0);
	if (mfd < 0) return TAWC_EFAULT;

	/* (3) Build the exec_state. When tawcroot has a rootfs view set
	 * up (production -r mode), capture rootfs path + bind specs +
	 * guest_exe so --exec-child can re-establish the view in the new
	 * tawcroot incarnation. Without these, the new process would run
	 * handler-less and against the host filesystem — guest's
	 * post-exec syscalls would either crash (no SIGSYS handler) or
	 * route to host paths (no rootfs_fd). */
	/* Static: over a kilobyte of pointer/name staging would bust the
	 * handler frame cap (stack_budget.h). Exec stages through static
	 * buffers — callers hold exec_lock (thread-safety note in
	 * exec_handler.h). */
	static const char *bind_src_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static const char *bind_dst_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static unsigned char bind_ro_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static char shm_name_buf[TAWCROOT_EXEC_STATE_MAX_SHM]
	                        [TAWCROOT_SHM_NAME_MAX + 1];
	static const char *shm_name_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	static int         shm_fd_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	tawcroot_exec_state_extras extras = { 0 };

	/* Virtual identity survives execve (fork inherits it for free;
	 * exec must ferry it — a dropped sshd session exec'ing the user's
	 * shell must not come back as fake root). Carried in both rootfs
	 * and legacy --exec-via-handler modes. */
	tawc_identity ident_snap;
	tawcroot_identity_get(&ident_snap);
	extras.identity = &ident_snap;

	if (tawcroot_rootfs_fd >= 0 && tawcroot_rootfs_host_path_len > 0) {
		extras.rootfs_host = tawcroot_rootfs_host_path;
		/* Don't ferry the parent's stashed guest_exe through. After
		 * the guest's execve, /proc/self/exe should resolve to the
		 * newly-exec'd binary, not the original tawcroot argv. With
		 * `extras.guest_exe == NULL`, --exec-child's
		 * `tawcroot_loader_exec_child` falls back to `st.path` — the
		 * exec target — which is the correct value for AT_EXECFN /
		 * /proc/self/exe synthesis. Without this, Firefox's stub binary
		 * (which looks at /proc/self/exe to find its own dir, then
		 * dlopen's libxul.so relative to it) sees /bin/bash and prints
		 * "Couldn't load XPCOM." */
		extras.guest_exe   = (const char *)0;
		/* Hardlink-emulation store: ferry the ORIGINAL store path.
		 * Deriving from rootfs_host in the child would be wrong
		 * after a guest chroot (rootfs_host is then the chrooted
		 * root, not the store's sibling). */
		extras.store_host = tawcroot_store_host_path_len > 0
			? tawcroot_store_host_path : (const char *)0;
		/* Source host paths come straight off the bind table.
		 * Earlier revisions recovered them via readlinkat
		 * /proc/self/fd/<src_fd>, which broke once gpgme's fork-child
		 * closefrom() raced against the recovery readlink — keeping
		 * the strings around makes the dependency on a live src_fd
		 * disappear.
		 *
		 * No lock needed for the bind-table snapshot itself: the
		 * append-only contract is no longer strictly true (chroot
		 * mutates `active` and rewrites `dst` in place for
		 * surviving binds), but a re-anchor only runs when the
		 * guest issues `chroot()` — chroot is itself trapped, so a
		 * concurrent execve trap on another thread would have to
		 * race AGAINST chroot mid-rewrite. The window is microseconds
		 * and there's no realistic workload that issues chroot from
		 * one thread while another execs (pacman 6.x's chroot is
		 * single-threaded, post-startup). shm by contrast is mutated
		 * by the guest at runtime (shm_open / shm_unlink
		 * interception), so its snapshot needs the export lock.
		 *
		 * Inactive binds (deactivated by chroot when they fell
		 * outside the new view) are SKIPPED here. Their src_fd
		 * stays reserved in the parent, but their dst was cleared
		 * to ""; passing them to --exec-child would trip
		 * tawcroot_path_add_bind's empty-dst -EINVAL. */
		uint32_t nb = 0;
		for (size_t i = 0; i < tawcroot_n_binds &&
				   nb < TAWCROOT_EXEC_STATE_MAX_BINDS; i++) {
			if (!tawcroot_binds[i].active) continue;
			bind_src_arr[nb] = tawcroot_binds[i].src;
			bind_dst_arr[nb] = tawcroot_binds[i].dst;
			bind_ro_arr[nb]  = (unsigned char)
				tawcroot_binds[i].read_only;
			nb++;
		}
		extras.n_binds  = nb;
		extras.bind_src = bind_src_arr;
		extras.bind_dst = bind_dst_arr;
		extras.bind_ro  = bind_ro_arr;
		extras.root_ro  = (uint32_t)tawcroot_root_ro;

		/* /dev/shm name table. The internal memfds are non-CLOEXEC
		 * and survive execveat as the same fd numbers, so the new
		 * tawcroot just re-registers (name, fd) without re-creating
		 * any kernel-side memfd object. Snapshot under one lock so
		 * concurrent shm ops on other threads can't shift indices
		 * mid-export. */
		size_t shm_n = tawcroot_shm_export_all(
			shm_name_buf, shm_fd_arr,
			TAWCROOT_EXEC_STATE_MAX_SHM);
		for (size_t i = 0; i < shm_n; i++)
			shm_name_arr[i] = shm_name_buf[i];
		extras.n_shm    = (uint32_t)shm_n;
		extras.shm_name = shm_name_arr;
		extras.shm_fd   = shm_fd_arr;
	}

	/* Proctitle: sizes the re-exec'd process's kernel arg region so
	 * proctitle.c's in-place rewrite fits the final cmdline EXACTLY.
	 * The loader installs (post-shebang) eff_argv, whose NUL-joined
	 * byte count is:
	 *   shebang:     interp/arg prepends (title_extra) + path +
	 *                argv[1..]  (binfmt_script drops caller argv[0])
	 *   non-shebang: argv[0] + argv[1..]  ("" when argc == 0)
	 * The execveat argv also carries "--exec-child <fd>", so the title
	 * is shrunk by that protocol overhead; the region then comes out
	 * at exactly the target size and the rewrite leaves no trailing
	 * NUL slack (matters to pkill -fx and anything reading cmdline
	 * byte-exactly). Cmdlines shorter than the overhead keep a few
	 * trailing NULs — nothing to shrink below the protocol args.
	 * Only the LENGTH matters; the content (space-joined guest argv,
	 * truncated or space-padded to length) is visible only in the µs
	 * between the execveat and the loader's rewrite, or if the loader
	 * dies mid-bootstrap. Static: caller holds exec_lock
	 * (thread-safety note in exec_handler.h). */
	static char title_buf[EXEC_TITLE_BUF_SIZE];
	{
		char fdtmp[24];
		int fdlen = tawc_int_to_str(fdtmp, sizeof fdtmp, (int)mfd);
		if (fdlen <= 0) fdlen = 1;
		size_t overhead = sizeof "--exec-child" + (size_t)fdlen + 1;

		size_t want = 0;   /* exact NUL-joined eff_argv byte count */
		for (int i = 1; i < argc && argv[i]; i++)
			want += tawc_strlen(argv[i]) + 1;
		if (title_extra > 0)
			want += title_extra + tawc_strlen(path) + 1;
		else
			want += (argc > 0 ? tawc_strlen(argv[0]) : 0) + 1;

		size_t tlen = want > overhead + 1 ? want - 1 - overhead : 0;
		if (tlen > sizeof title_buf - 1) tlen = sizeof title_buf - 1;

		size_t pos = 0;
		for (int i = 0; i < argc && argv[i] && pos < tlen; i++) {
			if (i > 0) title_buf[pos++] = ' ';
			const char *s = argv[i];
			while (*s && pos < tlen) title_buf[pos++] = *s++;
		}
		while (pos < tlen) title_buf[pos++] = ' ';
		title_buf[pos] = 0;
		extras.proctitle = title_buf;
	}

	static uint8_t state_buf[EXEC_STATE_BUF_SIZE];
	long w = tawcroot_exec_state_write(state_buf, sizeof state_buf,
	                                   path, argc, argv, envp, &extras);
	if (w < 0) { tawc_close((int)mfd); return w; }

	long bytes = 0;
	while (bytes < w) {
		long r = tawc_write((int)mfd, state_buf + bytes,
		                    (size_t)(w - bytes));
		if (r < 0) { tawc_close((int)mfd); return r; }
		if (r == 0) { tawc_close((int)mfd); return TAWC_EFAULT; }
		bytes += r;
	}

	/* Rewind the memfd so the child's lseek(SEEK_END) reports the
	 * right size and its mmap starts at offset 0. */
	if (tawc_lseek((int)mfd, 0, 0 /*SEEK_SET*/) < 0) {
		tawc_close((int)mfd);
		return TAWC_EFAULT;
	}

	return mfd;
}

long tawcroot_exec_handler_commit(int mfd)
{
	/* (4) Open /proc/self/exe so we can execveat ourselves with
	 * AT_EMPTY_PATH. Going through the path namespace would require
	 * us to know our own filesystem path, which depends on how
	 * tawcroot was invoked (in production, the APK's nativeLibraryDir
	 * libtawcroot.so; under tawcroot/test.sh --device, the test scratch
	 * dir). /proc/self/exe is always a working symlink to the
	 * current executable. */
	long exe_fd = tawc_openat(AT_FDCWD, "/proc/self/exe",
	                          O_RDONLY | O_CLOEXEC, 0);
	if (exe_fd < 0) {
		tawc_close((int)mfd);
		return TAWC_ENOEXEC;
	}

	/* (5) Build the new argv: ["<proctitle>", "--exec-child", "<fdstr>"].
	 *
	 * argv[0] is the serialized proctitle (space-joined guest cmdline
	 * plus shebang slack) so the kernel builds the new process's arg
	 * region big enough for the loader's in-place cmdline rewrite —
	 * without it, every guest process's /proc/<pid>/cmdline is stuck
	 * at "tawcroot --exec-child <fd>" (see proctitle.h). Entry
	 * classification only looks at argv[1] and argv[2], so argv[0] is
	 * free payload. The pointer targets the mmap'd state (NOT a shared
	 * static — commit runs without exec_lock; see header note); the
	 * kernel copies argv strings before the old mm goes away. */
	char fdstr[24];
	if (tawc_int_to_str(fdstr, sizeof fdstr, (int)mfd) <= 0) {
		tawc_close((int)exe_fd);
		tawc_close((int)mfd);
		return TAWC_EFAULT;
	}

	char arg0_fallback[] = "tawcroot";
	const char *arg0 = arg0_fallback;
	/* (5b) envp for the re-exec: the guest's envp, as pointers into
	 * the mapped state, so the kernel-built env region — what
	 * /proc/<pid>/environ shows, to the process itself and to ps e —
	 * carries the guest environment instead of nothing. The
	 * synthesized guest stack gets its envp from the SAME state, so
	 * the two views agree byte-for-byte. The new tawcroot incarnation
	 * never reads config from its environ (Environment rule,
	 * notes/tawcroot/architecture.md), so inheriting guest payload is
	 * safe. Fallback on any validation failure: empty env — identical
	 * to the pre-fix behaviour, and the guest still gets its real env
	 * from the state. */
	char *empty_env[1];
	empty_env[0] = (char *)0;
	char **envp_for_self = empty_env;
	long map_rv = -1, env_arr_rv = -1;
	size_t map_len = 0, env_arr_len = 0;
	long sz = tawc_lseek((int)mfd, 0, 2 /*SEEK_END*/);
	if (sz >= (long)sizeof(tawcroot_exec_state_header)) {
		long mrv = tawc_mmap((void *)0, (size_t)sz, TAWC_MM_PROT_READ,
		                     TAWC_MM_MAP_PRIVATE, (int)mfd, 0);
		if (!tawc_loader_mmap_is_err((uintptr_t)mrv)) {
			map_rv = mrv;
			map_len = (size_t)sz;
			const tawcroot_exec_state_header *h =
				(const tawcroot_exec_state_header *)(uintptr_t)mrv;
			const char *strings =
				(const char *)(uintptr_t)mrv + sizeof *h;
			const char *end;
			if (h->magic == TAWCROOT_EXEC_STATE_MAGIC &&
			    h->version == TAWCROOT_EXEC_STATE_VERSION &&
			    sizeof *h + (size_t)h->string_bytes <= (size_t)sz) {
				end = strings + h->string_bytes;
				if (h->proctitle_off != 0 &&
				    h->proctitle_off < h->string_bytes) {
					const char *p = strings + h->proctitle_off;
					const char *q = p;
					while (q < end && *q) q++;
					if (q < end) arg0 = p;
				}
				/* Pointer array in a private anon mapping —
				 * commit runs unlocked and may touch no shared
				 * statics (see header note). */
				if (h->envc <= TAWCROOT_EXEC_STATE_MAX_ENV) {
					size_t need = ((size_t)h->envc + 1) *
					              sizeof(char *);
					long ar = tawc_mmap((void *)0, need,
					        TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE,
					        TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON,
					        -1, 0);
					if (!tawc_loader_mmap_is_err((uintptr_t)ar)) {
						char **arr = (char **)(uintptr_t)ar;
						uint32_t i = 0;
						for (; i < h->envc; i++) {
							uint32_t off = h->envp_off[i];
							if (off >= h->string_bytes)
								break;
							const char *q = strings + off;
							while (q < end && *q) q++;
							if (q == end) break;
							arr[i] = (char *)(strings + off);
						}
						if (i == h->envc) {
							arr[i] = (char *)0;
							envp_for_self = arr;
							env_arr_rv = ar;
							env_arr_len = need;
						} else {
							(void)tawc_munmap(
							    (void *)(uintptr_t)ar,
							    need);
						}
					}
				}
			}
		}
	}

	char *new_argv[4];
	new_argv[0] = (char *)arg0;
	new_argv[1] = (char *)"--exec-child";
	new_argv[2] = fdstr;
	new_argv[3] = (char *)0;

	long er = tawc_execveat((int)exe_fd, "", new_argv, envp_for_self,
	                        AT_EMPTY_PATH);
	/* On success execveat does not return. On failure er is -errno. */
	if (env_arr_rv >= 0)
		(void)tawc_munmap((void *)(uintptr_t)env_arr_rv, env_arr_len);
	if (map_rv >= 0)
		(void)tawc_munmap((void *)(uintptr_t)map_rv, map_len);
	tawc_close((int)exe_fd);
	tawc_close((int)mfd);
	return er;
}

long tawcroot_exec_handler_perform(const char *path, int argc,
                                   const char *const *argv,
                                   const char *const *envp)
{
	long mfd = tawcroot_exec_handler_prepare(path, argc, argv, envp);
	if (mfd < 0) return mfd;
	return tawcroot_exec_handler_commit((int)mfd);
}
