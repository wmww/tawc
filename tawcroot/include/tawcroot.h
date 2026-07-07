/* Top-level types and entry-point contract for tawcroot.
 *
 * Phase 0 has only the entry point. Every later phase adds members
 * (bind table, dispatch table, fd-provenance table, exec_state_fd
 * format) into this header so the dependency graph stays acyclic.
 *
 * See notes/tawcroot/README.md.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Real entry point. _start (in arch/<arch>_start.S) sets up argc/argv
 * from the kernel's initial stack and tail-calls into here. We never
 * return — sys_exit_group from inside tawcroot_main, or
 * (later) the manual-load jump into guest code, terminates this
 * function's logical lifetime. */
void tawcroot_main(int argc, char **argv);

#ifndef TAWCROOT_TESTHOST
/* Parse a production `-b` spec "src:dst[:ro]" into NUL-terminated src
 * and dst buffers and the read-only flag. Returns 0 / -EINVAL. Lives
 * in main.c; exported (production builds only — the testhost binary
 * compiles main.c without the prod CLI) so the cleat unit table can
 * exercise the `:ro` forms directly. */
long tawcroot_parse_bind_spec(const char *spec, char *src_buf, size_t src_cap,
                              char *dst_buf, size_t dst_cap, int *ro_out);
#endif

#ifdef __cplusplus
}
#endif
