/* Path translation — phase-1 MVP.
 *
 * Today's policy:
 *   - Absolute guest path:
 *       Tokenize on `/`, fold `.` and `..` (clamp at root if they would
 *       escape — proot-compatible behavior described in
 *       notes/tawcroot.md "..&symlink escapes are blocked"). The result
 *       is a rootfs-relative suffix used with the rootfs O_PATH fd via
 *       *at syscalls.
 *   - Relative guest path:
 *       Resolve against the kernel cwd via raw `getcwd`. Strip the host
 *       rootfs prefix (so a kernel cwd of `<rootfs>/foo` becomes guest
 *       `/foo`). Then concatenate the relative remainder and re-run the
 *       absolute translator on the joined path.
 *       If the kernel cwd is outside the rootfs, return -ENOENT (don't
 *       leak host paths through guest-visible errors).
 *   - Empty result ("/") → suffix = "", caller passes AT_EMPTY_PATH.
 *
 * No allocations, no libc, async-signal-safe. Path buffers live on the
 * caller's stack (4 KB PATH_MAX is the hard cap). The `..`-fold uses an
 * in-place stack of byte offsets into the suffix buffer — see
 * `fold_path` below. We do **not** resolve symlinks here yet; that's the
 * next commit, parameterized per syscall semantics (follow / no-follow /
 * parent-for-create / parent-for-remove — see notes/tawcroot.md
 * §"Translation rules").
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "path.h"
#include "path_oracle.h"
#include "path_orchestrate.h"
#include "path_resolve.h"
#include "raw_sys.h"
#include "tawc_uapi.h"

#define TAWC_PATH_MAX 4096

int    tawcroot_rootfs_fd               = -1;
char   tawcroot_rootfs_host_path[4096]  = {0};
size_t tawcroot_rootfs_host_path_len    = 0;

char   tawcroot_guest_exe_path[4096]    = {0};
size_t tawcroot_guest_exe_path_len      = 0;

char   tawcroot_self_host_path[4096]    = {0};
size_t tawcroot_self_host_path_len      = 0;

int    tawcroot_openat2_works           = 0;

void tawcroot_set_guest_exe_path(const char *path)
{
	if (!path) {
		tawcroot_guest_exe_path[0] = 0;
		tawcroot_guest_exe_path_len = 0;
		return;
	}

	/* Canonicalize through the rootfs symlink view so /proc/self/exe
	 * synthesis returns what the kernel would have under a real chroot:
	 * the post-symlink real-path of the executable. Without this,
	 * $ORIGIN-rooted RPATH/RUNPATH lookups break when the exec target
	 * goes through a symlink — e.g. Firefox launched via /usr/sbin/firefox
	 * (→ /usr/bin/firefox → /usr/lib/firefox/firefox) would have $ORIGIN
	 * resolve to /usr/sbin and fail to dlopen libxul.so ("Couldn't load
	 * XPCOM"). Only applies when the resolved path stays in the rootfs
	 * view (base_fd == rootfs_fd); paths that route through a bind
	 * fall through to verbatim because the translator's suffix is
	 * relative to the bind src, not guest-absolute, and reconstructing
	 * the guest path from base_fd matters too little to justify the
	 * extra plumbing — exec'd binaries live in the rootfs proper in
	 * every flow we care about. */
	if (tawcroot_rootfs_fd >= 0) {
		char suffix[TAWC_PATH_MAX];
		tawcroot_path_result r = tawcroot_path_translate(
			path, suffix, sizeof suffix, TAWCROOT_PATH_FOLLOW);
		if (r.err == 0 && r.base_fd == tawcroot_rootfs_fd) {
			tawcroot_guest_exe_path[0] = '/';
			size_t i = 0;
			while (suffix[i] &&
			       i + 2 < sizeof tawcroot_guest_exe_path) {
				tawcroot_guest_exe_path[i + 1] = suffix[i];
				i++;
			}
			tawcroot_guest_exe_path[i + 1] = 0;
			tawcroot_guest_exe_path_len    = i + 1;
			return;
		}
	}

	size_t i = 0;
	while (path[i] && i + 1 < sizeof tawcroot_guest_exe_path) {
		tawcroot_guest_exe_path[i] = path[i];
		i++;
	}
	tawcroot_guest_exe_path[i]   = 0;
	tawcroot_guest_exe_path_len  = i;
}

