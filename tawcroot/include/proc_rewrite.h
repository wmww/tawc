/* /proc/self/maps reverse-translation.
 *
 * The kernel's view of `/proc/self/maps` lists every mapping with the
 * HOST path of its backing file. Programs that grep their own maps
 * (sandboxes computing "where did my libc load from", crash handlers
 * dumping map info, profilers/tracers resolving symbols) see paths that
 * don't exist in the guest's world view. We trap `openat` on
 * `/proc/self/maps` and `/proc/<our-pid>/maps`, read the kernel's
 * output into memory, reverse-translate each path against the rootfs
 * and bind tables, and return a memfd containing the rewritten content.
 *
 * This header exposes the pure rewriter (no syscalls, no globals) so
 * the cleat unit-test orchestrator can exercise it directly under
 * hosted glibc. The syscall-side glue (open the kernel file, allocate
 * buffers via raw mmap, write the memfd) lives in syscalls_fs.c.
 */

#pragma once

#include <stddef.h>

#include "path.h"

/* Inputs to the rewriter. All borrowed; the rewriter does not retain
 * pointers across the call. */
typedef struct {
	const char                 *rootfs_host_path;     /* "/data/.../rootfs" */
	size_t                      rootfs_host_path_len;
	const struct tawcroot_bind *binds;
	size_t                      n_binds;
} tawcroot_proc_rewrite_ctx;

/* Reverse-translate one host path back to a guest-visible path.
 *
 * Match rules (longest-prefix wins, component-boundary required):
 *   1. host_path starts with `<bind src>` + `/` (or equals it) → returns
 *      `/<bind dst>[/<rest>]`. Bind matches outrank rootfs because a
 *      bind is an authoritative replacement for that subtree.
 *   2. host_path starts with `<rootfs>` + `/` (or equals it) → returns
 *      `/[<rest>]`.
 *
 * Returns the number of bytes written to `out` (excluding the NUL),
 * `-ENOENT` if no prefix matched OR any input pointer is NULL OR
 * `out_cap` is zero (caller should leave the path alone), or
 * `-ENAMETOOLONG` if a valid `out_cap` is too small for the result.
 *
 * `out` is NUL-terminated on success. */
long tawcroot_proc_reverse_translate_path(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *host_path, size_t host_len,
	char *out, size_t out_cap);

/* Rewrite a `/proc/self/maps` buffer.
 *
 * Reads `in[0..in_len]` line by line. For each line, the path field
 * (everything after the inode column) is reverse-translated when it
 * matches a rootfs/bind prefix, and emitted verbatim otherwise. Lines
 * with bracketed pseudo-paths (`[heap]`, `[stack]`, `[vdso]`, …) and
 * empty paths are passed through. Lines without a trailing `\n` are
 * treated as the final partial line and emitted as-is.
 *
 * Returns the number of bytes written to `out`, or `-ENOSPC` if the
 * output buffer is too small. The output is NOT NUL-terminated — it's
 * a binary blob the caller writes to a memfd. */
long tawcroot_proc_maps_rewrite(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *in, size_t in_len,
	char *out, size_t out_cap);
