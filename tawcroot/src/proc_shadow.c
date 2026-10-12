/* See include/proc_shadow.h for the public surface and rationale. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "errno_neg.h"
#include "io.h"
#include "loader_map.h"
#include "path.h"
#include "proc_rewrite.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "stat_compat.h"
#include "syscalls_fs.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

/* Fast-out: does this guest-relative leaf even have a chance of
 * composing into a /proc/<x> path that we shadow? Legitimate first chars
 * are {b,c,s,t,m,e,l,u,v,digit} — covering "bus/" (pci/devices), "cwd",
 * "self/", "stat", "sys/", "task/", "maps", "exe", "loadavg", "uptime",
 * "version", and numeric "<tid>/" forms. Anything else (the vast
 * majority of fd-relative opens and stats — config files, dotfiles,
 * library names, etc.) skips the readlinkat. */
int tawcroot_could_be_proc_relative(const char *p)
{
	char c = p[0];
	return c == 'b' || c == 'c' || c == 's' || c == 't' || c == 'm' ||
	       c == 'e' || c == 'l' || c == 'u' || c == 'v' ||
	       (c >= '0' && c <= '9');
}


/* Return 1 iff `n` names our process: either our TGID (== getpid()) or
 * a TID belonging to it. Validation reads `/proc/<n>/status` and checks
 * the `Tgid:` line. Unknown / non-existent / cross-process TIDs return 0.
 *
 * Not cached: only the /proc/<n>/<x> path-classification call sites
 * reach here, and the common case (n == TGID) short-circuits without
 * any syscall. Per-thread crash dumpers walking every TID is the worst
 * case; if it ever shows up as hot we can stick a small MRU here. */
static int is_my_tid(long n)
{
	if (n <= 0 || n > 0x7fffffff) return 0;
	long mypid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	if (n == mypid) return 1;

	char path[64];
	size_t pos = 0;
	if (tawc_str_append(path, sizeof path, &pos, "/proc/") ||
	    tawc_str_append_dec(path, sizeof path, &pos, n) ||
	    tawc_str_append(path, sizeof path, &pos, "/status"))
		return 0;

	long fd = tawc_openat(AT_FDCWD, path,
			      O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return 0;
	char buf[512];
	long r = tawc_read((int)fd, buf, sizeof buf - 1);
	tawc_close((int)fd);
	if (r <= 0) return 0;
	buf[r] = 0;

	/* Walk lines looking for "Tgid:". The kernel emits it within the
	 * first ~12 lines, well inside the 511-byte read. Anchor at start
	 * of line so a hypothetical other field whose value happens to
	 * contain the byte sequence "Tgid:" can't fool the scan. */
	const char *p = buf;
	while (*p) {
		if ((p == buf || p[-1] == '\n') &&
		    p[0] == 'T' && p[1] == 'g' && p[2] == 'i' &&
		    p[3] == 'd' && p[4] == ':') {
			p += 5;
			while (*p == ' ' || *p == '\t') p++;
			long parsed = 0;
			while (*p >= '0' && *p <= '9') {
				parsed = parsed * 10 + (*p - '0');
				p++;
			}
			return parsed == mypid;
		}
		while (*p && *p != '\n') p++;
		if (*p == '\n') p++;
	}
	return 0;
}

/* If `path` is "/proc/self/<x>", "/proc/thread-self/<x>" or
 * "/proc/<tid>/<x>", return a pointer to "<x>" and set `*tid_out` to
 * the effective tid: -1 for a literal "self"/"thread-self" (both name
 * our process; every synthesized target — maps, exe, cwd, fd — is
 * per-mm/per-process, so the thread distinction doesn't matter here,
 * and letting thread-self fall through leaked untranslated host paths
 * via thread-self/maps and libtawcroot.so via thread-self/exe), else
 * the numeric tid. Ownership is NOT resolved here — the
 * caller decides via resolve_mine(), so classifications that don't
 * care (fd links) skip the /proc/<n>/status read entirely. Also peels
 * an optional "task/<tid>/" segment after `self/` or `<tid>/`, since
 * /proc/<pid>/task/<tid>/maps is per-mm (identical to .../maps) and
 * /exe is a symlink to the same exe; a peeled inner tid replaces the
 * effective tid (the kernel ENOENTs a task dir under the wrong pid, so
 * a mixed self/foreign pairing never produces output anyway). Returns
 * NULL when the shape doesn't match at all.
 *
 * Strict on the prefix bytes: paths like "/proc/foo/../self/exe" are
 * caught only after the guest's libc canonicalizes them (the typical
 * flow). */
static const char *strip_pid_prefix_rel(const char *t, long *tid_out)
{
	*tid_out = -1;
	const char *after_pid;
	if (t[0] == 's' && t[1] == 'e' && t[2] == 'l' && t[3] == 'f' &&
	    t[4] == '/') {
		after_pid = t + 5;
	} else if (t[0] == 't' && t[1] == 'h' && t[2] == 'r' &&
		   t[3] == 'e' && t[4] == 'a' && t[5] == 'd' &&
		   t[6] == '-' && t[7] == 's' && t[8] == 'e' &&
		   t[9] == 'l' && t[10] == 'f' && t[11] == '/') {
		after_pid = t + 12;
	} else if (t[0] >= '0' && t[0] <= '9') {
		long n = 0;
		const char *p = t;
		while (*p >= '0' && *p <= '9') {
			n = n * 10 + (*p - '0'); p++;
			if (n > 0x7fffffff) return 0;
		}
		if (p[0] != '/') return 0;
		*tid_out = n;
		after_pid = p + 1;
	} else {
		return 0;
	}

	/* Optional "task/<tid>/" peel. On any structural problem with the
	 * inner segment (no digits, overflow, missing trailing slash) we
	 * leave `after_pid` alone; the caller's tail-match (e.g. against
	 * "maps") then fails and synthesis doesn't fire. `/proc/self/task/`
	 * itself is never a synthesis target, so the difference is
	 * academic. */
	if (after_pid[0] == 't' && after_pid[1] == 'a' &&
	    after_pid[2] == 's' && after_pid[3] == 'k' &&
	    after_pid[4] == '/') {
		const char *q = after_pid + 5;
		long m = 0;
		const char *p = q;
		while (*p >= '0' && *p <= '9') {
			m = m * 10 + (*p - '0'); p++;
			if (m > 0x7fffffff) return after_pid;
		}
		if (p == q || p[0] != '/') return after_pid;
		*tid_out = m;
		return p + 1;
	}
	return after_pid;
}

static const char *strip_proc_pid_prefix(const char *path, long *tid_out)
{
	*tid_out = -1;
	if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
	    path[3] != 'o' || path[4] != 'c' || path[5] != '/')
		return 0;
	return strip_pid_prefix_rel(path + 6, tid_out);
}

