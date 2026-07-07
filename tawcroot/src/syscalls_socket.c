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
 * `sun_path` is 108 bytes including the trailing NUL, and the
 * production install prefix (/data/data/me.phie.tawc/distros/<id>/
 * rootfs, ~45+ bytes) is spent on every translated path — guest paths
 * that are fine natively would fail with ENAMETOOLONG only under long
 * install prefixes. So the rendering is tiered:
 *
 *   1. `<host_prefix>/<suffix>` — preferred (see render_host_path).
 *   2. `/proc/self/fd/<base_fd>/<suffix>` — when tier 1 overflows or
 *      no host prefix is known. base_fd is the long-lived rootfs/bind
 *      fd, so no fd churn and the string reverse-translates statelessly.
 *   3. `/proc/self/fd/<parent_fd>/<leaf>` — when the suffix itself is
 *      long: anchor an O_PATH fd at the leaf's parent, ~20 bytes plus
 *      the leaf regardless of prefix or guest-path depth. See
 *      render_parent_fd_path for the fd's lifetime.
 *
 * -ENAMETOOLONG only remains for a leaf too long for tier 3 — a path
 * the kernel could not bind natively either, give or take a few bytes.
 *
 * Abstract sockets (`sun_path[0] == '\0'`) and non-AF_UNIX families
 * pass through unchanged.
 *
 * The recvmsg msg_name / recvfrom src_addr datagram source address is
 * deliberately NOT reverse-translated. msg_name lives inside the guest
 * msghdr, so the seccomp filter can't trap conditionally, and recvmsg
 * is the hottest receive syscall in the system (every Wayland/X11/dbus
 * message) — trapping it would tax all guest receive traffic to cover
 * datagrams from peers that explicitly bound a filesystem path, which
 * no known consumer reads (glibc syslog and sd_notify senders don't
 * bind). Revisit if a workload does path-addressed datagram
 * request/reply: the symptom is vanishing replies, because the
 * host-path source address gets forward-translated again by sendto.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <ucontext.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "path.h"
#include "raw_sys.h"
#include "syscalls_socket.h"
#include "sysnr.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#define AF_UNIX_FAMILY  1
#define AF_NETLINK_FAMILY  16
#define NETLINK_AUDIT_PROTO 9

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
	long e = tawc_str_append(dst, cap, &i, host_prefix);
	if (!e && suffix && suffix[0]) {
		e = tawc_str_append(dst, cap, &i, "/");
		if (!e) e = tawc_str_append(dst, cap, &i, suffix);
	}
	return e ? e : (long)i;
}

/* Parent-dir anchors for over-budget bind paths (tier 3).
 *
 * For AF_UNIX pathname sockets the kernel stores the sockaddr bytes
 * passed to bind verbatim and hands them back from getsockname/
 * getpeername/accept. A tier-3 string embeds a parent-dir fd, so
 * reverse translation needs that fd to still be open (readlink of
 * /proc/self/fd/<fd> recovers the parent's host path). bind therefore
 * reserves its parent fd (guest-unclosable, hidden from /proc/self/fd
 * listings) and records it here for the life of the process image.
 * Entries dedup by (dev, ino): a daemon binding several sockets in one
 * directory (gpg-agent) consumes one slot.
 *
 * connect/sendto/sendmsg don't persist anything — their translated
 * string is consumed by the one syscall and never echoed back — so
 * their parent fd is transient (caller closes it via *close_fd).
 *
 * Same lock-free discipline as tawcroot_reserved_fds: write the slot,
 * publish the count with release order; handler-context readers load
 * with acquire. Table-full / reserve failure degrades to the transient
 * fd: the bind itself still works, only the reverse translation of its
 * getsockname string is lost (the guest sees the raw tier-3 spelling).
 * Reserved fds are CLOEXEC and this table dies with the process image,
 * so a stale tier-3 string surviving an execve finds no entry and is
 * passed through untouched instead of being readlinked through an
 * unrelated, reused fd number. */
#define TAWC_MAX_SOCK_PARENTS 16

static struct {
	int      fd;
	uint64_t dev;
	uint64_t ino;
} sock_parents[TAWC_MAX_SOCK_PARENTS];
static size_t n_sock_parents;

