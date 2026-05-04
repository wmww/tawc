/* AF_UNIX socket address translation.
 *
 * `bind(2)` and `connect(2)` take the path INSIDE a `struct sockaddr_un`,
 * not as a separate path argument the kernel resolves through *at-style
 * APIs. The kernel reads `sun_path` directly from the userspace
 * sockaddr and resolves it against the calling task's filesystem
 * namespace — bypassing every translation rule we apply through the
 * dispatch path.
 *
 * Symptom that pushed this in: `pacman-key --init` runs `gpg-agent
 * --daemon`, which calls `bind(fd, &(struct sockaddr_un){.sun_path =
 * "/root/.gnupg/S.gpg-agent"}, ...)`. Without translation the kernel
 * looks for `/root/` on the host, returns -ENOENT, and gpg-agent
 * exits 2.
 *
 * Translation strategy: rebuild the sockaddr on the handler stack with
 * a `/proc/self/fd/<base_fd>/<suffix>` path. The kernel's path resolver
 * handles `/proc/self/fd/N/x/y` correctly — re-rooting the lookup at
 * the directory referenced by fd N. This avoids needing to know the
 * rootfs's host-side prefix and works uniformly for paths that fall
 * through bind-mount sources.
 *
 * `sun_path` is 108 bytes. `/proc/self/fd/` = 14, plus a 4–5-digit fd,
 * plus '/', plus suffix. Even with a worst-case fd of "10000" + 100B
 * suffix we're at ~120B which overflows; we cap at 107 (leaving room
 * for the trailing NUL the kernel doesn't read but our fold does) and
 * return -ENAMETOOLONG when over budget.
 *
 * Abstract sockets (`sun_path[0] == '\0'`) and non-AF_UNIX families
 * pass through unchanged.
 */

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "path.h"
#include "raw_sys.h"
#include "syscalls_socket.h"
#include "sysnr.h"
#include "usercopy.h"

#define AF_UNIX_FAMILY  1

/* sockaddr_un layout, mirrored locally to avoid pulling <sys/un.h>:
 *   uint16_t sun_family;
 *   char     sun_path[108];
 * Total 110 bytes.
 *
 * We don't include <linux/un.h> because the freestanding handler build
 * runs without sysroot includes for kernel headers. This local mirror
 * matches the kernel ABI and stays small. */
struct tawc_sockaddr_un {
	uint16_t sun_family;
	char     sun_path[108];
};

/* Look up the host path stored for `base_fd`. Matches against rootfs
 * first, then the bind table. Returns NULL if nothing matches; in that
 * case the caller falls back to /proc/self/fd/. */
static const char *host_path_for_base_fd(int base_fd)
{
	if (base_fd == tawcroot_rootfs_fd && tawcroot_rootfs_host_path_len > 0)
		return tawcroot_rootfs_host_path;
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		if (tawcroot_binds[i].src_fd == base_fd)
			return tawcroot_binds[i].src;
	}
	return 0;
}

/* Render `<host_prefix>/<suffix>` into `dst`. Used when we know the
 * host-absolute path of the base directory; the kernel's namei resolves
 * a regular path uniformly, sidestepping the AF_UNIX-specific quirk
 * where `/proc/self/fd/N/...` namei works for stat/open but returns
 * ENOENT for bind/connect on some Android kernel + app-sandbox combos
 * (see tawcroot-gpg-agent-hangs-from-app-context fix). Returns byte
 * count excluding NUL, or -ENAMETOOLONG if it doesn't fit in `cap`. */
static long render_host_path(char *dst, size_t cap,
                             const char *host_prefix, const char *suffix)
{
	size_t i = 0;
	while (host_prefix[i]) {
		if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
		dst[i] = host_prefix[i];
		i++;
	}
	if (suffix && suffix[0]) {
		if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
		dst[i++] = '/';
		size_t j = 0;
		while (suffix[j]) {
			if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
			dst[i++] = suffix[j++];
		}
	}
	dst[i] = '\0';
	return (long)i;
}

/* Render `/proc/self/fd/<fd>/<suffix>` into `dst`. Fallback used when
 * `host_path_for_base_fd` doesn't have a stored host prefix. Returns
 * the byte count (excluding NUL) or -ENAMETOOLONG if the rendering
 * doesn't fit in `cap` bytes (cap should be ≤ sizeof sun_path). */
static long render_proc_fd_path(char *dst, size_t cap, int fd,
                                const char *suffix)
{
	static const char prefix[] = "/proc/self/fd/";
	const size_t prefix_len = sizeof prefix - 1;

	if (cap <= prefix_len) return TAWC_ENAMETOOLONG;

	size_t i = 0;
	for (; i < prefix_len; i++) dst[i] = prefix[i];

	/* Decimal fd. fd >= 0 by construction (we checked). */
	char fdbuf[12];
	int  fdn = 0;
	int  v = fd;
	if (v < 0) return TAWC_EFAULT;
	if (v == 0) fdbuf[fdn++] = '0';
	while (v) { fdbuf[fdn++] = '0' + (v % 10); v /= 10; }
	if (i + (size_t)fdn >= cap) return TAWC_ENAMETOOLONG;
	while (fdn--) dst[i++] = fdbuf[fdn];

	/* Suffix may be empty (the fd itself names the bind target). */
	if (suffix && suffix[0]) {
		if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
		dst[i++] = '/';
		size_t j = 0;
		while (suffix[j]) {
			if (i + 1 >= cap) return TAWC_ENAMETOOLONG;
			dst[i++] = suffix[j++];
		}
	}
	dst[i] = '\0';
	return (long)i;
}

