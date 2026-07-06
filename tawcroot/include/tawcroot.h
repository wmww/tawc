/* Top-level types and entry-point contract for tawcroot.
 *
 * Phase 0 has only the entry point. Every later phase adds members
 * (bind table, dispatch table, fd-provenance table, exec_state_fd
 * format) into this header so the dependency graph stays acyclic.
 *
 * See notes/tawcroot/README.md.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Real entry point. _start (in arch/<arch>_start.S) sets up argc/argv
 * from the kernel's initial stack and tail-calls into here. We never
 * return — sys_exit_group from inside tawcroot_main, or
 * (later) the manual-load jump into guest code, terminates this
 * function's logical lifetime. */
void tawcroot_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
