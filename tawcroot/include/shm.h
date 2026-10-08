/* In-handler emulation of POSIX `/dev/shm`.
 *
 * Android's /dev has no /dev/shm, and we run rootless without the
 * privilege to mount tmpfs. Instead of binding an app-private dir
 * (which makes "shm" disk-backed, see the deleted issue
 * tawcroot-dev-shm-disk-backed.md), the SIGSYS handler intercepts
 * path-bearing syscalls under /dev/shm/ and routes them to a small
 * (name → memfd) table. Guests see real POSIX shm semantics:
 *
 *   - shm_open creates a memfd_create-backed segment, returns a fd.
 *   - mmap/ftruncate/read/write/mremap/mprotect/munmap on the
 *     returned fd are real syscalls on a real kernel object — no
 *     handler involvement.
 *   - shm_unlink drops the name; the segment lives until the last
 *     fd closes (kernel refcount).
 *   - stat/access on /dev/shm and /dev/shm/<name> return synthetic
 *     answers consistent with the table contents.
 *
 * The internal memfd we keep is `!CLOEXEC` so it survives the
 * --exec-child re-exec dance — the exec_handler ferries (name, fd)
 * pairs through `exec_state`, and the new tawcroot incarnation
 * re-registers them, preserving cross-process visibility for
 * fork+execve patterns (Mozilla parent → content IPC).
 *
 * Narrower-mode reopens: Android SELinux denies `open` on the app's
 * own memfds, so an O_RDONLY reopen via /proc/self/fd can't get a
 * read-only description. When that happens and the segment is still
 * private to the caller (no other fd, no mapping), it is migrated to
 * an unnamed O_TMPFILE under the rootfs dir, which /proc/self/fd CAN
 * reopen. Only segments that ask for a narrower mode pay the
 * file-backing cost. Guest memfds reopened via /proc/self/fd/<n> get
 * the same treatment (Firefox's HaveMemfd probe + freezable regions).
 * Migrated files carry their seals in the mode bits (see
 * tawcroot_shm_seal_fcntl); enforcement is advisory.
 *
 * Async-signal-safe: a tiny spinlock guards the table; no malloc,
 * no libc.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define TAWCROOT_SHM_MAX        64
#define TAWCROOT_SHM_NAME_MAX   255

struct tawcroot_shm_entry {
	int  in_use;       /* 0 = free; 1 = active                     */
	int  fd;           /* internal memfd at high reserved range    */
	int  guest_fd;     /* fd handed out at create, -1 if unknown   */
	int  pid;          /* process that created it (guest_fd owner) */
	char name[TAWCROOT_SHM_NAME_MAX + 1];
};

/* If `path` is `/dev/shm/<name>` with non-empty name, returns a
 * pointer to <name> within `path`. Returns NULL otherwise. The
 * match is anchored to a leading `/dev/shm/` (POSIX shm names are
 * absolute by convention; relative-via-cwd is not supported). */
const char *tawcroot_shm_match(const char *path);

/* True if `path` is exactly `/dev/shm` or `/dev/shm/`. */
int tawcroot_shm_is_dir(const char *path);

/* shm_open(name, flags, mode). Returns a guest-visible fd, or
 * -errno. Honors O_CREAT, O_EXCL, O_TRUNC, O_CLOEXEC. */
long tawcroot_shm_open(const char *name, int flags, int mode);

/* Migrate guest memfd `fd` to a file-backed segment (see top) after
 * a denied /proc/self/fd/<fd> reopen, so a retry can succeed. Returns
 * 0 on migration, -errno if `fd` isn't a memfd or isn't private. */
long tawcroot_shm_migrate_guest_memfd(int fd);

/* fcntl F_ADD_SEALS / F_GET_SEALS on a migrated file: emulated from
 * its mode bits. Returns 1 with *ret set when handled, 0 to pass the
 * call through. */
int tawcroot_shm_seal_fcntl(int fd, int op, long arg, long *ret);

/* Drop a name from the table. Returns 0 / -ENOENT. The internal
 * memfd is closed; the segment survives if the guest still holds
 * its dup. */
long tawcroot_shm_unlink(const char *name);

/* Synthesize a stat for /dev/shm itself or for /dev/shm/<name>.
 * struct stat layout follows the kernel `newfstatat` output, same
 * as `decorate_stat` callers in syscalls_stat.c. */
struct stat;
struct statx;
void tawcroot_shm_stat_dir(struct stat *out);
void tawcroot_shm_statx_dir(struct statx *out, unsigned int mask);
long tawcroot_shm_stat_name(const char *name, struct stat *out);
long tawcroot_shm_statx_name(const char *name, struct statx *out,
			     unsigned int mask);

/* access semantics: 0 if the dir / name exists, -ENOENT otherwise.
 * Mode bits ignored — fake-root, everything readable/writable. */
long tawcroot_shm_access_dir(void);
long tawcroot_shm_access_name(const char *name);

/* exec_state ferry. The writer snapshots all live entries — name
 * BYTES included — in one lock acquisition, so concurrent shm ops on
 * other threads can neither shift indices nor mutate a name between
 * export and serialization. The reader re-registers them after the
 * new tawcroot incarnation re-establishes its rootfs view. Fds carry
 * across execveat because they are non-CLOEXEC. */
size_t tawcroot_shm_export_all(char (*names_out)[TAWCROOT_SHM_NAME_MAX + 1],
			       int *fds_out, size_t cap);
long   tawcroot_shm_register(const char *name, int fd);

/* Reset the table to empty. Called from --exec-child before
 * re-registration so a stale BSS init doesn't leak entries from a
 * previous incarnation that wasn't a fresh process. */
void tawcroot_shm_reset(void);
