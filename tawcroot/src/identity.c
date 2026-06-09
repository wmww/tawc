/* Fake-root identity surface — phase 1.
 *
 * Mirrors `proot -0`: getuid/geteuid/getgid/getegid return 0 regardless
 * of the actual host uid. Stat/chown decoration lands in syscalls/fs.c
 * once we have the dispatch + path translation working end-to-end.
 *
 * See notes/tawcroot.md "Fake-root identity and metadata".
 */

#include <stdint.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "identity.h"
#include "sysnr.h"
#include "usercopy.h"

static long fake_zero(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return 0;
}

/* getresuid/getresgid take three out-pointer args: ruid, euid, suid (or
 * the gid variants). bash uses these for $UID / $EUID — without
 * trapping these the guest sees the kernel's real uid (the app uid).
 * Write 0 into all three slots through the EFAULT-safe copy helper. */
static long fake_zero_resuid(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	uid_t zero = 0;
	void *p[3] = {
		(void *)(uintptr_t)args->a,
		(void *)(uintptr_t)args->b,
		(void *)(uintptr_t)args->c,
	};
	for (int i = 0; i < 3; i++) {
		if (!p[i]) return TAWC_EFAULT;  /* kernel rejects NULL too */
		long r = tawc_copy_to_guest(p[i], &zero, sizeof zero);
		if (r < 0) return r;
	}
	return 0;
}

void tawcroot_identity_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_getuid,     fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_geteuid,    fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_getgid,     fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_getegid,    fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_getresuid,  fake_zero_resuid);
	tawcroot_dispatch_install(TAWC_SYS_getresgid,  fake_zero_resuid);

	/* set*id family: fake success, mirroring proot -0. The guest
	 * believes it is root; the kernel would return EPERM from the
	 * unprivileged app uid, aborting daemons that drop privileges and
	 * package maintainer scripts (runuser, setpriv). Unconditional 0 —
	 * proot does the same and the get* side keeps reporting 0
	 * regardless of what was "set". setfsuid/setfsgid return the
	 * PREVIOUS fs id, which under fake-root is also 0. */
	tawcroot_dispatch_install(TAWC_SYS_setuid,     fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setgid,     fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setreuid,   fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setregid,   fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setresuid,  fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setresgid,  fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setfsuid,   fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setfsgid,   fake_zero);
	tawcroot_dispatch_install(TAWC_SYS_setgroups,  fake_zero);
}
