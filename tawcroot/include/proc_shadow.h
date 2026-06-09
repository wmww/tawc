/* /proc shadow synthesis + /proc/self path classification.
 *
 * A handful of /proc files need a synthesized stand-in before the guest
 * may open them — either because the kernel's content leaks host paths
 * the guest's world view doesn't contain (/proc/self/maps), or because
 * Android's sandbox makes the real file unreadable in a way guests
 * mishandle (/proc/sys/kernel/overflow{uid,gid}, /proc/bus/pci/devices).
 * See the comments on each synthesizer for the per-file story.
 *
 * Everything here is async-signal-safe (raw syscalls, no allocation
 * outside anonymous mmaps that are unmapped before return).
 */

#pragma once

#include <stddef.h>

/* Classify `path` against the shadowed /proc files and synthesize the
 * matching fd. Returns 1 on a hit (with *out set to the new fd or the
 * synthesizer's -errno) and 0 on no match. Centralising the dispatch
 * keeps the absolute-path and fd-relative open branches in lockstep —
 * a future shadow only needs one line added here. */
int tawcroot_proc_shadow_open(const char *path, long *out);

/* True iff `path` is /proc/self/exe (or /proc/<own-tid>/exe, with an
 * optional task/<tid>/ segment). Used by the readlink handlers for
 * guest-exe synthesis. */
int tawcroot_is_proc_self_exe(const char *path);

/* Fast-out for fd-relative opens: can this relative leaf even compose
 * into a /proc path we shadow? Cheap first-byte test that skips the
 * readlinkat for the vast majority of fd-relative opens. */
int tawcroot_could_be_proc_relative(const char *p);

/* Compose `dirfd`'s host path (via /proc/self/fd/<n>) with a relative
 * guest path into `out`, for re-classifying fd-relative /proc accesses
 * (e.g. openat(proc_dir_fd, "self/maps", ...)). Returns the composed
 * length or -errno. `dirfd` must be a real fd (not AT_FDCWD); the
 * guest path must be relative. */
long tawcroot_compose_fd_relative(int dirfd, const char *gpath_str,
				  char *out, size_t cap);
