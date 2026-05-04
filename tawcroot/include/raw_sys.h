/* Tiny libc-free wrappers around tawcroot_raw_syscall(). Used everywhere in
 * the handler hot path and in --exec-child bootstrap (where we can't yet call
 * libc because the inherited filter is live but the handler isn't installed).
 *
 * Naming: tawc_<syscall>(...). Returns the raw syscall return — negative
 * values are -errno, NOT errno-set-and-return-(-1). The handler reads them
 * directly without consulting any libc errno TLS slot. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sysnr.h"

extern long tawcroot_raw_syscall(long nr, long a, long b, long c,
				 long d, long e, long f);

#define TAWC_RAW(nr, ...) tawcroot_raw_syscall((nr), ##__VA_ARGS__)

static inline long tawc_write(int fd, const void *buf, size_t n)
{ return TAWC_RAW(TAWC_SYS_write, fd, (long)buf, (long)n, 0, 0, 0); }

static inline long tawc_exit_group(int status)
{ return TAWC_RAW(TAWC_SYS_exit_group, status, 0, 0, 0, 0, 0); }

static inline long tawc_getpid(void)
{ return TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0); }

static inline long tawc_getuid(void)
{ return TAWC_RAW(TAWC_SYS_getuid, 0, 0, 0, 0, 0, 0); }

static inline long tawc_prctl(int op, long a, long b, long c, long d)
{ return TAWC_RAW(TAWC_SYS_prctl, op, a, b, c, d, 0); }

static inline long tawc_seccomp(unsigned int op, unsigned int flags,
				const void *args)
{ return TAWC_RAW(TAWC_SYS_seccomp, op, flags, (long)args, 0, 0, 0); }

static inline long tawc_rt_sigaction(int sig, const void *act, void *old,
				     size_t sigsetsize)
{ return TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)act, (long)old,
		  (long)sigsetsize, 0, 0); }

static inline long tawc_read(int fd, void *buf, size_t n)
{ return TAWC_RAW(TAWC_SYS_read, fd, (long)buf, (long)n, 0, 0, 0); }

static inline long tawc_close(int fd)
{ return TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0); }

static inline long tawc_lseek(int fd, long off, int whence)
{ return TAWC_RAW(TAWC_SYS_lseek, fd, off, whence, 0, 0, 0); }

static inline long tawc_fcntl(int fd, int op, long arg)
{ return TAWC_RAW(TAWC_SYS_fcntl, fd, op, arg, 0, 0, 0); }

static inline long tawc_memfd_create(const char *name, unsigned int flags)
{ return TAWC_RAW(TAWC_SYS_memfd_create, (long)name, flags, 0, 0, 0, 0); }

static inline long tawc_execveat(int dirfd, const char *path,
				 char *const argv[], char *const envp[],
				 int flags)
{ return TAWC_RAW(TAWC_SYS_execveat, dirfd, (long)path, (long)argv,
		  (long)envp, flags, 0); }

static inline long tawc_openat(int dirfd, const char *path, int flags, int mode)
{ return TAWC_RAW(TAWC_SYS_openat, dirfd, (long)path, flags, mode, 0, 0); }

static inline long tawc_readlinkat(int dirfd, const char *path,
				   char *buf, size_t n)
{ return TAWC_RAW(TAWC_SYS_readlinkat, dirfd, (long)path, (long)buf,
		  (long)n, 0, 0); }

static inline long tawc_getdents64(int fd, void *buf, size_t n)
{ return TAWC_RAW(TAWC_SYS_getdents64, fd, (long)buf, (long)n, 0, 0, 0); }

/* openat2 (kernel ≥ 5.6). Same shape as openat but takes a struct
 * open_how instead of bare flags+mode, and supports `resolve` flags
 * including RESOLVE_IN_ROOT — the kernel's chroot-equivalent path
 * resolution mode. We use it for generic non-final-component symlink
 * resolution: under RESOLVE_IN_ROOT, absolute symlink targets
 * encountered during walk are reinterpreted relative to dirfd, and
 * `..` past dirfd stays at dirfd. Probe at init via
 * tawcroot_openat2_works; older kernels stay on the string-fold +
 * well-known-memo path. */
struct tawc_open_how {
	uint64_t flags;
	uint64_t mode;
	uint64_t resolve;
};

#define TAWC_RESOLVE_BENEATH      0x08
#define TAWC_RESOLVE_IN_ROOT      0x10
#define TAWC_RESOLVE_NO_SYMLINKS  0x04

static inline long tawc_openat2(int dirfd, const char *path,
				const struct tawc_open_how *how, size_t size)
{ return TAWC_RAW(TAWC_SYS_openat2, dirfd, (long)path, (long)how,
		  (long)size, 0, 0); }

/* mmap / mprotect / munmap / pread / getrandom — used by the manual
 * ELF loader (loader_io_prod.c) and elsewhere as needed. The loader
 * doesn't include raw_sys.h directly; it goes through a vtable so it
 * stays unit-testable from the cleat orchestrator. */
static inline long tawc_mmap(void *addr, size_t len, int prot, int flags,
                             int fd, long offset)
{ return TAWC_RAW(TAWC_SYS_mmap, (long)addr, (long)len, prot, flags, fd, offset); }

static inline long tawc_mprotect(void *addr, size_t len, int prot)
{ return TAWC_RAW(TAWC_SYS_mprotect, (long)addr, (long)len, prot, 0, 0, 0); }

static inline long tawc_munmap(void *addr, size_t len)
{ return TAWC_RAW(TAWC_SYS_munmap, (long)addr, (long)len, 0, 0, 0, 0); }

static inline long tawc_pread64(int fd, void *buf, size_t n, long off)
{ return TAWC_RAW(TAWC_SYS_pread64, fd, (long)buf, (long)n, off, 0, 0); }

static inline long tawc_getrandom(void *buf, size_t n, unsigned flags)
{ return TAWC_RAW(TAWC_SYS_getrandom, (long)buf, (long)n, flags, 0, 0, 0); }
