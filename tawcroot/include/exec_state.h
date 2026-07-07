/* exec_state — the binary blob the SIGSYS handler writes to a memfd
 * before re-execing tawcroot, and that --exec-child reads back to
 * resume the guest's `execve(2)` in the new tawcroot incarnation.
 *
 * =======================================================================
 *  WHY THIS EXISTS
 * =======================================================================
 *
 * `execve(2)` resets every signal handler to SIG_DFL on success — see
 * `man 2 execve`. Our SIGSYS handler does not survive a kernel exec.
 * If we let the guest's `execve` go through, the kernel-installed
 * seccomp filter is inherited (filters do persist) but our handler
 * is gone, so the guest's first path-bearing syscall delivers SIGSYS
 * with disposition SIG_DFL → the kernel kills the guest with code 31.
 *
 * Instead the handler intercepts `execve`, validates the binary,
 * writes its own state (guest path, argv, envp) into a non-CLOEXEC
 * memfd, then re-execs tawcroot itself with `--exec-child <fd>`.
 * The new tawcroot reads the memfd, reinstalls the SIGSYS handler
 * (the seccomp filter is already inherited), and uses the manual
 * loader to map the guest binary + jump into it. From the guest's
 * perspective this is indistinguishable from a normal execve.
 *
 * =======================================================================
 *  FORMAT v1
 * =======================================================================
 *
 *   struct tawcroot_exec_state_header {
 *       uint32_t magic;        // TAWCROOT_EXEC_STATE_MAGIC
 *       uint32_t version;      // 1
 *       uint32_t string_bytes; // total bytes following header
 *       uint32_t path_off;     // offset of guest path within strings
 *       uint32_t argc;
 *       uint32_t envc;
 *       uint32_t argv_off[MAX_ARGS];   // offsets within strings
 *       uint32_t envp_off[MAX_ENV];
 *   };
 *
 * Strings follow the header back-to-back, NUL-terminated. argv_off[i]
 * and envp_off[i] are byte offsets from the start of the strings area.
 * path_off is the same. Reader can mmap header + strings as a single
 * region and use the offsets directly to point at NUL-terminated C
 * strings.
 *
 * No allocations on the reader side: argv_buf[] / envp_buf[] arrays
 * of (argc + 1) / (envc + 1) `const char *` (with the trailing NULL)
 * are built by indexing strings + argv_off[i].
 *
 * Endianness: same-machine fd handoff, so host endianness throughout.
 * No portability concerns — the writer and reader are the same binary
 * (re-exec'd self).
 *
 * =======================================================================
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAWCROOT_EXEC_STATE_MAGIC   0x4358454Cu  /* 'LEXC' = "Loader EXeC" */
/* Version 3: adds the /dev/shm name table (n_shm + shm_name_off[] +
 * shm_fd[]) so the in-handler memfd-backed `/dev/shm` survives the
 * --exec-child dance. The internal memfds are non-CLOEXEC and survive
 * the execveat; we ferry their (name, fd_int) pairs through here so
 * the new tawcroot incarnation can re-register them. v2 readers don't
 * exist outside the tree — both ends are the same binary — so we
 * don't maintain back-compat.
 * Version 4: adds the virtual identity snapshot (has_identity +
 * identity) so a guest that dropped privileges keeps its tracked
 * uid/gid across execve.
 * Version 5: adds the hardlink-emulation store path (store_host_off,
 * linkstore.h). Ferried explicitly — NOT re-derived from rootfs_host,
 * which after a guest chroot is the chrooted root, not the store's
 * sibling. */
#define TAWCROOT_EXEC_STATE_VERSION 5
/* MAX_ARGS at 4096: shell glob expansions, linker invocations, and
 * pacman hooks routinely pass hundreds-to-thousands of args; the kernel
 * allows ~2 MB of argv strings. MAX_ENV at 1024 covers the busiest bash
 * + profile.d environments. Each entry costs 4 bytes in the (BSS, not
 * stack) header. */
#define TAWCROOT_EXEC_STATE_MAX_ARGS 4096
#define TAWCROOT_EXEC_STATE_MAX_ENV  1024
#define TAWCROOT_EXEC_STATE_MAX_BINDS 16
#define TAWCROOT_EXEC_STATE_MAX_SHM   64

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t string_bytes;
	uint32_t path_off;
	uint32_t argc;
	uint32_t envc;
	uint32_t argv_off[TAWCROOT_EXEC_STATE_MAX_ARGS];
	uint32_t envp_off[TAWCROOT_EXEC_STATE_MAX_ENV];
	/* Phase 2g: tawcroot per-process state to re-establish in
	 * --exec-child. A 0 offset means "absent" (no rootfs view active /
	 * no guest_exe stash). Bind specs are paired src/dst pointers
	 * indexed by [0..n_binds). When n_binds == 0, the bind arrays are
	 * meaningless. */
	uint32_t rootfs_host_off;
	uint32_t guest_exe_off;
	uint32_t store_host_off;   /* hardlink-emulation store; 0 = absent */
	uint32_t n_binds;
	uint32_t bind_src_off[TAWCROOT_EXEC_STATE_MAX_BINDS];
	uint32_t bind_dst_off[TAWCROOT_EXEC_STATE_MAX_BINDS];
	/* /dev/shm name table. fd numbers are inherited verbatim because
	 * the internal memfds are non-CLOEXEC and survive execveat. */
	uint32_t n_shm;
	uint32_t shm_name_off[TAWCROOT_EXEC_STATE_MAX_SHM];
	uint32_t shm_fd[TAWCROOT_EXEC_STATE_MAX_SHM];
	/* Virtual identity snapshot (identity.c). Fixed-size value, not
	 * strings — embedded directly. has_identity == 0 means "absent"
	 * (restore keeps the register-time root defaults). */
	uint32_t      has_identity;
	tawc_identity identity;
} tawcroot_exec_state_header;

