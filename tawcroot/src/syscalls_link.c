/* linkat / renameat handlers: the hardlink-emulation front end
 * (notes/tawcroot/link-emulation.md). The store itself lives in
 * linkstore.c. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "linkstore.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "syscalls_fs.h"
#include "syscalls_fs_internal.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

/* v1 link fallback tail (pre-store emulation, kept for stores that
 * cannot exist: no store path configured, store dir uncreatable, or a
 * cross-fs bind source that cannot reach the store by rename): move
 * the real file to the NEW name (RENAME_NOREPLACE preserves link()'s
 * EEXIST) and leave a guest-absolute symlink at the OLD name. The
 * direction matters: the link(tmp, final) + unlink(tmp) publish idiom
 * (git object/pack finalize) must leave real data at the final name.
 * `orig_rv` is the host linkat's failure, reported when the fallback
 * itself cannot proceed. */
static long link_fallback_v1(int src_fd, const char *src_suf,
			     int dst_fd, const char *dst_suf, long orig_rv)
{
	long re = TAWC_RAW(TAWC_SYS_renameat2, src_fd, (long)src_suf,
			   dst_fd, (long)dst_suf, RENAME_NOREPLACE, 0);
	/* EEXIST/EXDEV are errors link() itself defines — surface them.
	 * Anything else means the emulation can't work here; report the
	 * original link failure. */
	if (re == TAWC_EEXIST || re == TAWC_EXDEV) return re;
	if (re) return orig_rv;
	/* Compose the back-symlink's GUEST-absolute target by reverse-
	 * translating the destination's host path. "/" + dst_suf is only
	 * right when dst_fd is the rootfs — for a destination under a
	 * bind the suffix is bind-relative, and the mis-aimed symlink
	 * would dangle the original name (the publish idiom then unlinks
	 * the only working path to the data). A destination we cannot
	 * reverse-translate gets the rollback, not a wrong target. */
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *host       = scratch->buf[0];
	char *abs_target = scratch->buf[1];
	long se = TAWC_EINVAL;
	long hn = tawcroot_proc_fd_to_host_path(dst_fd, host,
						TAWCROOT_PATH_SCRATCH_SIZE);
	if (hn > 0) {
		size_t pos = (size_t)hn;
		if (!tawc_str_append(host, TAWCROOT_PATH_SCRATCH_SIZE,
				     &pos, "/") &&
		    !tawc_str_append(host, TAWCROOT_PATH_SCRATCH_SIZE,
				     &pos, dst_suf) &&
		    tawcroot_host_path_to_guest_abs(host, pos, abs_target,
						    TAWCROOT_PATH_SCRATCH_SIZE)
			    > 0)
			se = TAWC_RAW(TAWC_SYS_symlinkat, (long)abs_target,
				      src_fd, (long)src_suf, 0, 0, 0);
	}
	if (!se) return 0;
	/* Symlink-back failed: roll the rename back so the failed link
	 * leaves the tree unchanged. */
	TAWC_RAW(TAWC_SYS_renameat2, dst_fd, (long)dst_suf,
		 src_fd, (long)src_suf, 0, 0);
	return orig_rv;
}

/* True when a translate result landed inside the guest view but NOT
 * the rootfs — i.e. on a bind src dirfd. Emulated names must not be
 * planted there: the orchestrator skips the symlink resolver for
 * bind-routed paths, so a token symlink inside a bind is unresolvable
 * on FOLLOW opens (the data would be marooned in the store while
 * lstat claims a regular file). linkat instead degrades: NEW takes
 * the v1 fallback (both names stay real/openable), ADD returns EXDEV
 * (tools fall back to copy). Guest-supplied dirfd passthroughs are
 * not reserved and fall through unchanged. */
static int fs_path_in_bind(const struct fs_path *t)
{
	return t->fd != tawcroot_rootfs_fd &&
	       t->fd != tawcroot_store_link_fd &&
	       tawcroot_fd_is_reserved(t->fd);
}

