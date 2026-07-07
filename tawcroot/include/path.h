/* Path translation.
 *
 * Current surface, intentionally narrow: a single translator that takes
 * a guest-absolute path and yields a (base_fd, suffix) pair we can use
 * with `*at` syscalls. Bind-mount lookup, `..`/symlink clamping, and
 * fd-relative resolution all hang off the same return shape — see
 * notes/tawcroot/path-translation.md "Path translation".
 *
 * No allocations. The caller passes a stack buffer for the suffix; we
 * write the (NUL-terminated) result into it. That keeps the API
 * handler-safe.
 */

#pragma once

#include <stddef.h>

/* The CURRENT-ROOT fd kept O_PATH | O_DIRECTORY by init. Initially
 * the rootfs the supervisor opened; replaced by `tawcroot_chroot` on a
 * successful guest `chroot(2)`. Reserved into the high-fd range so the
 * guest can't close it.
 *
 * Every other piece of state in this header (rootfs_host_path, binds[],
 * the well-known-symlink memo) is co-named "current root view" and
 * updates atomically (well, in straight-line order) when chroot swaps
 * the root. See notes/tawcroot/path-translation.md §"chroot emulation". */
extern int tawcroot_rootfs_fd;

/* Result of translating a guest path:
 *   base_fd  — caller passes this as `dirfd` to *at syscalls
 *   suffix   — caller passes this as `pathname` (relative to base_fd).
 *              Empty string means "the directory referred to by base_fd
 *              itself" (use AT_EMPTY_PATH).
 * The translator writes `suffix` into the caller-provided buffer.
 *
 * Outcomes:
 *   - Path inside rootfs view  → base_fd = rootfs_fd, suffix = path[1..]
 *   - Path is "/" (the rootfs) → base_fd = rootfs_fd, suffix = ""
 *   - Path under a bind dst    → base_fd = bind src_fd, suffix relative
 *                                to the bind src
 *   - Otherwise: -errno (we deliberately don't escape outside the view).
 */
typedef struct {
	int   base_fd;
	long  err;       /* 0 on success, -errno otherwise */
	int   ro;        /* final route landed on a read-only bind (or an
	                  * RO root view). Informational for the few
	                  * fidelity consumers (statfs ST_RDONLY, linkat's
	                  * src EXDEV) — NEVER for enforcement, which
	                  * already happened inside the translator. */
} tawcroot_path_result;

/* Resolution mode — see notes/tawcroot/path-translation.md §"Translation rules" for the
 * full semantics. The mode parameterizes how the FINAL component of a
 * path is treated; parent components are always followed. The symlink
 * walker (path_resolve.c) varies leaf handling across modes; the
 * string-level fold treats all modes alike except for the
 * well-known-symlink memoizer, which avoids rewriting a path whose
 * SOLE component is a memoized symlink under NOFOLLOW/PARENT_*.
 *
 *  - FOLLOW         : default. open without O_NOFOLLOW, stat, access,
 *                     chmod, chdir, exec target. Resolve every
 *                     component including the final.
 *  - NOFOLLOW       : lstat, readlink, openat(O_NOFOLLOW),
 *                     fstatat(...,AT_SYMLINK_NOFOLLOW), fchownat(...,
 *                     AT_SYMLINK_NOFOLLOW). Resolve parents only;
 *                     pass the final component through literally.
 *  - PARENT_CREATE  : mkdir, mknod, symlink dst, openat(O_CREAT) for
 *                     a non-existent leaf. Resolve parents; the leaf
 *                     may not exist and must not be followed.
 *  - PARENT_REMOVE  : unlink, rmdir, rename dst. Resolve parents;
 *                     preserve leaf-op kernel semantics (unlink
 *                     removes the symlink itself, etc.).
 */
typedef enum {
	TAWCROOT_PATH_FOLLOW         = 0,
	TAWCROOT_PATH_NOFOLLOW       = 1,
	TAWCROOT_PATH_PARENT_CREATE  = 2,
	TAWCROOT_PATH_PARENT_REMOVE  = 3,
} tawcroot_path_mode;

