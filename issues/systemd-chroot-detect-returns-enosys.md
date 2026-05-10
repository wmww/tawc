# systemd's chroot-detection returns ENOSYS in pacman post-transaction hooks

After the chroot fix, pacman post-transaction hooks that ask "is the
current root booted?" return:

    Failed to check for chroot() environment: Function not implemented
      Skipped: Current root is not booted.

…in `systemd-hook` invocations for `systemctl daemon-reload`,
`udevadm`, `sysctl --system`, and similar. The hook handles the
ENOSYS gracefully and skips itself, so this isn't fatal — but it's
the wrong reason to skip. systemd should detect chroot via stat /
inode comparison and the hook should run (and then no-op because
systemd isn't PID 1 — that's a separate but legitimate skip).

## Root cause

**Unknown — original diagnosis was wrong.** This file previously
claimed *"tawcroot doesn't have a statx handler installed (verify
against the trap list)"*. That's not true: `tawcroot/src/syscalls_fs.c`
defines `handle_statx` at line 1343 and registers
`tawcroot_dispatch_install(TAWC_SYS_statx, handle_statx)` at line 2031.
On aarch64 it forwards to NR 291; on x86_64 NR 332.

systemd's `running_in_chroot_or_offline()` calls `read_one_line_file
("/proc/1/sched")` plus a stat compare of `/proc/1/root` and `/`. None
of these are obvious -ENOSYS sources on a 5.4+ kernel through tawcroot's
current handler set:

- `openat`/`read` for `/proc/1/sched` — handled, would return -EACCES
  or -ENOENT on failure, not -ENOSYS.
- `stat` (lp64-x86_64) → `handle_stat` → forwarded as `fstatat` — handled.
- `statx` (modern path) → `handle_statx` — handled.

Other candidates that aren't trapped by tawcroot but might be on
systemd's path:

- `name_to_handle_at` / `open_by_handle_at` — not in our dispatch
  table. If systemd uses these for chroot detection they'd RET_ALLOW
  to the kernel, and the kernel handles them (5.4+). Unlikely to
  produce ENOSYS but unverified.
- `statmount(2)` / `listmount(2)` — kernel 6.8+, RET_ALLOWed by us,
  -ENOSYS on a 5.4 kernel.

## Repro

    bash scripts/uninstall-distro.sh manjaro
    bash scripts/install-distro.sh manjaro tawcroot \
        mirrorProxy=http://127.0.0.1:8080/proxy/ distro=manjaro

Search the log for `chroot() environment: Function not implemented`.

Next debug step: `strace -f -e trace=!read,write` the systemd-hook
invocation under tawcroot and find which syscall actually returns
-38. Strongly suspect a kernel-version mismatch (kernel 5.4 vs.
systemd built against 6.8+ ABI) rather than tawcroot's handler set.

## Severity

Low — the hooks gracefully skip and the install reports
`[stage:DONE] Installed`. But the error noise is misleading and
hides real bugs in hooks that *do* care.
