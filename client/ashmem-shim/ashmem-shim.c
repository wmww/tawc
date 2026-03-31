/*
 * ashmem-shim: LD_PRELOAD library that redirects memfd_create() to use
 * Android's ashmem driver instead. This works around SELinux denials where
 * untrusted_app cannot mmap tmpfs-backed memfds created by other processes.
 *
 * ashmem fds are in the mlstrustedobject set, so they bypass MLS checks
 * and can be freely shared between processes via Unix sockets.
 *
 * Usage: LD_PRELOAD=/path/to/libashmem-shim.so weston-simple-shm
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ashmem ioctl definitions (from linux/ashmem.h) */
#define ASHMEM_NAME_LEN 256
#define __ASHMEMIOC 0x77
#define ASHMEM_SET_NAME _IOW(__ASHMEMIOC, 1, char[ASHMEM_NAME_LEN])
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)

/*
 * Track which fds are ashmem so we can intercept ftruncate on them.
 * Simple bitset - fds above MAX_FD_TRACK fall through to real ftruncate.
 */
#define MAX_FD_TRACK 1024
static uint64_t ashmem_fds[MAX_FD_TRACK / 64];
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;

static void fd_set_ashmem(int fd) {
    if (fd >= 0 && fd < MAX_FD_TRACK) {
        pthread_mutex_lock(&fd_lock);
        ashmem_fds[fd / 64] |= (1ULL << (fd % 64));
        pthread_mutex_unlock(&fd_lock);
    }
}

static void fd_clear_ashmem(int fd) {
    if (fd >= 0 && fd < MAX_FD_TRACK) {
        pthread_mutex_lock(&fd_lock);
        ashmem_fds[fd / 64] &= ~(1ULL << (fd % 64));
        pthread_mutex_unlock(&fd_lock);
    }
}

static int fd_is_ashmem(int fd) {
    if (fd < 0 || fd >= MAX_FD_TRACK)
        return 0;
    pthread_mutex_lock(&fd_lock);
    int result = (ashmem_fds[fd / 64] >> (fd % 64)) & 1;
    pthread_mutex_unlock(&fd_lock);
    return result;
}

/*
 * Replace memfd_create with ashmem.
 * libwayland-client calls memfd_create() via the syscall wrapper in glibc
 * to create SHM pools. We intercept it here.
 */
int memfd_create(const char *name, unsigned int flags) {
    (void)flags;

    int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "ashmem-shim: open(/dev/ashmem) failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (name) {
        /* ASHMEM_SET_NAME takes a fixed-size buffer, truncate if needed */
        char ashmem_name[ASHMEM_NAME_LEN];
        strncpy(ashmem_name, name, ASHMEM_NAME_LEN - 1);
        ashmem_name[ASHMEM_NAME_LEN - 1] = '\0';
        ioctl(fd, ASHMEM_SET_NAME, ashmem_name);
    }

    fd_set_ashmem(fd);

    fprintf(stderr, "ashmem-shim: memfd_create(\"%s\") -> ashmem fd %d\n",
            name ? name : "(null)", fd);
    return fd;
}

/*
 * Intercept ftruncate to translate to ASHMEM_SET_SIZE for ashmem fds.
 * libwayland calls ftruncate() to set the pool size after memfd_create().
 * ashmem requires ASHMEM_SET_SIZE ioctl instead.
 */
int ftruncate(int fd, off_t length) {
    if (fd_is_ashmem(fd)) {
        int ret = ioctl(fd, ASHMEM_SET_SIZE, (size_t)length);
        if (ret < 0) {
            fprintf(stderr,
                    "ashmem-shim: ASHMEM_SET_SIZE(%d, %ld) failed: %s\n", fd,
                    (long)length, strerror(errno));
            return -1;
        }
        fprintf(stderr, "ashmem-shim: ftruncate(%d, %ld) -> ASHMEM_SET_SIZE\n",
                fd, (long)length);
        return 0;
    }

    /* Not an ashmem fd - call real ftruncate via syscall */
    return syscall(SYS_ftruncate, fd, length);
}

/* Also intercept ftruncate64 in case glibc routes through it */
int ftruncate64(int fd, off_t length) {
    return ftruncate(fd, length);
}

/*
 * Intercept posix_fallocate - weston's os_create_anonymous_file() uses this
 * instead of ftruncate to size the buffer. fallocate doesn't work on device
 * fds, so we translate to ASHMEM_SET_SIZE for ashmem fds.
 */
int posix_fallocate(int fd, off_t offset, off_t len) {
    if (fd_is_ashmem(fd)) {
        int ret = ioctl(fd, ASHMEM_SET_SIZE, (size_t)(offset + len));
        if (ret < 0) {
            int err = errno;
            fprintf(stderr,
                    "ashmem-shim: posix_fallocate(%d, %ld, %ld) "
                    "-> ASHMEM_SET_SIZE failed: %s\n",
                    fd, (long)offset, (long)len, strerror(err));
            return err;
        }
        fprintf(stderr,
                "ashmem-shim: posix_fallocate(%d, %ld, %ld) "
                "-> ASHMEM_SET_SIZE\n",
                fd, (long)offset, (long)len);
        return 0;
    }

    /* Not ashmem - call real posix_fallocate */
    static int (*real_posix_fallocate)(int, off_t, off_t) = NULL;
    if (!real_posix_fallocate) {
        real_posix_fallocate = dlsym(RTLD_NEXT, "posix_fallocate");
    }
    return real_posix_fallocate(fd, offset, len);
}

int posix_fallocate64(int fd, off_t offset, off_t len) {
    return posix_fallocate(fd, offset, len);
}

/*
 * Intercept fallocate (the raw syscall wrapper) as well.
 */
int fallocate(int fd, int mode, off_t offset, off_t len) {
    if (fd_is_ashmem(fd)) {
        int ret = ioctl(fd, ASHMEM_SET_SIZE, (size_t)(offset + len));
        if (ret < 0) {
            int err = errno;
            fprintf(stderr,
                    "ashmem-shim: fallocate(%d, ..., %ld, %ld) "
                    "-> ASHMEM_SET_SIZE failed: %s\n",
                    fd, (long)offset, (long)len, strerror(err));
            return -1;
        }
        fprintf(stderr,
                "ashmem-shim: fallocate(%d, ..., %ld, %ld) "
                "-> ASHMEM_SET_SIZE\n",
                fd, (long)offset, (long)len);
        return 0;
    }

    return syscall(SYS_fallocate, fd, mode, offset, len);
}

/*
 * Intercept close to clean up our fd tracking.
 */
int close(int fd) {
    fd_clear_ashmem(fd);

    static int (*real_close)(int) = NULL;
    if (!real_close) {
        real_close = dlsym(RTLD_NEXT, "close");
    }
    return real_close(fd);
}
