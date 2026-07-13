/* Path translation.
 *
 * Today's policy:
 *   - Absolute guest path:
 *       Tokenize on `/`, fold `.` and `..` (clamp at root if they would
 *       escape — proot-compatible behavior described in
 *       notes/tawcroot/path-translation.md "..&symlink escapes are blocked"). The result
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
 * caller's stack (4 KB PATH_MAX is the hard cap). The `..`-fold lives
 * in path_fold.c; symlink resolution (parameterized per syscall
 * semantics: follow / no-follow / parent-for-create / parent-for-remove
 * — see notes/tawcroot/path-translation.md §"Translation rules") is wired up through the
 * oracle in the translate ctx and implemented in path_resolve.c.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "linkstore.h"
#include "path.h"
#include "path_oracle.h"
#include "path_orchestrate.h"
#include "path_resolve.h"
#include "path_scratch.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_uapi.h"

int    tawcroot_rootfs_fd               = -1;
int    tawcroot_root_ro                 = 0;
char   tawcroot_rootfs_host_path[4096]  = {0};
size_t tawcroot_rootfs_host_path_len    = 0;

char   tawcroot_guest_exe_path[4096]    = {0};
size_t tawcroot_guest_exe_path_len      = 0;

char   tawcroot_self_host_path[4096]    = {0};
size_t tawcroot_self_host_path_len      = 0;

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
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *suffix = scratch->buf[0];
		tawcroot_path_result r = tawcroot_path_translate(
			path, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
			TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
		if (r.err == 0 && r.base_fd == tawcroot_rootfs_fd) {
			size_t pos = 0;
			(void)tawc_str_append(tawcroot_guest_exe_path,
					      sizeof tawcroot_guest_exe_path,
					      &pos, "/");
			(void)tawc_str_append(tawcroot_guest_exe_path,
					      sizeof tawcroot_guest_exe_path,
					      &pos, suffix);
			tawcroot_guest_exe_path_len = pos;
			return;
		}
	}

	long n = tawc_str_copy(tawcroot_guest_exe_path,
			       sizeof tawcroot_guest_exe_path, path);
	tawcroot_guest_exe_path_len = n < 0 ? 0 : (size_t)n;
}

void tawcroot_init_self_host_path(void)
{
	long n = tawc_readlinkat(AT_FDCWD, "/proc/self/exe",
				 tawcroot_self_host_path,
				 sizeof tawcroot_self_host_path - 1);
	/* Exact-fit means possibly truncated; a truncated path would
	 * false-match prefixes in the /proc/self/exe substitution, so
	 * treat it like failure (substitution just never fires). */
	if (n < 0 || (size_t)n >= sizeof tawcroot_self_host_path - 1) {
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
	if (tawc_proc_fd_path(link, sizeof link, fd, 0) < 0)
		return TAWC_EINVAL;

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

/* Helper: copy `host[start..n)` into `out` as a guest-absolute path
 * with leading '/' (preserving / collapsing internal '/' runs). Used by
 * both the rootfs-prefix and bind-src branches of tawcroot_fd_to_guest_abs.
 * Returns the written length, or -ENAMETOOLONG. */
static long write_guest_abs_suffix(char *out, size_t out_cap,
                                   const char *host, size_t start, size_t n,
                                   const char *bind_dst, size_t bind_dst_len)
{
	size_t off = 0;
	if (off + 1 >= out_cap) return TAWC_ENAMETOOLONG;
	out[off++] = '/';
	for (size_t i = 0; i < bind_dst_len; i++) {
		if (off + 1 >= out_cap) return TAWC_ENAMETOOLONG;
		out[off++] = bind_dst[i];
	}
	for (size_t i = start; i < n; i++) {
		if (host[i] == '/' && off > 0 && out[off - 1] == '/') continue;
		if (off + 1 >= out_cap) return TAWC_ENAMETOOLONG;
		out[off++] = host[i];
	}
	out[off] = 0;
	return (long)off;
}

/* True iff `host[0..n)` starts with `prefix[0..pl)` and the next byte (if
 * any) is `/` — i.e. the prefix matches at a component boundary. */
static int host_prefix_match(const char *host, size_t n,
                             const char *prefix, size_t pl)
{
	if (pl == 0) return 0;
	if (pl > n)  return 0;
	for (size_t i = 0; i < pl; i++)
		if (host[i] != prefix[i]) return 0;
	if (n > pl && host[pl] != '/') return 0;
	return 1;
}

long tawcroot_host_path_to_guest_abs(const char *host, size_t n,
				     char *out, size_t out_cap)
{
	if (!host || !out) return TAWC_EFAULT;
	if (out_cap < 2)   return TAWC_ENAMETOOLONG;

	const struct tawcroot_bind *best_bind = 0;
	size_t best_pl = 0;

	if (host_prefix_match(host, n, tawcroot_rootfs_host_path,
	                      tawcroot_rootfs_host_path_len))
		best_pl = tawcroot_rootfs_host_path_len;

	for (size_t bi = 0; bi < tawcroot_n_binds; bi++) {
		const struct tawcroot_bind *b = &tawcroot_binds[bi];
		if (!b->active || b->src_len == 0) continue;
		if (!host_prefix_match(host, n, b->src, b->src_len))
			continue;
		if (b->src_len > best_pl) {
			best_bind = b;
			best_pl = b->src_len;
		}
	}

	if (best_bind)
		return write_guest_abs_suffix(out, out_cap, host,
		                              best_pl, n,
		                              best_bind->dst, best_bind->dst_len);
	if (best_pl > 0)
		return write_guest_abs_suffix(out, out_cap, host,
		                              best_pl, n, 0, 0);
	return TAWC_ENOENT;
}

int tawcroot_host_path_in_ro_bind(const char *host, size_t n)
{
	if (!host || n == 0) return 0;

	/* Same longest-prefix walk as tawcroot_host_path_to_guest_abs so
	 * the two can't disagree about which bind (or the rootfs) owns a
	 * host path — an RW bind nested under an RO bind src stays
	 * writable here exactly like it does in translation. */
	int    best_ro = 0;
	size_t best_pl = 0;

	if (host_prefix_match(host, n, tawcroot_rootfs_host_path,
	                      tawcroot_rootfs_host_path_len)) {
		best_pl = tawcroot_rootfs_host_path_len;
		best_ro = tawcroot_root_ro;
	}
	for (size_t bi = 0; bi < tawcroot_n_binds; bi++) {
		const struct tawcroot_bind *b = &tawcroot_binds[bi];
		if (!b->active || b->src_len == 0) continue;
		if (!host_prefix_match(host, n, b->src, b->src_len)) continue;
		if (b->src_len > best_pl) {
			best_pl = b->src_len;
			best_ro = b->read_only;
		}
	}
	return best_ro;
}

long tawcroot_fd_to_guest_abs(int fd, char *out, size_t out_cap)
{
	if (fd < 0 || fd == AT_FDCWD) return TAWC_EINVAL;
	if (out_cap < 2) return TAWC_ENAMETOOLONG;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *host = scratch->buf[0];
	long n = tawcroot_proc_fd_to_host_path(
		fd, host, TAWCROOT_PATH_SCRATCH_SIZE);
	if (n < 0) return n;

	return tawcroot_host_path_to_guest_abs(host, (size_t)n, out, out_cap);
}

/* Well-known-symlink memoization. After fold_absolute, the orchestrator
 * checks each memoized src against the path's leading components
 * (component boundary required). If matched, the prefix is replaced
 * with the target and the fold re-runs so any `..`/`.` in the target
 * collapses. Stored targets are always rootfs-root-anchored: a
 * relative symlink target is relative to the symlink's PARENT
 * directory, so memo_one prepends the src's parent ("usr/sbin" → "bin"
 * is stored as "usr/bin"; "var/run" → "../run" as "var/../run", which
 * the re-fold collapses to "run").
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

	/* Root-anchor the stored target. A relative symlink target is
	 * relative to the symlink's parent directory, so prepend src's
	 * parent (everything up to and including the last '/'); an
	 * absolute target is already root-anchored once the leading '/'
	 * is stripped. */
	size_t off = 0;
	if (!abs) {
		size_t parent_len = plen;
		while (parent_len > 0 && prefix[parent_len - 1] != '/')
			parent_len--;
		if (parent_len + tlen + 1 > sizeof m->target) {
			g_n_memo--;
			return;
		}
		for (size_t i = 0; i < parent_len; i++)
			m->target[i] = prefix[i];
		off = parent_len;
	}
	if (off + tlen + 1 > sizeof m->target) {
		g_n_memo--;
		return;
	}
	for (size_t i = 0; i < tlen; i++) m->target[off + i] = t[i];
	m->target[off + tlen] = 0;
	m->target_len = off + tlen;
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

long tawcroot_path_add_bind(const char *src_host, const char *dst_guest,
                            int read_only)
{
	if (tawcroot_n_binds >= TAWCROOT_MAX_BINDS) return TAWC_ENOSPC;
	struct tawcroot_bind *b = &tawcroot_binds[tawcroot_n_binds];

	/* Normalize dst through the lexical fold so it matches folded
	 * suffixes at lookup time — a dst with a trailing '/', '//' runs,
	 * or '.'/'..' components would otherwise be stored verbatim and
	 * never match (route_through_binds compares against folded
	 * suffixes). Empty after folding means root, which we don't allow
	 * as a bind target — that's just "the rootfs itself" and is
	 * handled by tawcroot_rootfs_fd. */
	{
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *abs = scratch->buf[0];
		size_t off = 0;
		long e = tawc_str_append(abs, TAWCROOT_PATH_SCRATCH_SIZE,
					 &off, "/");
		if (!e) e = tawc_str_append(abs, TAWCROOT_PATH_SCRATCH_SIZE,
					    &off, dst_guest);
		if (e) return e;
		long fr = tawcroot_path_fold_absolute(abs, b->dst, sizeof b->dst);
		if (fr < 0) return fr;
	}
	size_t n = tawc_strlen(b->dst);
	if (n == 0) return TAWC_EINVAL;

	long fd = tawc_openat(AT_FDCWD, src_host,
			      O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0) return fd;

	long resv = tawcroot_fd_reserve((int)fd);
	if (resv < 0) return resv;

	b->src_fd    = (int)resv;
	b->read_only = read_only ? 1 : 0;
	b->active    = 1;
	b->dst_len   = n;   /* b->dst already holds the folded form */

	/* Stash the host src path canonicalized through the kernel's view —
	 * /proc/self/fd of the just-opened src_fd resolves any symlinks /
	 * `..` / trailing slash, giving us the bytes /proc/self/fd will
	 * later return for any dirfd opened *through* this bind. That match
	 * is what lets dirfd_to_guest_abs reverse-translate bind-src
	 * dirfds and clamp fd-relative `..` at the bind-dst boundary.
	 * The canonical form is also re-openable, so it's still safe to ferry
	 * through to --exec-child. Fallback to the raw user-supplied path
	 * if /proc/self/fd is unavailable: the ferry still works, the
	 * reverse-translate just won't fire for symlink-traversed srcs. */
	{
		long pn = tawcroot_proc_fd_to_host_path(b->src_fd, b->src,
		                                        sizeof b->src);
		if (pn <= 0)
			pn = tawc_str_copy(b->src, sizeof b->src, src_host);
		b->src_len = pn > 0 ? (size_t)pn : 0;
	}
	tawcroot_n_binds++;
	return 0;
}

/* `tawcroot_path_fold_absolute` is implemented in `path_fold.c` (pure,
 * no syscalls — also linked into the cleat unit-test orchestrator).
 * The post-fold orchestration (bind → memo → resolver → bind) is in
 * `path_orchestrate.c`. */

/* Open a guest path read-only through the current root view. Falls
 * back to the host fs when no rootfs is configured (the legacy --exec /
 * --exec-via-handler diagnostics run before any rootfs exists).
 * Returns an O_RDONLY|O_CLOEXEC fd or -errno; a path that resolves to
 * the rootfs/bind directory itself is -EISDIR (exec/read of a dir).
 * Shared by the loader and the exec handler's pre-commit probe. */
long tawcroot_open_in_view(const char *guest_path)
{
	if (tawcroot_rootfs_fd < 0)
		return tawc_openat(AT_FDCWD, guest_path,
				   O_RDONLY | O_CLOEXEC, 0);
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *suffix = scratch->buf[0];
	tawcroot_path_result r = tawcroot_path_translate(
		guest_path, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
		TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
	if (r.err) return r.err;
	if (suffix[0] == 0) return TAWC_EISDIR;
	return tawc_openat(r.base_fd, suffix, O_RDONLY | O_CLOEXEC, 0);
}

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

/* Hardlink emulation: readlink a link object by token (only consulted
 * on token hits — a hardlinked SYMLINK's target must continue the
 * guest-side walk instead of terminating in the store). A missing
 * store fd here means a token was met while LATENT: upgrade in place
 * (cold — token hits only) so the symlink-object splice, and the
 * orchestrator's rebase after us, both see the live store. */
static long prod_readlink_store(void *ctx, const char *token,
				char *out, size_t out_cap)
{
	(void)ctx;
	int fd = tawcroot_store_link_fd;
	if (fd < 0) fd = tawcroot_linkstore_latent_upgrade();
	if (fd < 0) return TAWC_ENOENT;
	return TAWC_RAW(TAWC_SYS_readlinkat, fd,
			(long)token, (long)out, (long)out_cap, 0, 0);
}

static const struct tawcroot_path_oracle prod_oracle = {
	.ctx            = 0,
	.readlink       = prod_readlink,
	.readlink_store = prod_readlink_store,
};

/* Kernel-cwd-to-guest-absolute resolver: read the kernel cwd via raw
 * `getcwd` and reverse-translate through the shared longest-prefix
 * walk (rootfs AND bind srcs — a `cd` into a bind dst leaves the
 * kernel cwd at the bind src; matching only the rootfs prefix here
 * made getcwd and every relative path fail ENOENT after such a cd).
 * Returns the written length, or -ENOENT if the cwd is outside the
 * view (we deliberately don't leak host paths through guest errors).
 * Public — the getcwd handler reverse-translates through the same
 * function so the two can't drift. */
long tawcroot_cwd_to_guest_abs(char *out, size_t out_cap)
{
	if (tawcroot_rootfs_host_path_len == 0) return TAWC_ENOENT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *cwd = scratch->buf[0];
	long r = TAWC_RAW(TAWC_SYS_getcwd, (long)cwd,
			  TAWCROOT_PATH_SCRATCH_SIZE,
			  0, 0, 0, 0);
	if (r < 0) return r;
	if ((size_t)r == 0) return TAWC_ENOENT;

	/* getcwd returns the length INCLUDING the NUL on aarch64+x86_64
	 * Linux. Older kernels may return without NUL — the manpage is
	 * fuzzy; we treat trailing NULs defensively. */
	size_t cwd_len = (size_t)r;
	while (cwd_len > 0 && cwd[cwd_len - 1] == 0) cwd_len--;

	return tawcroot_host_path_to_guest_abs(cwd, cwd_len, out, out_cap);
}

static long prod_cwd_to_guest_abs(void *ctx, char *out, size_t out_cap)
{
	(void)ctx;
	return tawcroot_cwd_to_guest_abs(out, out_cap);
}

/* ---------------------------------------------------------------- */
/* Read-only binds: impure companions to the orchestrator's central   */
/* check. Both run in tawcroot_path_translate below — the wrapper     */
/* every production translation passes through (handlers via          */
/* translate_local, sockets, chroot, exec/loader) — so a future call  */
/* site can't forget them. They stay out of the pure                  */
/* tawcroot_path_translate_with_ctx: each needs kernel ground truth   */
/* (a readlink / a stat) the ctx-driven unit tests don't model.       */

/* Fast gate: any RO surface in the current view at all? Skips the
 * per-call kernel probes for the (universal today) all-RW
 * configuration. Shared with the fd-based stage-2 checks in
 * syscalls_fs.c. */
int tawcroot_view_has_ro(void)
{
	if (tawcroot_root_ro) return 1;
	for (size_t i = 0; i < tawcroot_n_binds; i++)
		if (tawcroot_binds[i].active && tawcroot_binds[i].read_only)
			return 1;
	return 0;
}

/* 1 iff `fd` is the src fd of an active bind of host /proc — the one
 * place where a suffix can be a kernel magic link that re-opens an
 * inode with fresh access mode or resolves through to a host dir. */
static int fd_is_proc_bind(int fd)
{
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (!b->active || b->src_fd != fd) continue;
		return b->src_len == 5 && tawc_starts_with(b->src, "/proc");
	}
	return 0;
}

/* Stage 2, /proc magic links: refuse write-intent translations whose
 * final route is a /proc-bind magic link naming (or resolving through
 * to) an RO part of the view. A guest holding an RO-bind file open
 * O_RDONLY can ask for open("/proc/self/fd/<n>", O_RDWR) and the
 * KERNEL re-opens the inode writable (the host mount is RW, so it
 * won't refuse like a real RO mount); chmod through the same link, or
 * any write through self/cwd/<name> after a cd into an RO bind, walks
 * the same hole. This is the one check that cannot live in the
 * orchestrator — only the kernel knows the link target. Readlink
 * exactly the link prefix, join the resolve-through remainder, and
 * RO-prefix-check the final host path (so an RW bind nested under an
 * RO src still wins by longest prefix). */
static long ro_check_proc_magic_link(tawcroot_path_mode mode,
				     tawcroot_path_intent intent,
				     int base_fd, const char *suffix)
{
	if (intent == TAWCROOT_PATH_INTENT_READ &&
	    mode != TAWCROOT_PATH_PARENT_CREATE &&
	    mode != TAWCROOT_PATH_PARENT_REMOVE)
		return 0;
	if (!tawcroot_view_has_ro() || !fd_is_proc_bind(base_fd))
		return 0;
	size_t ml = tawcroot_proc_magic_link_prefix(suffix);
	char lnk[64];
	if (ml == 0 || ml >= sizeof lnk) return 0;
	for (size_t i = 0; i < ml; i++) lnk[i] = suffix[i];
	lnk[ml] = 0;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *tgt = scratch->buf[0];
	long ln = tawc_readlinkat(base_fd, lnk, tgt,
				  TAWCROOT_PATH_SCRATCH_SIZE);
	if (ln <= 0 || ln >= (long)TAWCROOT_PATH_SCRATCH_SIZE) return 0;
	size_t pos = (size_t)ln;
	if (suffix[ml]) {
		if (tgt[pos - 1] == '/') pos--;  /* self/root is "/" */
		size_t joined = pos;
		if (tawc_str_append(tgt, TAWCROOT_PATH_SCRATCH_SIZE, &joined,
				    suffix + ml) == 0)
			pos = joined;
		/* Overlong join: judge the link target alone. */
	}
	return tawcroot_host_path_in_ro_bind(tgt, pos) ? TAWC_EROFS : 0;
}

/* Central-EROFS errno fidelity: for FOLLOW/NOFOLLOW leaf-must-exist
 * writes (chmod/chown/utimensat/truncate/setxattr/open-for-write/
 * access(W_OK)) the kernel looks the target up BEFORE mnt_want_write,
 * so a missing target earns ENOENT/ENOTDIR, not EROFS. Probe with the
 * walk's own follow semantics. CREATE intent and the PARENT_* modes
 * keep EROFS — there the kernel wants write access before the leaf
 * lookup. Relies on the refusal being the orchestrator's LAST step,
 * with (base_fd, out_suffix) fully built. Cold by definition. */
static long ro_refusal_errno(int base_fd, const char *suffix,
			     tawcroot_path_mode mode,
			     tawcroot_path_intent intent)
{
	if (intent != TAWCROOT_PATH_INTENT_WRITE ||
	    (mode != TAWCROOT_PATH_FOLLOW && mode != TAWCROOT_PATH_NOFOLLOW))
		return TAWC_EROFS;
	struct stat probe;
	long pr = TAWC_RAW(TAWC_SYS_fstatat, base_fd,
			   (long)(suffix[0] ? suffix : "."), (long)&probe,
			   mode == TAWCROOT_PATH_NOFOLLOW
				   ? AT_SYMLINK_NOFOLLOW : 0,
			   0, 0);
	if (pr == TAWC_ENOENT || pr == TAWC_ENOTDIR) return pr;
	return TAWC_EROFS;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */

tawcroot_path_result tawcroot_path_translate(const char *guest_path,
					     char *out_suffix, size_t out_cap,
					     tawcroot_path_mode mode,
					     tawcroot_path_intent intent)
{
	tawcroot_path_result r;
	r.err     = 0;
	r.base_fd = -1;
	r.ro      = 0;

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
	 * against the snapshot we take here — see notes/tawcroot/path-translation.md
	 * §"chroot emulation" for the bounded-failure analysis. */

	struct tawcroot_path_translate_ctx ctx = {
		.rootfs_base_fd   = tawcroot_rootfs_fd,
		.rootfs_ro        = tawcroot_root_ro,
		.binds            = tawcroot_binds,
		.n_binds          = tawcroot_n_binds,
		.memos            = g_memo,
		.n_memos          = g_n_memo,
		.store_link_fd    = tawcroot_store_link_fd,
		.store_upgrade    = tawcroot_linkstore_latent_upgrade,
		.oracle           = &prod_oracle,
		.cwd_to_guest_abs = prod_cwd_to_guest_abs,
		.cwd_ctx          = 0,
	};
	r = tawcroot_path_translate_with_ctx(
		&ctx, guest_path, out_suffix, out_cap, mode, intent);
	/* RO-bind impure companions (see block comment above). The
	 * orchestrator's EROFS is the only error that reaches here with
	 * (base_fd, out_suffix) valid — every earlier error returns
	 * before the central check populates them. */
	if (r.err == TAWC_EROFS)
		r.err = ro_refusal_errno(r.base_fd, out_suffix, mode, intent);
	else if (r.err == 0)
		r.err = ro_check_proc_magic_link(mode, intent, r.base_fd,
						 out_suffix);
	return r;
}