/* Caller-declared access intent, enforced against read-only binds at
 * the single point in tawcroot_path_translate_with_ctx (after the
 * final bind route is known). Deliberately zero-defaulting to WRITE:
 * a forgotten/zero-initialized intent in future code degrades to a
 * visible -EROFS on RO binds — a testable functional break — never a
 * silent write-through. The two mutating modes (PARENT_CREATE /
 * PARENT_REMOVE) force write intent inside the translator regardless
 * of the declaration, so create/remove/rename syscalls are covered
 * even by a mislabeled call site. */
typedef enum {
	TAWCROOT_PATH_INTENT_WRITE = 0,  /* mutates metadata/namespace/data */
	TAWCROOT_PATH_INTENT_READ  = 1,  /* observes only */
} tawcroot_path_intent;

/* Translate a guest path. `out_suffix` must point to a buffer of at
 * least `out_cap` bytes (PATH_MAX is sane). On success, the suffix is
 * NUL-terminated; on failure, contents are unspecified.
 *
 * Absolute paths are clamped to the rootfs view: leading `/` is
 * stripped, internal `..` components are reduced, and a `..` that
 * would escape the rootfs gets clamped at the root (proot-compatible:
 * `/foo/../../etc` becomes `etc`).
 *
 * Relative paths (no leading `/`) reverse-translate through the
 * kernel cwd, then re-run the absolute translator on the joined path.
 *
 * `mode` controls final-component handling — see `tawcroot_path_mode`.
 * `intent` declares read vs write access for the RO-bind check (see
 * `tawcroot_path_intent`); a write route into an RO bind is -EROFS. */
tawcroot_path_result tawcroot_path_translate(const char *guest_path,
					     char *out_suffix, size_t out_cap,
					     tawcroot_path_mode mode,
					     tawcroot_path_intent intent);

/* Configure paths to the host root and to the in-rootfs `/proc` mirror
 * for reverse-translation. Set at init from main.c / rootfs_smoke.c. */
extern char tawcroot_rootfs_host_path[4096];
extern size_t tawcroot_rootfs_host_path_len;

/* Bind-mount table. One entry per `-b src:dst` on the command line.
 * Resolution: after the absolute path is folded, longest-prefix-match
 * against the bind dst paths; on match, base_fd = bind.src_fd and the
 * suffix is the bytes after bind.dst.
 *
 * Fixed size — no malloc reachable from the handler. The current cap
 * comfortably fits the proot-style mount set (/system /vendor /apex
 * /system_ext /linkerconfig /dev /proc /sys + a couple of app-data
 * passthroughs). Bumps go through this header. */
#define TAWCROOT_MAX_BINDS 32

struct tawcroot_bind {
	int    src_fd;               /* O_PATH | O_DIRECTORY of the host src */
	int    read_only;            /* 1 → write-intent translations into
	                              * this bind refuse with -EROFS (checked
	                              * centrally in path_orchestrate.c).
	                              * Survives chroot re-anchoring (structs
	                              * are edited in place). */
	int    active;               /* 0 → ignored by every iterator; the
	                              * bind has been re-anchored out of the
	                              * current root view by chroot. The
	                              * src_fd stays reserved (un-closeable
	                              * by guest) but routes nothing.         */
	size_t dst_len;              /* length of `dst`, excluding NUL        */
	char   dst[256];             /* current-root-relative dst (NO leading
	                              * '/'). Re-anchored on chroot.          */
	size_t src_len;              /* length of `src`, excluding NUL. 0 ⇒
	                              * src not populated (test fixtures);
	                              * dirfd_to_guest_abs skips such binds.  */
	char   src[256];             /* canonicalized host src path (kernel
	                              * view via /proc/self/fd of src_fd at
	                              * add-bind time) — used by
	                              * dirfd_to_guest_abs for reverse-
	                              * translating bind-src dirfds, and
	                              * ferried through to --exec-child so
	                              * the new tawcroot can re-open it.
	                              * Stable across chroot — host fs doesn't
	                              * move.                                 */
};

extern struct tawcroot_bind tawcroot_binds[TAWCROOT_MAX_BINDS];
extern size_t               tawcroot_n_binds;

