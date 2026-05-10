/* Shared filesystem scaffolding for integration tests that need to
 * build a fake rootfs, copy fixture binaries into it, and wait for a
 * marker file to appear after running tawcroot.
 *
 * Used by:
 *   - tests/integration/test_prod_rootfs.c
 *   - tests/integration/test_exec_child.c
 *   - tests/integration/test_prod_fork.c
 *
 * The phase-1 / androidfilter suites have their own rootfs builder
 * with a much richer symlink topology; we deliberately don't share
 * with them because their needs and assertions are different.
 *
 * No cleat test() blocks live in rootfs_helpers.c — it's pure helpers
 * compiled into the orchestrator alongside the test files.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* `rm -rf <path>` via /bin/sh — predictable trees, no need for ftw. */
void rh_rmrf(const char *path);

bool rh_mkdir_p(const char *path, mode_t mode);

/* Copy `src` to `dst`, chmod to mode. Truncates `dst` if it exists. */
bool rh_copy_file(const char *src, const char *dst, mode_t mode);

bool rh_write_text(const char *path, const char *contents);

/* Build a tmpdir-unique marker path: "<TMPDIR>/<tag>-<pid>-<time_ns>".
 * Caller passes `out` of size `cap` (PATH_MAX is plenty). The path
 * itself is not created — callers test for it after the guest runs.
 *
 * Returns true on success; false if `cap` is too small. */
bool rh_make_marker_path(char *out, size_t cap, const char *tag);

/* Poll for `path` to exist. Total budget = 50ms × ticks. Returns true
 * if seen within budget, false on timeout. Used by --exec-child orphan
 * tests and the new fork tests, which can't observe the marker-bearing
 * process via waitpid (it's either orphaned or in a foreign process
 * tree). */
bool rh_poll_for_path(const char *path, int ticks);