/* Resolve a strip_proc_pid_prefix tid to "names our process". -1
 * ("self") is ours by definition; numeric tids pay the status read. */
static int resolve_mine(long tid)
{
	return tid < 0 ? 1 : is_my_tid(tid);
}

/* Byte length of the dir/entry MAGIC LINK prefix in a /proc-relative
 * suffix (no leading "/proc/") — the shapes where a syscall acts on the
 * host inode the link names (or resolves through) rather than on a
 * procfs file:
 *
 *   (self|thread-self|<pid>)/(task/<tid>/)?(fd|map_files)/<entry>
 *   (self|thread-self|<pid>)/(task/<tid>/)?(cwd|root)
 *
 * Callers readlink exactly the returned prefix (kernel ground truth,
 * also right when the path resolves THROUGH the link, e.g.
 * self/fd/3/sub or self/cwd/name). 0 = no match. Lives here, sharing
 * strip_pid_prefix_rel, so the grammar can't drift from the shadow
 * classifiers' (an earlier copy in syscalls_fs.c missed task/<tid>/).
 *
 * `kind` (optional) reports which containment rule the link falls
 * under — see the TAWCROOT_PROC_MAGIC_* comments in proc_shadow.h.
 * Ownership is resolved (one /proc/<n>/status read for a numeric pid
 * that isn't ours) only when the caller asks for a kind AND the grammar
 * already matched, so the prefix-only callers pay nothing. */
