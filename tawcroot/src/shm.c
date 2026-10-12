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
#include "linkstore.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "shm.h"
#include "stat_compat.h"
#include "syscalls_fs.h"
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

static struct tawcroot_shm_entry *find_entry_locked(const char *name)
{
	for (size_t i = 0; i < TAWCROOT_SHM_MAX; i++) {
		if (g_shm[i].in_use && tawc_streq(g_shm[i].name, name))
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
 * re-opening the internal fd through /proc/self/fd/<internal> with
 * the guest's requested access mode. A plain F_DUPFD shares ONE file
 * description, so (a) an O_RDONLY opener got a writable fd and (b) all
 * guest fds for a name shared a read/write/lseek offset. The re-open
 * gives a distinct description with the right access mode and its own
 * offset — matching real /dev/shm. Android SELinux denies it for
 * memfds; see reopen_for_guest. Returns the new guest fd or -errno. */
static long proc_reopen(int internal_fd, int flags)
{
	char path[32];
	long n = tawc_proc_fd_path(path, sizeof path, internal_fd, 0);
	if (n < 0) return n;
	int oflags = (flags & O_ACCMODE);
	if (flags & O_CLOEXEC) oflags |= O_CLOEXEC;
	return tawc_openat(AT_FDCWD, path, oflags, 0);
}

static long migrate_fds(const int *fds, size_t n);

/* proc_reopen, else — for a narrower-mode reopen of a segment this
 * process created — migrate it to a file (which /proc/self/fd can
 * reopen) and retry, else F_DUPFD (degraded but functional: shared
 * offset, access mode = internal's). Caller holds g_shm_lock. */
static long reopen_for_guest(struct tawcroot_shm_entry *e, int flags)
{
	long fd = proc_reopen(e->fd, flags);
	if (fd >= 0) return fd;
	if ((flags & O_ACCMODE) != O_RDWR && e->guest_fd >= 0 &&
	    e->pid == (int)tawc_getpid()) {
		int fds[2] = { e->guest_fd, e->fd };
		if (migrate_fds(fds, 2) == 0) {
			fd = proc_reopen(e->fd, flags);
			if (fd >= 0) return fd;
		}
	}
	return tawc_fcntl(e->fd,
			  (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
}

/* Publish through fdtab's one entry point (it owns the memory ordering
 * for the lock-free readers in the SIGSYS handler). A failure means the
 * table was full: the fd stays usable internally but is NOT protected —
 * tawcroot_fd_is_reserved won't recognise it, so the trapped close()
 * forwards to the kernel and the guest really closes our memfd.
 * Unreachable in practice (64 slots vs ≤ 33 live users, and shm_unlink
 * gives its slot back). */
static void add_to_reserved_list(int fd)
{
	(void)tawcroot_fd_record_reserved(fd);
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
		guest_fd = reopen_for_guest(e, flags);
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
		guest_fd = proc_reopen((int)internal, flags);
		if (guest_fd < 0)
			guest_fd = tawc_fcntl((int)internal,
					      (flags & O_CLOEXEC)
					      ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
		if (guest_fd < 0) {
			tawc_close((int)internal);
			shm_unlock();
			return guest_fd;
		}

		for (size_t i = 0; i < nlen; i++) slot->name[i] = name[i];
		slot->name[nlen] = 0;
		slot->fd = (int)internal;
		slot->guest_fd = (int)guest_fd;
		slot->pid = (int)tawc_getpid();
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
	e->guest_fd = -1;
	e->name[0] = 0;
	shm_unlock();

	/* Close our internal fd. The kernel-side memfd object stays
	 * alive as long as the guest holds its dup — POSIX shm
	 * "segment lives until the last fd closes" semantics. Drop the
	 * reserved-table entry FIRST: once the fd is closed the kernel can
	 * hand that number to the guest, and a stale entry would make us
	 * lie about a descriptor the guest owns. */
	if (internal_fd >= 0) {
		tawcroot_fd_forget_reserved(internal_fd);
		(void)tawc_close(internal_fd);
	}
	return 0;
}

/* ---------- migration to file backing ---------- */

/* A migrated segment is an unnamed O_TMPFILE with mode
 * SHM_FILE_TAG | seals: sticky + nlink 0 marks it (recognisable from
 * the fd alone, in any process it reaches), and the group/other bits
 * hold its F_SEAL_* set (SEAL..EXEC, 6 bits). */
#define SHM_FILE_TAG   01600
#define SHM_SEAL_MASK  077

static long raw_fstat(int fd, struct stat *st)
{
	return TAWC_RAW(TAWC_SYS_fstat, fd, (long)st, 0, 0, 0, 0);
}

static int in_list(int fd, const int *fds, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (fds[i] == fd) return 1;
	return 0;
}

/* 1 if an open fd outside `fds` refers to (dev, ino), 0 if none,
 * -errno if the scan failed. `buf` is one scratch buffer. */
static long other_fd_has_inode(const int *fds, size_t nfds,
			       unsigned long dev, unsigned long ino,
			       char *buf)
{
	long dfd = tawc_openat(AT_FDCWD, "/proc/self/fd",
			       O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (dfd < 0) return dfd;
	long found = 0;
	for (;;) {
		long n = tawc_getdents64((int)dfd, buf,
					 TAWCROOT_PATH_SCRATCH_SIZE);
		if (n <= 0) {
			if (n < 0) found = n;
			break;
		}
		long off = 0;
		/* linux_dirent64: d_reclen u16 at +16, d_name at +19. */
		while (off + 19 < n) {
			unsigned short reclen;
			memcpy(&reclen, buf + off + 16, sizeof reclen);
			if (reclen < 20 || off + reclen > n) break;
			const char *nm = buf + off + 19;
			off += reclen;
			if (nm[0] < '0' || nm[0] > '9') continue;
			long k = tawc_parse_long(nm);
			if (k < 0 || k == dfd || in_list((int)k, fds, nfds))
				continue;
			struct stat st;
			if (raw_fstat((int)k, &st) == 0 &&
			    st.st_dev == dev && st.st_ino == ino) {
				found = 1;
				goto out;
			}
		}
	}
out:
	tawc_close((int)dfd);
	return found;
}

/* 1 if /proc/self/maps has a mapping of (dev, ino), 0 if none,
 * -errno on failure. Line shape: "range perms offset MAJ:MIN inode
 * path"; parsed as a stream so lines may straddle reads. */
static long maps_has_inode(unsigned long dev, unsigned long ino, char *buf)
{
	unsigned long want_maj = ((dev >> 8) & 0xfff) |
				 ((dev >> 32) & ~0xfffUL);
	unsigned long want_min = (dev & 0xff) | ((dev >> 12) & ~0xffUL);
	long fd = tawc_openat(AT_FDCWD, "/proc/self/maps",
			      O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	long found = 0;
	int field = 0, prev_space = 0, colon = 0;
	unsigned long maj = 0, min = 0, in = 0;
	for (;;) {
		long n = tawc_read((int)fd, buf, TAWCROOT_PATH_SCRATCH_SIZE);
		if (n <= 0) {
			if (n < 0) found = n;
			break;
		}
		for (long i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n') {
				if (field >= 4 && in == ino &&
				    maj == want_maj && min == want_min) {
					found = 1;
					goto out;
				}
				field = prev_space = colon = 0;
				maj = min = in = 0;
				continue;
			}
			if (field > 4) continue;
			if (c == ' ') {
				if (!prev_space) field++;
				prev_space = 1;
				continue;
			}
			prev_space = 0;
			if (field == 3) {
				if (c == ':') { colon = 1; continue; }
				unsigned long d =
					(c >= '0' && c <= '9') ? (unsigned long)(c - '0') :
					(c >= 'a' && c <= 'f') ? (unsigned long)(c - 'a' + 10) :
					(unsigned long)(c - 'A' + 10);
				if (colon) min = min * 16 + d;
				else maj = maj * 16 + d;
			} else if (field == 4) {
				in = in * 10 + (unsigned long)(c - '0');
			}
		}
	}
out:
	tawc_close((int)fd);
	return found;
}

/* Copy `size` bytes src → dst, skipping all-zero chunks (dst was
 * ftruncated to size, so holes read back as zeros). */
static long copy_contents(int src, int dst, long size, char *buf)
{
	long r = TAWC_RAW(TAWC_SYS_ftruncate, dst, size, 0, 0, 0, 0);
	if (r < 0) return r;
	for (long off = 0; off < size; ) {
		long n = tawc_pread64(src, buf, TAWCROOT_PATH_SCRATCH_SIZE,
				      off);
		if (n < 0) return n;
		if (n == 0) break;
		int zero = 1;
		for (long i = 0; i < n; i++)
			if (buf[i]) { zero = 0; break; }
		if (!zero) {
			long w = TAWC_RAW(TAWC_SYS_pwrite64, dst, (long)buf,
					  n, off, 0, 0);
			if (w != n) return w < 0 ? w : TAWC_EIO;
		}
		off += n;
	}
	return 0;
}

/* Move the memfd behind `fds` (all naming one inode) onto a fresh
 * file: dup3 it over each fd so holders keep their numbers, CLOEXEC
 * state, status flags and offset. Refuses (-EBUSY) when the memfd is
 * visible anywhere else in this process — another fd or a mapping
 * would silently keep the old object. Fds in other processes (a child
 * forked in between) can't be seen; accepted, neither browser forks
 * between create and reopen. Returns 0 or -errno (nothing changed). */
static long migrate_fds(const int *fds, size_t n)
{
	struct stat st;
	long r = raw_fstat(fds[0], &st);
	if (r < 0) return r;
	for (size_t i = 1; i < n; i++) {
		struct stat o;
		r = raw_fstat(fds[i], &o);
		if (r < 0) return r;
		if (o.st_dev != st.st_dev || o.st_ino != st.st_ino)
			return TAWC_EBUSY;
	}
	/* Memfds (shmem) answer F_GET_SEALS; plain files EINVAL. */
	long seals = tawc_fcntl(fds[0], F_GET_SEALS, 0);
	if (seals < 0) return seals;
	if (tawcroot_rootfs_fd < 0) return TAWC_ENOENT;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	r = other_fd_has_inode(fds, n, st.st_dev, st.st_ino, scratch->buf[0]);
	if (r == 0) r = maps_has_inode(st.st_dev, st.st_ino, scratch->buf[0]);
	if (r != 0) return r > 0 ? TAWC_EBUSY : r;

	/* Unnamed, so nothing to unlink or sweep; both dirs are
	 * app-private (the guest can't see the inode by name). Prefer the
	 * store's tmp/: the rootfs root may deny the owner write (Arch
	 * ships `/` 0555) and O_TMPFILE needs it. */
	long tf = TAWC_ENOENT;
	long sd = tawcroot_linkstore_tmp_dirfd();
	if (sd >= 0)
		tf = tawc_openat((int)sd, ".",
				 TAWC_O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
	if (tf < 0)
		tf = tawc_openat(tawcroot_rootfs_fd, ".",
				 TAWC_O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
	if (tf < 0) return tf;
	r = TAWC_RAW(TAWC_SYS_fchmod, tf,
		     SHM_FILE_TAG | (seals & SHM_SEAL_MASK), 0, 0, 0, 0);
	if (r == 0 && st.st_size > 0)
		r = copy_contents(fds[0], (int)tf, (long)st.st_size,
				  scratch->buf[0]);
	long fl = tawc_fcntl(fds[0], F_GETFL, 0);
	long off = tawc_lseek(fds[0], 0, 1 /* SEEK_CUR */);
	if (r == 0 && fl >= 0) r = tawc_fcntl((int)tf, F_SETFL, fl);
	if (r == 0 && off > 0) {
		long s2 = tawc_lseek((int)tf, off, 0 /* SEEK_SET */);
		if (s2 < 0) r = s2;
	}
	if (r < 0) {
		tawc_close((int)tf);
		return r;
	}
	for (size_t i = 0; i < n; i++) {
		long fdfl = tawc_fcntl(fds[i], F_GETFD, 0);
		if (fdfl < 0) fdfl = 0;
		(void)TAWC_RAW(TAWC_SYS_dup3, tf, fds[i],
			       (fdfl & FD_CLOEXEC) ? O_CLOEXEC : 0, 0, 0, 0);
	}
	tawc_close((int)tf);
	return 0;
}

long tawcroot_shm_migrate_guest_memfd(int fd)
{
	if (fd < 0 || tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	/* Only memfds: tmpfs files also answer F_GET_SEALS. */
	char path[32], link[7];
	long r = tawc_proc_fd_path(path, sizeof path, fd, 0);
	if (r < 0) return r;
	r = tawc_readlinkat(AT_FDCWD, path, link, sizeof link);
	if (r < 0) return r;
	if (r != sizeof link || memcmp(link, "/memfd:", sizeof link) != 0)
		return TAWC_EINVAL;
	int fds[1] = { fd };
	return migrate_fds(fds, 1);
}

int tawcroot_shm_seal_fcntl(int fd, int op, long arg, long *ret)
{
	if (op != F_ADD_SEALS && op != F_GET_SEALS) return 0;
	struct stat st;
	if (raw_fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_nlink != 0 || (st.st_mode & 07700) != SHM_FILE_TAG)
		return 0;
	long cur = (long)(st.st_mode & SHM_SEAL_MASK);
	if (op == F_GET_SEALS) {
		*ret = cur;
		return 1;
	}
	/* Kernel order: fd must be writable, known bits, not sealed. */
	long fl = tawc_fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		*ret = fl;
	else if ((fl & O_ACCMODE) == O_RDONLY)
		*ret = TAWC_EPERM;
	else if ((unsigned long)arg & ~(unsigned long)SHM_SEAL_MASK)
		*ret = TAWC_EINVAL;
	else if (cur & F_SEAL_SEAL)
		*ret = TAWC_EPERM;
	else
		*ret = TAWC_RAW(TAWC_SYS_fchmod, fd,
				SHM_FILE_TAG | cur | arg, 0, 0, 0, 0);
	return 1;
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
	/* Mount id: the synthetic dir has no backing fd, but its
	 * "contents" are memfds, and every memfd lives on the kernel's
	 * one internal shm_mnt — so a throwaway probe memfd yields the
	 * id the entries report, and one probe serves the process
	 * lifetime. Without it, systemd chase()ing into /dev/shm
	 * EUNATCHes at the dir step (see tawcroot_statx_fill_mnt_id).
	 * The unsynchronized cache is benign: every racer computes and
	 * stores the same value. */
	if (mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) {
		/* Two flags, not a 0-means-unprobed sentinel: shm_mnt is
		 * kernel-internal and its id genuinely can be 0. Racing
		 * probers all store identical values, so the unordered
		 * writes are benign. */
		static uint64_t shm_mnt_id_cache;
		static int shm_mnt_id_known;
		if (!shm_mnt_id_known) {
			long pfd = tawc_memfd_create("tawcroot-mntid-probe",
						     MFD_CLOEXEC);
			if (pfd < 0) return;
			struct statx px;
			zero_statx(&px);
			tawcroot_statx_fill_mnt_id((int)pfd, 0, STATX_MNT_ID,
						   &px);
			(void)tawc_close((int)pfd);
			if (!(px.stx_mask & STATX_MNT_ID)) return;
			shm_mnt_id_cache = px.stx_mnt_id;
			shm_mnt_id_known = 1;
		}
		out->stx_mnt_id = shm_mnt_id_cache;
		out->stx_mask |= STATX_MNT_ID;
	}
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
	long rv = tawcroot_statx_compat(fd, "", AT_EMPTY_PATH, mask, out);
	if (rv < 0) return rv;
	out->stx_mode = (uint16_t)(S_IFREG | 0600);
	out->stx_uid = 0;
	out->stx_gid = 0;
	out->stx_mask |= STATX_TYPE | STATX_MODE | STATX_UID | STATX_GID;
	/* Pre-5.8 kernels return no STATX_MNT_ID for the memfd; systemd
	 * chase()ing into /dev/shm EUNATCHes without one (see
	 * tawcroot_statx_fill_mnt_id). Same shm_mnt id the dir
	 * synthesizer reports. */
	tawcroot_statx_fill_mnt_id(fd, NULL, mask, out);
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
	/* The creator's guest fd isn't ferried: no migration after exec. */
	slot->guest_fd = -1;
	slot->pid = 0;
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
		g_shm[i].guest_fd = -1;
		g_shm[i].name[0] = 0;
	}
	shm_unlock();
}
