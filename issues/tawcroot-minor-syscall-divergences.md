# tawcroot: minor syscall divergences from kernel semantics (grab-bag, remaining)

Collected small divergences from the 2026-06 review pass. The
field-relevant ones were fixed (chown existence/fd validation,
getdents64 mid-directory EOF, /proc shadow CLOEXEC, unlink/rmdir/linkat
errno shapes). The rest are documented so they don't have to be
re-discovered.

Remaining:

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
- **`/proc/self/cwd` readlink leaks the host path**: `getcwd` is
  reverse-translated but the cwd symlink is not, so
  `readlink("/proc/self/cwd")` returns e.g.
  `/data/.../rootfs/root` verbatim. notes/tawcroot.md §"`/proc`
  self-magic" calls for synthesizing `cwd` (and `root`) like `exe`;
  only `exe` is implemented. (`root -> /` happens to be correct since
  we never kernel-chroot, though it stays `/` after emulated chroot.)
- **Cross-process `/proc/<pid>/exe` shows the reader's exe**: the
  readlink substitution matches on the tawcroot binary's host path,
  so reading *another* tawcroot guest process's exe link returns the
  *calling* process's guest exe path (observed: `ls` reading bash's
  `/proc/<pid>/exe` got `/usr/bin/ls`).
- **`..` after a symlink component resolves lexically**: the fold
  collapses `..` before the resolver can ask whether the preceding
  component is a symlink, so `/a/sym/../x` (sym -> /b/c) hits `/a/x`
  where the kernel would hit `/b/x`. Demonstrated on the host build.
  Same structural cause as the (now fixed) trailing-slash erasure:
  the lexical fold runs first and the resolver only re-folds after
  splices. Note the lexical collapse is what makes `..`-escape
  containment trivially auditable (post-fold suffixes contain no
  `..`), so fixing fidelity here means the resolver must handle `..`
  mid-walk with containment re-checked per step — a fold/resolver
  rework, not a patch (unlike the trailing-slash marker, which only
  concerned the final component).
- **Deep paths**: the fold caps at 256 components (ENAMETOOLONG);
  kernel accepts ~2048 single-char components in 4096 bytes.

## Fixed (2026-06)

- Trailing-slash semantics: translate now detects trailing `/` runs
  and `/.` components on the raw guest string (the kernel's
  "leftover bytes after the final component" rule, erased by the
  fold) and re-attaches them: leaf symlinks resolve even under
  NOFOLLOW (through the resolver, so absolute targets stay clamped)
  and one `/` is re-appended to the final suffix so the kernel
  enforces the directory requirement with native errno shapes —
  `lstat("l/")` follows, `open("file/")` ENOTDIR, parent ops keep
  the leaf verbatim (`unlink("l/")` ENOTDIR, `mkdir("l/")` EEXIST),
  `open("x/", O_CREAT)` EISDIR. `ls -la /proc/self/` lists the
  directory again. See has_trailing_dir_marker in
  path_orchestrate.c; unit tests orch_trailing_slash_*, smoke
  rootfs_smoke.c::test_trailing_slash_semantics.
- Reserved-fd "behaves as EBADF" contract: a reserved dirfd with a
  relative path now EBADFs in the shared translate front door
  (translate_local), covering every *at handler at once; the
  fstatat/statx AT_EMPTY_PATH operate-on-dirfd shapes check too.
  Absolute paths still pass (kernel ignores dirfd there). Smoke:
  rootfs_smoke.c::test_reserved_dirfd_ebadf.
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