/* Total memfd bytes when both header + strings are written. */
static inline size_t tawcroot_exec_state_total_bytes(uint32_t string_bytes)
{ return sizeof(tawcroot_exec_state_header) + string_bytes; }

/* Result of unpacking. Strings live in an opaque buffer the caller
 * owns (typically the mmap'd memfd region); argv/envp are arrays of
 * pointers into that buffer.  These pointers are only valid as long
 * as `strings` is mapped/alive.
 *
 * argv has argc + 1 entries (NULL-terminated); envp has envc + 1. */
typedef struct {
	const char  *path;
	uint32_t     argc;
	uint32_t     envc;
	const char **argv;       /* size argc + 1 */
	const char **envp;       /* size envc + 1 */

	/* Phase 2g: per-process state. NULL when absent (--exec-via-handler
	 * test path doesn't carry a rootfs view; production -r mode does). */
	const char  *rootfs_host;       /* may be NULL */
	const char  *guest_exe;         /* may be NULL */
	const char  *store_host;        /* may be NULL */
	uint32_t     n_binds;
	const char  *bind_src[TAWCROOT_EXEC_STATE_MAX_BINDS];
	const char  *bind_dst[TAWCROOT_EXEC_STATE_MAX_BINDS];
	uint32_t     n_shm;
	const char  *shm_name[TAWCROOT_EXEC_STATE_MAX_SHM];
	int          shm_fd[TAWCROOT_EXEC_STATE_MAX_SHM];
	int           has_identity;
	tawc_identity identity;      /* valid iff has_identity */
} tawcroot_exec_state;

/* Optional inputs for the writer — may all be NULL/0 to indicate "no
 * tawcroot per-process state to carry". */
typedef struct {
	const char        *rootfs_host;     /* may be NULL */
	const char        *guest_exe;       /* may be NULL */
	const char        *store_host;      /* may be NULL */
	uint32_t           n_binds;
	const char *const *bind_src;        /* size n_binds */
	const char *const *bind_dst;        /* size n_binds */
	uint32_t           n_shm;
	const char *const *shm_name;        /* size n_shm */
	const int         *shm_fd;          /* size n_shm */
	const tawc_identity *identity;      /* may be NULL */
} tawcroot_exec_state_extras;

/* ---- Writer (handler side) ----
 *
 * Serialize a guest exec request into a buffer. Writes the header
 * followed by all strings. Returns the total bytes written, or
 * -errno on error:
 *   -E2BIG   argc > MAX_ARGS or envc > MAX_ENV
 *   -ENOSPC  buf_cap too small
 *
 * Caller-supplied `buf_cap` should be sized via
 * tawcroot_exec_state_estimate_bytes before allocation, or the call
 * can be made twice (once with a probe buf to learn the size).
 *
 * envp must be NULL-terminated; argv must be NULL-terminated (with
 * argv[argc] = NULL). The caller knows argc; envc is computed by
 * walking until NULL.
 */
long tawcroot_exec_state_write(void *buf, size_t buf_cap,
                               const char *path,
                               int argc, const char *const *argv,
                               const char *const *envp,
                               const tawcroot_exec_state_extras *extras);

/* Compute exact bytes needed for serializing the given exec request. */
size_t tawcroot_exec_state_estimate_bytes(const char *path,
                                          int argc, const char *const *argv,
                                          const char *const *envp,
                                          const tawcroot_exec_state_extras *extras);

/* ---- Reader (--exec-child side) ----
 *
 * Parse a serialized exec_state out of `buf` (the mmap'd memfd
 * contents). On success, fills `out` with pointers into `buf` plus
 * argv/envp pointer arrays (which the caller provides backing
 * storage for via `argv_buf` / `envp_buf` — sized to MAX_ARGS+1 /
 * MAX_ENV+1 respectively).
 *
 * Returns 0 on success, -EINVAL on bad magic / version / sizes /
 * out-of-range offsets.
 */
long tawcroot_exec_state_read(const void *buf, size_t buf_size,
                              const char **argv_buf, /* MAX_ARGS+1 */
                              const char **envp_buf, /* MAX_ENV+1  */
                              tawcroot_exec_state *out);

#ifdef __cplusplus
}
#endif
