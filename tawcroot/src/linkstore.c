/* Hardlink-emulation store. See include/linkstore.h for the contract
 * and notes/tawcroot/link-emulation.md for the design; comments
 * here cover only implementation-local reasoning.
 *
 * Lives in PROD_C (and PROD_C_FOR_TESTS): raw syscalls only, fixed
 * buffers, no allocation — async-signal-safe like every handler
 * dependency. */

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
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

/* ------------------------------------------------------------------ */
/* State                                                               */

int    tawcroot_store_link_fd            = -1;
char   tawcroot_store_host_path[4096]    = {0};
size_t tawcroot_store_host_path_len      = 0;
unsigned long tawcroot_store_dev         = 0;

static int g_state    = TAWCROOT_STORE_OFF;
static int g_store_fd = -1;   /* store root dir (tawcroot/) */
static int g_work_fd  = -1;   /* work/ */
static int g_tmp_fd   = -1;   /* tmp/ (linkable O_TMPFILE objects) */
static int g_lock_fd  = -1;   /* lock file, O_CLOEXEC, opened lazily */

#define STORE_VERSION_SUPPORTED 1

/* Forward decl — store_lock's LATENT upgrade needs it. */
static long store_open(int create);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

/* Kernel struct flock, LP64 layout (identical on aarch64 and x86_64).
 * Defined locally so hosted builds don't collide with glibc's. */
struct tawc_flock {
	short l_type;
	short l_whence;
	long  l_start;
	long  l_len;
	int   l_pid;
};

#ifndef F_RDLCK
# define F_RDLCK 0
# define F_WRLCK 1
# define F_UNLCK 2
#endif
#ifndef F_SETLK
# define F_SETLK  6
# define F_SETLKW 7
#endif

static long xunlinkat(int dirfd, const char *name)
{
	return TAWC_RAW(TAWC_SYS_unlinkat, dirfd, (long)name, 0, 0, 0, 0);
}

static long xrename(int ofd, const char *o, int nfd, const char *n,
		    unsigned int flags)
{
	return TAWC_RAW(TAWC_SYS_renameat2, ofd, (long)o, nfd, (long)n,
			flags, 0);
}

static long xsymlink(const char *target, int dirfd, const char *name)
{
	return TAWC_RAW(TAWC_SYS_symlinkat, (long)target, dirfd,
			(long)name, 0, 0, 0);
}

static int xexists(int dirfd, const char *name)
{
	struct stat st;
	return TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)name, (long)&st,
			AT_SYMLINK_NOFOLLOW, 0, 0) == 0;
}

/* io.h's tawc_long_to_str is signed; inode numbers need the full u64
 * range, hence a local unsigned variant. */
static void u64_to_dec(unsigned long v, char *out /* >= 21 bytes */)
{
	char tmp[21];
	int i = 0;
	do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	int j = 0;
	while (i) out[j++] = tmp[--i];
	out[j] = 0;
}

/* "<tok><suffix>" into out (e.g. work-entry names, ".cnt" sidecars). */
static long tok_name(char *out, size_t cap, const char *tok,
		     const char *suffix)
{
	size_t pos = 0;
	long e = tawc_str_append(out, cap, &pos, tok);
	if (!e) e = tawc_str_append(out, cap, &pos, suffix);
	return e;
}

/* "tawcroot:link:<tok>" into out. */
static long tok_target(char *out, size_t cap, const char *tok)
{
	size_t pos = 0;
	long e = tawc_str_append(out, cap, &pos, TAWCROOT_LINK_TOKEN_PREFIX);
	if (!e) e = tawc_str_append(out, cap, &pos, tok);
	return e;
}

int tawcroot_link_target_is_token(const char *target, const char **tok_out)
{
	if (!target) return 0;
	if (memcmp(target, TAWCROOT_LINK_TOKEN_PREFIX,
		   TAWCROOT_LINK_TOKEN_PREFIX_LEN) != 0)
		return 0;
	const char *tok = target + TAWCROOT_LINK_TOKEN_PREFIX_LEN;
	size_t n = 0;
	while (tok[n]) {
		char c = tok[n];
		if (!((c >= '0' && c <= '9') || c == '-')) return 0;
		n++;
	}
	if (n == 0 || n >= TAWCROOT_LINK_TOKEN_MAX) return 0;
	if (tok_out) *tok_out = tok;
	return 1;
}

long tawcroot_link_leaf_token(int dirfd, const char *suffix,
			      char *tok, size_t tok_cap)
{
	/* Token targets fit in 46 bytes; a longer target cannot be one.
	 * A 64-byte probe that comes back full is therefore "not a
	 * token" without a second read. */
	char t[64];
	long n = TAWC_RAW(TAWC_SYS_readlinkat, dirfd, (long)suffix,
			  (long)t, (long)sizeof t, 0, 0);
	if (n == TAWC_EINVAL || n == TAWC_ENOENT || n == TAWC_ENOTDIR)
		return 0;
	if (n < 0) return n;
	if ((size_t)n >= sizeof t) return 0;
	t[n] = 0;
	const char *p;
	if (!tawcroot_link_target_is_token(t, &p)) return 0;
	long ce = tawc_str_copy(tok, tok_cap, p);
	return ce < 0 ? ce : 1;
}

/* ------------------------------------------------------------------ */
/* Refcount sidecars                                                   */

#define CNT_UNKNOWN (~0UL)
/* Fixed-width record: 10 digits + '\n'. One pwrite at offset 0 —
 * atomic against SIGKILL and lock-free readers (intent-free CLOBBER
 * can die mid-update with nothing to repair it). */
#define CNT_BYTES 11