/* Read-only bit for the CURRENT root view itself. 0 at init (the
 * rootfs is always writable); set by the chroot handler when the guest
 * chroots INTO an RO bind dst — after that swap the whole root view IS
 * the bind src, but routing goes through tawcroot_rootfs_fd rather
 * than the bind table, so the flag must live beside the other
 * current-root-view globals. Ferried through exec_state (root_ro)
 * because a guest can exec after chrooting. */
extern int tawcroot_root_ro;

/* Add a bind. `dst` is normalized through the lexical fold (leading
 * '/' optional; trailing '/', '//' runs, and '.'/'..' components are
 * collapsed) so it matches folded suffixes at lookup time. Returns 0
 * on success, -errno on failure (no slot left, src open failed, dst
 * too long or empty after folding). Must be called BEFORE the seccomp
 * filter is installed; the call opens the src dir via raw `openat`.
 * The new bind is added with `active = 1`. Non-zero `read_only` marks
 * the bind RO (write-intent translations into it are -EROFS). */
long tawcroot_path_add_bind(const char *src_host, const char *dst_guest,
                            int read_only);

/* Re-anchor every bind in `binds[0..n_binds]` for a chroot to
 * `new_root_host`.
 *
 * For each currently-active bind, compute its host path as
 * `<cur_root_host>/<dst>`. After the chroot:
 *   - host path equals `new_root_host` (component-boundary check)
 *     — the bind WAS the new root. Mark inactive (the rootfs_fd
 *     swap covers the path; the bind is now redundant).
 *   - host path starts with `new_root_host + "/"` — strip the
 *     prefix; remainder is the new dst. Stays active.
 *   - otherwise — bind sits outside the new view. Mark inactive.
 *
 * Pure on bind state — no syscalls, no globals. Caller passes the
 * array (production hands `tawcroot_binds`, tests build their own).
 *
 * `cur_root_host` and `new_root_host` are NUL-terminated absolute
 * paths. Returns 0 on success, -ENAMETOOLONG if any surviving bind's
 * stripped dst would overflow the dst buffer (in which case that
 * bind is marked inactive too — other binds keep being processed,
 * one bad bind doesn't fail the whole call; the return value just
 * surfaces the worst case for caller-side visibility). */
long tawcroot_path_binds_reanchor(struct tawcroot_bind *binds, size_t n_binds,
                                  const char *cur_root_host, size_t cur_root_host_len,
                                  const char *new_root_host, size_t new_root_host_len);

/* Initialize the well-known-symlink memoization cache. Reads symlinks
 * for `lib`, `lib64`, `bin`, `sbin`, `var/run`, etc. from the rootfs
 * and stores their targets in a small fixed-size table. After this
 * runs, path translation will rewrite a guest path whose first
 * component matches a memoized symlink to the target form before
 * resolving against the rootfs fd.
 *
 * No-op for prefixes that aren't symlinks. Call after rootfs_fd is
 * set up and before the seccomp filter is installed. */
void tawcroot_path_memoize_well_known(void);

/* Lexical absolute-path canonicalizer (no symlink resolution; just `.`
 * and `..` folding with root-clamp). Input must start with '/' and be
 * NUL-terminated; output is rootfs-relative (no leading '/') in the
 * caller-supplied buffer. Returns 0 on success, -errno on failure
 * (-ENAMETOOLONG on overflow, -ENOENT on caller misuse). Public so
 * `path_resolve.c` can re-fold after splicing a symlink target. */
long tawcroot_path_fold_absolute(const char *path, char *out, size_t out_cap);

/* Read the canonical kernel-side host path of `fd` via /proc/self/fd/<fd>.
 * Writes a NUL-terminated string into `out`, strips trailing slashes
 * (matches the supervisor's invariant), and returns the length on
 * success. -EINVAL if the readlink yields an empty / non-absolute
 * result (typically /proc not mounted), -ENAMETOOLONG if `out_cap` is
 * too small. Used at chroot, fd-relative path-translation, and rootfs
 * init. */
long tawcroot_proc_fd_to_host_path(int fd, char *out, size_t out_cap);

/* Reverse-translate an fd visible to the guest into a guest-absolute path.
 * The fd may point into the rootfs or an active bind source. Output starts
 * with "/" and is NUL-terminated. */
