/* SIGSYS-handler-side execve interception. See include/exec_handler.h.
 *
 * Async-signal-safe; uses raw_sys.h for every syscall. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_handler.h"
#include "exec_state.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "loader_elf.h"
#include "loader_exec.h"
#include "path.h"
#include "raw_sys.h"
#include "shm.h"
#include "tawc_uapi.h"


/* Cap on serialized exec_state size we'll write into a memfd. Sized to
 * hold the full header (offset arrays for MAX_ARGS args + MAX_ENV envs)
 * plus everything the collection layer (syscalls_exec.c) accepts: 16 KB
 * path + 64 KB argv + 256 KB envp, with slack. Previously this was 64 KB
 * total, so any argv+envp over ~61 KB made the write -ENOSPC, which the
 * guest saw as a nonsensical execve()==ENOSPC for exactly the busy
 * environments the collection layer was sized for. BSS, not stack. */
#define EXEC_STATE_BUF_SIZE                                            \
	(sizeof(tawcroot_exec_state_header) +                         \
	 (16 * 1024) + (64 * 1024) + (256 * 1024) + 8192)

/* Validate an exec-probe fd the way execve(2) would validate the file:
 * directories are EISDIR, non-regular files and files with no execute
 * bit at all are EACCES. The O_RDONLY open alone passes for mode-644
 * files and directories, and by the time the loader discovers the
 * problem (post-execveat) the calling program is already gone — `bash
 * -c /etc` must get a clean error, not a destroyed shell. */
static long probe_check_executable(int fd)
{
	struct statx stx;
	long sr = TAWC_RAW(TAWC_SYS_statx, fd, (long)"",
	                   AT_EMPTY_PATH, STATX_TYPE | STATX_MODE,
	                   (long)&stx, 0);
	if (sr < 0) return sr;
	if (S_ISDIR(stx.stx_mode)) return TAWC_EISDIR;
	if (!S_ISREG(stx.stx_mode)) return TAWC_EACCES;
	if ((stx.stx_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
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
	struct tawc_loader_image img;
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, &img) != 0)
		return TAWC_ENOEXEC;  /* bad magic / class / wrong machine */
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
 * always-dies-post-commit behaviour it replaces. */
static long classify_loadable(int fd, int depth)
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
	long pe = tawcroot_shebang_read(fd, line, sizeof line, &interp, 0);
	if (pe < 0) return pe;

	long ifd = tawcroot_open_in_view(interp);
	if (ifd < 0) return ifd;  /* missing interpreter → ENOENT, etc. */
	long ck = probe_check_executable((int)ifd);
	if (ck == 0) ck = classify_loadable((int)ifd, depth + 1);
	tawc_close((int)ifd);
	return ck;
}

long tawcroot_exec_handler_prepare(const char *path, int argc,
                                   const char *const *argv,
                                   const char *const *envp)
{
	if (!path || !argv || !envp || argc < 0) return TAWC_EINVAL;

	/* (1) Probe the guest binary so failures surface as a clean -errno
	 * to the guest rather than a phantom-process exit later. In rootfs
	 * mode (tawcroot_rootfs_fd set) the path is guest-relative; route
	 * the probe through the translator so we open inside the view.
	 * In legacy --exec-via-handler mode (no rootfs) the path is a
	 * host-fs path, opened directly. */
	{
		long probe = tawcroot_open_in_view(path);
		if (probe < 0) return probe;
		long ck = probe_check_executable((int)probe);
		/* Classify the binary (ELF / shebang chain) before the commit
		 * so a non-ELF non-script, a missing shebang interpreter, or a
		 * wrong-arch ELF returns a clean errno to the guest instead of
		 * killing it with a loader exit code post-execveat. */
		if (ck == 0) ck = classify_loadable((int)probe, 0);
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
	const char *bind_src_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	const char *bind_dst_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	/* Static: 16 KB of name copies would blow the handler stack
	 * budget. Exec already stages through static buffers (see the
	 * thread-safety note in exec_handler.h). */
	static char shm_name_buf[TAWCROOT_EXEC_STATE_MAX_SHM]
	                        [TAWCROOT_SHM_NAME_MAX + 1];
	const char *shm_name_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	int         shm_fd_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
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
			nb++;
		}
		extras.n_binds  = nb;
		extras.bind_src = bind_src_arr;
		extras.bind_dst = bind_dst_arr;

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

	/* (5) Build the new argv: ["tawcroot", "--exec-child", "<fdstr>"]. */
	char fdstr[24];
	if (tawc_int_to_str(fdstr, sizeof fdstr, (int)mfd) <= 0) {
		tawc_close((int)exe_fd);
		tawc_close((int)mfd);
		return TAWC_EFAULT;
	}

	char arg0[] = "tawcroot";
	char arg1[] = "--exec-child";
	char *new_argv[4];
	new_argv[0] = arg0;
	new_argv[1] = arg1;
	new_argv[2] = fdstr;
	new_argv[3] = (char *)0;

	/* Inherit the supervisor's envp. Production callers (the SIGSYS
	 * handler) would normally pass the GUEST'S envp here so the new
	 * program sees what the guest configured. We honour that by
	 * forwarding `envp` through to the synthesized stack via
	 * exec_state — the *new tawcroot incarnation* sees a fresh envp
	 * from the kernel-built initial stack (we use it for our own
	 * supervisor needs, not the guest's), and `loader_exec_child`
	 * passes the guest's envp from exec_state to the loader. */
	char *envp_for_self[] = { (char *)0 };

	long er = tawc_execveat((int)exe_fd, "", new_argv, envp_for_self,
	                        AT_EMPTY_PATH);
	/* On success execveat does not return. On failure er is -errno. */
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