static size_t magic_link_prefix(const char *suf, int *kind)
{
	if (kind) *kind = TAWCROOT_PROC_MAGIC_NONE;
	long tid;
	const char *tail = strip_pid_prefix_rel(suf, &tid);
	if (!tail) return 0;
	const char *p;
	if (tawc_starts_with(tail, "fd/")) {
		p = tail + 3;
	} else if (tawc_starts_with(tail, "map_files/")) {
		p = tail + 10;
	} else if (tail[0] == 'c' && tail[1] == 'w' && tail[2] == 'd' &&
		   (tail[3] == 0 || tail[3] == '/')) {
		if (kind) *kind = TAWCROOT_PROC_MAGIC_CONTAIN;
		return (size_t)(tail + 3 - suf);
	} else if (tail[0] == 'r' && tail[1] == 'o' && tail[2] == 'o' &&
		   tail[3] == 't' && (tail[4] == 0 || tail[4] == '/')) {
		if (kind)
			*kind = resolve_mine(tid) ? TAWCROOT_PROC_MAGIC_ROOT_OWN
						  : TAWCROOT_PROC_MAGIC_CONTAIN;
		return (size_t)(tail + 4 - suf);
	} else {
		return 0;
	}
	const char *start = p;
	while (*p && *p != '/') p++;
	if (p == start) return 0;
	if (kind)
		*kind = resolve_mine(tid) ? TAWCROOT_PROC_MAGIC_FD_OWN
					  : TAWCROOT_PROC_MAGIC_CONTAIN;
	return (size_t)(p - suf);
}

size_t tawcroot_proc_magic_link_prefix(const char *suf)
{
	return magic_link_prefix(suf, 0);
}

size_t tawcroot_proc_magic_link_classify(const char *suf, int *kind)
{
	return magic_link_prefix(suf, kind);
}

/* Compose `dirfd`'s host path (resolved via /proc/self/fd/<n>) with a
 * relative guest path into `out`. Used to catch fd-relative /proc/self
 * accesses (e.g. openat(proc_dir_fd, "self/maps", ...)) before
 * strip_proc_self_prefix runs. Returns the composed length on success
 * or -errno. Caller passes the literal guest-supplied relative string;
 * dirfd must be non-negative and != AT_FDCWD. */
long tawcroot_compose_fd_relative(int dirfd, const char *gpath_str,
				char *out, size_t cap)
{
	if (dirfd < 0 || dirfd == AT_FDCWD) return TAWC_EINVAL;
	if (gpath_str[0] == '/') return TAWC_EINVAL;

	long n = tawcroot_proc_fd_to_host_path(dirfd, out, cap);
	if (n < 0) return n;
	size_t dl = (size_t)n;
	long e = 0;
	if (out[dl - 1] != '/') e = tawc_str_append(out, cap, &dl, "/");
	if (!e) e = tawc_str_append(out, cap, &dl, gpath_str);
	return e ? e : (long)dl;
}

int tawcroot_proc_link_classify(const char *path)
{
	long tid;
	const char *tail = strip_proc_pid_prefix(path, &tid);
	if (!tail) return TAWCROOT_PROC_LINK_NONE;
	if (tawc_streq(tail, "exe"))
		return resolve_mine(tid) ? TAWCROOT_PROC_LINK_EXE_SELF
					 : TAWCROOT_PROC_LINK_EXE_OTHER;
	/* Cross-process cwd links pass the kernel bytes through verbatim,
	 * like the rest of cross-process /proc — only ours synthesizes. */
	if (tawc_streq(tail, "cwd"))
		return resolve_mine(tid) ? TAWCROOT_PROC_LINK_CWD
					 : TAWCROOT_PROC_LINK_NONE;
	/* fd links: pid ownership is irrelevant (the readlink result is
	 * reverse-translated by prefix, equally correct for sibling
	 * tawcroot processes and a no-op for outside-view targets), so
	 * the status read is skipped entirely. */
	if (tail[0] == 'f' && tail[1] == 'd' && tail[2] == '/') {
		const char *p = tail + 3;
		if (*p < '0' || *p > '9') return TAWCROOT_PROC_LINK_NONE;
		while (*p >= '0' && *p <= '9') p++;
		if (*p == 0) return TAWCROOT_PROC_LINK_FD;
	}
	return TAWCROOT_PROC_LINK_NONE;
}

int tawcroot_proc_exe_classify(const char *path)
{
	switch (tawcroot_proc_link_classify(path)) {
	case TAWCROOT_PROC_LINK_EXE_SELF:  return TAWCROOT_PROC_EXE_SELF;
	case TAWCROOT_PROC_LINK_EXE_OTHER: return TAWCROOT_PROC_EXE_OTHER;
	default:                           return TAWCROOT_PROC_EXE_NONE;
	}
}

int tawcroot_is_proc_self_exe(const char *path)
{
	return tawcroot_proc_exe_classify(path) == TAWCROOT_PROC_EXE_SELF;
}

int tawcroot_is_proc_self_cwd(const char *path)
{
	return tawcroot_proc_link_classify(path) == TAWCROOT_PROC_LINK_CWD;
}

