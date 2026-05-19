/* SIGSYS dispatch handlers for `execve` / `execveat`.
 *
 * Wires the guest's exec syscalls into `tawcroot_exec_handler_perform`,
 * which builds an exec_state in a memfd and re-execs tawcroot with
 * --exec-child. See exec_handler.h.
 *
 * The handlers themselves are small adapters: they pull pointers and
 * argv/envp arrays out of the guest's saved registers, copy strings
 * via usercopy (so a wild guest pointer returns -EFAULT instead of
 * faulting the supervisor), and forward to the perform routine.
 *
 * Layered limitations:
 *
 *   - argv / envp are bounded by `MAX_ARGS` / `MAX_ENV`. Real-world
 *     processes pass <100; the cap is generous and matches
 *     exec_state's serialization limit.
 *
 *   - String length is bounded by `MAX_STR` per entry (4 KB). Linux
 *     itself caps argv strings at MAX_ARG_STRLEN (32 pages) so most
 *     normal usage fits comfortably.
 *
 *   - execveat's AT_SYMLINK_NOFOLLOW flag is intentionally not emulated
 *     yet; callers get -ENOSYS rather than a silently-followed symlink.
 */

#include <stddef.h>
#include <stdint.h>

#include "arch.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "exec_handler.h"
#include "path.h"
#include "raw_sys.h"
#include "syscalls_exec.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#define MAX_ARGS     256
#define MAX_ENV      256
/* Bash-launched binaries inherit an environment that easily blows past
 * a 4 KB per-string limit — `LS_COLORS` runs 5-6 KB in many configs,
 * `_=...` can reflect a long absolute path, and bash propagates a few
 * exported function bodies if there are any (BASH_FUNC_*). When a
 * single env string overflows our buffer, `tawc_copy_string_from_guest`
 * returns -ENAMETOOLONG, which `do_exec` returns as the execve(2)
 * result — bash then prints "File name too long" and gives up. 16 KB
 * comfortably handles the bash + /etc/profile.d combinations we hit
 * on Arch. */
#define MAX_STR      (16 * 1024)

/* Walk a guest pointer-array (argv or envp), copying each pointed-to
 * NUL-terminated string into a packed buffer and returning a parallel
 * array of pointers into that buffer.
 *
 *   guest_arr:    guest pointer to array of (char *) pointers
 *   strings:      caller-owned packed-string buffer (sized strings_cap)
 *   ptrs:         caller-owned array of (const char *) pointers
 *                 (sized cap+1 for NULL terminator)
 *   cap:          max number of array entries we'll accept
 *   strings_cap:  total bytes available in `strings`
 *
 * Returns the entry count (excluding NULL terminator) on success, or
 * -errno on failure (-E2BIG if too many entries, -ENOMEM if strings
 * too small, -EFAULT for bad guest pointers). */
static long collect_array(char *const *guest_arr,
                          char *strings, size_t strings_cap,
                          const char **ptrs, int cap)
{
	if (!guest_arr) return TAWC_EFAULT;

	size_t off = 0;
	int i = 0;
	for (;;) {
		if (i >= cap) return TAWC_E2BIG;

		/* Copy the pointer at guest_arr[i] into local p. */
		uintptr_t p = 0;
		long rc = tawc_copy_from_guest(&p, sizeof p,
		                               (const void *)&guest_arr[i]);
		if (rc < 0) return rc;
		if (p == 0) {
			ptrs[i] = (const char *)0;
			return i;
		}

		/* Copy the string at p. */
		size_t avail = strings_cap > off ? strings_cap - off : 0;
		if (avail == 0) return TAWC_E2BIG;
		size_t want = avail > MAX_STR ? MAX_STR : avail;
		long n = tawc_copy_string_from_guest(strings + off, want,
		                                     (const char *)p);
		if (n < 0) return n;
		ptrs[i] = strings + off;
		off += (size_t)n + 1;  /* +1 for the NUL */
		i++;
	}
}

/* Common path: parse arg0 = path pointer, arg1 = argv, arg2 = envp;
 * collect strings; call perform. Returns the value to write back to
 * the guest as the syscall return — typically -errno on failure
 * (perform doesn't return on success). */