long tawcroot_fd_to_guest_abs(int fd, char *out, size_t out_cap);

/* Reverse-translate a host path `host[0..n)` into a guest-absolute path
 * by longest-prefix match against the rootfs host path and the active
 * bind srcs. Output starts with "/" and is NUL-terminated; returns the
 * written length, -ENOENT when the host path is outside the view,
 * -ENAMETOOLONG on overflow. Shared by fd reverse-translation (dirfds),
 * getcwd, and relative-path cwd resolution so all three agree on what
 * "inside the view" means — getcwd previously matched only the rootfs
 * prefix, so a chdir into a bind dst broke every relative path. */
long tawcroot_host_path_to_guest_abs(const char *host, size_t n,
				     char *out, size_t out_cap);

/* True iff the HOST path `host[0..n)` lies (by longest-prefix match
 * against the rootfs host path and active bind srcs) in a read-only
 * part of the view: an RO bind src, or the rootfs itself while
 * tawcroot_root_ro is set. Nested RW-under-RO wins by longest prefix,
 * same as translation. Outside-view paths are 0 (not ours to police).
 * Stateless ground truth for the fd-based metadata residue (stage 2):
 * callers feed it the /proc/self/fd readlink of the fd in question —
 * no taint table, nothing to propagate, works for inherited and
 * SCM_RIGHTS-passed fds. */
int tawcroot_host_path_in_ro_bind(const char *host, size_t n);

/* Reverse-translate the kernel cwd into a guest-absolute path.
 * Returns the written length, -ENOENT when the cwd is outside the
 * view. Shared by the getcwd handler and relative-path translation. */
long tawcroot_cwd_to_guest_abs(char *out, size_t out_cap);

/* Open a guest path read-only through the current root view (host-fs
 * fallback when no rootfs is configured). Returns an O_RDONLY|O_CLOEXEC
 * fd or -errno; -EISDIR when the path names the rootfs/bind dir itself. */
long tawcroot_open_in_view(const char *guest_path);

/* `/proc/self/exe` synthesis (phase 2e).
 *
 * After manual-load the kernel's view of /proc/self/exe is the
 * libtawcroot binary, not the path the guest asked us to exec.
 * Programs that resolve $ORIGIN, parse argv[0] sanity-checking against
 * /proc/self/exe, or symlink-resolve to find their installed prefix
 * (Firefox, glibc dlopen) need the guest-visible path back. We stash
 * it here at production init and the readlinkat handler returns it
 * when the guest queries /proc/self/exe or /proc/<our-pid>/exe.
 *
 * Set BEFORE the seccomp filter is installed (or before the loader
 * jumps); read from the SIGSYS handler. The buffer is fixed-size and
 * immutable post-init — handler-safe per notes/tawcroot/sigsys-handler.md
 * "Threading and `vfork` invariants". */
extern char   tawcroot_guest_exe_path[4096];
extern size_t tawcroot_guest_exe_path_len;

/* Set the guest exe path. Called by production main / loader after
 * the guest's requested binary path is known. Truncates silently if
 * the path is longer than the buffer. */
void tawcroot_set_guest_exe_path(const char *path);

/* Host-side view of /proc/self/exe at production init: the absolute
 * filesystem path of `libtawcroot.so` itself (the loader binary).
 * Stashed once so the readlink handlers can substitute the guest exe
 * path whenever a query routes through `/proc/self/fd/<n>` and lands
 * on the loader's own host path — the bypass that breaks Firefox's
 * XPCOM lookup via glibc `realpath("/proc/self/exe")`. Empty if the
 * init readlink failed; in that case the substitution is skipped and
 * callers see the raw host path (the pre-fix behaviour). */
extern char   tawcroot_self_host_path[4096];
extern size_t tawcroot_self_host_path_len;

/* Read /proc/self/exe and stash it in `tawcroot_self_host_path`.
 * Called from `tawcroot_main` (production entry, after
 * `prod_rootfs_init` and before `tawcroot_loader_exec`) and from
 * `tawcroot_loader_exec_child` (post-execveat re-init, since execve
 * clears our globals and the fresh incarnation needs the path
 * re-stashed). Idempotent. */
void tawcroot_init_self_host_path(void);
