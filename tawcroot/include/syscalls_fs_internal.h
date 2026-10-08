/* Path plumbing shared by the fs handler files (syscalls_fs.c,
 * syscalls_stat.c, syscalls_link.c, syscalls_xattr.c,
 * syscalls_fs_legacy.c). Defined in syscalls_fs.c; not for use outside
 * the fs handlers. */

#pragma once

#include "path.h"
#include "path_scratch.h"

/* A translated guest path, ready to issue against the host: pass `fd`
 * as the *at dirfd and `path` as the pathname. `is_root` is set when
 * the guest path resolved to the directory `fd` itself; `path` is then
 * "" and the caller picks its syscall's semantics for that case ("."
 * for most, AT_EMPTY_PATH, or a direct errno). */
struct fs_path {
	int         fd;
	int         is_root;
	int         ro;    /* result.ro mirror — fidelity only (statfs,
	                    * linkat src), never enforcement. 0 on the
	                    * dirfd-passthrough branch (outside-view fds
	                    * aren't ours to police). */
	const char *path;
};

/* Fetch `guest_path` into scratch->buf[slot], then translate. Uses
 * slots `slot` and `slot + 1`; `out->path` points into `slot + 1`.
 * Honours a non-AT_FDCWD dirfd for relative paths. 0 / -errno. */
long tawcroot_fs_translate_at(struct tawcroot_path_scratch *scratch, int slot,
			      int dirfd, const char *guest_path,
			      tawcroot_path_mode mode,
			      tawcroot_path_intent intent, struct fs_path *out);

/* Same, for a path already fetched into scratch->buf[slot]. */
long tawcroot_fs_translate_local(struct tawcroot_path_scratch *scratch,
				 int slot, int dirfd, tawcroot_path_mode mode,
				 tawcroot_path_intent intent,
				 struct fs_path *out);

/* Fetch the guest path once into scratch->buf[slot] for a handler that
 * classifies intercepts before translating. Returns 0 / -errno. */
long tawcroot_fs_fetch_path(struct tawcroot_path_scratch *scratch, int slot,
			    const char *guest_path);

/* AT_EMPTY_PATH support: 1 when `gpath` is NULL or "", 0 when
 * non-empty, -EFAULT on an unreadable guest pointer. */
long tawcroot_fs_path_is_empty(const char *gpath);

/* Classify an already-fetched local path as a `/dev/shm` intercept.
 *   NONE — not shm (fall through to normal translation)
 *   NAME — `/dev/shm/<name>` (*name_out points into `local_path`)
 *   DIR  — `/dev/shm` directory itself */
enum { SHM_PEEK_NONE = 0, SHM_PEEK_NAME = 1, SHM_PEEK_DIR = 2 };
int tawcroot_fs_classify_shm(const char *local_path, const char **name_out);

/* /proc shadow classification of a fetched path, retrying through
 * fd-relative composition (uses scratch->buf[2]). Returns a
 * TAWCROOT_PROC_SHADOW_* kind. */
int tawcroot_fs_proc_shadow_classify_at(struct tawcroot_path_scratch *scratch,
					int dirfd, const char *path);

/* 1 iff `fd`'s kernel-side path lies in an RO part of the view (the
 * fd-based metadata-write check; see syscalls_fs.c). */
int tawcroot_fs_fd_in_ro_bind(int fd, int opath_passes);

/* Per-file registrars, called from tawcroot_fs_register. */
void tawcroot_fs_stat_register(void);
void tawcroot_fs_link_register(void);
void tawcroot_fs_xattr_register(void);
#if defined(__x86_64__)
void tawcroot_fs_legacy_register(void);
#endif