static long do_exec_path(const char *path,
                         char *const *guest_argv, char *const *guest_envp)
{
	if (!path) return TAWC_EFAULT;

	/* argv: 64 KB total, MAX_STR per string. argv overflow is rare —
	 * most callers pass a handful of short args — but ld-conf and
	 * make recipes can occasionally chain dozens of long paths. */
	static char argv_strings[64 * 1024];
	static const char *argv_ptrs[MAX_ARGS + 1];
	long argc = collect_array(guest_argv, argv_strings, sizeof argv_strings,
	                          argv_ptrs, MAX_ARGS);
	if (argc < 0) return argc;

	/* envp: 256 KB total. See MAX_STR comment for sizing rationale.
	 * Bash inherits a busy environment by the time it forks any child,
	 * and we pack the whole thing into our memfd before the
	 * execveat-into-self handoff — there's no opportunity to skip
	 * strings that don't fit. */
	static char envp_strings[256 * 1024];
	static const char *envp_ptrs[MAX_ENV + 1];
	long envc = collect_array(guest_envp, envp_strings, sizeof envp_strings,
	                          envp_ptrs, MAX_ENV);
	if (envc < 0) return envc;

	/* Perform never returns on success. On failure it returns -errno. */
	return tawcroot_exec_handler_perform(path, (int)argc,
	                                     argv_ptrs, envp_ptrs);
}

static long do_exec(const void *guest_path,
                    char *const *guest_argv, char *const *guest_envp)
{
	if (!guest_path) return TAWC_EFAULT;

	static char path_buf[MAX_STR];
	long pn = tawc_copy_string_from_guest(path_buf, sizeof path_buf,
	                                      (const char *)guest_path);
	if (pn < 0) return pn;
	return do_exec_path(path_buf, guest_argv, guest_envp);
}

/* execve(path, argv, envp). Both x86_64 (NR 59) and aarch64 (NR 221)
 * have this syscall. The earlier "aarch64 has no execve, glibc uses
 * execveat" claim was wrong — aarch64 does have execve(2), and glibc
 * uses it for plain execve() without dirfd. Validated empirically on
 * Android 14 / kernel 5.4: bash's `exec /bin/true` goes through NR 221
 * (execve), not NR 281 (execveat), so untrapped execve was getting
 * killed by Android's stacked filter while we sat there waiting for
 * an execveat trap that would never come. */
static long handle_execve(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return do_exec((const void *)args->a, (char *const *)args->b,
	               (char *const *)args->c);
}

static long append_relative(char *base, size_t *len, size_t cap,
                            const char *rel)
{
	if (*len == 0) return TAWC_EINVAL;
	if (base[*len - 1] != '/') {
		if (*len + 1 >= cap) return TAWC_ENAMETOOLONG;
		base[(*len)++] = '/';
	}
	size_t i = 0;
	while (rel[i]) {
		if (*len + 1 >= cap) return TAWC_ENAMETOOLONG;
		base[(*len)++] = rel[i++];
	}
	base[*len] = 0;
	return 0;
}

/* execveat(dirfd, path, argv, envp, flags). Handles the common fexecve(3)
 * shape: execveat(fd, "", argv, envp, AT_EMPTY_PATH), plus dirfd-relative
 * non-empty paths that can be reverse-translated into the rootfs view. */
static long handle_execveat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	int flags = (int)args->e;

	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return TAWC_EINVAL;
	if (flags & AT_SYMLINK_NOFOLLOW) return TAWC_ENOSYS;
	if (dirfd == AT_FDCWD)
		return do_exec((const void *)args->b, (char *const *)args->c,
		               (char *const *)args->d);

	static char guest_path[MAX_STR];
	long pn = tawc_copy_string_from_guest(guest_path, sizeof guest_path,
	                                      (const char *)args->b);
	if (pn < 0) return pn;

	if (guest_path[0] == '/') {
		return do_exec_path(guest_path, (char *const *)args->c,
		                    (char *const *)args->d);
	}

	if (guest_path[0] == 0 && !(flags & AT_EMPTY_PATH))
		return TAWC_ENOENT;

	static char resolved[MAX_STR];
	long rn = tawcroot_fd_to_guest_abs(dirfd, resolved, sizeof resolved);
	if (rn < 0) return rn;

	if (guest_path[0] != 0) {
		size_t len = (size_t)rn;
		long ar = append_relative(resolved, &len, sizeof resolved,
		                          guest_path);
		if (ar < 0) return ar;
	}

	return do_exec_path(resolved, (char *const *)args->c,
	                    (char *const *)args->d);
}

void tawcroot_exec_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_execve,   handle_execve);
	tawcroot_dispatch_install(TAWC_SYS_execveat, handle_execveat);
}
