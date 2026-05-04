# tawcroot: `/proc/self/{exe,maps}` synthesis misses fd-relative + `/proc/<tid>` paths

The two `/proc/self`-synthesizing handlers (`handle_readlinkat` for
`/proc/self/exe` and `handle_openat` for `/proc/self/maps`) both go
through `strip_proc_self_prefix(path)` in `tawcroot/src/syscalls_fs.c`,
which requires:

- `path[0] == '/'` (absolute), AND
- second component is literally `self` or a decimal pid that equals
  `getpid()` (TGID).

So these guest forms are **not** caught:

- `openat(proc_dir_fd, "self/maps", ...)` — guest opens `/proc` first
  and uses the dirfd. Some sandboxes do this.
- `openat(AT_FDCWD, "/proc/self/task/<tid>/maps", ...)` — used by some
  per-thread crash dumpers.
- `openat(AT_FDCWD, "/proc/<our-tid>/maps", ...)` where `<our-tid>` is
  a non-main thread's TID. Per-task data differs but `/maps` is
  per-mm and identical content. Some debuggers iterate by TID.

In all three the kernel returns the real (host-pathed) maps file or
the libtawcroot exe-link, and we silently miss the rewrite/synthesis.

Same caveat as the existing handlers: the missed cases are uncommon
and the failure mode is "guest sees host paths" not "crash". Today's
workloads (Firefox, pacman, bash) all canonicalize through libc, which
emits the literal absolute form we already catch.

## Fix sketch

The fd-relative variant can be detected without a full fd-provenance
table: at the top of `handle_openat` and `handle_readlinkat`, if
`dirfd >= 0` and the guest path is relative, resolve `dirfd` via
`/proc/self/fd/<n>` (cheap raw `readlinkat`), join with the guest path,
and re-run `strip_proc_self_prefix`. Cost is one extra readlinkat per
non-AT_FDCWD relative call; only matters when the path matches `/proc`.

The `/proc/self/task/<tid>/maps` and `/proc/<tid>/maps` forms can be
caught by extending `strip_proc_self_prefix`:

- After matching `/proc/<digits>/`, accept either `getpid()` (TGID) or
  any tid in our process. Cheap check: `/proc/<n>/status` `Tgid:` line
  must equal `getpid()`. Cache results across handler invocations —
  TID-set changes infrequently relative to maps reads.
- After matching `/proc/self/`, also accept `task/<digits>/<x>` as a
  prefix (apply the same TGID check on `<digits>`).

Best done together with the fd-provenance work tracked in
`tawcroot-fd-provenance-not-tracked.md` since both want a per-fd
shadow with kernel-resolved metadata.

## Severity

Low. No current workload hits this; the bug is "subtle silent host-path
leak" not a correctness break. Worth fixing when fd-provenance lands.