/* Common bind/connect translator. `nr` is the syscall to issue
 * (TAWC_SYS_bind or TAWC_SYS_connect); the ABI is identical. */
static long do_translate_unix_addr(int nr, const tawcroot_syscall_args *args)
{
	int sockfd = (int)args->a;
	const void *guest_addr = (const void *)(uintptr_t)args->b;
	long  addrlen = (long)args->c;

	/* Defensive: if no addr or impossibly small, pass through. */
	if (!guest_addr || addrlen < (long)sizeof(uint16_t)) {
		return TAWC_RAW(nr, args->a, args->b, args->c, 0, 0, 0);
	}

	/* Cap addrlen at our local sockaddr_un size. The kernel does the
	 * same — anything past 110B is ignored. */
	if (addrlen > (long)sizeof(struct tawc_sockaddr_un)) {
		addrlen = sizeof(struct tawc_sockaddr_un);
	}

	struct tawc_sockaddr_un un_in;
	long e = tawc_copy_from_guest(&un_in, (size_t)addrlen, guest_addr);
	if (e < 0) return TAWC_EFAULT;

	/* Non-AF_UNIX: untouched. */
	if (un_in.sun_family != AF_UNIX_FAMILY) {
		return TAWC_RAW(nr, args->a, args->b, args->c, 0, 0, 0);
	}

	/* Determine the path-bytes length the guest provided. */
	long path_bytes = addrlen - (long)sizeof(uint16_t);
	if (path_bytes <= 0) {
		/* Nameless / autobind. Pass through. */
		return TAWC_RAW(nr, args->a, args->b, args->c, 0, 0, 0);
	}

	/* Abstract socket: sun_path[0] == 0. No filesystem translation. */
	if (un_in.sun_path[0] == '\0') {
		return TAWC_RAW(nr, args->a, args->b, args->c, 0, 0, 0);
	}

	/* Bind takes a NUL-terminated or addrlen-bounded path. The kernel
	 * uses min(strnlen, path_bytes). We mirror that to extract the
	 * guest's intended path string. */
	if (path_bytes > (long)sizeof un_in.sun_path) {
		path_bytes = sizeof un_in.sun_path;
	}
	char guest_path[109];
	long pl = 0;
	while (pl < path_bytes && un_in.sun_path[pl] != '\0') {
		guest_path[pl] = un_in.sun_path[pl];
		pl++;
	}
	guest_path[pl] = '\0';

	/* Translate. PARENT_CREATE for bind() (the leaf is a socket node
	 * the kernel will create). connect() should be FOLLOW (the socket
	 * already exists), but PARENT_CREATE folds + clamps the same
	 * way and the kernel does the existence check itself; using
	 * PARENT_CREATE for both is fine and saves a code path here. */
	char suffix[1024];
	tawcroot_path_result r = tawcroot_path_translate(guest_path,
	                                                 suffix, sizeof suffix,
	                                                 TAWCROOT_PATH_PARENT_CREATE);
	if (r.err) return r.err;

	/* Re-pack a sockaddr_un. Prefer the host-absolute path when we know
	 * it (rootfs and binds we set up): some Android app-sandbox
	 * kernels return ENOENT for AF_UNIX bind/connect via
	 * `/proc/self/fd/N/...` even though the same path works for stat
	 * and open. Fall back to /proc/self/fd/N/... only if we don't have
	 * a stored host path for this base_fd. */
	struct tawc_sockaddr_un un_out;
	un_out.sun_family = AF_UNIX_FAMILY;
	const char *host_prefix = host_path_for_base_fd(r.base_fd);
	long n;
	if (host_prefix) {
		n = render_host_path(un_out.sun_path, sizeof un_out.sun_path,
		                     host_prefix, suffix);
	} else {
		n = render_proc_fd_path(un_out.sun_path, sizeof un_out.sun_path,
		                        r.base_fd, suffix);
	}
	if (n < 0) return n;

	/* New addrlen: family bytes + path bytes + 1 (NUL). The kernel
	 * accepts a NUL-inclusive length for filesystem sockets. */
	long new_addrlen = (long)sizeof(uint16_t) + n + 1;
	return TAWC_RAW(nr, (long)sockfd, (long)&un_out,
	                new_addrlen, 0, 0, 0);
}

static long handle_bind(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return do_translate_unix_addr(TAWC_SYS_bind, args);
}

static long handle_connect(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return do_translate_unix_addr(TAWC_SYS_connect, args);
}

/* `accept` is RET_TRAP'd by Android's untrusted_app seccomp filter (the
 * app sandbox allows `accept4` but not the legacy `accept`). Glibc inside
 * a tawcroot-hosted chroot still calls the legacy `accept` for code that
 * predates SOCK_CLOEXEC, so without this handler gpg-agent et al see
 * accept return -ENOSYS and busy-loop on the accept-fail-then-retry path.
 *
 * Convert accept(fd, addr, addrlen) -> accept4(fd, addr, addrlen, 0):
 * the fourth flags arg defaults to 0 which has identical semantics to
 * accept. The accept4 syscall is allowed by Android's filter, so issuing
 * it from our stub IP gets through. */
static long handle_accept(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return TAWC_RAW(TAWC_SYS_accept4, args->a, args->b, args->c, 0, 0, 0);
}

void tawcroot_socket_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_bind,    handle_bind);
	tawcroot_dispatch_install(TAWC_SYS_connect, handle_connect);
	tawcroot_dispatch_install(TAWC_SYS_accept,  handle_accept);
}
