#include <sys/stat.h>

#include "errno_neg.h"
#include "raw_sys.h"
#include "stat_compat.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

long tawcroot_statx_compat(int dirfd, const char *path, int flags,
                         unsigned int mask, struct statx *out)
{
	long rv = TAWC_RAW(TAWC_SYS_statx, dirfd, (long)path, flags,
	                  mask, (long)out, 0);
	if (rv != TAWC_ENOSYS) return rv;

	/* statx arrived in Linux 4.11. Keep its flag validation when
	 * falling back, then strip the statx-only sync policy bits. */
	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT |
	              AT_EMPTY_PATH | AT_STATX_SYNC_TYPE))
		return TAWC_EINVAL;
	if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE ||
	    (mask & 0x80000000U))
		return TAWC_EINVAL;

	struct stat st;
	rv = TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)path, (long)&st,
	              flags & ~AT_STATX_SYNC_TYPE, 0, 0);
	if (rv < 0) return rv;

	memset(out, 0, sizeof *out);
	out->stx_mask = STATX_BASIC_STATS;
	out->stx_blksize = st.st_blksize;
	out->stx_nlink = st.st_nlink;
	out->stx_uid = st.st_uid;
	out->stx_gid = st.st_gid;
	out->stx_mode = st.st_mode;
	out->stx_ino = st.st_ino;
	out->stx_size = st.st_size;
	out->stx_blocks = st.st_blocks;
	out->stx_atime.tv_sec = st.st_atim.tv_sec;
	out->stx_atime.tv_nsec = st.st_atim.tv_nsec;
	out->stx_mtime.tv_sec = st.st_mtim.tv_sec;
	out->stx_mtime.tv_nsec = st.st_mtim.tv_nsec;
	out->stx_ctime.tv_sec = st.st_ctim.tv_sec;
	out->stx_ctime.tv_nsec = st.st_ctim.tv_nsec;
	out->stx_dev_major = TAWC_MAJOR(st.st_dev);
	out->stx_dev_minor = TAWC_MINOR(st.st_dev);
	out->stx_rdev_major = TAWC_MAJOR(st.st_rdev);
	out->stx_rdev_minor = TAWC_MINOR(st.st_rdev);
	/* Birth time, attributes, and mount ID remain unadvertised.
	 * The caller can fill a mount ID from /proc/self/fdinfo. */
	return 0;
}
