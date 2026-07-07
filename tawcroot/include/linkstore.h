/* Hardlink-emulation store (notes/tawcroot/link-emulation.md).
 *
 * Android untrusted_app SELinux denies link(2) on app_data_file (both
 * `file` and `lnk_file` classes — verified on the emulator). Full
 * emulation: a hardlinked file's data lives in a **link object** under
 * `<store>/link/<token>`, outside the guest tree; every guest name for
 * it is a symlink whose target is the opaque literal
 * `tawcroot:link:<token>`. tawcroot intercepts every consumer of those
 * symlinks so the guest sees N regular-file names sharing one inode.
 *
 * On-disk layout under the store dir (production:
 * `distros/<id>/tawcroot/`, a sibling of `rootfs/` on the same fs):
 *
 *   version           store format version (ASCII integer)
 *   lock              fcntl-record-lock file (created once, never
 *                     deleted)
 *   intent            single-slot crash journal (+ intent.new twin)
 *   link/<token>      link objects (hardlink cluster data)
 *   link/<token>.cnt  refcount sidecars (the only count store)
 *   work/<token>.<role>  transient staged/parked name symlinks
 *
 * Token = the source file's inode number in decimal (known before the
 * object rename, which preserves it), suffixed `-<k>` on collision
 * (host-level copies of a distro renumber inodes, so a fresh token can
 * collide with a stale baked object).
 *
 * Concurrency: every structural mutation runs under an in-process
 * futex mutex (threads of one process share fcntl lock ownership)
 * plus a whole-file fcntl(F_SETLKW) record lock on `lock` (kernel-
 * released on SIGKILL; fork/exec escape it in the safe direction —
 * children inherit the fd but never the lock, and O_CLOEXEC makes
 * exec release it). Readers are lock-free.
 *
 * Crash safety: monotone ordering (count is always >= live referrers;
 * increment before a name appears, decrement after it is gone) plus a
 * single-slot intent file written via rename before each multi-syscall
 * mutation. Recovery keys on store-internal state only (`work/`,
 * `link/`, the intent) — guest-tree paths move under lock-free renames
 * and are only ever touched by recovery after verifying the entry is
 * the matching token symlink. See notes/tawcroot/link-emulation.md for
 * the full window analysis.
 *
 * All functions are async-signal-safe (raw syscalls, fixed buffers,
 * no allocation) and callable from the SIGSYS handlers. */

#pragma once

#include <stddef.h>

#define TAWCROOT_LINK_TOKEN_PREFIX      "tawcroot:link:"
#define TAWCROOT_LINK_TOKEN_PREFIX_LEN  14
/* 20 digits (u64 ino) + "-" + collision counter + NUL, rounded up. */
#define TAWCROOT_LINK_TOKEN_MAX         32
/* Prefix all guest-authored symlink targets are checked against
 * (forgery guard): any target starting with "tawcroot:" is refused,
 * reserving the namespace for future in-band target formats too. */
#define TAWCROOT_LINK_GUARD_PREFIX      "tawcroot:"
#define TAWCROOT_LINK_GUARD_PREFIX_LEN  9

/* Store lifecycle state. */
enum tawcroot_store_state {
	TAWCROOT_STORE_OFF = 0,   /* no store path configured / init failed */
	TAWCROOT_STORE_LATENT,    /* path known; store dir absent — created
	                           * on the first mutation that needs it   */
	TAWCROOT_STORE_READY,     /* fds open, version supported           */
	TAWCROOT_STORE_DEGRADED,  /* version newer than supported: token
	                           * detection stays on, mutations refuse  */
};

/* link/ dirfd (reserved high-fd) when the store is open, else -1.
 * Handlers compare a translate result's base_fd against this to detect
 * "resolved into the store". Read lock-free from handlers. */
extern int tawcroot_store_link_fd;

/* Host path of the store dir (e.g. …/distros/<id>/tawcroot), ferried
 * through exec_state so --exec-child re-opens the same store even
 * after a guest chroot changed the rootfs view. Empty = OFF. */
extern char   tawcroot_store_host_path[4096];
extern size_t tawcroot_store_host_path_len;

/* st_dev of the open store (0 while closed): cheap early-out for
 * fd-based probes — link objects can only live on the store's fs. */
extern unsigned long tawcroot_store_dev;

/* Configure the store from supervisor init. `store_host_path` may be
 * NULL (emulation off — today's v1 symlink fallback stays). If the
 * store dir exists, opens+reserves the fds, checks the version, and
 * runs the O(1) session-start intent check (locks + repairs when a
 * previous holder died mid-operation). If it does not exist, the store
 * stays LATENT and is created on first use. Never fails hard: any
 * error degrades to OFF (raw EPERM link fallback path). */
void tawcroot_linkstore_configure(const char *store_host_path);

int tawcroot_linkstore_state(void);

/* Resolver-side LATENT upgrade: a token symlink was encountered while
 * this process still believes no store exists (it started before
 * another process created the very first link — e.g. a shell whose
 * `ln` child minted the store; without this the parent's next open of
 * the new name ENOENTs until it mutates or restarts). Mirrors
 * store_lock's in-place upgrade. Cold: callers only reach it on
 * actual token hits. Returns the link/ dirfd, or -1 (not LATENT /
 * store still absent / open failed). */
int tawcroot_linkstore_latent_upgrade(void);

/* Pure: does `target` (NUL-terminated symlink target text) carry the
 * opaque token form? On match returns 1 and points *tok_out at the
 * token bytes inside `target`. */