int tawcroot_is_proc_fd_link(const char *path)
{
	return tawcroot_proc_link_classify(path) == TAWCROOT_PROC_LINK_FD;
}

int tawcroot_proc_self_fd_num(const char *path)
{
	long tid;
	const char *tail = strip_proc_pid_prefix(path, &tid);
	if (!tail || tail[0] != 'f' || tail[1] != 'd' || tail[2] != '/')
		return -1;
	const char *p = tail + 3;
	long n = 0;
	if (*p < '0' || *p > '9') return -1;
	while (*p >= '0' && *p <= '9') {
		n = n * 10 + (*p - '0'); p++;
		if (n > 0x7fffffff) return -1;
	}
	if (*p != 0 || !resolve_mine(tid)) return -1;
	return (int)n;
}

static int is_proc_self_maps(const char *path)
{
	long tid;
	const char *tail = strip_proc_pid_prefix(path, &tid);
	return tail && tawc_streq(tail, "maps") && resolve_mine(tid);
}

/* The one classifier behind all four surfaces. Every shadowed path is
 * under /proc, so the prefix test fast-outs the overwhelming majority
 * of stats and accesses before any string compare.
 *
 * /proc/bus/pci: only the `devices` file matches. The directory itself
 * and its per-bus subdirs are not shadowed — guests that want to walk
 * them get the kernel's normal -EACCES, same as before. */
int tawcroot_proc_shadow_classify(const char *path)
{
	if (!tawc_starts_with(path, "/proc/"))
		return TAWCROOT_PROC_SHADOW_NONE;
	if (tawc_streq(path, "/proc/sys/kernel/overflowuid"))
		return TAWCROOT_PROC_SHADOW_OVERFLOWUID;
	if (tawc_streq(path, "/proc/sys/kernel/overflowgid"))
		return TAWCROOT_PROC_SHADOW_OVERFLOWGID;
	if (tawc_streq(path, "/proc/bus/pci/devices"))
		return TAWCROOT_PROC_SHADOW_PCI_DEVICES;
	if (tawc_streq(path, "/proc/stat"))
		return TAWCROOT_PROC_SHADOW_STAT;
	if (tawc_streq(path, "/proc/version"))
		return TAWCROOT_PROC_SHADOW_VERSION;
	if (tawc_streq(path, "/proc/uptime"))
		return TAWCROOT_PROC_SHADOW_UPTIME;
	if (tawc_streq(path, "/proc/loadavg"))
		return TAWCROOT_PROC_SHADOW_LOADAVG;
	/* Last: the only kind whose match can cost a /proc/<n>/status
	 * read (numeric-pid ownership resolution). */
	if (is_proc_self_maps(path))
		return TAWCROOT_PROC_SHADOW_MAPS;
	return TAWCROOT_PROC_SHADOW_NONE;
}

/* /proc/self/maps shadow fd. Read the kernel's maps file in full,
 * reverse-translate each path field via the rootfs/bind tables, and
 * write the result into a memfd that we hand back to the guest.
 *
 * Both buffers GROW as needed rather than capping at a fixed size: a
 * Firefox-scale process (>10k mappings) overflows a 1 MiB cap, and the
 * old code truncated the read mid-line (the rewriter then processed a
 * cut-off partial line) and ENOSPC'd when reverse-translation made the
 * output longer than the input. We start at 1 MiB (covers most
 * processes in one allocation, minimising self-perturbation of the
 * maps we're reading) and double on demand.
 *
 * All allocations are anonymous mmaps (not on the SIGSYS handler's tiny
 * stack) and freed before return. memfd_create needs no privileges and
 * works on every kernel we target (≥ 3.17). */
#define MAPS_BUF_SIZE  ((size_t)1 << 20)
/* Hard ceiling so a runaway never exhausts address space; 256 MiB is
 * orders of magnitude past any real /proc/self/maps. */
#define MAPS_BUF_MAX   ((size_t)256 << 20)

static long maps_mmap(size_t cap)
{
	long r = tawc_mmap(0, cap, TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE,
			   TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON, -1, 0);
	if (r < 0 && r > -4096) return r;
	if (r == 0) return TAWC_ENOMEM;
	return r;
}

/* Read the whole of an already-open fd into a growable anonymous
 * mapping. On success sets the region and cap out-params to the mapping
 * and returns the byte length; on failure unmaps and returns -errno. */
