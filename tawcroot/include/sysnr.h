/* Per-arch syscall numbers we use. Kept in one file so we can grep the set.
 * Bionic ships <sys/syscall.h> for these but we stay libc-free in handler/
 * runtime code; defining them here keeps the surface small and explicit.
 *
 * Linux numbers don't shift between kernel releases for any syscall we care
 * about. The two arches diverge enough that a per-arch table is clearer than
 * a unified one. */

#pragma once

#if defined(__aarch64__)
# define TAWC_SYS_read           63
# define TAWC_SYS_write          64
# define TAWC_SYS_close          57
# define TAWC_SYS_getpid        172
# define TAWC_SYS_getuid        174
# define TAWC_SYS_geteuid       175
# define TAWC_SYS_getgid        176
# define TAWC_SYS_getegid       177
# define TAWC_SYS_exit_group     94
# define TAWC_SYS_rt_sigaction  134
# define TAWC_SYS_rt_sigprocmask 135
# define TAWC_SYS_rt_sigreturn  139
# define TAWC_SYS_prctl         167
# define TAWC_SYS_seccomp       277
# define TAWC_SYS_openat         56
# define TAWC_SYS_readlinkat     78
# define TAWC_SYS_fstatat        79
# define TAWC_SYS_statx         291
# define TAWC_SYS_fcntl          25
# define TAWC_SYS_mmap          222
# define TAWC_SYS_mprotect      226
# define TAWC_SYS_munmap        215
# define TAWC_SYS_getcwd         17
# define TAWC_SYS_execveat      281
# define TAWC_SYS_execve        221
# define TAWC_SYS_memfd_create  279
# define TAWC_SYS_getrandom     278
# define TAWC_SYS_brk           214
# define TAWC_SYS_lseek          62
# define TAWC_SYS_chdir          49
# define TAWC_SYS_fchdir         50
# define TAWC_SYS_faccessat2    439
# define TAWC_SYS_faccessat      48
# define TAWC_SYS_utimensat      88
# define TAWC_SYS_mkdirat        34
# define TAWC_SYS_unlinkat       35
# define TAWC_SYS_symlinkat      36
# define TAWC_SYS_fchmodat       53
# define TAWC_SYS_fchownat       54
# define TAWC_SYS_renameat       38
# define TAWC_SYS_renameat2     276
# define TAWC_SYS_linkat         37
# define TAWC_SYS_truncate       45
# define TAWC_SYS_ftruncate      46
# define TAWC_SYS_openat2       437
# define TAWC_SYS_mknodat        33
# define TAWC_SYS_statfs         43
# define TAWC_SYS_fstatfs        44
# define TAWC_SYS_setxattr        5
# define TAWC_SYS_lsetxattr       6
# define TAWC_SYS_fsetxattr       7
# define TAWC_SYS_getxattr        8
# define TAWC_SYS_lgetxattr       9
# define TAWC_SYS_fgetxattr      10
# define TAWC_SYS_listxattr      11
# define TAWC_SYS_llistxattr     12
# define TAWC_SYS_flistxattr     13
# define TAWC_SYS_removexattr    14
# define TAWC_SYS_lremovexattr   15
# define TAWC_SYS_fremovexattr   16
# define TAWC_SYS_dup            23
# define TAWC_SYS_dup3           24    /* aarch64 has no dup2 */
# define TAWC_SYS_close_range   436
# define TAWC_SYS_process_vm_readv  270
# define TAWC_SYS_process_vm_writev 271
# define TAWC_SYS_gettid           178
# define TAWC_SYS_pread64           67
# define TAWC_SYS_io_uring_setup   425
# define TAWC_SYS_getresuid       148
# define TAWC_SYS_getresgid       150
# define TAWC_SYS_clone3          435
# define TAWC_SYS_clone           220
# define TAWC_SYS_bind            200
# define TAWC_SYS_connect         203
# define TAWC_SYS_accept          202
# define TAWC_SYS_accept4         242
/* Defense-in-depth denials — never honoured, always trap to -EPERM. See
 * issues/tawcroot-phase3-syscall-gaps.md §1. */