void tawcroot_init_self_host_path(void)
{
	long n = tawc_readlinkat(AT_FDCWD, "/proc/self/exe",
				 tawcroot_self_host_path,
				 sizeof tawcroot_self_host_path - 1);
	if (n < 0) {
		tawcroot_self_host_path[0]   = 0;
		tawcroot_self_host_path_len  = 0;
		return;
	}
	tawcroot_self_host_path[n]   = 0;
	tawcroot_self_host_path_len  = (size_t)n;
}

struct tawcroot_bind tawcroot_binds[TAWCROOT_MAX_BINDS];
size_t               tawcroot_n_binds = 0;

long tawcroot_proc_fd_to_host_path(int fd, char *out, size_t out_cap)
{
	if (!out || out_cap < 2) return TAWC_EINVAL;

	char link[64];
	const char *prefix = "/proc/self/fd/";
	size_t pl = 0;
	while (prefix[pl]) { link[pl] = prefix[pl]; pl++; }
	int wrote = tawc_int_to_str(link + pl, sizeof link - pl, fd);
	if (wrote <= 0) return TAWC_EINVAL;

	long n = tawc_readlinkat(AT_FDCWD, link, out, out_cap - 1);
	if (n < 0) return n;
	if ((size_t)n >= out_cap - 1) return TAWC_ENAMETOOLONG;
	/* Empty / non-absolute readlink result is a /proc-not-mounted style
	 * failure. Surface as -EINVAL so callers can distinguish it from a
	 * "path is too long" overflow. In practice we're already broken if
	 * /proc isn't mounted (chroot init also depends on it), so the
	 * precise errno doesn't matter much. */
	if (n == 0 || out[0] != '/') return TAWC_EINVAL;
	while (n > 1 && out[n - 1] == '/') n--;
	out[n] = 0;
	return n;
}

void tawcroot_path_probe_openat2(void)
{
	struct tawc_open_how how;
	how.flags   = 0x200000 /*O_PATH*/ | 0x80000 /*O_CLOEXEC*/;
	how.mode    = 0;
	how.resolve = TAWC_RESOLVE_IN_ROOT;
	long fd = tawc_openat2(AT_FDCWD, "/", &how, sizeof how);
	if (fd >= 0) {
		tawcroot_openat2_works = 1;
		(void)TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
	} else {
		tawcroot_openat2_works = 0;
	}
}

/* Well-known-symlink memoization. After fold_absolute, the orchestrator
 * checks each memoized src against the path's first segment (component
 * boundary required). If matched, the segment is replaced with the
 * target form and the fold re-runs so any `..`/`.` in the target
 * collapses. Targets may be relative ("usr/lib") or absolute
 * ("/usr/lib") — the absolute case is handled by re-fold which strips
 * a leading `/`.
 *
 * The hit set is the typical glibc-rootfs symlink layout. Bumps go
 * through this table, not run-time discovery — finder cost is paid
 * once at init. The match logic itself lives in path_orchestrate.c so
 * tests can supply their own table. */
#define MEMO_MAX 16

static struct tawcroot_symlink_memo g_memo[MEMO_MAX];
static size_t                       g_n_memo = 0;

static void memo_one(const char *prefix)
{
	if (g_n_memo >= MEMO_MAX) return;
	size_t plen = tawc_strlen(prefix);
	if (plen + 1 > sizeof g_memo[0].src) return;

	char buf[256];
	long n = TAWC_RAW(TAWC_SYS_readlinkat, tawcroot_rootfs_fd,
			  (long)prefix, (long)buf, (long)sizeof buf,
			  0, 0);
	if (n <= 0) return;
	if ((size_t)n == sizeof buf) return;  /* truncated — skip memo */
	buf[n] = 0;

	struct tawcroot_symlink_memo *m = &g_memo[g_n_memo++];
	for (size_t i = 0; i < plen; i++) m->src[i] = prefix[i];
	m->src[plen] = 0;
	m->src_len = plen;

	int abs = (buf[0] == '/');
	const char *t = abs ? buf + 1 : buf;
	while (*t == '/') t++;
	size_t tlen = tawc_strlen(t);
	if (tlen + 1 > sizeof m->target) {
		g_n_memo--;
		return;
	}
	for (size_t i = 0; i < tlen; i++) m->target[i] = t[i];
	m->target[tlen] = 0;
	m->target_len = tlen;
	m->target_absolute = abs;
}