static long read_all_growable(int fd, long *region, size_t *cap)
{
	size_t c = MAPS_BUF_SIZE;
	long reg = maps_mmap(c);
	if (reg < 0) return reg;
	char *buf = (char *)(uintptr_t)reg;
	size_t len = 0;
	for (;;) {
		if (len == c) {
			if (c >= MAPS_BUF_MAX) break;  /* ceiling: stop reading */
			size_t nc = c * 2;
			long nreg = maps_mmap(nc);
			if (nreg < 0) {
				(void)tawc_munmap((void *)(uintptr_t)reg, c);
				return nreg;
			}
			char *nbuf = (char *)(uintptr_t)nreg;
			for (size_t k = 0; k < len; k++) nbuf[k] = buf[k];
			(void)tawc_munmap((void *)(uintptr_t)reg, c);
			reg = nreg; buf = nbuf; c = nc;
		}
		long n = tawc_read(fd, buf + len, c - len);
		if (n == 0) break;
		if (n < 0) {
			(void)tawc_munmap((void *)(uintptr_t)reg, c);
			return n;
		}
		len += (size_t)n;
	}
	*region = reg;
	*cap = c;
	return (long)len;
}

/* Create a CLOEXEC memfd preloaded with `len` bytes and rewound to
 * offset 0. Returns the fd or -errno. */
static long memfd_from_bytes(const char *name, const char *bytes, size_t len)
{
	long memfd = tawc_memfd_create(name, 1U /*MFD_CLOEXEC*/);
	if (memfd < 0) return memfd;
	size_t written = 0;
	while (written < len) {
		long w = tawc_write((int)memfd, bytes + written,
				    len - written);
		if (w <= 0) {
			tawc_close((int)memfd);
			return w < 0 ? w : TAWC_EFAULT;
		}
		written += (size_t)w;
	}
	long sk = tawc_lseek((int)memfd, 0, 0 /*SEEK_SET*/);
	if (sk < 0) {
		tawc_close((int)memfd);
		return sk;
	}
	return memfd;
}

static long open_proc_maps_shadow(void)
{
	long src = tawc_openat(AT_FDCWD, "/proc/self/maps",
			       O_RDONLY | O_CLOEXEC, 0);
	if (src < 0) return src;

	long in_region;
	size_t in_cap;
	long in_len = read_all_growable((int)src, &in_region, &in_cap);
	tawc_close((int)src);
	if (in_len < 0) return in_len;
	char *in_buf = (char *)(uintptr_t)in_region;

	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = tawcroot_rootfs_host_path,
		.rootfs_host_path_len = tawcroot_rootfs_host_path_len,
		.binds                = tawcroot_binds,
		.n_binds              = tawcroot_n_binds,
	};

	/* Output: reverse-translation can lengthen lines (a bind dst
	 * longer than its src), so size above the input by half again plus
	 * a megabyte and grow-retry on ENOSPC. */
	size_t out_cap = (size_t)in_len + (size_t)in_len / 2 + MAPS_BUF_SIZE;
	long out_region = maps_mmap(out_cap);
	if (out_region < 0) {
		(void)tawc_munmap((void *)(uintptr_t)in_region, in_cap);
		return out_region;
	}
	long out_len;
	for (;;) {
		out_len = tawcroot_proc_maps_rewrite(
			&ctx, in_buf, (size_t)in_len,
			(char *)(uintptr_t)out_region, out_cap);
		if (out_len != TAWC_ENOSPC) break;
		if (out_cap >= MAPS_BUF_MAX) break;  /* ceiling */
		(void)tawc_munmap((void *)(uintptr_t)out_region, out_cap);
		out_cap *= 2;
		out_region = maps_mmap(out_cap);
		if (out_region < 0) {
			(void)tawc_munmap((void *)(uintptr_t)in_region, in_cap);
			return out_region;
		}
	}
	(void)tawc_munmap((void *)(uintptr_t)in_region, in_cap);
	if (out_len < 0) {
		(void)tawc_munmap((void *)(uintptr_t)out_region, out_cap);
		return out_len;
	}
	long memfd = memfd_from_bytes("tawcroot-maps",
				      (const char *)(uintptr_t)out_region,
				      (size_t)out_len);
	(void)tawc_munmap((void *)(uintptr_t)out_region, out_cap);
	return memfd;
}