/* linkat AT_EMPTY_PATH source: the file `olddirfd` itself refers to.
 * A store-resident fd (an open link object) is detected BEFORE the
 * host attempt — a host linkat from an object fd would mint an
 * uncounted referrer on any device whose policy allows it. A named
 * in-view source emulates via NEW on its host-real path (the fd's
 * /proc link tracks renames, so this is the current name); a nameless
 * source (memfd, O_TMPFILE, fully unlinked) gets a deliberate EXDEV
 * per the plan (accepted until the tmp/ stage lands).
 *
 * The host attempt's emulation gate includes ENOENT: the kernel
 * refuses unprivileged AT_EMPTY_PATH linkat with ENOENT (the
 * CAP_DAC_READ_SEARCH check), not EPERM — and uses ENOENT for
 * nlink==0 sources too, which the fstat below routes to EXDEV. */
static long linkat_empty_path(struct tawcroot_path_scratch *scratch,
			      int olddirfd, int newdirfd,
			      const char *newpath, int flags)
{
	if (tawcroot_fd_is_reserved(olddirfd)) return TAWC_EBADF;

	struct fs_path tnew;
	long e = tawcroot_fs_translate_at(scratch, 0, newdirfd, newpath,
					  TAWCROOT_PATH_PARENT_CREATE,
					  TAWCROOT_PATH_INTENT_WRITE, &tnew);
	if (e) return e;
	if (tnew.is_root) return TAWC_ENOENT;

	/* Same LATENT upgrade as handle_linkat: the host-path compares
	 * below need the canonical store path only store_open derives. */
	if (tawcroot_store_link_fd < 0 &&
	    tawcroot_linkstore_state() == TAWCROOT_STORE_LATENT)
		(void)tawcroot_linkstore_latent_upgrade();

	char *hostp = scratch->buf[2];
	long hn = tawcroot_proc_fd_to_host_path(olddirfd, hostp,
						TAWCROOT_PATH_SCRATCH_SIZE);
	if (hn <= 0) hostp[0] = 0;

	/* RO-bind source (AT_EMPTY_PATH spelling): a same-fs host linkat
	 * would mint a rootfs-named hardlink whose content stays writable —
	 * the classic RO-bind hardlink escape. EXDEV, like the path form
	 * below (tools degrade to copy). Same stateless host-path ground
	 * truth as the stage-2 fd checks. */
	if (hn > 0 && tawcroot_host_path_in_ro_bind(hostp, (size_t)hn))
		return TAWC_EXDEV;

	char tok[TAWCROOT_LINK_TOKEN_MAX];
	if (tawcroot_link_host_path_token(hostp, tok, sizeof tok)) {
		if (tawcroot_linkstore_state() != TAWCROOT_STORE_READY)
			return TAWC_EPERM;
		/* Emulated names never land in binds (fs_path_in_bind). */
		if (fs_path_in_bind(&tnew)) return TAWC_EXDEV;
		return tawcroot_link_add(tok, tnew.fd, tnew.path);
	}
	/* Linkable O_TMPFILE fd: publish (the idiom this flag exists
	 * for — linkat(fd, "", dst, AT_EMPTY_PATH)). */
	if (tawcroot_link_host_path_tmp(hostp, tok, sizeof tok))
		return tawcroot_link_publish_tmp(tok, tnew.fd, tnew.path);

	long rv = TAWC_RAW(TAWC_SYS_linkat, olddirfd, (long)"",
			   tnew.fd, (long)tnew.path, flags, 0);
	if (rv == 0 ||
	    (rv != TAWC_EACCES && rv != TAWC_EPERM && rv != TAWC_ENOENT))
		return rv;

	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, olddirfd, (long)&st, 0, 0, 0, 0) != 0)
		return rv;
	if (S_ISDIR(st.st_mode)) return rv;
	if (st.st_nlink == 0 || hostp[0] != '/') return TAWC_EXDEV;

	/* The /proc link is best-effort: verify it still names THIS inode
	 * (a racing rename/unlink or a " (deleted)" suffix must not make
	 * NEW move some other file into the store), and that it lies
	 * inside the guest view (rootfs or a bind) — moving an arbitrary
	 * app-reachable host file into the store would strand a token
	 * symlink nothing outside tawcroot can read. */
	struct stat pst;
	if (TAWC_RAW(TAWC_SYS_fstatat, AT_FDCWD, (long)hostp, (long)&pst,
		     AT_SYMLINK_NOFOLLOW, 0, 0) != 0 ||
	    pst.st_dev != st.st_dev || pst.st_ino != st.st_ino)
		return TAWC_EXDEV;
	char *gview = scratch->buf[3];
	if (tawcroot_host_path_to_guest_abs(hostp, (size_t)hn, gview,
					    TAWCROOT_PATH_SCRATCH_SIZE) <= 0)
		return TAWC_EXDEV;

	/* Either name landing in a bind: v1, not token symlinks
	 * (fs_path_in_bind). The source side is judged by host path —
	 * in-view but not under the rootfs prefix means a bind. */
	int src_in_bind =
		!(hn >= (long)tawcroot_rootfs_host_path_len &&
		  memcmp(hostp, tawcroot_rootfs_host_path,
			 tawcroot_rootfs_host_path_len) == 0 &&
		  ((size_t)hn == tawcroot_rootfs_host_path_len ||
		   hostp[tawcroot_rootfs_host_path_len] == '/'));
	if (fs_path_in_bind(&tnew) || src_in_bind)
		return link_fallback_v1(AT_FDCWD, hostp,
					tnew.fd, tnew.path, rv);

	switch (tawcroot_linkstore_state()) {
	case TAWCROOT_STORE_READY:
	case TAWCROOT_STORE_LATENT: {
		long nrv = tawcroot_link_new(AT_FDCWD, hostp,
					     tnew.fd, tnew.path);
		if (nrv == TAWC_EPERM &&
		    tawcroot_linkstore_state() == TAWCROOT_STORE_DEGRADED)
			return rv;
		if (nrv == TAWC_EXDEV || nrv == TAWC_EPERM)
			return link_fallback_v1(AT_FDCWD, hostp,
						tnew.fd, tnew.path, rv);
		return nrv;
	}
	case TAWCROOT_STORE_DEGRADED:
		return rv;
	default:
		return link_fallback_v1(AT_FDCWD, hostp,
					tnew.fd, tnew.path, rv);
	}
}

