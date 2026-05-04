/* SIGSYS-handler-side execve interception. See include/exec_handler.h.
 *
 * Async-signal-safe; uses raw_sys.h for every syscall. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_handler.h"
#include "exec_state.h"
#include "path.h"
#include "raw_sys.h"
#include "shm.h"
#include "tawc_uapi.h"

/* Cap on serialized exec_state size we'll write into a memfd. The
 * exec_state header is itself ~2 KB (offset arrays for 256 args + 256
 * envs), and strings can grow. 64 KB is comfortably more than any
 * sane invocation. */
#define EXEC_STATE_BUF_SIZE  (64 * 1024)

/* Tiny int-to-string for the fd argv slot. Returns the number of
 * bytes written (excluding NUL). 32-byte buffer is more than enough
 * for a 64-bit unsigned. */
static size_t u_to_str(unsigned long v, char *buf, size_t cap)
{
	if (cap < 2) return 0;
	char tmp[24];
	size_t n = 0;
	if (v == 0) tmp[n++] = '0';
	while (v && n < sizeof tmp) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	if (n >= cap) return 0;
	for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
	buf[n] = '\0';
	return n;
}

long tawcroot_exec_handler_perform(const char *path, int argc,
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
	if (tawcroot_rootfs_fd >= 0) {
		char suffix[4096];
		tawcroot_path_result r = tawcroot_path_translate(
		    path, suffix, sizeof suffix, TAWCROOT_PATH_FOLLOW);
		if (r.err) return r.err;
		if (suffix[0] == 0) return TAWC_EISDIR;
		long probe = tawc_openat(r.base_fd, suffix,
		                         O_RDONLY | O_CLOEXEC, 0);
		if (probe < 0) return probe;
		tawc_close((int)probe);
	} else {
		long probe = tawc_openat(AT_FDCWD, path,
		                         O_RDONLY | O_CLOEXEC, 0);
		if (probe < 0) return probe;
		tawc_close((int)probe);
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
	const char *shm_name_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	int         shm_fd_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	tawcroot_exec_state_extras extras = { 0 };
	tawcroot_exec_state_extras *extras_p = NULL;

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
		uint32_t nb = (uint32_t)tawcroot_n_binds;
		if (nb > TAWCROOT_EXEC_STATE_MAX_BINDS)
			nb = TAWCROOT_EXEC_STATE_MAX_BINDS;
		extras.n_binds = nb;
		/* Source host paths come straight off the bind table.
		 * Earlier revisions recovered them via readlinkat
		 * /proc/self/fd/<src_fd>, which broke once gpgme's fork-child
		 * closefrom() raced against the recovery readlink — keeping
		 * the strings around makes the dependency on a live src_fd
		 * disappear. */
		for (uint32_t i = 0; i < nb; i++) {
			bind_src_arr[i] = tawcroot_binds[i].src;
			bind_dst_arr[i] = tawcroot_binds[i].dst;
		}
		extras.bind_src = bind_src_arr;
		extras.bind_dst = bind_dst_arr;

		/* /dev/shm name table. The internal memfds are non-CLOEXEC
		 * and survive execveat as the same fd numbers, so the new
		 * tawcroot just re-registers (name, fd) without re-creating
		 * any kernel-side memfd object. Snapshot under one lock so
		 * concurrent shm ops on other threads can't shift indices
		 * mid-export. */
		size_t shm_n = tawcroot_shm_export_all(
			shm_name_arr, shm_fd_arr,
			TAWCROOT_EXEC_STATE_MAX_SHM);
		extras.n_shm    = (uint32_t)shm_n;
		extras.shm_name = shm_name_arr;
		extras.shm_fd   = shm_fd_arr;
		extras_p = &extras;
	}

	static uint8_t state_buf[EXEC_STATE_BUF_SIZE];
	long w = tawcroot_exec_state_write(state_buf, sizeof state_buf,
	                                   path, argc, argv, envp, extras_p);
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

	/* (4) Open /proc/self/exe so we can execveat ourselves with
	 * AT_EMPTY_PATH. Going through the path namespace would require
	 * us to know our own filesystem path, which depends on how
	 * tawcroot was invoked (in production, the APK's nativeLibraryDir
	 * libtawcroot.so; under tawcroot/test --device, the test scratch
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
	if (u_to_str((unsigned long)mfd, fdstr, sizeof fdstr) == 0) {
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
