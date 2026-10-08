/* xattr handlers: path-bearing *xattr / l*xattr, plus the fd-based
 * writers trapped for the read-only-bind check. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "io.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "syscalls_fs.h"
#include "syscalls_fs_internal.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

/* Path-bearing xattr handlers. The xattr syscalls don't have an *at
 * family; we route through `/proc/self/fd/<base_fd>/<suffix>`. The
 * `l*xattr` variants don't follow leaf symlinks: we use NOFOLLOW path
 * mode (so the resolver leaves the leaf alone) and dispatch to the
 * matching `l*xattr` syscall (the kernel handles the leaf-NOFOLLOW
 * itself).
 *
 * Empty-suffix corner case: if the guest path resolves to the rootfs/
 * bind dir itself (`use_empty == 1`), the dispatched path is just
 * `/proc/self/fd/<n>`, which IS a symlink in procfs. FOLLOW variants
 * are correct (the kernel walks the magic symlink and lands on the
 * dirfd's underlying inode); NOFOLLOW variants would target the
 * procfs entry itself rather than the dirfd's inode — semantically
 * wrong, so we short-circuit `l*xattr` on the empty suffix with
 * -EOPNOTSUPP. Programs almost never l*xattr the rootfs/bind root
 * directly, but the code shouldn't silently misroute when they do.
 *
 * `f*xattr` (fd-based) is NOT trapped — fds are opaque to us and the
 * kernel's resolution against an open fd is already correct.
 *
 * Most calls will return -EOPNOTSUPP / -ENOTSUP from the kernel
 * (Android app-private storage doesn't carry xattrs); the value of
 * trapping is that the guest sees the right errno against the right
 * path rather than a host-relative error. */
#define DECLARE_PATH_XATTR(name, sysnr, narg, pmode, pintent)             \
static long handle_##name(const tawcroot_syscall_args *args, ucontext_t *uc) \
{                                                                          \
	(void)uc;                                                          \
	const char *gpath = (const char *)(uintptr_t)args->a;                  \
	if (!gpath) return TAWC_EFAULT;                                        \
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);                                   \
	struct fs_path t;                                                      \
	long e = tawcroot_fs_translate_at(scratch, 0, AT_FDCWD, gpath,         \
					  pmode, pintent, &t);                 \
	if (e) return e;                                                   \
	if (t.is_root && (pmode) == TAWCROOT_PATH_NOFOLLOW) {                  \
		return TAWC_EOPNOTSUPP; /* see big comment above */            \
	}                                                                      \
	char *host_path = scratch->buf[2];                                      \
	long bp = tawc_proc_fd_path(host_path,                                  \
				    TAWCROOT_PATH_SCRATCH_SIZE,               \
				    t.fd, t.path);                             \
	if (bp < 0) return bp;                                             \
	return TAWC_RAW(sysnr, (long)host_path,                            \
			args->b, args->c, args->d,                         \
			(narg) > 4 ? args->e : 0,                          \
			(narg) > 5 ? args->f : 0);                         \
}

DECLARE_PATH_XATTR(setxattr,     TAWC_SYS_setxattr,     5, TAWCROOT_PATH_FOLLOW,
		   TAWCROOT_PATH_INTENT_WRITE)
DECLARE_PATH_XATTR(lsetxattr,    TAWC_SYS_lsetxattr,    5, TAWCROOT_PATH_NOFOLLOW,
		   TAWCROOT_PATH_INTENT_WRITE)
DECLARE_PATH_XATTR(getxattr,     TAWC_SYS_getxattr,     4, TAWCROOT_PATH_FOLLOW,
		   TAWCROOT_PATH_INTENT_READ)
DECLARE_PATH_XATTR(lgetxattr,    TAWC_SYS_lgetxattr,    4, TAWCROOT_PATH_NOFOLLOW,
		   TAWCROOT_PATH_INTENT_READ)
DECLARE_PATH_XATTR(listxattr,    TAWC_SYS_listxattr,    3, TAWCROOT_PATH_FOLLOW,
		   TAWCROOT_PATH_INTENT_READ)
DECLARE_PATH_XATTR(llistxattr,   TAWC_SYS_llistxattr,   3, TAWCROOT_PATH_NOFOLLOW,
		   TAWCROOT_PATH_INTENT_READ)
DECLARE_PATH_XATTR(removexattr,  TAWC_SYS_removexattr,  2, TAWCROOT_PATH_FOLLOW,
		   TAWCROOT_PATH_INTENT_WRITE)
DECLARE_PATH_XATTR(lremovexattr, TAWC_SYS_lremovexattr, 2, TAWCROOT_PATH_NOFOLLOW,
		   TAWCROOT_PATH_INTENT_WRITE)

/* fd-based xattr WRITERS (stage 2). Previously untrapped by design —
 * the kernel's fd resolution is already correct — but an fd opened
 * read-only through an RO bind still accepts metadata writes on the
 * host-RW mount, so trap, RO-check, and pass through raw otherwise.
 * The read-side f*xattr (fgetxattr/flistxattr) stay untrapped. Mostly
 * EOPNOTSUPP on app-data anyway; same-fs RO binds need the refusal. */
static long handle_fsetxattr(const tawcroot_syscall_args *args,
			     ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	if (tawcroot_fs_fd_in_ro_bind(fd, 1)) return TAWC_EROFS;
	return TAWC_RAW(TAWC_SYS_fsetxattr, args->a, args->b, args->c,
			args->d, args->e, 0);
}

static long handle_fremovexattr(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	int fd = (int)args->a;
	if (tawcroot_fd_is_reserved(fd)) return TAWC_EBADF;
	if (tawcroot_fs_fd_in_ro_bind(fd, 1)) return TAWC_EROFS;
	return TAWC_RAW(TAWC_SYS_fremovexattr, args->a, args->b, 0, 0, 0, 0);
}

void tawcroot_fs_xattr_register(void)
{
	/* Path-bearing xattr family. f*xattr variants take an fd and stay
	 * untrapped — the kernel's fd-based resolution is already correct.
	 * Most of these will return -EOPNOTSUPP on Android app-private
	 * storage; trapping ensures the failure is against the guest-
	 * visible path rather than a host-relative one. */
	tawcroot_dispatch_install(TAWC_SYS_setxattr,    handle_setxattr);
	tawcroot_dispatch_install(TAWC_SYS_lsetxattr,   handle_lsetxattr);
	tawcroot_dispatch_install(TAWC_SYS_getxattr,    handle_getxattr);
	tawcroot_dispatch_install(TAWC_SYS_lgetxattr,   handle_lgetxattr);
	tawcroot_dispatch_install(TAWC_SYS_listxattr,   handle_listxattr);
	tawcroot_dispatch_install(TAWC_SYS_llistxattr,  handle_llistxattr);
	tawcroot_dispatch_install(TAWC_SYS_removexattr, handle_removexattr);
	tawcroot_dispatch_install(TAWC_SYS_lremovexattr,handle_lremovexattr);
	/* fd-based xattr writers — trapped for the RO-bind stage-2 check
	 * only; non-RO fds pass through raw. */
	tawcroot_dispatch_install(TAWC_SYS_fsetxattr,   handle_fsetxattr);
	tawcroot_dispatch_install(TAWC_SYS_fremovexattr,handle_fremovexattr);
}
