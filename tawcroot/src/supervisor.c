/* Per-process tawcroot supervisor bootstrap.
 *
 * See include/supervisor.h. This is the deduplicated init that
 * prod_rootfs_init (main.c) and tawcroot_loader_exec_child
 * (loader_exec.c) used to open-code in two places that drifted apart
 * each time a new piece of supervisor state was added.
 *
 * Lives in PROD_C only — uses raw_sys helpers and assumes the trap
 * filter is either inherited (child path) or about to be installed
 * by the caller (prod path, post-init).
 */

#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "fdtab.h"
#include "handler.h"
#include "io.h"
#include "path.h"
#include "raw_sys.h"
#include "shm.h"
#include "supervisor.h"
#include "tawc_uapi.h"
#include "usercopy.h"

void tawcroot_supervisor_init(const struct tawcroot_supervisor_args *args)
{
	/* (1) Open the rootfs O_PATH. Reserve into the high-fd range so
	 * the guest can't address it with low-numbered relative dirfds. */
	long rfd = tawc_openat(AT_FDCWD, args->rootfs_host_path,
	                       O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	if (rfd < 0) {
		tawc_io_str("tawcroot: open rootfs failed: ");
		tawc_io_str(args->rootfs_host_path);
		tawc_io_str("\n");
		tawc_exit_group(90);
	}
	long resv = tawcroot_fd_reserve((int)rfd);
	if (resv < 0) tawc_exit_group(91);
	tawcroot_rootfs_fd = (int)resv;

	/* (2) Stash the canonical host path for getcwd reverse-translation
	 * and the readlink handler. Canonicalise via /proc/self/fd/<n>:
	 * the user-supplied path may differ from the kernel's view (on
	 * Android, app dirs accessed via /data/user/0/<pkg> resolve to
	 * /data/data/<pkg> in the underlying mount, and getcwd reports
	 * the mount-side path). Without canonicalisation the prefix-
	 * match in handle_getcwd fails inside the chroot, glibc's
	 * getcwd returns ENOENT, and everything that depends on cwd
	 * (mkdir -p, bash's PWD, the relative-path resolver) misbehaves.
	 *
	 * Idempotent on the --exec-child path (the parent already
	 * canonicalised; re-running the readlink yields the same string).
	 * Fall back to the user-supplied path if /proc isn't mounted yet
	 * or the readlink otherwise fails — the legacy mismatch bug is
	 * then visible but legacy behaviour is preserved.
	 *
	 * Strip trailing slashes so the prefix match in path.c stays
	 * unambiguous. */
	{
		char canon[sizeof tawcroot_rootfs_host_path];
		long rl = tawcroot_proc_fd_to_host_path(tawcroot_rootfs_fd,
		                                        canon, sizeof canon);
		/* On readlink failure (/proc not mounted yet, or some other
		 * oddity) fall back to the user-supplied path; the legacy
		 * mismatch bug from before canonicalisation reappears, but
		 * tawcroot is already broken in deeper ways without /proc. */
		long kn = tawc_str_copy(tawcroot_rootfs_host_path,
		                        sizeof tawcroot_rootfs_host_path,
		                        rl > 0 ? canon : args->rootfs_host_path);
		size_t k = kn > 0 ? (size_t)kn : 0;
		while (k > 1 && tawcroot_rootfs_host_path[k - 1] == '/') k--;
		tawcroot_rootfs_host_path[k] = 0;
		tawcroot_rootfs_host_path_len = k;
	}

	/* (3) usercopy probe (process_vm_readv via the stub). */
	long up = tawc_usercopy_init();
	if (up < 0) {
		tawc_io_str("tawcroot: usercopy probe failed\n");
		tawc_exit_group(92);
	}

	/* (4) Bind table. Strings must outlive this call — the table
	 * keeps pointers, not copies. Both callers satisfy this: prod
	 * copies cli args into a stack buffer that lives for the rest
	 * of tawcroot_main; child reads from the mmap'd exec_state which
	 * stays mapped through to tawcroot_loader_exec. */
	for (size_t i = 0; i < args->n_binds; i++) {
		if (!args->bind_src[i] || !args->bind_dst[i]) continue;
		long br = tawcroot_path_add_bind(args->bind_src[i],
		                                 args->bind_dst[i]);
		if (br < 0) {
			tawc_io_str("tawcroot: bind add failed for ");
			tawc_io_str(args->bind_src[i]);
			tawc_io_str(" -> ");
			tawc_io_str(args->bind_dst[i]);
			tawc_io_str("\n");
			tawc_exit_group(93);
		}
	}

	/* (5) Inherited /dev/shm name table. Top-level entries pass
	 * n_shm == 0 and skip this step. The internal memfds are
	 * non-CLOEXEC and inherited verbatim across the execveat — fd
	 * numbers stay valid, we just rebuild the (name -> fd) map and
	 * re-add fds to the reserved list so close-trapping protects
	 * them. */
	if (args->n_shm > 0) {
		tawcroot_shm_reset();
		for (size_t i = 0; i < args->n_shm; i++) {
			if (!args->shm_names[i]) continue;
			long sr = tawcroot_shm_register(args->shm_names[i],
			                                args->shm_fds[i]);
			if (sr < 0) tawc_exit_group(95);
		}
	}

	/* (6) Memoise well-known guest paths and initialise the dispatch
	 * table BEFORE installing the handler. The handler reads both. */
	tawcroot_path_memoize_well_known();
	tawcroot_dispatch_init();

	/* (7) Install the SIGSYS handler. After this point we can run
	 * code that may itself trap under Android's stacked filter. */
	long inst = tawcroot_install_handler();
	if (inst != 0) tawc_exit_group(94);

	/* (8) Reset the inherited signal mask before any further trap-
	 * driven syscalls. Several launchers inherit a non-empty mask:
	 *   - The JVM that spawned `/system/bin/sh` -> ProcessBuilder routinely
	 *     blocks SIGCHLD/SIGPIPE/SIGUSR1/SIGSYS; that bitset persists
	 *     through fork+execve into us.
	 *   - In the --exec-child path, the SIGSYS handler that re-exec'd
	 *     us was running with SIGSYS auto-masked (siginfo handler
	 *     contract), and the execveat inherits that bit. Bash and other
	 *     shells additionally block SIGSYS before fork+execve.
	 * With SIGSYS blocked, the kernel kills the process by default
	 * instead of invoking our handler; with other signals blocked,
	 * daemons spawned later (gpg-agent, anything that select()'s on
	 * SIGCHLD) hang. Fresh empty mask matches a clean login shell. */
	{
		uint64_t empty = 0;
		(void)TAWC_RAW(TAWC_SYS_rt_sigprocmask, 2 /*SIG_SETMASK*/,
		               (long)&empty, 0, 8, 0, 0);
	}

	/* (9) Stash /proc/self/exe (libtawcroot's host path) so the
	 * readlink handler can recognise it bouncing back through
	 * `readlinkat(O_PATH-fd, "")` (the bypass glibc's realpath uses
	 * for /proc/self/exe). Pinned for the life of the APK install,
	 * so the value stays valid across subsequent execveats. */
	tawcroot_init_self_host_path();
}