static int sock_parent_find(uint64_t dev, uint64_t ino)
{
	size_t n = __atomic_load_n(&n_sock_parents, __ATOMIC_ACQUIRE);
	for (size_t i = 0; i < n; i++) {
		if (sock_parents[i].dev == dev && sock_parents[i].ino == ino)
			return sock_parents[i].fd;
	}
	return -1;
}

static int sock_parent_fd_known(int fd)
{
	size_t n = __atomic_load_n(&n_sock_parents, __ATOMIC_ACQUIRE);
	for (size_t i = 0; i < n; i++) {
		if (sock_parents[i].fd == fd) return 1;
	}
	return 0;
}

void tawcroot_socket_reset(void)
{
	n_sock_parents = 0;
}

/* Tier-3 rendering: `/proc/self/fd/<parent_fd>/<leaf>`. `persist`
 * (bind) reserves the parent fd and records it in sock_parents;
 * otherwise the fd is handed back through *close_fd for the caller to
 * close after the syscall. Suffixes without a '/' would re-render the
 * already-overflowed tier-2 string, so they stay -ENAMETOOLONG. */
static long render_parent_fd_path(char *dst, size_t cap, int base_fd,
				  const char *suffix, int persist,
				  int *close_fd)
{
	long last = -1;
	for (long i = 0; suffix[i]; i++) {
		if (suffix[i] == '/') last = i;
	}
	if (last < 0) return TAWC_ENAMETOOLONG;

	char parent[1024];
	if (last >= (long)sizeof parent) return TAWC_ENAMETOOLONG;
	for (long i = 0; i < last; i++) parent[i] = suffix[i];
	parent[last] = '\0';
	const char *leaf = suffix + last + 1;

	long pfd = tawc_openat(base_fd, parent,
			       O_PATH | O_CLOEXEC | O_DIRECTORY, 0);
	if (pfd < 0) return pfd;

	if (persist) {
		struct stat st;
		if (TAWC_RAW(TAWC_SYS_fstat, pfd, (long)&st, 0, 0, 0, 0) == 0) {
			int known = sock_parent_find(st.st_dev, st.st_ino);
			if (known >= 0) {
				tawc_close((int)pfd);
				return tawc_proc_fd_path(dst, cap, known, leaf);
			}
			size_t n = n_sock_parents;
			if (n < TAWC_MAX_SOCK_PARENTS) {
				long r = tawcroot_fd_reserve((int)pfd);
				if (r >= 0) {
					sock_parents[n].fd  = (int)r;
					sock_parents[n].dev = st.st_dev;
					sock_parents[n].ino = st.st_ino;
					__atomic_store_n(&n_sock_parents, n + 1,
							 __ATOMIC_RELEASE);
					return tawc_proc_fd_path(dst, cap,
								 (int)r, leaf);
				}
			}
		}
		/* fstat failed / table full / reserve failed: fall through
		 * to the transient fd — bind still works, reverse
		 * translation of this socket's name is lost. */
	}
	long m = tawc_proc_fd_path(dst, cap, (int)pfd, leaf);
	if (m < 0) {
		tawc_close((int)pfd);
		return m;
	}
	*close_fd = (int)pfd;
	return m;
}

/* Build a translated sockaddr_un from a guest one. Reads `addrlen`
 * bytes of guest sockaddr at `guest_addr`, and if it's a pathname
 * AF_UNIX address, rewrites the path inside the rootfs view into
 * `un_out`, setting `*out_len` to the new addrlen. Returns:
 *   1  — translated (use un_out / *out_len)
 *   0  — pass through unchanged (non-UNIX, abstract, nameless)
 *  <0  — -errno
 * Shared by bind/connect and sendto/sendmsg. `persist` marks a bind
 * (tier-3 parent fds are kept for reverse translation); *close_fd is
 * set to a transient fd the caller must close after issuing the
 * syscall, or -1. */