# define TAWC_SYS_chroot           51
# define TAWC_SYS_pivot_root       41
# define TAWC_SYS_mount            40
# define TAWC_SYS_umount2          39    /* aarch64 has no legacy umount */
# define TAWC_SYS_unshare          97
# define TAWC_SYS_setns           268
#elif defined(__x86_64__)
# define TAWC_SYS_read            0
# define TAWC_SYS_write           1
# define TAWC_SYS_close           3
# define TAWC_SYS_getpid         39
# define TAWC_SYS_getuid        102
# define TAWC_SYS_geteuid       107
# define TAWC_SYS_getgid        104
# define TAWC_SYS_getegid       108
# define TAWC_SYS_exit_group    231
# define TAWC_SYS_rt_sigaction   13
# define TAWC_SYS_rt_sigprocmask 14
# define TAWC_SYS_rt_sigreturn   15
# define TAWC_SYS_prctl         157
# define TAWC_SYS_seccomp       317
# define TAWC_SYS_openat        257
# define TAWC_SYS_readlinkat    267
# define TAWC_SYS_fstatat       262   /* newfstatat */
# define TAWC_SYS_statx         332
# define TAWC_SYS_fcntl          72
# define TAWC_SYS_mmap            9
# define TAWC_SYS_mprotect       10
# define TAWC_SYS_munmap         11
# define TAWC_SYS_getcwd         79
# define TAWC_SYS_execve         59
# define TAWC_SYS_execveat      322
# define TAWC_SYS_memfd_create  319
# define TAWC_SYS_getrandom     318
# define TAWC_SYS_brk            12
# define TAWC_SYS_lseek           8
# define TAWC_SYS_chdir          80
# define TAWC_SYS_fchdir         81
# define TAWC_SYS_faccessat2    439
# define TAWC_SYS_faccessat     269
# define TAWC_SYS_utimensat     280
# define TAWC_SYS_mkdirat       258
# define TAWC_SYS_unlinkat      263
# define TAWC_SYS_symlinkat     266
# define TAWC_SYS_fchmodat      268
# define TAWC_SYS_fchownat      260
# define TAWC_SYS_renameat      264
# define TAWC_SYS_renameat2     316
# define TAWC_SYS_linkat        265
# define TAWC_SYS_truncate       76
# define TAWC_SYS_ftruncate      77
# define TAWC_SYS_openat2       437
# define TAWC_SYS_mknod         133
# define TAWC_SYS_mknodat       259
# define TAWC_SYS_statfs        137
# define TAWC_SYS_fstatfs       138
# define TAWC_SYS_setxattr      188
# define TAWC_SYS_lsetxattr     189
# define TAWC_SYS_fsetxattr     190
# define TAWC_SYS_getxattr      191
# define TAWC_SYS_lgetxattr     192
# define TAWC_SYS_fgetxattr     193
# define TAWC_SYS_listxattr     194
# define TAWC_SYS_llistxattr    195
# define TAWC_SYS_flistxattr    196
# define TAWC_SYS_removexattr   197
# define TAWC_SYS_lremovexattr  198
# define TAWC_SYS_fremovexattr  199
# define TAWC_SYS_dup            32
# define TAWC_SYS_dup2           33
# define TAWC_SYS_dup3          292
# define TAWC_SYS_close_range   436
/* Legacy syscalls Android's untrusted_app filter RET_TRAPs on x86_64 —
 * we route them through *at variants in the handler. They don't exist
 * on aarch64 (separate numbers / not allocated). */
# define TAWC_SYS_stat            4
# define TAWC_SYS_lstat           6
# define TAWC_SYS_access         21
# define TAWC_SYS_chmod          90
# define TAWC_SYS_chown          92
# define TAWC_SYS_lchown         94
# define TAWC_SYS_mkdir          83
# define TAWC_SYS_rmdir          84
# define TAWC_SYS_unlink         87
# define TAWC_SYS_symlink        88
# define TAWC_SYS_link           86  /* x86_64 link(2); was incorrectly 9 (mmap) */
# define TAWC_SYS_rename         82
# define TAWC_SYS_readlink       89
# define TAWC_SYS_process_vm_readv  310
# define TAWC_SYS_process_vm_writev 311
# define TAWC_SYS_gettid           186
# define TAWC_SYS_pread64           17
# define TAWC_SYS_io_uring_setup   425
# define TAWC_SYS_getresuid       118
# define TAWC_SYS_getresgid       120
# define TAWC_SYS_clone3          435
# define TAWC_SYS_bind             49
# define TAWC_SYS_connect          42
# define TAWC_SYS_accept           43
# define TAWC_SYS_accept4         288
/* Defense-in-depth denials — never honoured, always trap to -EPERM. See
 * issues/tawcroot-phase3-syscall-gaps.md §1. */
# define TAWC_SYS_chroot          161
# define TAWC_SYS_pivot_root      155
# define TAWC_SYS_mount           165
# define TAWC_SYS_umount2         166   /* x86_64 NR 22 (old umount) is unused; glibc routes through umount2 */
# define TAWC_SYS_unshare         272
# define TAWC_SYS_setns           308
#else
# error "unsupported arch"
#endif
