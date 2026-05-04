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
 * (0 <= rv <= n). The buffer is NOT bounds-checked beyond reclen
 * sanity; on a malformed dirent (zero or runaway reclen) the function
 * bails and returns the original length unchanged.
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
