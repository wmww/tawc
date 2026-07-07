/* Per-process tawcroot supervisor bootstrap.
 *
 * Installs the rootfs view, bind table, shm map, dispatch table,
 * SIGSYS handler, signal-mask reset, and the /proc/self/exe stash.
 * Used by both:
 *
 *   - prod_rootfs_init (top-level `-r ROOTFS …` entry, in main.c)
 *   - tawcroot_loader_exec_child (the SIGSYS-handler-driven re-exec
 *     in loader_exec.c)
 *
 * The shared bootstrap is centralised so future supervisor state
 * (new per-process tables, additional probes) lands in one place
 * instead of accumulating in two places that drift apart.
 *
 * Differences each caller still owns:
 *   - prod additionally installs the seccomp filter and sets
 *     PR_SET_NO_NEW_PRIVS (single-shot; both inherited via execve).
 *   - prod canonicalises its `-r` argument via /proc/self/fd readlink
 *     before calling here. The child path receives the already-
 *     canonical path through the exec_state memfd.
 *   - The child path passes the inherited shm name table via this
 *     struct; prod's first call leaves shm_names == NULL.
 *   - guest_exe_path is set by the caller after init returns (prod
 *     uses argv[cmd_start]; child uses st.guest_exe || st.path).
 */

#pragma once

#include <stddef.h>

struct tawcroot_supervisor_args {
	/* Canonical absolute host path to the rootfs. The caller is
	 * responsible for canonicalisation; trailing slashes are stripped
	 * here. The string itself must outlive this call (its bytes are
	 * copied into tawcroot_rootfs_host_path during init). */
	const char *rootfs_host_path;

	/* Bind table — parallel arrays of n_binds (src, dst) pairs.
	 * src is a host-absolute path; dst is a guest-absolute path.
	 * Strings must outlive this call (the host bytes are copied into
	 * the bind table by tawcroot_path_add_bind). */
	const char *const *bind_src;
	const char *const *bind_dst;
	size_t              n_binds;

	/* Host path of the hardlink-emulation store (linkstore.h), or
	 * NULL for no store (emulation degrades to the v1 symlink
	 * fallback). prod derives it from the -r argument's parent dir
	 * (`<rootfs>/../tawcroot`); the child path receives the ORIGINAL
	 * store path through exec_state — never re-derived, because a
	 * guest chroot changes rootfs_host_path but not the store. */
	const char *store_host_path;

	/* Inherited /dev/shm name table for re-registration. NULL/0 iff
	 * this is the top-level entry (no shm yet). The fds in shm_fds
	 * must be valid open memfds in the calling process; supervisor
	 * adds them back to the reserved-fd set so close-trapping
	 * protects them. */
	const char *const *shm_names;
	const int        *shm_fds;
	size_t              n_shm;
};

/* Sets up the tawcroot supervisor view in this process. On any step
 * failure, calls tawc_exit_group with one of the codes below and
 * never returns. On success, returns to the caller.
 *
 * Failure exit codes (chosen from a fresh range so they don't collide
 * with caller-specific pre-supervisor failures or the loader's
 * 60..79):
 *   90  open rootfs failed
 *   91  reserve rootfs fd failed
 *   92  usercopy probe failed
 *   93  bind add failed (some entry)
 *   94  install handler failed
 *   95  shm register failed (some entry)
 */
void tawcroot_supervisor_init(const struct tawcroot_supervisor_args *args);
