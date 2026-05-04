/* SIGSYS dispatch handlers for execve / execveat. The handlers are
 * thin adapters around tawcroot_exec_handler_perform (see
 * exec_handler.h). See syscalls_exec.c.
 */

#pragma once

/* Register the exec handler set in the dispatch table. Called from
 * tawcroot_dispatch_init. */
void tawcroot_exec_register(void);
