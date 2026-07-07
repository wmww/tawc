# tawcroot: legacy x86_64 getdents (NR 78) bypasses the getdents64 fixups

`handle_getdents64` exists to (a) hide reserved fds from
`/proc/self/fd` listings — the fix for the glibc closefrom livelock
(pacman-key at 100% CPU) — and (b) rewrite DT_LNK→DT_UNKNOWN for
hardlink-emulation token names so type-trusting walkers stat into the
fixed-up handlers. Legacy x86_64 `getdents(2)` (NR 78) is not in
sysnr.h and not trapped, so any caller issuing the legacy NR gets
raw kernel dirents: reserved fds visible again, emulated hardlinks
listed as DT_LNK (they then read as symlinks to `tawcroot:link:<tok>`
targets or vanish from find/rg-style walks).

Exposure is x86_64-emulator-only (aarch64 never had the NR) and only
for legacy callers — glibc/musl use getdents64 — so this is a
consistency gap, not a burning bug. A handler would need to emulate
via getdents64 into scratch and repack records into the legacy
`linux_dirent` layout (d_type lives in the LAST byte of each record
there), reusing the same reserved-fd/token logic.

Related watch-item (unverified, needs the emulator): the x86_64
legacy trap set was grown empirically (open/stat/poll/epoll_wait/
dup2/...). If Android's x86_64 app seccomp filter RET_TRAPs other
legacy NRs we don't register (candidates: select 23, epoll_create
213, inotify_init 253, signalfd, eventfd), our SIGSYS handler answers
-ENOSYS; callers without an ENOSYS fallback break. Checking
`SYSCALLS.TXT`/`SECCOMP_*.TXT` in bionic for the emulator image, or
just probing the NRs in the device suite, would settle the list.
