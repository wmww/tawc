/* In-handler `/dev/shm` emulation. See include/shm.h.
 *
 * Threading: a single spinlock (`g_shm_lock`) guards the table. The
 * fast path (lookup, mutate) holds it; we also hold it across the
 * memfd_create + dup syscalls in the create path so a racing
 * shm_open(same-name) can't observe a half-initialized slot. Mozilla's
 * IPC SHM is not contended in practice, so contention is effectively
 * zero — fine to keep the lock simple.
 *
 * Internal fd lifecycle: every entry's `fd` lives in the reserved
 * high-fd range (TAWCROOT_RESERVED_FD_BASE..) and is **non-CLOEXEC**
 * so it survives the SIGSYS-handler-driven execveat re-exec. The
 * exec_handler ferries (name, fd) pairs through exec_state; the new
 * tawcroot incarnation calls tawcroot_shm_register to rebuild the
 * table without re-creating the kernel-side memfd objects.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "raw_sys.h"
#include "shm.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

#ifndef O_ACCMODE
# define O_ACCMODE 00000003
#endif

_Static_assert(__atomic_always_lock_free(sizeof(int), 0),
	       "int atomics must be lock-free for AS-safety");

static struct tawcroot_shm_entry g_shm[TAWCROOT_SHM_MAX];
static volatile int              g_shm_lock = 0;

static void shm_lock(void)
{
	while (__atomic_test_and_set(&g_shm_lock, __ATOMIC_ACQUIRE)) {
		/* Tight spin — handler-side, low contention in practice. */
	}
}

static void shm_unlock(void)
{
	__atomic_clear(&g_shm_lock, __ATOMIC_RELEASE);
}

/* `/dev/shm` prefix match. POSIX shm names that glibc passes through
 * are always absolute (`shm_open` requires `name[0] == '/'`); the
 * full path is `/dev/shm/<name>`. We also reject embedded `/` — POSIX
 * shm has no subdir semantics, and emulating them adds zero value. */
const char *tawcroot_shm_match(const char *path)
{
	if (!path) return 0;
	static const char prefix[] = "/dev/shm/";
	for (size_t i = 0; i < sizeof prefix - 1; i++) {
		if (path[i] != prefix[i]) return 0;
	}
	const char *name = path + (sizeof prefix - 1);
	if (*name == 0) return 0;
	for (const char *p = name; *p; p++) {
		if (*p == '/') return 0;
	}
	/* "." and ".." are the directory itself / its parent, not shm
	 * names — tawcroot_shm_match must not claim them or "/dev/shm/."
	 * ENOENTs instead of resolving to the dir. */
	if (name[0] == '.' && name[1] == 0) return 0;
	if (name[0] == '.' && name[1] == '.' && name[2] == 0) return 0;
	return name;
}