static unsigned long cnt_read(const char *tok)
{
	char name[TAWCROOT_LINK_TOKEN_MAX + 8];
	if (tok_name(name, sizeof name, tok, ".cnt")) return CNT_UNKNOWN;
	long fd = tawc_openat(tawcroot_store_link_fd, name,
			      O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return CNT_UNKNOWN;
	char buf[CNT_BYTES + 1];
	long n = tawc_read((int)fd, buf, CNT_BYTES);
	tawc_close((int)fd);
	if (n != CNT_BYTES || buf[CNT_BYTES - 1] != '\n') return CNT_UNKNOWN;
	unsigned long v = 0;
	for (int i = 0; i < CNT_BYTES - 1; i++) {
		if (buf[i] < '0' || buf[i] > '9') return CNT_UNKNOWN;
		v = v * 10 + (unsigned long)(buf[i] - '0');
	}
	if (v == 0) return CNT_UNKNOWN;  /* count 0 with a live sidecar is torn state */
	return v;
}

static long cnt_write(const char *tok, unsigned long v)
{
	if (v == CNT_UNKNOWN) return 0;  /* unmaintainable count: leave as-is */
	char name[TAWCROOT_LINK_TOKEN_MAX + 8];
	long e = tok_name(name, sizeof name, tok, ".cnt");
	if (e) return e;
	long fd = tawc_openat(tawcroot_store_link_fd, name,
			      O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) return fd;
	char buf[CNT_BYTES];
	for (int i = CNT_BYTES - 2; i >= 0; i--) {
		buf[i] = (char)('0' + v % 10);
		v /= 10;
	}
	buf[CNT_BYTES - 1] = '\n';
	long w = TAWC_RAW(TAWC_SYS_pwrite64, fd, (long)buf, CNT_BYTES,
			  0, 0, 0);
	tawc_close((int)fd);
	return w == CNT_BYTES ? 0 : (w < 0 ? w : TAWC_EIO);
}

static void cnt_unlink(const char *tok)
{
	char name[TAWCROOT_LINK_TOKEN_MAX + 8];
	if (!tok_name(name, sizeof name, tok, ".cnt"))
		(void)xunlinkat(tawcroot_store_link_fd, name);
}

unsigned long tawcroot_link_count_for_stat(const char *tok)
{
	if (tawcroot_store_link_fd < 0) return 2;
	unsigned long v = cnt_read(tok);
	return (v == CNT_UNKNOWN || v < 1) ? 2 : v;
}

/* Shared matcher for "<store><sub><name>" host paths; `sub` is
 * "/link/" or "/tmp/". The remainder is validated with the token
 * charset/length rules (both name families are inode decimals with an
 * optional collision suffix), which also rejects readlink's
 * " (deleted)" suffix and any deeper path components. */
static int host_path_in_store(const char *path, const char *sub,
			      char *name, size_t name_cap)
{
	if (!path || tawcroot_store_host_path_len == 0) return 0;
	size_t n = tawcroot_store_host_path_len;
	size_t sn = strlen(sub);
	if (strlen(path) <= n + sn) return 0;
	if (memcmp(path, tawcroot_store_host_path, n) != 0) return 0;
	if (memcmp(path + n, sub, sn) != 0) return 0;
	const char *rem = path + n + sn;
	char tgt[64];
	size_t pos = 0;
	if (tawc_str_append(tgt, sizeof tgt, &pos, TAWCROOT_LINK_TOKEN_PREFIX) ||
	    tawc_str_append(tgt, sizeof tgt, &pos, rem))
		return 0;
	const char *p;
	if (!tawcroot_link_target_is_token(tgt, &p)) return 0;
	return tawc_str_copy(name, name_cap, p) < 0 ? 0 : 1;
}

int tawcroot_link_host_path_token(const char *path, char *tok,
				  size_t tok_cap)
{
	return host_path_in_store(path, "/link/", tok, tok_cap);
}

int tawcroot_link_host_path_tmp(const char *path, char *name,
				size_t name_cap)
{
	return host_path_in_store(path, "/tmp/", name, name_cap);
}

/* ------------------------------------------------------------------ */
/* Version file                                                        */

static long version_read(void)
{
	long fd = tawc_openat(g_store_fd, "version",
			      O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	char buf[16];
	long n = tawc_read((int)fd, buf, sizeof buf - 1);
	tawc_close((int)fd);
	if (n <= 0) return TAWC_EINVAL;
	long v = 0;
	for (long i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return v > 0 ? v : TAWC_EINVAL;
}

/* version > supported → flip to DEGRADED (token detection stays on,
 * mutations refuse). One helper for the open-time and lock-time checks
 * so the two can never diverge across a version bump. Deliberately
 * silent: both callers can run in SIGSYS-handler context, where io.h's
 * fd-2 logging is forbidden (it would splice into the guest program's
 * stderr); configure() logs the one session-start occurrence from
 * supervisor context. */
static int version_gate(long ver)
{
	if (ver <= STORE_VERSION_SUPPORTED) return 0;
	g_state = TAWCROOT_STORE_DEGRADED;
	return 1;
}

static long version_write_initial(void)
{
	long fd = tawc_openat(g_store_fd, "version.new",
			      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return fd;
	static const char v[] = "1\n";
	long w = tawc_write((int)fd, v, 2);
	tawc_close((int)fd);
	if (w != 2) return w < 0 ? w : TAWC_EIO;
	return xrename(g_store_fd, "version.new", g_store_fd, "version", 0);
}

/* ------------------------------------------------------------------ */
/* Intent slot                                                         */

enum { OP_NONE = 0, OP_ADD = 1, OP_DEL = 2, OP_NEW = 3 };

struct intent_hdr {
	uint32_t magic;      /* 'TWLI' */
	uint32_t op;
	uint64_t cnt;        /* count-before; CNT_UNKNOWN when unmaintained */
	uint32_t tok_len;    /* excl. NUL */
	uint32_t p1_len;
	uint32_t p2_len;
};
#define INTENT_MAGIC 0x494c5754u

/* Write the slot via intent.new + rename so it is never torn. Paths
 * are HOST-real (guest views depend on per-session bind tables and
 * chroot state that recovery cannot assume); the file is store-
 * internal and never guest-visible. */
static long intent_write(uint32_t op, const char *tok, unsigned long cnt,
			 const char *p1, const char *p2)
{
	struct intent_hdr h = {
		.magic   = INTENT_MAGIC,
		.op      = op,
		.cnt     = cnt,
		.tok_len = (uint32_t)strlen(tok),
		.p1_len  = (uint32_t)(p1 ? strlen(p1) : 0),
		.p2_len  = (uint32_t)(p2 ? strlen(p2) : 0),
	};
	long fd = tawc_openat(g_store_fd, "intent.new",
			      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return fd;
	/* Every write must land in full (tawc_write is a one-shot raw
	 * syscall, no retry loop): a short-written intent.new renamed into
	 * the slot would read as garbled == OP_NONE at recovery — the
	 * mutation would run believing it is journaled while its crash
	 * window is unprotected. Short → abort the mutation instead. */
	int ok = tawc_write((int)fd, &h, sizeof h) == (long)sizeof h &&
		 tawc_write((int)fd, tok, h.tok_len) == (long)h.tok_len &&
		 (!h.p1_len ||
		  tawc_write((int)fd, p1, h.p1_len) == (long)h.p1_len) &&
		 (!h.p2_len ||
		  tawc_write((int)fd, p2, h.p2_len) == (long)h.p2_len);
	tawc_close((int)fd);
	if (!ok) {
		(void)xunlinkat(g_store_fd, "intent.new");
		return TAWC_EIO;
	}
	return xrename(g_store_fd, "intent.new", g_store_fd, "intent", 0);
}

static void intent_clear(void)
{
	(void)xunlinkat(g_store_fd, "intent");
}

/* Parse the slot into caller buffers. Returns the op (OP_NONE when the
 * slot is empty/absent/garbled — a garbled slot is cleared: it can only
 * come from a pre-crash torn intent.new that never got renamed, or fs
 * corruption; either way there is nothing recoverable in it). */
static int intent_read(char *tok, size_t tok_cap, unsigned long *cnt,
		       char *p1, size_t p1_cap, char *p2, size_t p2_cap)
{
	long fd = tawc_openat(g_store_fd, "intent", O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return OP_NONE;
	struct intent_hdr h;
	long n = tawc_read((int)fd, &h, sizeof h);
	int op = OP_NONE;
	if (n == sizeof h && h.magic == INTENT_MAGIC &&
	    h.op >= OP_ADD && h.op <= OP_NEW &&
	    h.tok_len > 0 && h.tok_len < tok_cap &&
	    h.p1_len < p1_cap && h.p2_len < p2_cap) {
		if (tawc_read((int)fd, tok, h.tok_len) == (long)h.tok_len &&
		    tawc_read((int)fd, p1, h.p1_len) == (long)h.p1_len &&
		    tawc_read((int)fd, p2, h.p2_len) == (long)h.p2_len) {
			tok[h.tok_len] = 0;
			p1[h.p1_len]   = 0;
			p2[h.p2_len]   = 0;
			*cnt = (unsigned long)h.cnt;
			/* Validate the token so recovery can't be steered
			 * into weird link/ names by a corrupt slot. */
			char tgt[64];
			if (!tok_target(tgt, sizeof tgt, tok)) {
				const char *dummy;
				if (tawcroot_link_target_is_token(tgt, &dummy))
					op = (int)h.op;
			}
		}
	}
	tawc_close((int)fd);
	return op;
}

/* ------------------------------------------------------------------ */
/* Recovery                                                            */

/* True iff the entry at (dirfd, name) is a symlink whose target is
 * exactly `tawcroot:link:<tok>`. The single guard every recovery and
 * live-abort path uses before touching a guest-tree or work/ entry —
 * recorded paths can be re-occupied by anything between crash and
 * repair, and parked entries can be a racing rename's victim. One
 * copy so those paths can never disagree about what belongs to a
 * cluster. */
static int entry_matches_token(int dirfd, const char *name, const char *tok)
{
	if (!name || !name[0]) return 0;
	char t[64];
	long n = TAWC_RAW(TAWC_SYS_readlinkat, dirfd, (long)name,
			  (long)t, (long)sizeof t, 0, 0);
	if (n <= 0 || (size_t)n >= sizeof t) return 0;
	t[n] = 0;
	const char *got;
	if (!tawcroot_link_target_is_token(t, &got)) return 0;
	return tawc_streq(got, tok);
}

/* Repair a pending intent. Runs under the store lock; recheck-then-act
 * with absolute values, idempotent — recovery itself can be killed and
 * re-run. Rolls to the safe side (overcount/leak) wherever a window is
 * ambiguous. */
static void recover_locked(void)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char tok[TAWCROOT_LINK_TOKEN_MAX];
	char *p1 = scratch->buf[0];
	char *p2 = scratch->buf[1];
	unsigned long n = CNT_UNKNOWN;

	int op = intent_read(tok, sizeof tok, &n,
			     p1, TAWCROOT_PATH_SCRATCH_SIZE,
			     p2, TAWCROOT_PATH_SCRATCH_SIZE);
	if (op == OP_NONE) {
		intent_clear();
		return;
	}

	char wname[TAWCROOT_LINK_TOKEN_MAX + 8];

	switch (op) {
	case OP_ADD:
		/* Staged present → the publish never happened: roll back
		 * (count := n, drop staging). Staged absent → either the
		 * publish completed or the crash predates staging; count :=
		 * n+1 covers both (worst case a +1 leak). */
		if (tok_name(wname, sizeof wname, tok, ".add")) break;
		if (xexists(g_work_fd, wname)) {
			(void)cnt_write(tok, n);
			(void)xunlinkat(g_work_fd, wname);
		} else {
			/* The redo increment must land or the count goes
			 * BELOW live referrers. If it can't be written,
			 * drop the sidecar: a missing count reports 2 and
			 * its cluster is never deleted — leak-safe. */
			if (n != CNT_UNKNOWN && cnt_write(tok, n + 1) < 0)
				cnt_unlink(tok);
		}
		break;

	case OP_DEL:
		if (tok_name(wname, sizeof wname, tok, ".del")) break;
		if (xexists(g_work_fd, wname)) {
			if (entry_matches_token(g_work_fd, wname, tok)) {
				/* Roll forward: the name is gone from the
				 * guest tree; finish the decrement. */
				if (n != CNT_UNKNOWN) {
					if (n <= 1) {
						(void)xunlinkat(
							tawcroot_store_link_fd,
							tok);
						cnt_unlink(tok);
					} else {
						(void)cnt_write(tok, n - 1);
					}
				}
			} else if (n != CNT_UNKNOWN) {
				/* Parked entry isn't a reference to this
				 * cluster (racing rename swapped the name
				 * before the park): no decrement. */
				(void)cnt_write(tok, n);
			}
			(void)xunlinkat(g_work_fd, wname);
		} else {
			/* Park never happened, or the op completed through
			 * the parked unlink. Object still present → restore
			 * the pre-op count (a crash after the parked unlink
			 * re-runs this too: +0/+1 overcount, leak-safe). */
			if (xexists(tawcroot_store_link_fd, tok) &&
			    n != CNT_UNKNOWN)
				(void)cnt_write(tok, n);
		}
		break;

	case OP_NEW: {
		char sname[TAWCROOT_LINK_TOKEN_MAX + 8];
		char dname[TAWCROOT_LINK_TOKEN_MAX + 8];
		if (tok_name(sname, sizeof sname, tok, ".src")) break;
		if (tok_name(dname, sizeof dname, tok, ".dst")) break;

		if (xexists(tawcroot_store_link_fd, tok)) {
			/* Object present → roll forward by consuming
			 * still-staged entries only. A publish atomically
			 * destroys its staged entry, so staged-absent is a
			 * store-internal witness the publish happened
			 * exactly once. NEVER recreate a name from a
			 * recorded path — that mints phantom referrers
			 * when lock-free guest renames moved the original.
			 * EEXIST / vanished-parent parks the entry (leak,
			 * safe). */
			if (xexists(g_work_fd, dname) && p2[0])
				(void)xrename(g_work_fd, dname,
					      AT_FDCWD, p2,
					      RENAME_NOREPLACE);
			if (xexists(g_work_fd, sname) && p1[0])
				(void)xrename(g_work_fd, sname,
					      AT_FDCWD, p1,
					      RENAME_NOREPLACE);
		} else {
			/* Object absent → the point of no return was never
			 * crossed: roll back. Delete staged entries; delete
			 * the published dst only if it verifies as the
			 * matching token symlink (moved → content-safe
			 * strand). */
			(void)xunlinkat(g_work_fd, dname);
			(void)xunlinkat(g_work_fd, sname);
			if (entry_matches_token(AT_FDCWD, p2, tok))
				(void)xunlinkat(AT_FDCWD, p2);
			cnt_unlink(tok);
		}
		break;
	}
	}
	intent_clear();
}

/* ------------------------------------------------------------------ */
/* Lock                                                                */

/* In-process mutex: fcntl record locks are owned per-process, so
 * threads of one process would pass through each other's file lock.
 * Plain futex — SIGKILL takes the whole process, so thread-vs-thread
 * exclusion needs no crash robustness. Known bounded failures (same
 * stance as the identity seqlock): a fork while a sibling thread holds
 * this mutex leaves the CHILD's copy locked forever (first link op in
 * the child would wedge — no known workload forks mid-dpkg-burst and
 * then hardlinks), and a guest signal handler interrupting a link op
 * on the same thread that then links again self-deadlocks. */
static int g_mu;

static void mu_lock(void)
{
	int expected = 0;
	if (__atomic_compare_exchange_n(&g_mu, &expected, 1, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	for (;;) {
		int prev = __atomic_exchange_n(&g_mu, 2, __ATOMIC_ACQUIRE);
		if (prev == 0) return;
		(void)TAWC_RAW(TAWC_SYS_futex, (long)&g_mu,
			       128 /*PRIVATE*/ | 0 /*WAIT*/, 2, 0, 0, 0);
	}
}

static void mu_unlock(void)
{
	if (__atomic_exchange_n(&g_mu, 0, __ATOMIC_RELEASE) == 2)
		(void)TAWC_RAW(TAWC_SYS_futex, (long)&g_mu,
			       128 /*PRIVATE*/ | 1 /*WAKE*/, 1, 0, 0, 0);
}

static long file_lock(int fd, short type, int wait)
{
	struct tawc_flock fl = { .l_type = type };
	long rv;
	do {
		rv = TAWC_RAW(TAWC_SYS_fcntl, fd,
			      wait ? F_SETLKW : F_SETLK, (long)&fl, 0, 0, 0);
	} while (rv == TAWC_EINTR);
	return rv;
}

/* Acquire the store mutation lock: in-process mutex, then the fcntl
 * record lock (on a lazily opened, reserved, O_CLOEXEC fd — exec
 * closes it, which releases this process's record locks: the escape
 * degrades to "crashed holder", which the intent slot recovers).
 * Re-checks the version (a guest can outlive the session that started
 * it; a newer APK may migrate the store underneath) and repairs any
 * leftover intent. Returns 0 or -errno with everything released. */
static long store_lock(void)
{
	mu_lock();
	/* Another process (a concurrent session, or a killed child of
	 * this very session) may have created the store since our
	 * configure ran. Upgrade in place — EEXIST-tolerant creation
	 * also heals a store whose creator died between mkdirs. */
	if (g_state == TAWCROOT_STORE_LATENT &&
	    xexists(AT_FDCWD, tawcroot_store_host_path)) {
		if (store_open(1) < 0) g_state = TAWCROOT_STORE_OFF;
	}
	if (g_state != TAWCROOT_STORE_READY) {
		mu_unlock();
		return TAWC_EPERM;
	}
	if (g_lock_fd < 0) {
		long fd = tawc_openat(g_store_fd, "lock",
				      O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (fd < 0) { mu_unlock(); return fd; }
		long resv = tawcroot_fd_reserve((int)fd);
		if (resv < 0) {
			tawc_close((int)fd);
			mu_unlock();
			return resv;
		}
		g_lock_fd = (int)resv;
	}
	long rv = file_lock(g_lock_fd, F_WRLCK, 1);
	if (rv < 0) { mu_unlock(); return rv; }

	long ver = version_read();
	if (ver > 0 && version_gate(ver)) {
		(void)file_lock(g_lock_fd, F_UNLCK, 0);
		mu_unlock();
		return TAWC_EPERM;
	}

	recover_locked();
	return 0;
}

static void store_unlock(void)
{
	(void)file_lock(g_lock_fd, F_UNLCK, 0);
	mu_unlock();
}

/* ------------------------------------------------------------------ */
/* Store open / create                                                 */

static long open_subdir(int parent, const char *name, int create)
{
	if (create) {
		long mk = TAWC_RAW(TAWC_SYS_mkdirat, parent, (long)name,
				   0700, 0, 0, 0);
		if (mk < 0 && mk != TAWC_EEXIST) return mk;
	}
	return tawc_openat(parent, name,
			   O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
}

/* Open (optionally creating) the store at tawcroot_store_host_path and
 * reserve the fds. mkdir steps are EEXIST-tolerant so a killed creator
 * is healed by the next opener. Returns 0 / -errno with no fds leaked
 * on failure. */
static long store_open(int create)
{
	long sfd = open_subdir(AT_FDCWD, tawcroot_store_host_path, create);
	if (sfd < 0) return sfd;
	long lfd = open_subdir((int)sfd, "link", create);
	if (lfd < 0) { tawc_close((int)sfd); return lfd; }
	long wfd = open_subdir((int)sfd, "work", create);
	if (wfd < 0) {
		tawc_close((int)sfd);
		tawc_close((int)lfd);
		return wfd;
	}
	/* tmp/ arrived after link//work/ shipped, so a v1 store created
	 * by earlier code may lack it — create=1 healing covers that (no
	 * version bump: old code never touches tmp/, and its entries are
	 * uncounted scratch no other invariant depends on). */
	long tfd = open_subdir((int)sfd, "tmp", create);
	if (tfd < 0) {
		tawc_close((int)sfd);
		tawc_close((int)lfd);
		tawc_close((int)wfd);
		return tfd;
	}

	long r1 = tawcroot_fd_reserve((int)sfd);
	long r2 = r1 < 0 ? r1 : tawcroot_fd_reserve((int)lfd);
	long r3 = r2 < 0 ? r2 : tawcroot_fd_reserve((int)wfd);
	long r4 = r3 < 0 ? r3 : tawcroot_fd_reserve((int)tfd);
	if (r4 < 0) {
		/* Reserve failures close nothing themselves; drop every fd
		 * a failed-or-skipped reserve left ours (an early failure
		 * propagates through r2..r4, so plain `< 0` covers both the
		 * failing slot and every skipped one after it — the old
		 * `&& prev >= 0` guards leaked all later fds into guest-
		 * reachable low numbers). Already-reserved fds stay
		 * reserved (the table has no remove) — harmless, idle. */
		if (r1 < 0) tawc_close((int)sfd);
		if (r2 < 0) tawc_close((int)lfd);
		if (r3 < 0) tawc_close((int)wfd);
		if (r4 < 0) tawc_close((int)tfd);
		return r4;
	}
	g_store_fd = (int)r1;
	tawcroot_store_link_fd = (int)r2;
	g_work_fd = (int)r3;
	g_tmp_fd = (int)r4;

	/* Canonicalise the store path from the opened fd (same trick as
	 * supervisor step 2 for the rootfs): the -r argument may spell it
	 * through symlinks — /data/user/0 → /data/data on Android — and
	 * every host-path comparison against /proc fd links (linkat
	 * source backstops, fd-nlink fixup, tmpfile publish) needs the
	 * kernel's canonical spelling or it silently misses. */
	long cl = tawcroot_proc_fd_to_host_path(
		g_store_fd, tawcroot_store_host_path,
		sizeof tawcroot_store_host_path);
	if (cl > 0) tawcroot_store_host_path_len = (size_t)cl;

	/* Cache the store's device: the fd-nlink fixup's early-out for
	 * fds on other filesystems (/system binds, memfds) — objects can
	 * only live on the store's own fs. */
	struct stat sst;
	if (TAWC_RAW(TAWC_SYS_fstat, g_store_fd, (long)&sst, 0, 0, 0, 0) == 0)
		tawcroot_store_dev = (unsigned long)sst.st_dev;

	long ver = version_read();
	if (ver == TAWC_ENOENT || ver == TAWC_EINVAL) {
		/* Fresh (or killed-mid-creation) store: stamp v1. The check
		 * must ship from the first release — "absence means v1"
		 * fails because v1 binaries would never look. */
		long vw = version_write_initial();
		if (vw < 0) ver = vw; else ver = STORE_VERSION_SUPPORTED;
	}
	if (ver < 0) {
		g_state = TAWCROOT_STORE_OFF;
		return ver;
	}
	if (version_gate(ver)) return 0;
	g_state = TAWCROOT_STORE_READY;
	return 0;
}

/* LATENT → READY on the first mutation that needs the store. */
static long store_ensure(void)
{
	if (g_state == TAWCROOT_STORE_READY) return 0;
	if (g_state != TAWCROOT_STORE_LATENT) return TAWC_EPERM;
	mu_lock();
	long rv = 0;
	if (g_state == TAWCROOT_STORE_LATENT) {
		rv = store_open(1);
		if (rv < 0) g_state = TAWCROOT_STORE_OFF;
		else if (g_state != TAWCROOT_STORE_READY) rv = TAWC_EPERM;
	} else if (g_state != TAWCROOT_STORE_READY) {
		rv = TAWC_EPERM;
	}
	mu_unlock();
	return rv;
}

int tawcroot_linkstore_state(void)
{
	return g_state;
}

int tawcroot_linkstore_latent_upgrade(void)
{
	if (g_state == TAWCROOT_STORE_READY) return tawcroot_store_link_fd;
	if (g_state != TAWCROOT_STORE_LATENT) return -1;
	mu_lock();
	if (g_state == TAWCROOT_STORE_LATENT &&
	    xexists(AT_FDCWD, tawcroot_store_host_path)) {
		if (store_open(1) < 0) g_state = TAWCROOT_STORE_OFF;
	}
	mu_unlock();
	return g_state == TAWCROOT_STORE_READY ? tawcroot_store_link_fd : -1;
}

void tawcroot_linkstore_configure(const char *store_host_path)
{
	g_state = TAWCROOT_STORE_OFF;
	g_store_fd = -1;
	tawcroot_store_link_fd = -1;
	g_work_fd = -1;
	g_tmp_fd = -1;
	g_lock_fd = -1;
	tawcroot_store_host_path[0] = 0;
	tawcroot_store_host_path_len = 0;
	tawcroot_store_dev = 0;

	if (!store_host_path || store_host_path[0] != '/') return;
	long n = tawc_str_copy(tawcroot_store_host_path,
			       sizeof tawcroot_store_host_path,
			       store_host_path);
	if (n <= 0) return;
	tawcroot_store_host_path_len = (size_t)n;

	if (!xexists(AT_FDCWD, tawcroot_store_host_path)) {
		g_state = TAWCROOT_STORE_LATENT;
		return;
	}
	/* create=1 even though the root exists: the EEXIST-tolerant
	 * subdir mkdirs heal a store whose creator was killed between
	 * steps. */
	if (store_open(1) < 0) {
		g_state = TAWCROOT_STORE_OFF;
		return;
	}
	/* The one place the version gate may log: configure runs from
	 * supervisor init, never a SIGSYS handler (io.h contract). */
	if (g_state == TAWCROOT_STORE_DEGRADED) {
		tawc_io_str("tawcroot: linkstore version is newer than "
			    "supported; mutations disabled\n");
	}
	/* O(1) session-start crash check: only lock (and repair) when a
	 * previous holder actually died mid-operation. */
	if (g_state == TAWCROOT_STORE_READY &&
	    xexists(g_store_fd, "intent"))
		(void)tawcroot_linkstore_recover_now();
}

long tawcroot_linkstore_recover_now(void)
{
	long rv = store_lock();  /* recovery runs inside */
	if (rv < 0) return rv;
	store_unlock();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Host-path composition for intent records                            */

/* Compose the host-real path of (dirfd, suffix) into `out`. Best
 * effort: an empty result just makes the (rare) matching recovery
 * branch a no-op — leak-safe by construction. */
static void compose_host_path(int dirfd, const char *suffix,
			      char *out, size_t cap)
{
	out[0] = 0;
	long n = tawcroot_proc_fd_to_host_path(dirfd, out, cap);
	if (n <= 0) { out[0] = 0; return; }
	size_t pos = (size_t)n;
	if (suffix && suffix[0]) {
		if (tawc_str_append(out, cap, &pos, "/") ||
		    tawc_str_append(out, cap, &pos, suffix))
			out[0] = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Mutations                                                           */

long tawcroot_link_add(const char *tok, int dfd, const char *dsuf)
{
	long rv = store_lock();
	if (rv < 0) return TAWC_EPERM;

	/* Linking a dangling emulated name (object lost to a partial
	 * host-side copy, or a crashed-NEW window): "not linked yet". */
	if (!xexists(tawcroot_store_link_fd, tok)) {
		store_unlock();
		return TAWC_ENOENT;
	}

	char wname[TAWCROOT_LINK_TOKEN_MAX + 8];
	char target[64];
	if (tok_name(wname, sizeof wname, tok, ".add") ||
	    tok_target(target, sizeof target, tok)) {
		store_unlock();
		return TAWC_ENAMETOOLONG;
	}

	unsigned long n = cnt_read(tok);

	{
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *p1 = scratch->buf[0];
		compose_host_path(dfd, dsuf, p1, TAWCROOT_PATH_SCRATCH_SIZE);
		rv = intent_write(OP_ADD, tok, n, p1, "");
	}
	if (rv < 0) { store_unlock(); return rv; }

	/* Tokens are recycled inodes: drop any previous era's strand for
	 * this token+role so the recovery discriminator ("staged
	 * present?") can never read another era's residue as its own. */
	(void)xunlinkat(g_work_fd, wname);

	rv = xsymlink(target, g_work_fd, wname);
	if (rv < 0) {
		intent_clear();
		store_unlock();
		return rv;
	}

	/* Monotone: increment BEFORE the name appears — and the increment
	 * must actually LAND before the name may appear. Publishing after
	 * a failed write (EMFILE/ENOSPC can fail cleanly with the old
	 * count intact) would leave count < live referrers, the one
	 * direction that deletes objects under live names; every other
	 * error path in this module rolls to overcount. Fail the link
	 * instead. (CNT_UNKNOWN counts are exempt: unmaintainable sidecars
	 * report 2 and their clusters are never deleted.) */
	rv = cnt_write(tok, n == CNT_UNKNOWN ? CNT_UNKNOWN : n + 1);
	if (rv < 0) {
		(void)xunlinkat(g_work_fd, wname);
		intent_clear();
		store_unlock();
		return rv;
	}

	rv = xrename(g_work_fd, wname, dfd, dsuf, RENAME_NOREPLACE);
	if (rv < 0) {
		/* EEXIST is link(2)'s own contract; ENOENT = dst parent
		 * vanished. Roll back under the lock. */
		(void)cnt_write(tok, n);
		(void)xunlinkat(g_work_fd, wname);
		intent_clear();
		store_unlock();
		return rv;
	}

	intent_clear();
	store_unlock();
	return 0;
}

long tawcroot_link_del(const char *tok, int dfd, const char *dsuf)
{
	long rv = store_lock();
	if (rv < 0) {
		/* Degraded/locked-out store: the guest's unlink must still
		 * work — plain unlink of the name symlink; the object
		 * leaks (overcount direction, safe). */
		return TAWC_RAW(TAWC_SYS_unlinkat, dfd, (long)dsuf,
				0, 0, 0, 0);
	}

	char wname[TAWCROOT_LINK_TOKEN_MAX + 8];
	if (tok_name(wname, sizeof wname, tok, ".del")) {
		store_unlock();
		return TAWC_ENAMETOOLONG;
	}

	unsigned long n = cnt_read(tok);

	{
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *p1 = scratch->buf[0];
		compose_host_path(dfd, dsuf, p1, TAWCROOT_PATH_SCRATCH_SIZE);
		rv = intent_write(OP_DEL, tok, n, p1, "");
	}
	if (rv < 0) { store_unlock(); return rv; }

	(void)xunlinkat(g_work_fd, wname);  /* prior-era residue */

	/* Park: atomic disappearance of the guest name. */
	rv = xrename(dfd, dsuf, g_work_fd, wname, 0);
	if (rv < 0) {
		intent_clear();
		store_unlock();
		return rv;  /* ENOENT = lost the race to another unlink */
	}

	/* The park moved whatever was at the name AT RENAME TIME. Verify
	 * it really was this cluster's token symlink; a racing lock-free
	 * rename may have swapped the entry between the handler's probe
	 * and the lock. A mismatched FILE still fulfils the guest's unlink
	 * (equivalent to the unlink serializing after the swap) but must
	 * not decrement a count it never contributed to — that's the one
	 * route to data loss. A mismatched DIRECTORY must not vanish into
	 * work/ — the kernel's unlink would EISDIR and leave it intact, so
	 * put it back and report EISDIR (if a create raced the one-syscall
	 * vacancy, the put-back fails and the directory stays parked in
	 * work/ — a content-safe strand, same class as NEW's parked
	 * entries; still EISDIR). */
	if (entry_matches_token(g_work_fd, wname, tok)) {
		if (n != CNT_UNKNOWN) {
			if (n <= 1) {
				(void)xunlinkat(tawcroot_store_link_fd, tok);
				cnt_unlink(tok);
			} else {
				(void)cnt_write(tok, n - 1);
			}
		}
	} else {
		struct stat pst;
		if (TAWC_RAW(TAWC_SYS_fstatat, g_work_fd, (long)wname,
			     (long)&pst, AT_SYMLINK_NOFOLLOW, 0, 0) == 0 &&
		    S_ISDIR(pst.st_mode)) {
			(void)xrename(g_work_fd, wname, dfd, dsuf,
				      RENAME_NOREPLACE);
			intent_clear();
			store_unlock();
			return TAWC_EISDIR;
		}
	}
	(void)xunlinkat(g_work_fd, wname);

	intent_clear();
	store_unlock();
	return 0;
}

long tawcroot_link_new(int sfd, const char *ssuf, int dfd, const char *dsuf)
{
	long rv = store_ensure();
	if (rv < 0) return TAWC_EPERM;
	rv = store_lock();
	if (rv < 0) return TAWC_EPERM;

	/* Token = source inode (the rename preserves it). NOREPLACE plus
	 * this existence retry covers host-level distro copies that
	 * renumber inodes: a fresh token can collide with a stale baked
	 * object, and plain rename would silently destroy it. */
	struct stat st;
	rv = TAWC_RAW(TAWC_SYS_fstatat, sfd, (long)ssuf, (long)&st,
		      AT_SYMLINK_NOFOLLOW, 0, 0);
	if (rv < 0) { store_unlock(); return rv; }

	char base[24];
	char tok[TAWCROOT_LINK_TOKEN_MAX];
	u64_to_dec((unsigned long)st.st_ino, base);
	(void)tawc_str_copy(tok, sizeof tok, base);
	for (unsigned k = 1; xexists(tawcroot_store_link_fd, tok); k++) {
		if (k > 999) { store_unlock(); return TAWC_EIO; }
		char suff[8];
		suff[0] = '-';
		u64_to_dec(k, suff + 1);
		size_t pos = 0;
		if (tawc_str_append(tok, sizeof tok, &pos, base) ||
		    tawc_str_append(tok, sizeof tok, &pos, suff)) {
			store_unlock();
			return TAWC_EIO;
		}
	}

	char sname[TAWCROOT_LINK_TOKEN_MAX + 8];
	char dname[TAWCROOT_LINK_TOKEN_MAX + 8];
	char target[64];
	if (tok_name(sname, sizeof sname, tok, ".src") ||
	    tok_name(dname, sizeof dname, tok, ".dst") ||
	    tok_target(target, sizeof target, tok)) {
		store_unlock();
		return TAWC_ENAMETOOLONG;
	}

	{
		TAWCROOT_PATH_SCRATCH_AUTO(scratch);
		char *p1 = scratch->buf[0];
		char *p2 = scratch->buf[1];
		compose_host_path(sfd, ssuf, p1, TAWCROOT_PATH_SCRATCH_SIZE);
		compose_host_path(dfd, dsuf, p2, TAWCROOT_PATH_SCRATCH_SIZE);
		rv = intent_write(OP_NEW, tok, CNT_UNKNOWN, p1, p2);
	}
	if (rv < 0) { store_unlock(); return rv; }

	/* Stage BOTH names before the object rename: recovery's roll-
	 * forward treats "staged absent" as proof the publish happened,
	 * which for the trailing src entry is only sound if the object's
	 * presence implies the entry was staged. */
	(void)xunlinkat(g_work_fd, dname);
	(void)xunlinkat(g_work_fd, sname);
	rv = xsymlink(target, g_work_fd, dname);
	if (rv == 0) rv = xsymlink(target, g_work_fd, sname);
	if (rv < 0) {
		(void)xunlinkat(g_work_fd, dname);
		(void)xunlinkat(g_work_fd, sname);
		intent_clear();
		store_unlock();
		return rv;
	}

	/* Publish dst (atomic appearance; link()'s EEXIST for free). */
	rv = xrename(g_work_fd, dname, dfd, dsuf, RENAME_NOREPLACE);
	if (rv < 0) {
		(void)xunlinkat(g_work_fd, dname);
		(void)xunlinkat(g_work_fd, sname);
		intent_clear();
		store_unlock();
		return rv;
	}

	/* First link creates/overwrites the sidecar, never trusts a
	 * stale one (host-copied distros bake stale .cnt files against
	 * renumbered inodes). */
	(void)cnt_write(tok, 2);

	/* The point of no return. */
	rv = xrename(sfd, ssuf, tawcroot_store_link_fd, tok,
		     RENAME_NOREPLACE);
	if (rv < 0) {
		/* Live-path abort under the lock: delete staged src,
		 * verified-match-delete the published dst, drop the
		 * sidecar. dst was briefly visible, then gone — an
		 * unserializable race, accepted. EXDEV = cross-fs bind
		 * source; caller falls back to the v1 emulation. */
		(void)xunlinkat(g_work_fd, sname);
		if (entry_matches_token(dfd, dsuf, tok))
			(void)xunlinkat(dfd, dsuf);
		cnt_unlink(tok);
		intent_clear();
		store_unlock();
		return rv;
	}

	/* Publish src into the one-syscall vacancy. EEXIST = a create
	 * raced the vacancy; ENOENT = parent vanished. Either way the
	 * staged entry stays parked (count stays high: safe leak). */
	(void)xrename(g_work_fd, sname, sfd, ssuf, RENAME_NOREPLACE);

	intent_clear();
	store_unlock();
	return 0;
}

long tawcroot_link_clobber(int ofd, const char *osuf,
			   int nfd, const char *nsuf,
			   unsigned int rflags)
{
	long rv = store_lock();
	if (rv < 0) {
		/* Degraded/locked-out store: the guest's rename must still
		 * work — plain rename, count goes stale high (leak). */
		return xrename(ofd, osuf, nfd, nsuf, rflags);
	}

	/* Re-verify under the lock; the handler's probe was lock-free. */
	char cur[TAWCROOT_LINK_TOKEN_MAX];
	long hit = tawcroot_link_leaf_token(nfd, nsuf, cur, sizeof cur);
	if (hit != 1) {
		store_unlock();
		return xrename(ofd, osuf, nfd, nsuf, rflags);
	}

	unsigned long n = cnt_read(cur);
	rv = xrename(ofd, osuf, nfd, nsuf, rflags);
	if (rv < 0) {
		store_unlock();
		return rv;
	}
	/* Decrement AFTER the name is gone (monotone). No intent — see
	 * linkstore.h; crash windows here are +1 overcount or a count-0
	 * orphan object, both leak-safe, and CLOBBER can't be store-keyed
	 * without breaking rename's atomic-replace guarantee. */
	if (n != CNT_UNKNOWN) {
		if (n <= 1) {
			(void)xunlinkat(tawcroot_store_link_fd, cur);
			cnt_unlink(cur);
		} else {
			(void)cnt_write(cur, n - 1);
		}
	}
	store_unlock();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Linkable O_TMPFILE objects (tmp/)                                   */

long tawcroot_link_tmpfile_open(int dfd, const char *dsuf, int flags,
				int mode)
{
	/* Kernel shape first: O_TMPFILE requires write access. */
	if ((flags & O_ACCMODE) == O_RDONLY) return TAWC_EINVAL;

	/* The guest's operand must be an (existing) directory — the file
	 * itself lives in the store, so reproduce the kernel's errno by
	 * probing. */
	struct stat dst;
	long rv = TAWC_RAW(TAWC_SYS_fstatat, dfd, (long)dsuf, (long)&dst,
			   0, 0, 0);
	if (rv < 0) return rv;
	if (!S_ISDIR(dst.st_mode)) return TAWC_ENOTDIR;

	if (store_ensure() < 0 || g_tmp_fd < 0) return TAWC_EAGAIN;

	/* Create under a letter-prefixed unique name (never confusable
	 * with the digit-shaped published names), then rename to the
	 * file's inode decimal. No store lock: both names are keyed to
	 * state only this thread holds — the unique name by pid+counter,
	 * the ino name because a LIVE occupant of that name would need a
	 * live file with our fresh file's inode, which the fs just handed
	 * out. A stale occupant (host-level store copy renumbered inodes)
	 * is uncounted scratch; the plain rename clobbering it is the
	 * cleanup. */
	static int g_tmp_seq;
	long pid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	int  seq = __atomic_add_fetch(&g_tmp_seq, 1, __ATOMIC_RELAXED);
	char uname[48];
	char dec[24];
	size_t pos = 0;
	u64_to_dec((unsigned long)pid, dec);
	if (tawc_str_append(uname, sizeof uname, &pos, "n") ||
	    tawc_str_append(uname, sizeof uname, &pos, dec))
		return TAWC_EAGAIN;
	u64_to_dec((unsigned long)seq, dec);
	if (tawc_str_append(uname, sizeof uname, &pos, "-") ||
	    tawc_str_append(uname, sizeof uname, &pos, dec))
		return TAWC_EAGAIN;

	long fd = tawc_openat(g_tmp_fd, uname,
			      (flags | O_CREAT | O_EXCL), mode);
	if (fd < 0) return fd;

	struct stat st;
	rv = TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0);
	char iname[24];
	if (rv == 0) {
		u64_to_dec((unsigned long)st.st_ino, iname);
		rv = xrename(g_tmp_fd, uname, g_tmp_fd, iname, 0);
	}
	if (rv < 0) {
		(void)xunlinkat(g_tmp_fd, uname);
		tawc_close((int)fd);
		return rv;
	}
	return fd;
}

long tawcroot_link_publish_tmp(const char *name, int dfd, const char *dsuf)
{
	if (g_state != TAWCROOT_STORE_READY || g_tmp_fd < 0)
		return TAWC_EXDEV;
	return xrename(g_tmp_fd, name, dfd, dsuf, RENAME_NOREPLACE);
}

void tawcroot_linkstore_tmp_sweep(void)
{
	if (g_state != TAWCROOT_STORE_READY || g_tmp_fd < 0) return;
	long dfd = tawc_openat(g_tmp_fd, ".",
			       O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (dfd < 0) return;
	long now[2];
	if (TAWC_RAW(TAWC_SYS_clock_gettime, 0 /*CLOCK_REALTIME*/,
		     (long)now, 0, 0, 0, 0) != 0) {
		tawc_close((int)dfd);
		return;
	}
	long cutoff = now[0] - 7L * 24 * 3600;

	/* No lock: unlink is atomic and the only raceable loser is a
	 * 7-day-old temp whose owner publishes right now — its rename
	 * ENOENTs, the same degradation the age gate already accepts. */
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *buf = scratch->buf[0];
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_getdents64, dfd, (long)buf,
				  TAWCROOT_PATH_SCRATCH_SIZE, 0, 0, 0);
		if (n <= 0) break;
		long off = 0;
		/* linux_dirent64: d_reclen u16 at +16, d_name at +19. */
		while (off + 19 < n) {
			unsigned short reclen;
			memcpy(&reclen, buf + off + 16, sizeof reclen);
			if (reclen < 20 || off + reclen > n) break;
			const char *nm = buf + off + 19;
			off += reclen;
			if (nm[0] == '.') continue;
			struct stat st;
			if (TAWC_RAW(TAWC_SYS_fstatat, dfd, (long)nm,
				     (long)&st, AT_SYMLINK_NOFOLLOW,
				     0, 0) != 0)
				continue;
			if (S_ISREG(st.st_mode) && st.st_mtime < cutoff)
				(void)xunlinkat((int)dfd, nm);
		}
	}
	tawc_close((int)dfd);
}