/* /proc/sys/kernel/overflow{uid,gid} shadow fd. Returns a memfd preloaded
 * with the Linux-conventional "65534\n" (documented in
 * Documentation/admin-guide/sysctl/kernel.rst). Stays in lockstep with
 * the kernel default; the value hasn't changed since the sysctl landed,
 * and uid/gid have always shared it. The `memfd_name` distinguishes
 * the two in /proc/self/fd/<fd> readlinks — "/memfd:tawcroot-overflowuid
 * (deleted)" — which is useful when reading the diagnostic output of a
 * guest that strerror()s its way through bwrap. */
static long open_proc_overflow_id_shadow(const char *memfd_name)
{
	return memfd_from_bytes(memfd_name, "65534\n", 6);
}

/* /proc/bus/pci/devices shadow fd. Returns an empty memfd — that's the
 * legitimate "no PCI devices visible" state that libpci's procfs back-
 * end is designed to handle. See the head-of-handle_openat comment for
 * why this matters (Mozilla glxtest -> WebRender disable cascade). The
 * memfd starts at offset 0 with size 0, so no write loop or lseek is
 * needed; -errno from memfd_create flows back to the guest verbatim,
 * same as the other two shadows. */
static long open_proc_bus_pci_devices_shadow(void)
{
	return tawc_memfd_create("tawcroot-pci-devices",
				 1U /*MFD_CLOEXEC*/);
}

static void realtime_now(long *sec, long *nsec)
{
	struct { long sec; long nsec; } rt = { 0, 0 };
	(void)TAWC_RAW(TAWC_SYS_clock_gettime, 0 /*CLOCK_REALTIME*/,
		       (long)&rt, 0, 0, 0, 0);
	*sec = rt.sec;
	*nsec = rt.nsec;
}

/* CLOCK_BOOTTIME as (seconds, centiseconds). Both /proc/uptime fields
 * and the /proc/stat idle line derive from this one helper — they have
 * to agree or procps computes a negative CPU usage. */
static void boottime_now(long *sec, long *cs)
{
	struct { long sec; long nsec; } bt = { 0, 0 };
	(void)TAWC_RAW(TAWC_SYS_clock_gettime, 7 /*CLOCK_BOOTTIME*/,
		       (long)&bt, 0, 0, 0, 0);
	if (bt.sec < 0) bt.sec = 0;
	*sec = bt.sec;
	*cs  = bt.nsec / 10000000;
}

static long open_proc_stat_shadow(void)
{
	long rt_sec, rt_nsec, up_sec, up_cs;
	realtime_now(&rt_sec, &rt_nsec);
	boottime_now(&up_sec, &up_cs);
	long btime = rt_sec - up_sec;
	if (btime < 1) btime = 1;
	/* USER_HZ is 100 on both supported arches. */
	long idle_ticks = up_sec * 100;

	char buf[256];
	size_t pos = 0;
	long e = 0;
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, "cpu  0 0 0 ");
	if (!e) e = tawc_str_append_dec(buf, sizeof buf, &pos, idle_ticks);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos,
	                            " 0 0 0 0 0 0\nintr 0\nctxt 0\nbtime ");
	if (!e) e = tawc_str_append_dec(buf, sizeof buf, &pos, btime);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos,
	                            "\nprocesses 1\nprocs_running 1\n"
	                            "procs_blocked 0\n");
	if (e) return TAWC_EFAULT;
	return memfd_from_bytes("tawcroot-stat", buf, pos);
}

/* Append "<sec>.<cs>" with the centiseconds zero-padded to two digits,
 * the fixed shape every /proc/uptime parser expects. */
static long append_secs_cs(char *buf, size_t cap, size_t *pos,
			   long sec, long cs)
{
	long e = tawc_str_append_dec(buf, cap, pos, sec);
	if (!e) e = tawc_str_append(buf, cap, pos, cs < 10 ? ".0" : ".");
	if (!e) e = tawc_str_append_dec(buf, cap, pos, cs);
	return e;
}

/* /proc/version shadow fd. Android labels the real file
 * `u:object_r:proc_version:s0`, which untrusted_app may neither read
 * nor getattr, and the denial is dontaudit'ed so logcat shows nothing.
 * LibreOffice's `oosplash` stats it once and hard-exits with
 * "ERROR: /proc not mounted" when that fails, so a plain `libreoffice
 * --version` never starts. uname(2) is unaffected, so the real kernel
 * strings are available: the synthesized file has the kernel's own
 * shape — release, builder parenthetical, compiler parenthetical,
 * version — with only the two parentheticals invented.
 * `tawcroot@android` doubles as the marker the hosted tests grep for,
 * since a test host's real /proc/version IS readable and would
 * otherwise be indistinguishable from a passthrough. */