/* linkat: translate both operands, then emulate.
 *
 * Order matters (notes/tawcroot/link-emulation.md):
 *   1. Emulated-name source is detected BEFORE the host attempt —
 *      symlinks are a different SELinux class (lnk_file), so a host
 *      linkat could *succeed* and hardlink the token symlink itself:
 *      a phantom referrer the count never learns about. (The emulator
 *      policy denies lnk_file link too, but other devices may not.)
 *   2. A source already inside the store is store-aware: ADD with the
 *      token from the path. The resolver catches token names in the
 *      rootfs view (FOLLOW lands on link/); the O_PATH backstop below
 *      catches every other FOLLOW spelling (/proc/self/fd/N through a
 *      /proc bind is the live one). NEW must never rename a
 *      store-resident source — that would rename the object itself
 *      out of link/, dangling the whole cluster.
 *   3. Otherwise host linkat first; only EACCES/EPERM engages the
 *      emulation, and a directory source keeps the kernel's EPERM. */
static long handle_linkat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int          olddirfd = (int)args->a;
	const char  *oldpath  = (const char *)(uintptr_t)args->b;
	int          newdirfd = (int)args->c;
	const char  *newpath  = (const char *)(uintptr_t)args->d;
	int          flags    = (int)args->e;

	/* AT_SYMLINK_FOLLOW (0x400) selects whether the source is followed
	 * if it's a symlink. Default: don't follow (link to the symlink).
	 * The destination is a new entry → PARENT_CREATE. */
	tawcroot_path_mode src_mode = (flags & 0x400 /*AT_SYMLINK_FOLLOW*/)
		? TAWCROOT_PATH_FOLLOW
		: TAWCROOT_PATH_NOFOLLOW;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);

	if (flags & AT_EMPTY_PATH) {
		long empty = tawcroot_fs_path_is_empty(oldpath);
		if (empty < 0) return empty;
		if (empty)
			return linkat_empty_path(scratch, olddirfd,
						 newdirfd, newpath, flags);
	}

	/* Source translates with READ intent — translation must succeed
	 * for us to learn told.ro; the write-refusal decision is the
	 * handler-level EXDEV below, not the central EROFS. The dst's
	 * PARENT_CREATE is forced write, so both-in-RO EROFSes there
	 * first. */
	struct fs_path told, tnew;
	long e1 = tawcroot_fs_translate_at(scratch, 0, olddirfd, oldpath,
					   src_mode, TAWCROOT_PATH_INTENT_READ,
					   &told);
	if (e1) return e1;
	long e2 = tawcroot_fs_translate_at(scratch, 2, newdirfd, newpath,
					   TAWCROOT_PATH_PARENT_CREATE,
					   TAWCROOT_PATH_INTENT_WRITE, &tnew);
	if (e2) return e2;
	/* Kernel: a root/empty operand to link is ENOENT (the empty-name
	 * lookup), not EINVAL. */
	if (told.is_root || tnew.is_root) return TAWC_ENOENT;

	/* RO-bind source: refuse with EXDEV, deliberately. Kernel-faithful
	 * for every cross-fs RO bind (system partitions, shared storage;
	 * `cp -al` and git degrade to copy on EXDEV). For a SAME-fs RO
	 * bind the host linkat would succeed and hand out a rootfs-named
	 * hardlink whose content is then writable — the classic
	 * RO-bind-mount hardlink escape — so we diverge from kernel
	 * fidelity on purpose and refuse. Documented divergence
	 * (notes/tawcroot/path-translation.md §"Read-only binds"). */
	if (told.ro) return TAWC_EXDEV;

	/* A LATENT process may be running beside one that already minted
	 * the store (it is created lazily, so any long-lived guest that
	 * started before the distro's first emulated hardlink is LATENT).
	 * Open it BEFORE source detection: with the store fds closed the
	 * probes below all skip, and an existing token name would be
	 * treated as a plain symlink source — a phantom referrer where
	 * host policy allows lnk_file links, a nested token object (both
	 * names ENOENT) where it doesn't. Cold: linkat only, and one
	 * existence probe per call while LATENT. */
	if (tawcroot_store_link_fd < 0 &&
	    tawcroot_linkstore_state() == TAWCROOT_STORE_LATENT)
		(void)tawcroot_linkstore_latent_upgrade();

	if (tawcroot_store_link_fd >= 0) {
		int  ready = tawcroot_linkstore_state()
			     == TAWCROOT_STORE_READY;
		char tok[TAWCROOT_LINK_TOKEN_MAX];
		const char *add_tok = 0;
		if (told.fd == tawcroot_store_link_fd) {
			/* FOLLOW resolution landed in the store. A residual
			 * '/' means a token was used as a directory. */
			for (const char *q = told.path; *q; q++)
				if (*q == '/') return TAWC_ENOTDIR;
			add_tok = told.path;
		} else if (src_mode == TAWCROOT_PATH_NOFOLLOW &&
			   tawcroot_link_leaf_token(told.fd, told.path, tok,
						    sizeof tok) == 1) {
			add_tok = tok;
		} else if (src_mode == TAWCROOT_PATH_FOLLOW) {
			/* Mandatory backstop: FOLLOW spellings the resolver
			 * never sees can still land on an object —
			 * /proc/self/fd/N routes through the /proc bind
			 * (binds skip the resolver) and its magic-link
			 * target is the raw store path. O_PATH-open the
			 * source (kernel chases the leaf, magic links
			 * included) and compare its host path against
			 * <store>/link/. Guest-authored absolute symlinks
			 * can't forge this: the resolver folds their
			 * targets into the guest view before we get here. */
			long pfd = tawc_openat(told.fd, told.path,
					       O_PATH | O_CLOEXEC, 0);
			if (pfd >= 0) {
				char *hostp = scratch->buf[0];
				long hn = tawcroot_proc_fd_to_host_path(
					(int)pfd, hostp,
					TAWCROOT_PATH_SCRATCH_SIZE);
				tawc_close((int)pfd);
				if (hn > 0 &&
				    tawcroot_link_host_path_token(
					    hostp, tok, sizeof tok))
					add_tok = tok;
				/* Linkable O_TMPFILE via the magic-link
				 * spelling (open(2)'s documented publish
				 * idiom): one atomic rename. */
				else if (hn > 0 &&
					 tawcroot_link_host_path_tmp(
						 hostp, tok, sizeof tok))
					return tawcroot_link_publish_tmp(
						tok, tnew.fd, tnew.path);
			}
		}
		if (add_tok) {
			/* Degraded store: mutations refuse; raw EPERM. */
			if (!ready) return TAWC_EPERM;
			/* Emulated names never land in binds (see
			 * fs_path_in_bind). */
			if (fs_path_in_bind(&tnew)) return TAWC_EXDEV;
			return tawcroot_link_add(add_tok, tnew.fd, tnew.path);
		}
	}

	long rv = TAWC_RAW(TAWC_SYS_linkat, told.fd, (long)told.path,
			   tnew.fd, (long)tnew.path, flags, 0);
	if (rv == 0 || (rv != TAWC_EACCES && rv != TAWC_EPERM))
		return rv;

	/* link(2) on a directory is the kernel's own EPERM,
	 * indistinguishable from an SELinux denial by errno alone —
	 * emulating it would "hardlink" the directory by renaming it
	 * away. Stat and pass the kernel's error through. */
	struct stat st;
	long te = TAWC_RAW(TAWC_SYS_fstatat, told.fd, (long)told.path,
			   (long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
	if (te || S_ISDIR(st.st_mode)) return rv;

	/* A plain-symlink NOFOLLOW source becomes a symlink OBJECT: NEW
	 * renames the symlink itself into link/ (fstatat above is
	 * NOFOLLOW, so the token is the symlink's inode), and the
	 * resolver splices the object's target back into the guest walk
	 * (readlink_store oracle member) — real hardlinked-symlink
	 * semantics, relative targets resolving against each NAME's
	 * directory. */

	/* Either operand on a bind: v1 keeps both names real/openable
	 * where NEW's token symlinks would be unresolvable (see
	 * fs_path_in_bind). */
	if (fs_path_in_bind(&told) || fs_path_in_bind(&tnew))
		return link_fallback_v1(told.fd, told.path,
					tnew.fd, tnew.path, rv);

	switch (tawcroot_linkstore_state()) {
	case TAWCROOT_STORE_READY:
	case TAWCROOT_STORE_LATENT: {
		long nrv = tawcroot_link_new(told.fd, told.path,
					     tnew.fd, tnew.path);
		/* EXDEV: the source cannot reach the store by rename
		 * (cross-fs bind) — degrade to the v1 emulation, both
		 * ends stay on the bind's fs. EPERM: the store could not
		 * be created/locked — v1 keeps today's behavior — UNLESS
		 * the lock-time version re-check just flipped the store
		 * to DEGRADED (newer format): then raw EPERM, no writes. */
		if (nrv == TAWC_EPERM &&
		    tawcroot_linkstore_state() == TAWCROOT_STORE_DEGRADED)
			return rv;
		if (nrv == TAWC_EXDEV || nrv == TAWC_EPERM)
			return link_fallback_v1(told.fd, told.path,
						tnew.fd, tnew.path, rv);
		return nrv;
	}
	case TAWCROOT_STORE_DEGRADED:
		return rv;  /* newer store: raw EPERM, zero corruption */
	default:
		return link_fallback_v1(told.fd, told.path,
					tnew.fd, tnew.path, rv);
	}
}

/* renameat2 — translate both operands; old is PARENT_REMOVE, new is
 * PARENT_CREATE. The kernel takes (olddirfd, oldpath, newdirfd, newpath,
 * flags); when the guest passes non-AT_FDCWD dirfds with relative paths
 * we let the kernel resolve off the dirfd (see tawcroot_fs_translate_at).
 * Both renameat and renameat2 funnel through here — renameat just passes
 * flags=0 to the kernel renameat2 (the latter is a strict superset and
 * present on every kernel we target). */
static long do_renameat(int olddirfd, const char *oldpath,
			int newdirfd, const char *newpath,
			unsigned int rflags)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	/* Both operands are forced-write modes → uniform EROFS when
	 * either lands in an RO bind, BEFORE any host attempt or
	 * emulation branch. (Kernel gives EXDEV for the cross-mount
	 * flavor; uniform EROFS is equally terminal for `mv`.) */
	struct fs_path told, tnew;
	long e1 = tawcroot_fs_translate_at(scratch, 0, olddirfd, oldpath,
					   TAWCROOT_PATH_PARENT_REMOVE,
					   TAWCROOT_PATH_INTENT_WRITE, &told);
	if (e1) return e1;
	long e2 = tawcroot_fs_translate_at(scratch, 2, newdirfd, newpath,
					   TAWCROOT_PATH_PARENT_CREATE,
					   TAWCROOT_PATH_INTENT_WRITE, &tnew);
	if (e2) return e2;
	if (told.is_root || tnew.is_root) return TAWC_EINVAL;

	/* Hardlink emulation. Only the plain-replace shape needs work:
	 *  - both names resolve to the same token → POSIX same-inode
	 *    no-op (return 0, both names remain; the kernel does this by
	 *    inode, we do it by token);
	 *  - dst is an emulated name → CLOBBER: rename under the store
	 *    lock, then decrement the clobbered cluster.
	 * NOREPLACE gets the kernel's EEXIST off the symlink itself;
	 * EXCHANGE atomically swaps the two entries with no count change;
	 * src-emulated needs nothing (opaque targets are location-
	 * independent). */
	if (tawcroot_store_link_fd >= 0 &&
	    !(rflags & (RENAME_NOREPLACE | RENAME_EXCHANGE))) {
		char ntok[TAWCROOT_LINK_TOKEN_MAX];
		if (tawcroot_link_leaf_token(tnew.fd, tnew.path, ntok,
					     sizeof ntok) == 1) {
			char otok[TAWCROOT_LINK_TOKEN_MAX];
			if (tawcroot_link_leaf_token(told.fd, told.path, otok,
						     sizeof otok) == 1 &&
			    tawc_streq(otok, ntok))
				return 0;
			if (tawcroot_linkstore_state()
			    == TAWCROOT_STORE_READY)
				return tawcroot_link_clobber(
					told.fd, told.path,
					tnew.fd, tnew.path, rflags);
			/* Non-READY store: plain rename below — the
			 * clobbered object leaks (overcount, safe). */
		}
	}
	return TAWC_RAW(TAWC_SYS_renameat2, told.fd, (long)told.path,
			tnew.fd, (long)tnew.path, rflags, 0);
}

static long handle_renameat2(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	return do_renameat((int)args->a,
			   (const char *)(uintptr_t)args->b,
			   (int)args->c,
			   (const char *)(uintptr_t)args->d,
			   (unsigned int)args->e);
}

static long handle_renameat(const tawcroot_syscall_args *args,
			    ucontext_t *uc)
{
	(void)uc;
	return do_renameat((int)args->a,
			   (const char *)(uintptr_t)args->b,
			   (int)args->c,
			   (const char *)(uintptr_t)args->d, 0);
}

void tawcroot_fs_link_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_linkat,      handle_linkat);
	tawcroot_dispatch_install(TAWC_SYS_renameat,    handle_renameat);
	tawcroot_dispatch_install(TAWC_SYS_renameat2,   handle_renameat2);
}
