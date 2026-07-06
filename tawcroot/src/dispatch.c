/* Dispatch table storage + lookup.
 *
 * The table is a fixed-size flat C array. After init it is treated as
 * immutable (handlers stored once, never mutated) so the SIGSYS handler
 * can read it without locking. See notes/tawcroot/sigsys-handler.md "Threading and
 * `vfork` invariants".
 */

#include <stddef.h>

#include "chroot.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "identity.h"
#include "io.h"
#include "raw_sys.h"
#include "syscalls_control.h"
#include "syscalls_exec.h"
#include "syscalls_fs.h"
#include "syscalls_socket.h"

static tawcroot_handler_fn g_dispatch[TAWCROOT_DISPATCH_MAX];

long tawcroot_deny_enosys(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_ENOSYS;
}

long tawcroot_deny_eperm(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_EPERM;
}

void tawcroot_dispatch_install(int nr, tawcroot_handler_fn fn)
{
	/* Out-of-range (review finding D5): abort loudly so a future handler
	 * with `nr >= TAWCROOT_DISPATCH_MAX` doesn't silently never trap. */
	if (nr < 0 || nr >= TAWCROOT_DISPATCH_MAX) {
		tawc_io_str("tawcroot: dispatch_install: nr out of range: ");
		tawc_io_dec(nr);
		tawc_io_str(" (max=");
		tawc_io_dec(TAWCROOT_DISPATCH_MAX);
		tawc_io_str(")\n");
		tawc_exit_group(70);
	}
	/* Collision (review finding D6): two registrars asking for the same
	 * slot is a programming error. Today's groups are disjoint; abort if
	 * that ever stops being true so the cause shows up at startup, not
	 * via an obscurely-misbehaving handler later. */
	if (g_dispatch[nr] && g_dispatch[nr] != fn) {
		tawc_io_str("tawcroot: dispatch_install: collision on nr ");
		tawc_io_dec(nr);
		tawc_io_str("\n");
		tawc_exit_group(71);
	}
	g_dispatch[nr] = fn;
}

tawcroot_handler_fn tawcroot_dispatch_get(int nr)
{
	if (nr < 0 || nr >= TAWCROOT_DISPATCH_MAX) return 0;
	return g_dispatch[nr];
}

void tawcroot_dispatch_init(void)
{
	tawcroot_identity_register();
	tawcroot_fs_register();
	tawcroot_fd_register();
	tawcroot_control_register();
	tawcroot_exec_register();
	tawcroot_socket_register();
	tawcroot_chroot_register();
}

size_t tawcroot_dispatch_trap_list(int *out, size_t out_cap)
{
	size_t n = 0;
	for (int nr = 0; nr < TAWCROOT_DISPATCH_MAX; nr++) {
		if (!g_dispatch[nr]) continue;
		if (n < out_cap) out[n] = nr;
		n++;
	}
	return n;
}
