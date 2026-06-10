/* SIGSYS dispatch handlers for execve / execveat. The handlers are
 * thin adapters around exec_handler.h's prepare/commit pair. See
 * syscalls_exec.c.
 */

#pragma once

/* Register the exec handler set in the dispatch table. Called from
 * tawcroot_dispatch_init. */
void tawcroot_exec_register(void);
