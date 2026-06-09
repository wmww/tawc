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
# define TAWC_SYS_getppid       173
# define TAWC_SYS_getuid        174
# define TAWC_SYS_geteuid       175
# define TAWC_SYS_getgid        176
# define TAWC_SYS_getegid       177
# define TAWC_SYS_exit           93
# define TAWC_SYS_exit_group     94
# define TAWC_SYS_rt_sigaction  134
# define TAWC_SYS_rt_sigprocmask 135
# define TAWC_SYS_rt_sigreturn  139
# define TAWC_SYS_prctl         167
# define TAWC_SYS_seccomp       277
# define TAWC_SYS_openat         56
# define TAWC_SYS_readlinkat     78
# define TAWC_SYS_fstatat        79
# define TAWC_SYS_fstat          80
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
# define TAWC_SYS_fchown         55
# define TAWC_SYS_fchownat       54
# define TAWC_SYS_renameat       38
# define TAWC_SYS_renameat2     276
# define TAWC_SYS_linkat         37
# define TAWC_SYS_truncate       45
# define TAWC_SYS_ftruncate      46
# define TAWC_SYS_openat2       437
# define TAWC_SYS_fchmodat2     452
# define TAWC_SYS_inotify_init1      26
# define TAWC_SYS_inotify_add_watch  27
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
# define TAWC_SYS_getdents64     61
# define TAWC_SYS_ioctl          29
# define TAWC_SYS_process_vm_readv  270
# define TAWC_SYS_process_vm_writev 271
# define TAWC_SYS_gettid           178
# define TAWC_SYS_pread64           67
# define TAWC_SYS_io_uring_setup   425
# define TAWC_SYS_io_uring_enter   426
# define TAWC_SYS_io_uring_register 427
# define TAWC_SYS_getresuid       148
# define TAWC_SYS_getresgid       150
/* set*id family — faked to success under fake-root (identity.c).
 * There is no seteuid/setegid syscall; libc routes through setres*id. */
# define TAWC_SYS_setregid        143
# define TAWC_SYS_setgid          144
# define TAWC_SYS_setreuid        145
# define TAWC_SYS_setuid          146
# define TAWC_SYS_setresuid       147
# define TAWC_SYS_setresgid       149
# define TAWC_SYS_setfsuid        151
# define TAWC_SYS_setfsgid        152
# define TAWC_SYS_setgroups       159
# define TAWC_SYS_clone3          435
# define TAWC_SYS_clone           220
# define TAWC_SYS_bind            200
# define TAWC_SYS_connect         203
# define TAWC_SYS_accept          202
# define TAWC_SYS_accept4         242
/* chroot has its own handler in src/chroot.c that swaps the rootfs
 * view bookkeeping. The other five trap to -EPERM (defense-in-depth;
 * see fake_eperm in src/syscalls_control.c). */
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
# define TAWC_SYS_getppid       110
# define TAWC_SYS_getuid        102
# define TAWC_SYS_geteuid       107
# define TAWC_SYS_getgid        104
# define TAWC_SYS_getegid       108
# define TAWC_SYS_exit           60
# define TAWC_SYS_exit_group    231
# define TAWC_SYS_rt_sigaction   13
# define TAWC_SYS_rt_sigprocmask 14
# define TAWC_SYS_rt_sigreturn   15
# define TAWC_SYS_prctl         157
# define TAWC_SYS_seccomp       317
# define TAWC_SYS_openat        257
# define TAWC_SYS_readlinkat    267
# define TAWC_SYS_fstatat       262   /* newfstatat */
# define TAWC_SYS_fstat           5
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
# define TAWC_SYS_fchown         93
# define TAWC_SYS_fchownat      260
# define TAWC_SYS_renameat      264
# define TAWC_SYS_renameat2     316
# define TAWC_SYS_linkat        265
# define TAWC_SYS_truncate       76
# define TAWC_SYS_ftruncate      77
# define TAWC_SYS_openat2       437
# define TAWC_SYS_fchmodat2     452
# define TAWC_SYS_inotify_init1     294
# define TAWC_SYS_inotify_add_watch 254
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
# define TAWC_SYS_getdents64    217
# define TAWC_SYS_ioctl          16
/* Legacy syscalls Android's untrusted_app filter RET_TRAPs on x86_64 —
 * we route them through *at variants in the handler. They don't exist
 * on aarch64 (separate numbers / not allocated). */
# define TAWC_SYS_open            2
# define TAWC_SYS_creat          85
# define TAWC_SYS_stat            4
# define TAWC_SYS_lstat           6
# define TAWC_SYS_access         21
# define TAWC_SYS_utime         132
# define TAWC_SYS_utimes        235
# define TAWC_SYS_futimesat     261
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
# define TAWC_SYS_io_uring_enter   426
# define TAWC_SYS_io_uring_register 427
# define TAWC_SYS_getresuid       118
# define TAWC_SYS_getresgid       120
/* set*id family — faked to success under fake-root (identity.c). */
# define TAWC_SYS_setuid          105
# define TAWC_SYS_setgid          106
# define TAWC_SYS_setreuid        113
# define TAWC_SYS_setregid        114
# define TAWC_SYS_setgroups       116
# define TAWC_SYS_setresuid       117
# define TAWC_SYS_setresgid       119
# define TAWC_SYS_setfsuid        122
# define TAWC_SYS_setfsgid        123
# define TAWC_SYS_clone3          435
# define TAWC_SYS_bind             49
# define TAWC_SYS_connect          42
/* accept→accept4, poll→ppoll, epoll_wait→epoll_pwait: Android's
 * untrusted_app filter RET_TRAPs the legacy variant on x86_64 in favour
 * of the modern one. We route the legacy through the modern from the
 * stub. aarch64 has no legacy syscall in any of these pairs (numbers
 * unallocated). */
# define TAWC_SYS_accept           43
# define TAWC_SYS_accept4         288
# define TAWC_SYS_poll              7
# define TAWC_SYS_ppoll           271
# define TAWC_SYS_epoll_wait      232
# define TAWC_SYS_epoll_pwait     281
/* chroot has its own handler in src/chroot.c that swaps the rootfs
 * view bookkeeping. The other five trap to -EPERM (defense-in-depth;
 * see fake_eperm in src/syscalls_control.c). */
# define TAWC_SYS_chroot          161
# define TAWC_SYS_pivot_root      155
# define TAWC_SYS_mount           165
# define TAWC_SYS_umount2         166   /* x86_64 has no legacy umount; glibc routes through umount2 */
# define TAWC_SYS_unshare         272
# define TAWC_SYS_setns           308
#else
# error "unsupported arch"
#endif
