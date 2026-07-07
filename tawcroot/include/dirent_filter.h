/* Pure-function helpers for the getdents64 reserved-fd filter.
 *
 * Lives in PROD_C_FOR_TESTS so the cleat unit-test orchestrator can
 * exercise the logic under hosted glibc without forking a guest.
 * No syscalls, no globals — caller hands in everything. */

#pragma once

#include <stddef.h>

/* Returns 1 if the readlink target `link` (length `n`) names
 * /proc/self/fd or /proc/<digits>/fd, 0 otherwise. The trailing NUL
 * is not required (we use `n` exclusively). Used as the gate to
 * decide whether to compact getdents64 output: non-proc dirfds
 * short-circuit. */
int tawcroot_dirent_filter_is_proc_fd_link(const char *link, long n);

/* Returns 1 if `name` (NUL-terminated) is a strict decimal integer
 * (no leading sign, no spaces) whose value appears in
 * `reserved_fds[0..n_reserved)`. Caller passes the reserved table
 * directly so tests can control it. */
int tawcroot_dirent_filter_dname_is_reserved(const char *name,
					     const int *reserved_fds,
					     size_t n_reserved);

/* Compact a linux_dirent64 buffer in place, dropping entries whose
 * d_name parses to a reserved fd. Returns the new byte length
 * (0 <= rv <= n). The buffer is guest memory, so records are sanity-
 * checked (reclen bounds, name NUL inside the record). On a malformed
 * dirent the function bails: if nothing was dropped yet the buffer is
 * untouched and the original length is returned; otherwise the
 * compacted prefix length is returned and the malformed tail dropped.
 *
 * Layout of linux_dirent64:
 *   u64  d_ino    (offset 0)
 *   s64  d_off    (offset 8)
 *   u16  d_reclen (offset 16)
 *   u8   d_type   (offset 18)
 *   char d_name[] (offset 19)
 * d_name is NUL-terminated within d_reclen bytes (kernel guarantees). */
long tawcroot_dirent_filter_compact(void *buf, long n,
				    const int *reserved_fds,
				    size_t n_reserved);

/* Rewrite d_type DT_LNK → DT_UNKNOWN in a linux_dirent64 buffer, in
 * place (hardlink emulation: emulated names are symlinks on disk, but
 * DT_LNK would let type-trusting walkers — find's FTS_NOSTAT, fd,
 * ripgrep — classify them without ever statting into the fixed-up
 * stat handlers). DT_UNKNOWN forces the lstat. Applied to rootfs-view
 * directories only, and it degrades every symlink's d_type there —
 * the accepted cost. Same malformed-record bail as compact, except no
 * bytes ever move, so the return is always `n`. */
long tawcroot_dirent_filter_delink_types(void *buf, long n);