static long translate_unix_sockaddr(const void *guest_addr, long addrlen,
				    struct tawc_sockaddr_un *un_out,
				    long *out_len, int persist, int *close_fd)
{
	*close_fd = -1;
	if (!guest_addr || addrlen < (long)sizeof(uint16_t)) return 0;
	if (addrlen > (long)sizeof(struct tawc_sockaddr_un))
		addrlen = sizeof(struct tawc_sockaddr_un);

	struct tawc_sockaddr_un un_in;
	long e = tawc_copy_from_guest(&un_in, (size_t)addrlen, guest_addr);
	if (e < 0) return TAWC_EFAULT;

	if (un_in.sun_family != AF_UNIX_FAMILY) return 0;

	long path_bytes = addrlen - (long)sizeof(uint16_t);
	if (path_bytes <= 0) return 0;            /* nameless / autobind */
	if (un_in.sun_path[0] == '\0') return 0;  /* abstract socket */

	if (path_bytes > (long)sizeof un_in.sun_path)
		path_bytes = sizeof un_in.sun_path;
	char guest_path[109];
	long pl = 0;
	while (pl < path_bytes && un_in.sun_path[pl] != '\0') {
		guest_path[pl] = un_in.sun_path[pl];
		pl++;
	}
	guest_path[pl] = '\0';

	/* Mode/intent split per caller: bind() creates the socket file —
	 * PARENT_CREATE (forced write intent) is correct, and an RO bind
	 * dst refuses with EROFS like the kernel. connect/sendto/sendmsg
	 * only *reach* an existing socket: FOLLOW + READ, so connecting to
	 * a socket inside an RO bind stays legal. FOLLOW is independently
	 * more kernel-faithful for connect — the kernel follows a leaf
	 * symlink when connecting, PARENT_CREATE does not. */
	char suffix[1024];
	tawcroot_path_result r = tawcroot_path_translate(
		guest_path, suffix, sizeof suffix,
		persist ? TAWCROOT_PATH_PARENT_CREATE : TAWCROOT_PATH_FOLLOW,
		persist ? TAWCROOT_PATH_INTENT_WRITE
			: TAWCROOT_PATH_INTENT_READ);
	if (r.err) return r.err;

	un_out->sun_family = AF_UNIX_FAMILY;
	const char *host_prefix = host_path_for_base_fd(r.base_fd);
	long n;
	if (host_prefix) {
		n = render_host_path(un_out->sun_path, sizeof un_out->sun_path,
				     host_prefix, suffix);
		if (n == TAWC_ENAMETOOLONG)
			n = tawc_proc_fd_path(un_out->sun_path,
					      sizeof un_out->sun_path,
					      r.base_fd, suffix);
	} else {
		n = tawc_proc_fd_path(un_out->sun_path, sizeof un_out->sun_path,
				      r.base_fd, suffix);
	}
	if (n == TAWC_ENAMETOOLONG)
		n = render_parent_fd_path(un_out->sun_path,
					  sizeof un_out->sun_path,
					  r.base_fd, suffix, persist, close_fd);
	if (n < 0) return n;
	*out_len = (long)sizeof(uint16_t) + n + 1;
	return 1;
}

/* Common bind/connect translator. `nr` is the syscall to issue
 * (TAWC_SYS_bind or TAWC_SYS_connect); the ABI is identical. */
static long do_translate_unix_addr(int nr, const tawcroot_syscall_args *args)
{
	struct tawc_sockaddr_un un_out;
	long new_addrlen;
	int close_fd;
	long t = translate_unix_sockaddr(
		(const void *)(uintptr_t)args->b, (long)args->c,
		&un_out, &new_addrlen, nr == TAWC_SYS_bind, &close_fd);
	if (t < 0) return t;
	if (t == 0)
		return TAWC_RAW(nr, args->a, args->b, args->c, 0, 0, 0);
	long rv = TAWC_RAW(nr, args->a, (long)&un_out, new_addrlen, 0, 0, 0);
	if (close_fd >= 0) tawc_close(close_fd);
	return rv;
}

/* Expand a tier-2/3 `/proc/self/fd/<n>/<rest>` spelling into the full
 * host path in `out`. Only fds we planted are followed: the rootfs/bind
 * base fds (host prefix known statelessly) and recorded sock_parents
 * (readlink of the still-open reserved fd). Anything else — including
 * a stale tier-3 string whose fd died with an execve — returns <0 and
 * the caller leaves the address untouched. Returns the expanded
 * length. */
