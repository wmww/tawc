/* Runtime-control syscall handlers — guest-side denials and shadow
 * virtualization for seccomp/prctl/rt_sigaction/rt_sigprocmask and the
 * mount-management family. See syscalls_control.c.
 */

#pragma once

/* Register the control handler set in the dispatch table. Called from
 * tawcroot_dispatch_init. */
void tawcroot_control_register(void);