void tawcroot_path_memoize_well_known(void)
{
	g_n_memo = 0;
	memo_one("lib");
	memo_one("lib64");
	memo_one("usr/lib64");
	memo_one("bin");
	memo_one("sbin");
	memo_one("usr/sbin");
	memo_one("var/run");
}

long tawcroot_path_add_bind(const char *src_host, const char *dst_guest)
{
	if (tawcroot_n_binds >= TAWCROOT_MAX_BINDS) return TAWC_ENOSPC;
	struct tawcroot_bind *b = &tawcroot_binds[tawcroot_n_binds];

	/* Strip leading '/' from dst. Empty after stripping means root,
	 * which we don't currently allow as a bind target — that's just
	 * "the rootfs itself" and is handled by tawcroot_rootfs_fd. */
	const char *d = dst_guest;
	while (*d == '/') d++;
	size_t n = 0; while (d[n]) n++;
	if (n == 0) return TAWC_EINVAL;
	if (n + 1 > sizeof b->dst) return TAWC_ENAMETOOLONG;

	long fd = tawc_openat(AT_FDCWD, src_host,
			      O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0) return fd;

	long resv = tawcroot_fd_reserve((int)fd);
	if (resv < 0) return resv;

	b->src_fd  = (int)resv;
	b->active  = 1;
	b->dst_len = n;
	for (size_t i = 0; i < n; i++) b->dst[i] = d[i];
	b->dst[n] = 0;

	/* Stash the host src path canonicalized through the kernel's view —
	 * /proc/self/fd of the just-opened src_fd resolves any symlinks /
	 * `..` / trailing slash, giving us the bytes /proc/self/fd will
	 * later return for any dirfd opened *through* this bind. That match
	 * is what lets dirfd_to_guest_abs reverse-translate bind-src
	 * dirfds and clamp fd-relative `..` at the bind-dst boundary
	 * (issues/tawcroot-fd-relative-dotdot-escapes-bind-src.md). The
	 * canonical form is also re-openable, so it's still safe to ferry
	 * through to --exec-child. Fallback to the raw user-supplied path
	 * if /proc/self/fd is unavailable: the ferry still works, the
	 * reverse-translate just won't fire for symlink-traversed srcs. */
	{
		long pn = tawcroot_proc_fd_to_host_path(b->src_fd, b->src,
		                                        sizeof b->src);
		if (pn > 0) {
			b->src_len = (size_t)pn;
		} else {
			size_t k = 0;
			while (src_host[k] && k + 1 < sizeof b->src) {
				b->src[k] = src_host[k];
				k++;
			}
			b->src[k]  = 0;
			b->src_len = k;
		}
	}
	tawcroot_n_binds++;
	return 0;
}

/* `tawcroot_path_fold_absolute` is implemented in `path_fold.c` (pure,
 * no syscalls — also linked into the cleat unit-test orchestrator).
 * The post-fold orchestration (bind → memo → resolver → bind) is in
 * `path_orchestrate.c`. */

/* ---------------------------------------------------------------- */
/* Production oracle + cwd source for the orchestration.             */

/* Production oracle: readlink against tawcroot_rootfs_fd via raw
 * syscall. The empty-suffix case (rootfs root itself) is handled
 * locally — readlinkat would return -EINVAL too, but skipping the
 * syscall is cheaper and clearer. */
static long prod_readlink(void *ctx, const char *suffix,
			  char *out, size_t out_cap)
{
	(void)ctx;
	if (!suffix || suffix[0] == 0) return TAWC_EINVAL;
	if (tawcroot_rootfs_fd < 0)    return TAWC_EBADF;
	long n = TAWC_RAW(TAWC_SYS_readlinkat, tawcroot_rootfs_fd,
			  (long)suffix, (long)out, (long)out_cap, 0, 0);
	return n;
}