int tawcroot_link_target_is_token(const char *target, const char **tok_out);

/* Reactive leaf probe for NOFOLLOW handlers: readlinkat (dirfd,suffix)
 * once; returns 1 and fills `tok` when the entry is a token symlink,
 * 0 when it is anything else (including not-a-symlink and ENOENT),
 * <0 only on unexpected errors worth propagating. */
long tawcroot_link_leaf_token(int dirfd, const char *suffix,
			      char *tok, size_t tok_cap);

/* st_nlink for an emulated name: the sidecar count, or 2 when the
 * sidecar is missing/garbled (degraded but data-safe — such clusters
 * are also never deleted). */
unsigned long tawcroot_link_count_for_stat(const char *tok);

/* Does the host-absolute `path` name an object inside this store's
 * link/ dir (exactly `<store>/link/<token>`, no deeper components)?
 * On match copies the token into `tok` and returns 1. Pure string work
 * against the configured store path — linkat's mandatory source
 * backstop: fd-shaped source spellings (/proc/self/fd/N, AT_EMPTY_PATH)
 * reach the store without the resolver ever seeing a token target, and
 * NEW must never rename a store-resident source (it would rename the
 * object itself out of link/, dangling the whole cluster). Works for
 * `-k`-suffixed collision tokens because the token comes from the path,
 * not an ino lookup. */
int tawcroot_link_host_path_token(const char *path, char *tok,
				  size_t tok_cap);

/* Same shape for `<store>/tmp/<name>`: linkable-O_TMPFILE objects
 * awaiting publish. Names are the file's inode in decimal, so the
 * charset/length rules are the token ones. */
int tawcroot_link_host_path_tmp(const char *path, char *name,
				size_t name_cap);

/* Linkable O_TMPFILE emulation (plan: the publish idiom is a hardlink
 * of a NAMELESS inode — rename-based emulation needs a name, so the
 * "file" is created at tmp/<ino> and its fd returned). `dfd`/`dsuf`
 * locate the guest's directory operand — only stat-probed, to
 * reproduce the kernel's ENOENT/ENOTDIR; the file itself lives in the
 * store. `flags` are the guest's, minus the O_TMPFILE bits. Returns a
 * guest-usable fd, -errno, or -EAGAIN as the "no store — caller falls
 * through to the passthrough open" sentinel (scratch keeps working;
 * a later publish attempt gets the documented EXDEV). */
long tawcroot_link_tmpfile_open(int dfd, const char *dsuf, int flags,
				int mode);

/* Publish a tmp/ object at a guest name: one atomic NOREPLACE rename —
 * linkat's EEXIST for free, no lock, no intent (tmp entries are
 * uncounted scratch), and the caller's still-open fd keeps referencing
 * the same inode at its new home. */
long tawcroot_link_publish_tmp(const char *name, int dfd, const char *dsuf);

/* Age-gated stray-temp sweep, run once per TOP-LEVEL session start
 * (never from --exec-child: it runs per guest exec). Eager sweeping is
 * unsound — the session model does not guarantee guest teardown
 * (init-reparented exec-broker descendants outlive sessions,
 * concurrent sessions exist; spike-verified), so a surviving guest's
 * live temp would be unlinked under it. Entries whose mtime is older
 * than 7 days are strays (live temps take writes, refreshing mtime);
 * the worst mistake degrades that temp's eventual publish to EXDEV —
 * no data loss. O(one directory). */
void tawcroot_linkstore_tmp_sweep(void);

/* Mutations. All take the store lock, write the intent slot, run the
 * monotone-ordered syscall sequence, clear, unlock. Return 0 or a
 * kernel-shaped -errno for the guest.
 *
 * add: new name (dfd,dsuf) for the existing cluster `tok`
 *      (linkat with an emulated source). -EEXIST when dst exists.
 * del: remove emulated name (dfd,dsuf) referencing `tok`
 *      (unlinkat). The parked entry is verified to be the matching
 *      token symlink before the count moves; a mismatched entry (the
 *      name was swapped by a racing rename) is unlinked without a
 *      decrement — same observable as unlinking the swapped file.
 * new: first link — src (sfd,ssuf) becomes the object, both names
 *      become token symlinks. -EEXIST when dst exists; -EXDEV when
 *      src cannot reach the store by rename (cross-fs bind) — caller
 *      falls back to the v1 symlink emulation. */
long tawcroot_link_add(const char *tok, int dfd, const char *dsuf);
long tawcroot_link_del(const char *tok, int dfd, const char *dsuf);
long tawcroot_link_new(int sfd, const char *ssuf, int dfd, const char *dsuf);

/* rename onto an emulated name (CLOBBER). Performs the host rename
 * under the store lock, then decrements `tok`'s count (deleting the
 * object at zero). Writes no intent — see the plan (an intent record
 * is actively harmful here); crash windows degrade to a +1 overcount
 * or a count-0 orphan, both safe. `rflags` are the guest's renameat2
 * flags (never NOREPLACE/EXCHANGE — the handler routes those to plain
 * passthrough). The clobbered token is re-probed under the lock (the
 * handler's probe was lock-free), so no token argument is taken; a dst
 * that no longer reads as a token symlink degrades to a plain rename. */
long tawcroot_link_clobber(int ofd, const char *osuf,
			   int nfd, const char *nsuf,
			   unsigned int rflags);

/* Test hook: run the session-start intent check (lock + recover if a
 * slot is pending). Used by the kill-matrix harness; production calls
 * this from configure. Returns 0 or -errno. */
long tawcroot_linkstore_recover_now(void);
