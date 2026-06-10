/* SIGSYS-handler-side execve interception.
 *
 * Catches the guest's `execve` / `execveat`, validates the binary,
 * writes the request into a memfd, and re-execs tawcroot itself with
 * `--exec-child <fd>`. From the guest's point of view the syscall
 * succeeds and a new program is running, exactly like a kernel-
 * driven execve — except the new tawcroot process inherits our
 * SIGSYS handler chain (we re-install it in `--exec-child` after
 * the kernel-execve resets handlers to SIG_DFL).
 *
 * Why we can't let the guest's execve go through:
 * - The kernel resets every signal handler to SIG_DFL on a successful
 *   exec (`man 2 execve`). The seccomp filter is preserved (filters
 *   ride along), so the new program runs with our trap set installed
 *   but no SIGSYS handler.
 * - The new program's first path-bearing syscall traps to SIGSYS
 *   with disposition SIG_DFL → kernel kills the program.
 *
 * This file's contract is two phases:
 *
 *   `tawcroot_exec_handler_prepare(path, argc, argv, envp)`
 *     - path: the UNTRANSLATED guest path. prepare translates it for
 *       the probe, and the exec-child re-translates it after re-exec
 *       (the rootfs view is rebuilt from the exec_state extras).
 *     - argv: NULL-terminated guest-supplied argv.
 *     - envp: NULL-terminated guest-supplied envp.
 *     - Validates the target like execve would (openable, regular
 *       file, some execute bit; directories are -EISDIR).
 *     - Creates non-CLOEXEC memfd, writes exec_state, rewinds.
 *     - Returns the memfd (>= 0) or -errno. Stages through static
 *       buffers — callers hold syscalls_exec.c's exec_lock.
 *
 *   `tawcroot_exec_handler_commit(mfd)`
 *     - Opens /proc/self/exe, execveat-s self with
 *       `--exec-child <fdstr>`. Touches no shared statics — callers
 *       must have RELEASED exec_lock first (see below).
 *     - On success: never returns (control transfers to the new
 *       tawcroot incarnation).
 *     - On failure: closes mfd and returns -errno. Caller (the SIGSYS
 *       handler) surfaces this back to the guest as the result of its
 *       `execve` syscall.
 *
 * The handler uses raw_sys.h for every syscall. The exec path stages
 * argv/envp/exec_state in static buffers (here and in syscalls_exec.c);
 * a process-global spinlock in syscalls_exec.c (exec_lock) serializes
 * the static-buffer phase so two concurrent execs — or a CLONE_VM
 * child exec'ing while the parent execs — can't interleave.
 *
 * The phase split exists because the lock must be RELEASED before the
 * execveat commit point. A posix_spawn child (CLONE_VM|CLONE_VFORK)
 * shares the parent's address space; a lock still held when the child
 * execveats away is leaked set in the PARENT's memory (and via fork
 * snapshots into every later child), so the next exec anywhere in the
 * family spins forever. Firefox hit exactly this: glxtest is
 * posix_spawn'd, then the startup-crash relaunch fork+execve hung.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 1 of the re-exec dance: validate the target and serialize the
 * exec request. See module header.
 *
 * `path` is the guest path the guest wanted to exec (translation
 * happens inside). `argv` and `envp` are NULL-terminated arrays of
 * NUL-terminated strings (same shape as argv/envp passed to execve).
 *
 * Returns the non-CLOEXEC exec_state memfd (>= 0) on success, or
 * -errno:
 *   probe errors    raw open/translate errno (-ENOENT, -EACCES, …),
 *                   -EISDIR for directories, -EACCES for non-regular
 *                   or non-executable files
 *   -ENOSPC        exec_state too large for the serialization buffer
 *   -EFAULT        memfd creation / write / rewind failed */
long tawcroot_exec_handler_prepare(const char *path, int argc,
                                   const char *const *argv,
                                   const char *const *envp);

/* Phase 2: execveat self as `--exec-child <mfd>`. Never returns on
 * success. On failure closes `mfd` and returns -errno (-ENOEXEC when
 * /proc/self/exe couldn't be opened, else the execveat errno). */
long tawcroot_exec_handler_commit(int mfd);

/* prepare + commit in one call, for single-threaded callers that don't
 * stage through the SIGSYS dispatch path (testhost --exec-via-handler).
 * Never returns 0 — either the dance succeeds and control transfers
 * to the new tawcroot, or we return a negative errno. */
long tawcroot_exec_handler_perform(const char *path, int argc,
                                   const char *const *argv,
                                   const char *const *envp);

#ifdef __cplusplus
}
#endif
