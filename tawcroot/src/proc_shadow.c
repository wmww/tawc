/* See include/proc_shadow.h for the public surface and rationale. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "io.h"
#include "path.h"
#include "proc_rewrite.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

/* Fast-out: does this guest-relative leaf even have a chance of
 * composing into a /proc/<x> path that we shadow? Legitimate first chars
 * are {b,s,t,m,e,digit} — covering "bus/" (pci/devices), "self/", "sys/",
 * "task/", "maps", "exe", and numeric "<tid>/" forms. Anything else (the
 * vast majority of fd-relative opens — config files, dotfiles, library
 * names, etc.) skips the readlinkat. */
int tawcroot_could_be_proc_relative(const char *p)
{
	char c = p[0];
	return c == 'b' || c == 's' || c == 't' || c == 'm' || c == 'e' ||
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
			      0 /*O_RDONLY*/ | 0x80000 /*O_CLOEXEC*/, 0);
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

/* If `path` is "/proc/self/<x>" or "/proc/<tid>/<x>" — where <tid> is
 * our TGID or any TID belonging to it — return a pointer to "<x>". Also
 * peels an optional "task/<tid>/" segment after `self/` or `<tid>/`,
 * since /proc/<pid>/task/<tid>/maps is per-mm (identical to .../maps) and
 * /exe is a symlink to the same exe. Returns NULL on no match.
 *
 * Strict on the prefix bytes: paths like "/proc/foo/../self/exe" are
 * caught only after the guest's libc canonicalizes them (the typical
 * flow). */
static const char *strip_proc_self_prefix(const char *path)
{
	if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
	    path[3] != 'o' || path[4] != 'c' || path[5] != '/')
		return 0;
	const char *t = path + 6;
	const char *after_pid;
	if (t[0] == 's' && t[1] == 'e' && t[2] == 'l' && t[3] == 'f' &&
	    t[4] == '/') {
		after_pid = t + 5;
	} else if (t[0] >= '0' && t[0] <= '9') {
		long n = 0;
		const char *p = t;
		while (*p >= '0' && *p <= '9') {
			n = n * 10 + (*p - '0'); p++;
			if (n > 0x7fffffff) return 0;
		}
		if (p[0] != '/') return 0;
		if (!is_my_tid(n)) return 0;
		after_pid = p + 1;
	} else {
		return 0;
	}

	/* Optional "task/<tid>/" peel. On any structural problem with the
	 * inner segment (no digits, overflow, missing trailing slash, or
	 * the tid isn't ours) we leave `after_pid` alone; the caller's
	 * tail-match (e.g. against "maps") then fails and synthesis
	 * doesn't fire. Returning NULL would also be safe but loses the
	 * outer match — and `/proc/self/task/<x>` is never a synthesis
	 * target anyway, so the difference is academic. */
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
		if (!is_my_tid(m)) return after_pid;
		return p + 1;
	}
	return after_pid;
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

int tawcroot_is_proc_self_exe(const char *path)
{
	const char *tail = strip_proc_self_prefix(path);
	return tail && tawc_streq(tail, "exe");
}

static int is_proc_self_maps(const char *path)
{
	const char *tail = strip_proc_self_prefix(path);
	return tail && tawc_streq(tail, "maps");
}

/* Match the /proc/sys/kernel/overflow{uid,gid} pair. Returns the memfd
 * label on a hit (the suffix is the only thing that varies between the
 * two), NULL otherwise. The label is what shows up under
 * /proc/self/fd/<fd> as "/memfd:tawcroot-overflow{uid,gid} (deleted)" —
 * useful when reading the diagnostic output of a guest that strerror()s
 * its way through bwrap. */
static const char *match_proc_sys_overflow_id(const char *path)
{
	if (tawc_streq(path, "/proc/sys/kernel/overflowuid"))
		return "tawcroot-overflowuid";
	if (tawc_streq(path, "/proc/sys/kernel/overflowgid"))
		return "tawcroot-overflowgid";
	return NULL;
}

/* Match the single file libpci's procfs back-end opens to enumerate
 * PCI devices. The directory itself (`/proc/bus/pci`) and per-bus
 * subdirs are not matched — guests that want to walk them get the
 * kernel's normal -EACCES, same as today. */
static int is_proc_bus_pci_devices(const char *path)
{
	return tawc_streq(path, "/proc/bus/pci/devices");
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
	long r = tawc_mmap(0, cap, 3 /*PROT_READ|PROT_WRITE*/,
			   0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0);
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
			       0 /*O_RDONLY*/ | 0x80000 /*O_CLOEXEC*/, 0);
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
 * the two in /proc/self/fd/<fd> readlinks for diagnostic clarity. */
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

/* Classify `path` against the three /proc shadows and synthesize the
 * matching fd. Returns 1 on a hit (with *out set to the new fd or the
 * synthesizer's -errno) and 0 on no match. Centralising the dispatch
 * keeps the absolute-path and fd-relative branches in handle_openat
 * automatically in lockstep — a future fourth shadow only needs one
 * line added here. */
int tawcroot_proc_shadow_open(const char *path, long *out)
{
	const char *overflow_name;
	if (is_proc_self_maps(path)) {
		*out = open_proc_maps_shadow();
		return 1;
	}
	overflow_name = match_proc_sys_overflow_id(path);
	if (overflow_name) {
		*out = open_proc_overflow_id_shadow(overflow_name);
		return 1;
	}
	if (is_proc_bus_pci_devices(path)) {
		*out = open_proc_bus_pci_devices_shadow();
		return 1;
	}
	return 0;
}