static long open_proc_version_shadow(void)
{
	/* Linux's `new_utsname`: six __NEW_UTS_LEN+1 == 65-byte fields. */
	struct { char f[6][65]; } uts;
	memset(&uts, 0, sizeof uts);
	if (TAWC_RAW(TAWC_SYS_uname, (long)&uts, 0, 0, 0, 0, 0) < 0)
		return TAWC_EIO;
	uts.f[2][64] = 0;
	uts.f[3][64] = 0;

	char buf[320];
	size_t pos = 0;
	long e = tawc_str_append(buf, sizeof buf, &pos, "Linux version ");
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, uts.f[2]);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos,
				    " (tawcroot@android) (tawcroot) ");
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, uts.f[3]);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, "\n");
	if (e) return TAWC_EFAULT;
	return memfd_from_bytes("tawcroot-version", buf, pos);
}

/* /proc/uptime shadow fd. Also denied to untrusted_app; `uptime`, `w`
 * and procps in general want it. Idle is reported EQUAL to uptime,
 * matching the all-ticks-idle cpu line the /proc/stat shadow derives
 * from the same boottime_now(). */
static long open_proc_uptime_shadow(void)
{
	long sec, cs;
	boottime_now(&sec, &cs);

	char buf[96];
	size_t pos = 0;
	long e = append_secs_cs(buf, sizeof buf, &pos, sec, cs);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, " ");
	if (!e) e = append_secs_cs(buf, sizeof buf, &pos, sec, cs);
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, "\n");
	if (e) return TAWC_EFAULT;
	return memfd_from_bytes("tawcroot-uptime", buf, pos);
}

/* /proc/loadavg shadow fd. Fixed zero loads (we cannot see the host
 * scheduler), one runnable of one, and our own pid as "last pid" —
 * every consumer treats that field as informational. */
static long open_proc_loadavg_shadow(void)
{
	char buf[96];
	size_t pos = 0;
	long e = tawc_str_append(buf, sizeof buf, &pos, "0.00 0.00 0.00 1/1 ");
	if (!e) e = tawc_str_append_dec(buf, sizeof buf, &pos,
					TAWC_RAW(TAWC_SYS_getpid,
						 0, 0, 0, 0, 0, 0));
	if (!e) e = tawc_str_append(buf, sizeof buf, &pos, "\n");
	if (e) return TAWC_EFAULT;
	return memfd_from_bytes("tawcroot-loadavg", buf, pos);
}

long tawcroot_proc_shadow_open(int kind)
{
	switch (kind) {
	case TAWCROOT_PROC_SHADOW_MAPS:
		return open_proc_maps_shadow();
	case TAWCROOT_PROC_SHADOW_OVERFLOWUID:
		return open_proc_overflow_id_shadow("tawcroot-overflowuid");
	case TAWCROOT_PROC_SHADOW_OVERFLOWGID:
		return open_proc_overflow_id_shadow("tawcroot-overflowgid");
	case TAWCROOT_PROC_SHADOW_PCI_DEVICES:
		return open_proc_bus_pci_devices_shadow();
	case TAWCROOT_PROC_SHADOW_STAT:
		return open_proc_stat_shadow();
	case TAWCROOT_PROC_SHADOW_VERSION:
		return open_proc_version_shadow();
	case TAWCROOT_PROC_SHADOW_UPTIME:
		return open_proc_uptime_shadow();
	case TAWCROOT_PROC_SHADOW_LOADAVG:
		return open_proc_loadavg_shadow();
	default:
		return TAWC_EINVAL;
	}
}

/* ---- metadata surfaces ------------------------------------------------
 *
 * Deliberately NOT "build the memfd and fstat it": for MAPS that would
 * read and rewrite the whole maps file on every stat, and an `ls -l
 * /proc/self` stats a lot. A procfs regular-file inode is fully
 * describable without its content — mode, one link, root-owned, size 0,
 * "now" for all three times — so synthesize the numbers directly.
 */

/* Device of the procfs mount. /proc itself is readable and statable
 * (Android's denials are per-file labels, not the mount), so one probe
 * serves the process lifetime; racing probers all store the same value.
 * Nothing in the target workloads compares st_dev, but keeping the
 * shadows on the procfs device costs one syscall total. */