static long expand_proc_fd_sun_path(const char *host, long hl,
				    char *out, size_t out_cap)
{
	static const char pfx[] = "/proc/self/fd/";
	const long pfx_len = (long)sizeof pfx - 1;
	if (hl <= pfx_len) return TAWC_ENOENT;
	for (long i = 0; i < pfx_len; i++) {
		if (host[i] != pfx[i]) return TAWC_ENOENT;
	}

	long i = pfx_len;
	long fd = 0;
	int digits = 0;
	while (i < hl && host[i] >= '0' && host[i] <= '9') {
		fd = fd * 10 + (host[i] - '0');
		if (fd > 0x7fffffff) return TAWC_ENOENT;
		i++;
		digits++;
	}
	if (!digits || i >= hl || host[i] != '/') return TAWC_ENOENT;
	const char *rest = host + i + 1;

	size_t pos = 0;
	const char *known = host_path_for_base_fd((int)fd);
	long e;
	if (known) {
		e = tawc_str_append(out, out_cap, &pos, known);
	} else if (sock_parent_fd_known((int)fd)) {
		e = tawcroot_proc_fd_to_host_path((int)fd, out, out_cap);
		if (e < 0) return e;
		pos = (size_t)e;
		e = 0;
	} else {
		return TAWC_ENOENT;
	}
	if (!e) e = tawc_str_append(out, out_cap, &pos, "/");
	if (!e) e = tawc_str_append(out, out_cap, &pos, rest);
	return e ? e : (long)pos;
}

/* Reverse-translate a HOST sockaddr_un the kernel wrote into an out
 * param back into the guest view, in place. `kern_addr` holds the
 * kernel's sockaddr and `*kern_lenp` its addrlen; on a successful
 * rewrite the guest-view sockaddr replaces it and *kern_lenp updates.
 * A non-pathname / outside-view address is left untouched. The kernel
 * truncates sun_path to fit the guest buffer; we match by truncating
 * the guest path to 108 bytes. */
