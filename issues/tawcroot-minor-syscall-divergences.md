# tawcroot: minor syscall divergences from kernel semantics (grab-bag, remaining)

Collected small divergences from the 2026-06 review pass. The
field-relevant ones were fixed (chown existence/fd validation,
getdents64 mid-directory EOF, /proc shadow CLOEXEC, unlink/rmdir/linkat
errno shapes). The rest are documented so they don't have to be
re-discovered.

Remaining:

- **Reserved-fd "behaves as EBADF" contract is partial** (fdtab.h):
  `openat(1000, "etc/shadow", …)` with a reserved dirfd passes the
  dirfd through; `fstatat(1000, "", AT_EMPTY_PATH)`/`statx` stat the
  reserved fd. (`close`/`dup`/`fcntl`/`readlinkat-empty`/`fchdir`/
  `fstat`/`fchown` now check `tawcroot_fd_is_reserved`.)
- **execveat(AT_SYMLINK_NOFOLLOW)** → ENOSYS placeholder (documented
  in syscalls_exec.c; fine until something uses it).
- **Signal-shadow staleness across fork**: a forked child inherits
  the parent's tid-keyed `blocked` table (COW); kernel tid reuse in
  the child can read a stale "SIGSYS blocked" bit until that thread's
  first rt_sigprocmask. Bounded to one wrong shadow-mask read.
- **Path scratch pool holds while acquiring**: one handler chain can
  hold 3–5 of the 128 slots simultaneously; acquire spins forever on
  exhaustion, so enough concurrent mid-chain threads could in
  principle livelock. Theoretical at 128 slots / realistic thread
  counts.
- **Trailing-slash semantics are erased by the fold**: `lstat("/l/")`
  should follow `l`, `open("/file/")` should ENOTDIR, `unlink("/d/")`
  should fail — the fold strips the slash and nothing downstream
  re-attaches the "must be a directory" requirement.
- **Deep paths**: the fold caps at 256 components (ENAMETOOLONG);
  kernel accepts ~2048 single-char components in 4096 bytes.

## Fixed (2026-06)

- chown family: fchownat now does a translate + fstatat existence
  probe before faking 0 (missing path → ENOENT); fchown validates the
  fd (bad fd → EBADF, reserved fd → EBADF).
- getdents64 no longer fakes EOF when a single batch contains only
  reserved-fd entries — it re-issues until a non-empty filtered batch
  or true kernel EOF.
- /proc shadow memfds clear FD_CLOEXEC when the guest opened without
  O_CLOEXEC, so the fd survives the guest's next exec.
- unlink("/") → EISDIR, rmdir("/") → EBUSY, linkat empty operand →
  ENOENT (were all the catch-all EINVAL).
