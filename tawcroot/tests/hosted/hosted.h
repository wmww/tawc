/* Harness for hosted handler-level tests.
 *
 * These tests compile the whole production tree into the cleat tests
 * binary (ASan+UBSan) and call syscall handlers directly, in-process:
 * no seccomp filter, no SIGSYS delivery, no fork. "Guest memory" is
 * plain test-owned buffers — usercopy.c targets the current task via
 * process_vm_readv/writev, so handlers' EFAULT-safe copy paths work
 * unmodified. The rootfs is a real tmpdir tree.
 *
 * What this layer is for: handler logic coverage (decode → copy from
 * guest → translate → real syscall → write result back) with memory-
 * error detection, plus hook-based fault injection the fork layers
 * can't do. What it is NOT for: filter install, SIGSYS mechanics,
 * _start/bootstrap — those stay in the fork-based handler/integration
 * layers against the real freestanding binaries.
 *
 * The entry points are macros that capture `test_ctx` from the calling
 * test body (cleat's assertion macros need it), wrapping the _impl
 * functions below.
 */

#pragma once

#include <cleat/test.h>
#include <stdbool.h>
#include <stddef.h>

#include "raw_syscall_host.h"

typedef struct {
	char root[4096];          /* host path of the tmpdir rootfs */
	char saved_cwd[4096];
	int  fds_before;          /* open-fd count at setup, for leak diff */
} th_view;

/* Build a tmpdir rootfs skeleton and make it the current root view:
 *
 *   <root>/etc/probe          "from-rootfs\n"
 *   <root>/etc/sub/           (empty dir)
 *   <root>/usr/lib/probe.so   "fake-lib\n"
 *   <root>/lib -> usr/lib     (well-known symlink, memoized)
 *   <root>/run/, <root>/tmp/  (empty dirs)
 *
 * Opens + reserves the rootfs fd, sets the canonical host path, runs
 * the usercopy probe, memoizes well-known symlinks, and installs the
 * dispatch table — supervisor_init steps 1-6, minus binds/shm, minus
 * the SIGSYS handler and filter. Asserts (cleat) on any failure.
 *
 * `tag` distinguishes concurrent trees; keep it filename-safe. */
#define th_setup(v, tag) th_view_setup_impl(test_ctx, (v), (tag))

/* Add a bind: creates <root>-bind-<n>/ as the host src dir (with a
 * probe.txt inside), then routes guest `dst` to it. Returns the host
 * src path (static buffer — valid until the next add_bind). The _ro
 * variant marks the bind read-only. */
#define th_add_bind(v, dst) th_view_add_bind_impl(test_ctx, (v), (dst), 0)
#define th_add_bind_ro(v, dst) th_view_add_bind_impl(test_ctx, (v), (dst), 1)

/* Undo everything setup did: reset the raw-syscall hook, close every
 * reserved fd, clear the bind table and root-view globals, restore the
 * cwd, delete the tree, and assert no fd leaked relative to setup. */
#define th_teardown(v) th_view_teardown_impl(test_ctx, (v))

/* Call the dispatch handler for `nr` exactly as the SIGSYS handler
 * would, with a synthesized args struct. The fs/fd/proc handlers
 * ignore the ucontext (NULL here) — do not th_sys() the sigmask
 * handlers in syscalls_control.c, which dereference it. Asserts that
 * a handler is registered for `nr`. Args are cast to long, so string
 * literals and pointers pass through unchanged. */
#define th_sys(nr, a, b, c, d, e, f)                                      \
	th_sys_impl(test_ctx, (long)(nr), (long)(a), (long)(b),          \
		    (long)(c), (long)(d), (long)(e), (long)(f))

void th_view_setup_impl(TestCtx *test_ctx, th_view *v, const char *tag);
const char *th_view_add_bind_impl(TestCtx *test_ctx, th_view *v,
				  const char *dst, int ro);
void th_view_teardown_impl(TestCtx *test_ctx, th_view *v);
long th_sys_impl(TestCtx *test_ctx, long nr, long a, long b, long c,
		 long d, long e, long f);