int tawcroot_shm_is_dir(const char *path)
{
	if (!path) return 0;
	static const char d[] = "/dev/shm";
	size_t i;
	for (i = 0; i < sizeof d - 1; i++) {
		if (path[i] != d[i]) return 0;
	}
	if (path[i] == 0) return 1;
	if (path[i] == '/' && path[i + 1] == 0) return 1;
	return 0;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

static struct tawcroot_shm_entry *find_entry_locked(const char *name)
{
	for (size_t i = 0; i < TAWCROOT_SHM_MAX; i++) {
		if (g_shm[i].in_use && name_eq(g_shm[i].name, name))
			return &g_shm[i];
	}
	return 0;
}

static struct tawcroot_shm_entry *find_free_locked(void)
{
	for (size_t i = 0; i < TAWCROOT_SHM_MAX; i++) {
		if (!g_shm[i].in_use) return &g_shm[i];
	}
	return 0;
}

/* Move `fd` to the high reserved range with F_DUPFD (NOT CLOEXEC) and
 * close the original. Caller adds the new fd to the reserved list.
 * Inheritability is what gives us "survives execveat" — the rest of
 * the reserved range is CLOEXEC, but shm fds need the opposite
 * because there's no host-fs path to re-open them by. */
static long dup_to_reserved_inheritable(int fd)
{
	long r = tawc_fcntl(fd, F_DUPFD, TAWCROOT_RESERVED_FD_BASE);
	if (r < 0) return r;
	tawc_close(fd);
	return r;
}

/* Hand the guest a fresh open file description on the segment by
 * re-opening the internal memfd through /proc/self/fd/<internal> with
 * the guest's requested access mode. A plain F_DUPFD shares ONE file
 * description, so (a) an O_RDONLY opener got a writable fd and (b) all
 * guest fds for a name shared a read/write/lseek offset. The re-open
 * gives a distinct description with the right access mode and its own
 * offset — matching real /dev/shm. Falls back to F_DUPFD (degraded but
 * functional: shared offset, access mode = internal's) if /proc isn't
 * available. Returns the new guest fd or -errno. */
static long reopen_for_guest(int internal_fd, int flags)
{
	char path[32];
	if (tawc_proc_fd_path(path, sizeof path, internal_fd, 0) > 0) {
		int oflags = (flags & O_ACCMODE);
		if (flags & O_CLOEXEC) oflags |= O_CLOEXEC;
		long fd = tawc_openat(AT_FDCWD, path, oflags, 0);
		if (fd >= 0) return fd;
	}
	/* Fallback: dup shares the file description but keeps the segment
	 * usable when /proc/self/fd is unreachable. */
	return tawc_fcntl(internal_fd,
			  (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
}

static void add_to_reserved_list(int fd)
{
	if (tawcroot_n_reserved_fds >= TAWCROOT_MAX_RESERVED_FDS) {
		/* Table full: the fd stays usable internally but is NOT
		 * protected — neither the BPF close trap (baked at install)
		 * nor tawcroot_fd_is_reserved will recognise it, so a guest
		 * close() reaches the kernel. Unreachable in practice (64
		 * slots vs ≤ 33 users). Note that even WITH a slot, fds
		 * reserved after filter install are missed by the BPF close
		 * fast path — see issues/tawcroot-close-fastpath-misses-
		 * runtime-reserved-fds.md. */
		return;
	}
	tawcroot_reserved_fds[tawcroot_n_reserved_fds++] = fd;
}

long tawcroot_shm_open(const char *name, int flags, int mode)
{
	(void)mode;
	if (!name || name[0] == 0) return TAWC_EINVAL;
	size_t nlen = 0;
	while (name[nlen]) nlen++;
	if (nlen > TAWCROOT_SHM_NAME_MAX) return TAWC_ENAMETOOLONG;

	int has_create = (flags & O_CREAT)   != 0;
	int has_excl   = (flags & O_EXCL)    != 0;
	int has_trunc  = (flags & O_TRUNC)   != 0;

	long guest_fd = -1;

	shm_lock();
	struct tawcroot_shm_entry *e = find_entry_locked(name);
	if (e) {
		if (has_create && has_excl) {
			shm_unlock();
			return TAWC_EEXIST;
		}
		/* Re-open a guest-facing fd from the internal fd UNDER THE
		 * LOCK so a concurrent unlink+create-different-name can't
		 * recycle the kernel slot underneath us. Re-open (not dup)
		 * gives the guest its own file description + access mode. */
		guest_fd = reopen_for_guest(e->fd, flags);
		shm_unlock();
		if (guest_fd < 0) return guest_fd;
	} else {
		if (!has_create) {
			shm_unlock();
			return TAWC_ENOENT;
		}
		struct tawcroot_shm_entry *slot = find_free_locked();
		if (!slot) {
			shm_unlock();
			return TAWC_ENOSPC;
		}

		/* Hold the lock across create + the dup-for-guest so a racing
		 * shm_open(same-name) can't observe a half-built slot, and
		 * a racing unlink can't close our internal fd before the
		 * caller's dup is established. */
		long src = tawc_memfd_create(name, MFD_ALLOW_SEALING);
		if (src < 0) {
			shm_unlock();
			return src;
		}
		long internal = dup_to_reserved_inheritable((int)src);
		if (internal < 0) {
			tawc_close((int)src);
			shm_unlock();
			return internal;
		}
		add_to_reserved_list((int)internal);
		guest_fd = reopen_for_guest((int)internal, flags);
		if (guest_fd < 0) {
			tawc_close((int)internal);
			shm_unlock();
			return guest_fd;
		}

		for (size_t i = 0; i < nlen; i++) slot->name[i] = name[i];
		slot->name[nlen] = 0;
		slot->fd = (int)internal;
		slot->in_use = 1;
		shm_unlock();
	}

	/* ftruncate the GUEST fd (independent lifetime from the table's
	 * internal fd) so a concurrent unlink can't redirect it. */
	if (has_trunc) {
		long tr = TAWC_RAW(TAWC_SYS_ftruncate, guest_fd, 0,
				   0, 0, 0, 0);
		if (tr < 0) {
			tawc_close((int)guest_fd);
			return tr;
		}
	}
	return guest_fd;
}

long tawcroot_shm_unlink(const char *name)
{
	if (!name || name[0] == 0) return TAWC_EINVAL;
	int internal_fd = -1;
	shm_lock();
	struct tawcroot_shm_entry *e = find_entry_locked(name);
	if (!e) {
		shm_unlock();
		return TAWC_ENOENT;
	}
	internal_fd = e->fd;
	e->in_use = 0;
	e->fd = -1;
	e->name[0] = 0;
	shm_unlock();

	/* Close our internal fd. The kernel-side memfd object stays
	 * alive as long as the guest holds its dup — POSIX shm
	 * "segment lives until the last fd closes" semantics. We don't
	 * remove from `tawcroot_reserved_fds[]` (append-only); the now-
	 * stale entry harmlessly extends the BPF close-trap fast-path,
	 * which the handler-side check resolves correctly because the
	 * fd is no longer in `g_shm`. */
	if (internal_fd >= 0) (void)tawc_close(internal_fd);
	return 0;
}

/* ---------- stat / statx synthesis ---------- */


static void zero_stat(struct stat *out)
{
	uint8_t *p = (uint8_t *)out;
	for (size_t i = 0; i < sizeof *out; i++) p[i] = 0;
}

static void zero_statx(struct statx *out)
{
	uint8_t *p = (uint8_t *)out;
	for (size_t i = 0; i < sizeof *out; i++) p[i] = 0;
}

void tawcroot_shm_stat_dir(struct stat *out)
{
	zero_stat(out);
	out->st_mode = S_IFDIR | 01777;  /* world-RWX with sticky, like /tmp */
	out->st_nlink = 2;
	out->st_uid = 0;
	out->st_gid = 0;
}

void tawcroot_shm_statx_dir(struct statx *out, unsigned int mask)
{
	(void)mask;
	zero_statx(out);
	out->stx_mode = (uint16_t)(S_IFDIR | 01777);
	out->stx_nlink = 2;
	out->stx_uid = 0;
	out->stx_gid = 0;
	/* No STATX_SIZE: stx_size is 0 and a directory has no meaningful
	 * size — advertising the bit while leaving the field 0 is a lie
	 * the guest can observe. */
	out->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK |
			STATX_UID | STATX_GID;
}

long tawcroot_shm_stat_name(const char *name, struct stat *out)
{
	if (!name) return TAWC_EINVAL;
	shm_lock();
	struct tawcroot_shm_entry *e = find_entry_locked(name);
	int fd = e ? e->fd : -1;
	shm_unlock();
	if (fd < 0) return TAWC_ENOENT;

	zero_stat(out);
	long rv = TAWC_RAW(TAWC_SYS_fstatat, fd, (long)"", (long)out,
			   AT_EMPTY_PATH, 0, 0);
	if (rv < 0) return rv;
	out->st_mode = S_IFREG | 0600;
	out->st_uid = 0;
	out->st_gid = 0;
	return 0;
}

long tawcroot_shm_statx_name(const char *name, struct statx *out,
			     unsigned int mask)
{
	if (!name) return TAWC_EINVAL;
	shm_lock();
	struct tawcroot_shm_entry *e = find_entry_locked(name);
	int fd = e ? e->fd : -1;
	shm_unlock();
	if (fd < 0) return TAWC_ENOENT;

	zero_statx(out);
	long rv = TAWC_RAW(TAWC_SYS_statx, fd, (long)"",
			   AT_EMPTY_PATH, mask, (long)out, 0);
	if (rv < 0) return rv;
	out->stx_mode = (uint16_t)(S_IFREG | 0600);
	out->stx_uid = 0;
	out->stx_gid = 0;
	out->stx_mask |= STATX_TYPE | STATX_MODE | STATX_UID | STATX_GID;
	return 0;
}

long tawcroot_shm_access_dir(void)
{
	return 0;
}

long tawcroot_shm_access_name(const char *name)
{
	if (!name) return TAWC_EINVAL;
	shm_lock();
	struct tawcroot_shm_entry *e = find_entry_locked(name);
	int present = (e != 0);
	shm_unlock();
	return present ? 0 : TAWC_ENOENT;
}

/* ---------- exec_state ferry ---------- */

size_t tawcroot_shm_export_all(char (*names_out)[TAWCROOT_SHM_NAME_MAX + 1],
			       int *fds_out, size_t cap)
{
	size_t n = 0;
	shm_lock();
	for (size_t i = 0; i < TAWCROOT_SHM_MAX && n < cap; i++) {
		if (!g_shm[i].in_use) continue;
		/* COPY the name bytes while the lock is held. Returning
		 * pointers into the live table let a concurrent shm_unlink
		 * (name[0] = 0) or slot reuse garble the name between
		 * export and serialization. */
		size_t k = 0;
		while (g_shm[i].name[k]) {
			names_out[n][k] = g_shm[i].name[k];
			k++;
		}
		names_out[n][k] = 0;
		fds_out[n] = g_shm[i].fd;
		n++;
	}
	shm_unlock();
	return n;
}

long tawcroot_shm_register(const char *name, int fd)
{
	if (!name || name[0] == 0 || fd < 0) return TAWC_EINVAL;
	size_t nlen = 0;
	while (name[nlen]) nlen++;
	if (nlen > TAWCROOT_SHM_NAME_MAX) return TAWC_ENAMETOOLONG;

	shm_lock();
	if (find_entry_locked(name)) {
		shm_unlock();
		return TAWC_EEXIST;
	}
	struct tawcroot_shm_entry *slot = find_free_locked();
	if (!slot) {
		shm_unlock();
		return TAWC_ENOSPC;
	}
	for (size_t i = 0; i < nlen; i++) slot->name[i] = name[i];
	slot->name[nlen] = 0;
	slot->fd = fd;
	slot->in_use = 1;
	add_to_reserved_list(fd);
	shm_unlock();
	return 0;
}

void tawcroot_shm_reset(void)
{
	shm_lock();
	for (size_t i = 0; i < TAWCROOT_SHM_MAX; i++) {
		g_shm[i].in_use = 0;
		g_shm[i].fd = -1;
		g_shm[i].name[0] = 0;
	}
	shm_unlock();
}