static void reverse_translate_unix_sockaddr(struct tawc_sockaddr_un *kern_addr,
					    long *kern_lenp)
{
	long klen = *kern_lenp;
	if (klen < (long)sizeof(uint16_t)) return;
	if (kern_addr->sun_family != AF_UNIX_FAMILY) return;
	long path_bytes = klen - (long)sizeof(uint16_t);
	if (path_bytes <= 0) return;
	if (kern_addr->sun_path[0] == '\0') return;  /* abstract */

	if (path_bytes > (long)sizeof kern_addr->sun_path)
		path_bytes = sizeof kern_addr->sun_path;
	char host[109];
	long hl = 0;
	while (hl < path_bytes && kern_addr->sun_path[hl] != '\0') {
		host[hl] = kern_addr->sun_path[hl];
		hl++;
	}
	host[hl] = '\0';

	/* A tier-2/3 /proc/self/fd spelling (bound over-budget) expands
	 * to its full host path first; the prefix walk below then maps
	 * it into the guest view like any other host path. */
	char full[1200];
	const char *hp = host;
	long fl = expand_proc_fd_sun_path(host, hl, full, sizeof full);
	if (fl > 0) {
		hp = full;
		hl = fl;
	}

	char guest[109];
	long gn = tawcroot_host_path_to_guest_abs(hp, (size_t)hl,
						  guest, sizeof guest);
	if (gn < 0) return;  /* outside the view — leave host path as-is */

	if (gn > (long)sizeof kern_addr->sun_path)
		gn = sizeof kern_addr->sun_path;  /* kernel-style truncation */
	for (long i = 0; i < gn; i++) kern_addr->sun_path[i] = guest[i];
	if (gn < (long)sizeof kern_addr->sun_path)
		kern_addr->sun_path[gn] = '\0';
	*kern_lenp = (long)sizeof(uint16_t) + gn +
		(gn < (long)sizeof kern_addr->sun_path ? 1 : 0);
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

/* sendto(fd, buf, len, flags, dest_addr, addrlen): a connectionless
 * AF_UNIX client (glibc syslog(3) to /dev/log, sd_notify to
 * NOTIFY_SOCKET, some DNS helpers) carries the destination path in
 * dest_addr. Untranslated it went to the host fs and the message
 * vanished. Translate dest_addr exactly like connect; everything else
 * (buf/len/flags) passes through. NULL dest_addr → ordinary send. */
static long handle_sendto(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const void *dest = (const void *)(uintptr_t)args->e;
	long addrlen = (long)args->f;
	if (!dest || addrlen <= 0) {
		return TAWC_RAW(TAWC_SYS_sendto, args->a, args->b, args->c,
				args->d, args->e, args->f);
	}
	struct tawc_sockaddr_un un_out;
	long new_addrlen;
	int close_fd;
	long t = translate_unix_sockaddr(dest, addrlen, &un_out, &new_addrlen,
					 0, &close_fd);
	if (t < 0) return t;
	if (t == 0) {
		return TAWC_RAW(TAWC_SYS_sendto, args->a, args->b, args->c,
				args->d, args->e, args->f);
	}
	long rv = TAWC_RAW(TAWC_SYS_sendto, args->a, args->b, args->c,
			   args->d, (long)&un_out, new_addrlen);
	if (close_fd >= 0) tawc_close(close_fd);
	return rv;
}

/* Local mirror of struct msghdr (64-bit ABI; matches the kernel's
 * user_msghdr layout). We only touch msg_name / msg_namelen; the iov /
 * control fields are forwarded as the guest gave them. */
struct tawc_msghdr {
	uint64_t msg_name;        /* void*  */
	uint32_t msg_namelen;     /* socklen_t */
	uint32_t _pad;
	uint64_t msg_iov;         /* struct iovec* */
	uint64_t msg_iovlen;
	uint64_t msg_control;
	uint64_t msg_controllen;
	int32_t  msg_flags;
	uint32_t _pad2;
};

/* sendmsg(fd, msghdr*, flags): the destination sockaddr lives in
 * msg_name. Copy the msghdr, translate msg_name into a stack-local
 * sockaddr, repoint, and re-issue. Same connectionless-client failure
 * mode as sendto. */
static long handle_sendmsg(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	const void *gmsg = (const void *)(uintptr_t)args->b;
	if (!gmsg) {
		return TAWC_RAW(TAWC_SYS_sendmsg, args->a, args->b, args->c,
				0, 0, 0);
	}
	struct tawc_msghdr mh;
	long e = tawc_copy_from_guest(&mh, sizeof mh, gmsg);
	if (e < 0) return TAWC_EFAULT;
	if (mh.msg_name == 0 || mh.msg_namelen == 0) {
		return TAWC_RAW(TAWC_SYS_sendmsg, args->a, args->b, args->c,
				0, 0, 0);
	}
	struct tawc_sockaddr_un un_out;
	long new_addrlen;
	int close_fd;
	long t = translate_unix_sockaddr(
		(const void *)(uintptr_t)mh.msg_name, (long)mh.msg_namelen,
		&un_out, &new_addrlen, 0, &close_fd);
	if (t < 0) return t;
	if (t == 0) {
		return TAWC_RAW(TAWC_SYS_sendmsg, args->a, args->b, args->c,
				0, 0, 0);
	}
	/* Repoint a COPY of the msghdr at our translated sockaddr; the
	 * guest's msghdr stays untouched. */
	mh.msg_name = (uint64_t)(uintptr_t)&un_out;
	mh.msg_namelen = (uint32_t)new_addrlen;
	long rv = TAWC_RAW(TAWC_SYS_sendmsg, args->a, (long)&mh, args->c,
			   0, 0, 0);
	if (close_fd >= 0) tawc_close(close_fd);
	return rv;
}

/* Shared tail for getsockname / getpeername / accept4: after the kernel
 * fills (addr, addrlen) with a HOST sockaddr_un, reverse-translate the
 * sun_path back into the guest view so a server re-publishing its bound
 * path advertises a guest-visible path (and we don't leak the host
 * prefix). `nr` is the syscall to issue; for accept4 the return value
 * is the new fd (passed through). guest_addr/guest_lenp are out params;
 * either may be NULL (caller doesn't want the address). */
static long getname_with_reverse(int nr, long a, long guest_addr_l,
				 long guest_lenp_l, long d)
{
	void *guest_addr = (void *)(uintptr_t)guest_addr_l;
	void *guest_lenp = (void *)(uintptr_t)guest_lenp_l;
	if (!guest_addr || !guest_lenp) {
		/* No address requested (or accept4 with NULL addr): forward. */
		return TAWC_RAW(nr, a, guest_addr_l, guest_lenp_l, d, 0, 0);
	}
	uint32_t guest_cap;
	long e = tawc_copy_from_guest(&guest_cap, sizeof guest_cap, guest_lenp);
	if (e < 0) return TAWC_EFAULT;

	struct tawc_sockaddr_un kbuf;
	uint32_t klen = sizeof kbuf;
	long rv = TAWC_RAW(nr, a, (long)&kbuf, (long)&klen, d, 0, 0);
	if (rv < 0) return rv;

	long kern_len = (long)klen;
	reverse_translate_unix_sockaddr(&kbuf, &kern_len);

	/* Copy back up to the guest's buffer cap (kernel truncates); write
	 * the FULL translated length to *addrlen regardless (kernel
	 * semantics — the guest learns the real size even when truncated). */
	uint32_t copy = (uint32_t)kern_len;
	if (copy > guest_cap) copy = guest_cap;
	if (copy > 0) {
		long ce = tawc_copy_to_guest(guest_addr, &kbuf, copy);
		if (ce < 0) return TAWC_EFAULT;
	}
	uint32_t report = (uint32_t)kern_len;
	long le = tawc_copy_to_guest(guest_lenp, &report, sizeof report);
	if (le < 0) return TAWC_EFAULT;
	return rv;
}

static long handle_getsockname(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	(void)uc;
	return getname_with_reverse(TAWC_SYS_getsockname, args->a,
				    args->b, args->c, 0);
}

static long handle_getpeername(const tawcroot_syscall_args *args,
			       ucontext_t *uc)
{
	(void)uc;
	return getname_with_reverse(TAWC_SYS_getpeername, args->a,
				    args->b, args->c, 0);
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
 * it from our stub IP gets through.
 *
 * The peer address out-param is reverse-translated like getpeername:
 * an AF_UNIX peer that bound a filesystem path would otherwise hand the
 * accepting server the HOST sun_path. */
static long handle_accept(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return getname_with_reverse(TAWC_SYS_accept4, args->a,
				    args->b, args->c, 0);
}

/* accept4 carries flags in arg d; route through the same reverse-
 * translating path. Trapping it (not just legacy accept) is what makes
 * the peer-address reverse translation actually fire for modern
 * SOCK_CLOEXEC callers. */
static long handle_accept4(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	return getname_with_reverse(TAWC_SYS_accept4, args->a,
				    args->b, args->c, args->d);
}

/* socket(AF_NETLINK, *, NETLINK_AUDIT) is denied by Android's SELinux
 * app policy with EACCES. libaudit consumers (Debian libpam, sshd built
 * with --with-linux-audit) only tolerate EINVAL/EPROTONOSUPPORT/
 * EAFNOSUPPORT from audit_open() — "kernel without CONFIG_AUDIT" — and
 * treat every other errno as a hard failure: pam_acct_mgmt returns
 * PAM_SYSTEM_ERR (breaking su/login/sshd rootfs-wide) and sshd fatals
 * on TTY logins from linux_audit_write_entry. Answer EPROTONOSUPPORT —
 * the exact errno netlink_create() gives for a protocol left
 * unregistered by an audit-less kernel — so the guest sees that
 * kernel. Everything else passes through. */
static long handle_socket(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if ((int)args->a == AF_NETLINK_FAMILY &&
	    (int)args->c == NETLINK_AUDIT_PROTO)
		return TAWC_EPROTONOSUPPORT;
	return TAWC_RAW(TAWC_SYS_socket, args->a, args->b, args->c, 0, 0, 0);
}

void tawcroot_socket_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_socket,      handle_socket);
	tawcroot_dispatch_install(TAWC_SYS_bind,        handle_bind);
	tawcroot_dispatch_install(TAWC_SYS_connect,     handle_connect);
	tawcroot_dispatch_install(TAWC_SYS_accept,      handle_accept);
	tawcroot_dispatch_install(TAWC_SYS_accept4,     handle_accept4);
	tawcroot_dispatch_install(TAWC_SYS_sendto,      handle_sendto);
	tawcroot_dispatch_install(TAWC_SYS_sendmsg,     handle_sendmsg);
	tawcroot_dispatch_install(TAWC_SYS_getsockname, handle_getsockname);
	tawcroot_dispatch_install(TAWC_SYS_getpeername, handle_getpeername);
}
