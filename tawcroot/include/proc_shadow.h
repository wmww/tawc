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

/* One-pass classification of the /proc magic links the readlink
 * handler synthesizes or post-processes. Strips the /proc/<pid> prefix
 * once and resolves pid ownership lazily (a /proc/<n>/status read) only
 * for the kinds that need it — fd links skip it entirely. The
 * single-kind helpers below are thin wrappers; prefer this in handlers
 * that test more than one kind. */
#define TAWCROOT_PROC_LINK_NONE      0  /* not a magic link we handle */
#define TAWCROOT_PROC_LINK_EXE_SELF  1  /* /proc/<own>/exe */
#define TAWCROOT_PROC_LINK_EXE_OTHER 2  /* /proc/<other-pid>/exe */
#define TAWCROOT_PROC_LINK_CWD       3  /* /proc/<own>/cwd */
#define TAWCROOT_PROC_LINK_FD        4  /* /proc/<any-pid>/fd/<n> */
int tawcroot_proc_link_classify(const char *path);

/* True iff `path` is /proc/self/exe (or /proc/<own-tid>/exe, with an
 * optional task/<tid>/ segment). Used by the readlink handlers for
 * guest-exe synthesis. */
int tawcroot_is_proc_self_exe(const char *path);

/* Finer-grained exe-link classification: distinguishes "/proc/<x>/exe
 * naming OUR process" (synthesize the stashed guest exe path) from
 * "naming some OTHER process" (must NOT be substituted — every tawcroot
 * guest's kernel exe is the same libtawcroot.so, so the readlink-result
 * equality check alone would return the CALLER's guest exe for a
 * different process's link). */
#define TAWCROOT_PROC_EXE_NONE  0  /* not a /proc/<x>/exe path */
#define TAWCROOT_PROC_EXE_SELF  1  /* our pid, or a tid of ours */
#define TAWCROOT_PROC_EXE_OTHER 2  /* a (numeric) pid that isn't ours */
int tawcroot_proc_exe_classify(const char *path);

/* True iff `path` is /proc/self/cwd (or /proc/<own-tid>/cwd, with an
 * optional task/<tid>/ segment). The readlink handler synthesizes the
 * guest cwd via tawcroot_cwd_to_guest_abs — the kernel's link target is
 * the host path, which the guest's world view doesn't contain. */
int tawcroot_is_proc_self_cwd(const char *path);

/* True iff `path` is /proc/self/fd/<n> or /proc/<pid>/fd/<n> (optional
 * task/<tid>/ segment), for ANY pid. The kernel resolves these magic
 * links to HOST paths; the readlink handler reverse-translates the
 * result through the rootfs/bind prefix walk so in-view targets come
 * back as guest paths (outside-view targets pass through verbatim). */
int tawcroot_is_proc_fd_link(const char *path);

/* Byte length of the dir/entry magic-link prefix in a /proc-RELATIVE
 * suffix (no leading "/proc/"): (self|thread-self|<pid>)/(task/<tid>/)?
 * then fd/<entry>, map_files/<entry>, cwd, or root. 0 = no match. Used
 * by the read-only-bind stage-2 check (readlink exactly the prefix,
 * RO-prefix-check the target); shares the pid/task grammar with the
 * shadow classifiers above so two matchers can't disagree. */
size_t tawcroot_proc_magic_link_prefix(const char *suf);

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