static unsigned int proc_dev_major_cache, proc_dev_minor_cache;
static int proc_dev_known;

static void proc_dev_probe(void)
{
	if (proc_dev_known) return;
	struct statx sx;
	memset(&sx, 0, sizeof sx);
	if (tawcroot_statx_compat(AT_FDCWD, "/proc", 0, STATX_INO, &sx) != 0)
		return;
	proc_dev_major_cache = sx.stx_dev_major;
	proc_dev_minor_cache = sx.stx_dev_minor;
	proc_dev_known = 1;
}

/* A fixed per-kind inode, parked far above any real procfs number so it
 * can't collide with one the guest saw through a passthrough stat. */
static unsigned long shadow_ino(int kind)
{
	return 0x7a77c0000000UL + (unsigned long)kind;
}

long tawcroot_proc_shadow_stat(int kind, struct stat *out)
{
	if (kind <= TAWCROOT_PROC_SHADOW_NONE ||
	    kind > TAWCROOT_PROC_SHADOW_KIND_MAX)
		return TAWC_EINVAL;
	proc_dev_probe();
	long sec, nsec;
	realtime_now(&sec, &nsec);

	memset(out, 0, sizeof *out);
	out->st_dev = (__typeof__(out->st_dev))
		TAWC_MKDEV(proc_dev_major_cache, proc_dev_minor_cache);
	out->st_ino = (__typeof__(out->st_ino))shadow_ino(kind);
	out->st_mode = S_IFREG | 0444;
	out->st_nlink = 1;
	out->st_uid = 0;
	out->st_gid = 0;
	out->st_size = 0;
	out->st_blksize = 1024;
	out->st_blocks = 0;
	out->st_atim.tv_sec = sec; out->st_atim.tv_nsec = nsec;
	out->st_mtim.tv_sec = sec; out->st_mtim.tv_nsec = nsec;
	out->st_ctim.tv_sec = sec; out->st_ctim.tv_nsec = nsec;
	return 0;
}

long tawcroot_proc_shadow_statx(int kind, unsigned int mask,
				struct statx *out)
{
	if (kind <= TAWCROOT_PROC_SHADOW_NONE ||
	    kind > TAWCROOT_PROC_SHADOW_KIND_MAX)
		return TAWC_EINVAL;
	proc_dev_probe();
	long sec, nsec;
	realtime_now(&sec, &nsec);

	memset(out, 0, sizeof *out);
	out->stx_dev_major = proc_dev_major_cache;
	out->stx_dev_minor = proc_dev_minor_cache;
	out->stx_ino = shadow_ino(kind);
	out->stx_mode = (uint16_t)(S_IFREG | 0444);
	out->stx_nlink = 1;
	out->stx_uid = 0;
	out->stx_gid = 0;
	out->stx_size = 0;
	out->stx_blksize = 1024;
	out->stx_blocks = 0;
	out->stx_atime.tv_sec = sec; out->stx_atime.tv_nsec = (uint32_t)nsec;
	out->stx_mtime.tv_sec = sec; out->stx_mtime.tv_nsec = (uint32_t)nsec;
	out->stx_ctime.tv_sec = sec; out->stx_ctime.tv_nsec = (uint32_t)nsec;
	out->stx_mask = STATX_BASIC_STATS;
	/* Mount id: no backing fd for the synthetic file, but it claims to
	 * live on the procfs mount, so an O_PATH fd of /proc answers for
	 * it. Same reason the shm synthesizers fill one — systemd's
	 * chase() EUNATCHes without it. */
	if (mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) {
		long pfd = tawc_openat(AT_FDCWD, "/proc",
				       O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
		if (pfd >= 0) {
			tawcroot_statx_fill_mnt_id((int)pfd, 0, mask, out);
			tawc_close((int)pfd);
		}
	}
	return 0;
}

long tawcroot_proc_shadow_access(int kind, int mode)
{
	if (kind <= TAWCROOT_PROC_SHADOW_NONE ||
	    kind > TAWCROOT_PROC_SHADOW_KIND_MAX)
		return TAWC_EINVAL;
	/* The shadows are 0444 regular files: F_OK and R_OK pass, any
	 * write or execute probe is -EACCES. faccessat carries no flags
	 * and faccessat2-with-flags is already -ENOSYS'd by the handler,
	 * so there is no NOFOLLOW variant to consider. */
	if (mode & (2 /*W_OK*/ | 1 /*X_OK*/)) return TAWC_EACCES;
	return 0;
}
