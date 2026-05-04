/* Filesystem syscall handlers — phase 1. See syscalls_fs.c. */

#pragma once

/* Register the path-bearing fs handler set in the dispatch table.
 * Called from tawcroot_dispatch_init. */
void tawcroot_fs_register(void);
