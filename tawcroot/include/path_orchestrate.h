/* Path-translation orchestration — fold → bind → memo → resolver → bind.
 *
 * `tawcroot_path_translate` (path.c) is the production wrapper. It
 * reads the process-global rootfs/bind/memo tables and wires up a
 * raw_sys-backed cwd source and readlink oracle. It then calls into
 * `tawcroot_path_translate_with_ctx` here, which is pure with respect
 * to the OS — every external dependency is in the context struct, so
 * the orchestration itself compiles into both the freestanding
 * production binary and the hosted-glibc cleat test orchestrator
 * (PROD_C_FOR_TESTS in tawcroot/Makefile).
 *
 * Tests use this directly to exercise the inter-stage seams (B5/D3
 * findings: bind-vs-memo collisions, fold→memo→resolve→bind ordering,
 * symlink-target re-fold correctness). The pure sub-stages already had
 * unit coverage before this refactor; the orchestration didn't because
 * `tawcroot_path_translate` reached `getcwd`, `readlinkat`, and process
 * globals through `raw_sys.h`, which the hosted build can't link.
 */

#pragma once

#include <stddef.h>

#include "path.h"
#include "path_oracle.h"

/* Memoized well-known symlink. Populated at init by
 * `tawcroot_path_memoize_well_known()` in path.c (production); tests
 * supply their own arrays directly via the context.
 *
 * The match condition is "src is a path-prefix of suf, with a
 * component boundary" — see `apply_memo` in path_orchestrate.c. The
 * target replaces src in-place and must be rootfs-root-anchored (no
 * leading '/'): the builder anchors relative symlink targets at the
 * symlink's parent directory before storing them. The target may
 * contain '.'/'..' components; the orchestration loop re-folds after
 * every rewrite. */
struct tawcroot_symlink_memo {
	char   src[32];
	size_t src_len;
	char   target[256];
	size_t target_len;
};

/* Context for `tawcroot_path_translate_with_ctx`. Production fills
 * this from process globals; tests build their own.
 *
 * Lifetime: caller-owned. The orchestration reads it but does not
 * outlive the call.
 */
struct tawcroot_path_translate_ctx {
	/* `base_fd` filled into the result when no bind matches. In
	 * production this is the live rootfs O_PATH fd; tests typically
	 * pass a sentinel value (e.g. -100) and assert on the field. */
	int    rootfs_base_fd;

	/* Non-zero when the root view itself is read-only (the guest
	 * chrooted into an RO bind dst). Production fills this from
	 * tawcroot_root_ro; tests set it directly. Rootfs-routed
	 * write-intent translations then refuse with -EROFS exactly like
	 * RO-bind-routed ones. */
	int    rootfs_ro;

	/* Bind table. Empty (`n_binds == 0`) is fine. */
	const struct tawcroot_bind         *binds;
	size_t                              n_binds;

	/* Well-known-symlink memo table. Empty is fine. */
	const struct tawcroot_symlink_memo *memos;
	size_t                              n_memos;

	/* link/ dirfd of the hardlink-emulation store (linkstore.h), or
	 * <= 0 when no store is open. When the resolver surfaces an
	 * opaque `tawcroot:link:<token>` target, the result is re-based
	 * here (base_fd = store_link_fd, suffix = token). With no store,
	 * such a hit yields -ENOENT — the dangling-symlink semantics the
	 * degraded mode promises. Zero-initialized contexts (tests)
	 * behave exactly as before: fd 0 counts as "none" and token
	 * targets are treated as ordinary relative targets. */
	int    store_link_fd;

	/* Optional: called on a token hit when store_link_fd is <= 0.
	 * Production wires tawcroot_linkstore_latent_upgrade so a process
	 * that started before the store existed (LATENT) opens it the
	 * first time it actually MEETS a token, instead of ENOENTing reads
	 * until its first mutation. Returns the (now-open) link/ dirfd or
	 * -1; cold — only runs on token hits. May be NULL (tests). */
	int  (*store_upgrade)(void);

	/* Readlink oracle for the manual symlink resolver. May be NULL,
	 * in which case the resolver pass is skipped entirely (the
	 * orchestration only does fold + memo + bind, no symlink
	 * walking). Production always supplies one; tests that don't
	 * care about symlinks usually pass an oracle whose `readlink`
	 * returns -EINVAL ("no symlinks anywhere") or NULL when they
	 * specifically want to verify the no-resolver path. */
	const struct tawcroot_path_oracle  *oracle;

	/* Resolve the current working directory to a guest-absolute path
	 * (must start with '/', NUL-terminated). Called only when the
	 * input path is relative.
	 *
	 * Returns 0 on success and writes into `out` (capacity `out_cap`).
	 * Negative return is a -errno passed through to the caller (e.g.
	 * -ENOENT when cwd is outside the rootfs view). May be NULL; in
	 * that case relative inputs return -ENOENT. */
	long  (*cwd_to_guest_abs)(void *cwd_ctx, char *out, size_t out_cap);
	void   *cwd_ctx;
};

tawcroot_path_result tawcroot_path_translate_with_ctx(
	const struct tawcroot_path_translate_ctx *ctx,
	const char *guest_path, char *out_suffix, size_t out_cap,
	tawcroot_path_mode mode, tawcroot_path_intent intent);
