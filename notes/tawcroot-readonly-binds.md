# Read-only fake bindmounts in tawcroot (design notes, not implemented)

Tawcroot's bind table is read/write only — `tawcroot_path_add_bind`
opens the src as `O_PATH | O_DIRECTORY`, the kernel only uses that fd
as the starting inode for path resolution, and write-mode `openat`s
through the bind succeed. Anything inside the rootfs can write
through to the host-side bind src, which is shared host state for
binds like `/data/data/me.phie.tawc/share`.

We've worked around this for the moment by **copying** APK-shipped
files into each rootfs (`TawcInstaller`) instead of binding them — see
`notes/installation.md` "Why copy, not bind". That keeps libhybris and
the glvnd vendor JSON per-rootfs-owned. The remaining shared-write
exposure is just the wayland socket dir + Xwayland's xtmp dir under
`<appData>/share/`, surfaced as `/usr/share/tawc/`.

This note records what an actual RO-bind primitive would look like
inside tawcroot if/when we want stronger enforcement (e.g. so we can
go back to binds for libhybris and not pay the per-install copy cost
plus the per-app-update wipe-and-recopy churn).

## Why kernel-level RO doesn't fall out for free

Two paths the kernel offers don't help us:

1. **Open the bind src `O_RDONLY` instead of `O_PATH`.** Doesn't
   work. The src fd is used as the `dirfd` argument to `openat(dirfd,
   suffix, flags)` for resolving each guest path; the kernel uses
   `dirfd` only to provide the starting inode for path resolution —
   the new fd's access mode comes from the openat *flags*, not from
   how `dirfd` itself was opened. Same reason `openat(O_PATH dirfd,
   "x", O_RDWR)` returns a writable fd even though `O_PATH` is
   otherwise unreadable/unwritable.
2. **Real `mount --bind -o ro` semantics.** Available, but requires
   `CLONE_NEWUSER | CLONE_NEWNS` to gain unprivileged
   `CAP_SYS_ADMIN` inside the namespace, then doing actual mount(2)
   syscalls. That's a much heavier mechanism than tawcroot's
   path-translation model — at that point we're closer to chroot
   than tawcroot. Worth keeping in the back pocket for a future
   "stronger isolation mode" but not the natural extension of the
   existing design.

## What an actual RO bind would look like

A new bool on `struct tawcroot_bind` (`int read_only`) plus per-syscall
enforcement. Two pieces:

### Per-syscall path-bearing-write blocks

For each path-translating syscall handler that resolves through a
bind: after the bind match, check `bind.read_only` and the syscall's
intent. Refuse with `EROFS` (POSIX standard for "tried to write a
read-only filesystem") for:

- `openat` with `O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND`
- `creat`
- `unlink` / `unlinkat`
- `mkdir` / `mkdirat`
- `rmdir` (via `unlinkat(AT_REMOVEDIR)`)
- `rename` / `renameat` / `renameat2` (target side; source side
  needs to refuse too because rename-out-of-RO is a remove)
- `link` / `linkat` (target side)
- `symlink` / `symlinkat` (target side)
- `mknod` / `mknodat`
- `chmod` / `fchmodat`
- `chown` / `lchown` / `fchownat`
- `truncate`
- `utime` / `utimes` / `utimensat` / `futimesat`
- `setxattr` / `lsetxattr` / `removexattr` / `lremovexattr`
- `openat2` — check `RESOLVE_*` flags + open flags

That's ~20 syscalls. Most already pass through `path_orchestrate.c`
so the check is "right after the bind match, look at `args->flags`".

### Fd taint table

A bind-resolved openat returns an fd. Subsequent fd-based syscalls
(`fchmod`, `ftruncate`, `fchown`, `fsetxattr`) operate on that fd
without revisiting the path layer — and a rootfs process that opens a
RO-bound path with `O_RDONLY` can then `fchmod` itself to writable,
bypassing the path-layer block entirely.

To plug this: track which fds were opened through a RO bind. A simple
approach is a fixed-size array indexed by fd (handler-private,
process-local because tawcroot already runs per-tracee), with each
slot a single bit "RO-tainted." Set on bind-resolved `openat`/`openat2`
return; clear on `close`/`close_range`/`dup3`-replace; check in
`fchmod`/`ftruncate`/`fchown`/`fsetxattr`/`fremovexattr` and refuse
with `EROFS`.

Size: kernel `RLIMIT_NOFILE` defaults around 1024 on Android, processes
that bump it stay well under 64K. A 64K-bit (8 KB) bitmap is plenty
and avoids dynamic allocation in the handler.

`dup`/`dup2`/`dup3` propagate the taint from src to dst. `fcntl(F_DUPFD)`
likewise. `execve` resets the table for non-CLOEXEC fds (kernel keeps
them; CLOEXEC fds get closed naturally and we'd see the implied
`close`).

### Things that don't need work

- `mmap(PROT_WRITE, MAP_SHARED)` on a RO-tainted fd: already fails
  in the kernel because the kernel checks the fd's access mode (which
  was `O_RDONLY` since we refused the write-mode openat). Same
  rationale for `write(fd, ...)`.
- New file creation: `O_CREAT` is on the openat block list above;
  `linkat`/`renameat`/`mknodat` cover the rest.
- Hardlinks pointing into a RO subtree: linkat target side check
  (already on the list).
- Reading: explicitly fine. RO bind is allowed-read, refused-write.

### Where this would slot in

`tawcroot/src/handler.c` has the dispatch table. The check would be
a single inline at the top of each affected handler, after path
resolution returns the matching bind. ~5 lines per handler, plus
the fd taint table in a new `tawcroot/src/fd_taint.c`. Total surface
maybe 300-500 LOC.

Risk: missing a syscall that mutates state through a tainted fd. The
mitigation is that the kernel itself checks the fd's open mode for
most write paths; a missed check leaks a write capability but only
through paths that bypass the kernel's own mode check, which is a
small set.

## Why we're not doing it now

- The copy-based approach in `TawcInstaller` solves the immediate
  shared-state problem cleanly. Adds disk cost (~12 MB per rootfs)
  and a one-time copy on APK upgrade, but neither is hot-path.
- RO-bind effort is meaningful (~few hundred LOC, plus careful
  fd-taint review) for a benefit we don't actively need yet.
- Worth revisiting if (a) we end up copying significantly more per
  rootfs and the disk cost matters, or (b) we want to share other
  state types we'd rather not duplicate (large-asset blobs, etc.).