static const struct tawcroot_path_oracle prod_oracle = {
	.ctx      = 0,
	.readlink = prod_readlink,
};

/* Production cwd-to-guest-absolute resolver: read the kernel cwd via
 * raw `getcwd`, validate against the rootfs host prefix, and write
 * the in-rootfs guest-absolute view (e.g. host `/data/.../rootfs/foo`
 * → guest `/foo`). Returns -ENOENT if the cwd is outside the rootfs
 * view (we deliberately don't leak host paths through guest errors). */
static long prod_cwd_to_guest_abs(void *ctx, char *out, size_t out_cap)
{
	(void)ctx;
	if (tawcroot_rootfs_host_path_len == 0) return TAWC_ENOENT;

	char cwd[TAWC_PATH_MAX];
	long r = TAWC_RAW(TAWC_SYS_getcwd, (long)cwd, (long)sizeof cwd,
			  0, 0, 0, 0);
	if (r < 0) return r;
	if ((size_t)r == 0) return TAWC_ENOENT;

	/* getcwd returns the length INCLUDING the NUL on aarch64+x86_64
	 * Linux. Older kernels may return without NUL — the manpage is
	 * fuzzy; we treat trailing NULs defensively. */
	size_t cwd_len = (size_t)r;
	while (cwd_len > 0 && cwd[cwd_len - 1] == 0) cwd_len--;

	/* Match the rootfs host prefix by bytes, with a component-
	 * boundary requirement on the next byte so a sibling whose name
	 * happens to share the rootfs prefix (e.g. rootfs="/tmp/rfs"
	 * vs. cwd="/tmp/rfs-evil/x") doesn't count as inside. */
	if (cwd_len < tawcroot_rootfs_host_path_len) return TAWC_ENOENT;
	for (size_t i = 0; i < tawcroot_rootfs_host_path_len; i++) {
		if (cwd[i] != tawcroot_rootfs_host_path[i]) return TAWC_ENOENT;
	}
	if (cwd_len > tawcroot_rootfs_host_path_len &&
	    cwd[tawcroot_rootfs_host_path_len] != '/') return TAWC_ENOENT;

	/* Write the guest-absolute view: drop the host prefix, keep the
	 * leading '/' (or synthesize one if cwd IS the rootfs root). */
	size_t off = 0;
	if (off + 1 >= out_cap) return TAWC_ENAMETOOLONG;
	out[off++] = '/';
	for (size_t i = tawcroot_rootfs_host_path_len; i < cwd_len; i++) {
		if (cwd[i] == '/' && i == tawcroot_rootfs_host_path_len) continue;
		if (off + 1 >= out_cap) return TAWC_ENAMETOOLONG;
		out[off++] = cwd[i];
	}
	out[off] = 0;
	return 0;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */

tawcroot_path_result tawcroot_path_translate(const char *guest_path,
					     char *out_suffix, size_t out_cap,
					     tawcroot_path_mode mode)
{
	tawcroot_path_result r;
	r.err     = 0;
	r.base_fd = -1;

	if (!guest_path || out_cap == 0) {
		r.err = TAWC_EFAULT;
		return r;
	}
	if (tawcroot_rootfs_fd < 0) {
		r.err = TAWC_ENOENT;
		return r;
	}

	/* No lazy-reopen needed: handle_close fake-succeeds for reserved
	 * fds, so the guest cannot kill rootfs_fd or bind src_fds. The
	 * globals read below are written post-init only by the chroot(2)
	 * handler (chroot.c). A concurrent multi-threaded chroot races
	 * against the snapshot we take here — see notes/tawcroot.md
	 * §"chroot emulation" for the bounded-failure analysis. */

	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = tawcroot_rootfs_fd,
		.binds            = tawcroot_binds,
		.n_binds          = tawcroot_n_binds,
		.memos            = g_memo,
		.n_memos          = g_n_memo,
		.oracle           = &prod_oracle,
		.cwd_to_guest_abs = prod_cwd_to_guest_abs,
		.cwd_ctx          = 0,
	};
	return tawcroot_path_translate_with_ctx(&ctx, guest_path,
						out_suffix, out_cap, mode);
}
