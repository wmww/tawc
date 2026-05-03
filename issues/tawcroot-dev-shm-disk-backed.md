# tawcroot's `/dev/shm` is disk-backed; should be `memfd`-based via in-handler emulation

`TawcrootMethod` binds `<filesDir>/tawcroot-dev-shm` (real on-disk
app-private storage) at `/dev/shm` so Firefox's `shm_open(3)` calls
succeed. This was added 2026-05-02 as the fix for the firefox
parent-process SEGV (`MOZ_RELEASE_ASSERT(mHandle.IsValid() &&
mMapping.IsValid())` from `SharedMemory::Allocate`/`Map`); see
`issues/tawcroot-firefox-segfault.md`.

It's the same workaround `ProotMethod` has carried for years (see
`issues/proot-dev-shm-disk-backed.md`) and it has the same three
problems:

1. **Every SHM write hits flash.** Firefox's parent-to-content IPC
   SHM is hot. Writing it through ext4 instead of tmpfs is wasted
   IO and flash wear.
2. **Storage growth.** Files accumulate in `tawcroot-dev-shm/`
   across crashes (Firefox cleans up its segments on graceful
   exit, not on SIGSEGV/SIGKILL/OOM-kill). No mechanism prunes
   them.
3. **`filesDir` (not `cacheDir`) means it's not even reclaimable
   under storage pressure** — chosen to avoid SIGBUS on live
   mappings if Android purges the backing file mid-session, but
   the side effect is unbounded growth.

The chroot path doesn't have this problem because chroot runs as
real uid 0 with write access to the host's devtmpfs, so glibc's
`shm_open` just creates `/dev/shm/<name>` there (devtmpfs is a
real in-kernel tmpfs).

## What the original tawcroot design called for

`notes/tawcroot.md` "Bootstrap & entry" used to claim:

> `/dev/shm` is handled in-handler: `shm_open` and any path-bearing
> syscall under `/dev/shm/<name>` is emulated via `memfd_create` (or
> an equivalent anon-fd primitive) and a small in-process name table,
> so guest code sees POSIX shm semantics without any host directory
> backing it. No `tawcroot-dev-shm` directory, no `-b …:/dev/shm`
> entry, and the installer's `TawcrootMethod` must not create or
> reference such a path.

That section now reflects the as-built behavior (disk-backed bind).
This issue tracks getting back to the original design.

## What the in-handler emulation looks like

Concretely:

- A small process-wide name table mapping `<name> → memfd`. Lives
  in `path.c` (or a new `shm.c`) alongside the bind table; same
  lock-free snapshot-on-update discipline as the bind table for
  vfork/multithread safety.
- Special-case in `handle_openat`/`handle_open` for paths starting
  with `/dev/shm/`: strip the prefix, look up name in the table.
  - If `O_CREAT` and not present: `memfd_create(name, MFD_CLOEXEC
    | MFD_ALLOW_SEALING)`, store fd-clone in the table, return a
    dup'd fd to the guest. Honor `O_EXCL` on conflict.
  - If present: return a dup'd fd from the table entry. Honor
    `O_TRUNC` by `ftruncate(fd, 0)`.
  - The table holds an O_PATH-equivalent ref so `unlink` can
    correctly delete-by-name without affecting still-open fds
    (matches POSIX shm semantics — the segment lives until the
    last fd closes).
- Special-case in `handle_unlinkat`/`handle_unlink` for
  `/dev/shm/<name>`: drop from the name table, close the table's
  ref. Return 0 (or -ENOENT if not present).
- Special-case in `handle_newfstatat`/`handle_statx`/`handle_access`
  / etc. for `/dev/shm/<name>`: synthesize a sensible stat (mode
  0600, owner uid 0 to match fake-root, size from `fstat(memfd)`).

`memfd_create(2)` is in the trap set already (or rather, it's
not — passes through to the kernel from raw_sys callers; see
`raw_sys.h::tawc_memfd_create` used by `exec_handler.c`). The
emulation handler issues `memfd_create` via `TAWC_RAW` so the IP
allowlist accepts it.

## What this lets us delete

Once the in-handler emulation lands:

- `TawcrootMethod.devShmDir` field
- The `mkdirs(devShmDir)` call in `runInside`
- `-b "$devShmDir:/dev/shm"` in `tawcrootArgv`
- `DEV_SHM_DIR=...` and the `-b "$DEV_SHM_DIR:/dev/shm"` lines in
  `enter.sh`
- The `<filesDir>/tawcroot-dev-shm` directory itself (uninstaller
  doesn't currently know about it; would need to clean it up)

## Compatibility / kernel requirements

- `memfd_create(2)` — kernel ≥ 3.17. Universal on Android
  versions we target.
- Already used by tawcroot internally for the exec_state memfd
  (`exec_handler.c::tawcroot_exec_handler_perform`); no new
  dependency.
- Allowed by Android's `untrusted_app` / `runas_app` seccomp
  filters — verified by the existing exec_handler use.

## Open questions

- **Cross-process visibility.** Linux POSIX shm uses `/dev/shm`
  as a name namespace shared across processes. The memfd path
  needs the parent and child to see the same fd-by-name. The
  --exec-child re-exec dance already passes the fd table via
  exec_state — we'd need to ferry the shm name table the same
  way (or, simpler, make name-table entries inheritable across
  fork by keeping the memfd fds non-CLOEXEC in the parent and
  re-enrolling them in --exec-child). Mozilla parent → content
  IPC actively relies on this.
- **`fcntl(F_ADD_SEALS)` semantics.** Mozilla SharedMemory
  sets seals on its segments. `MFD_ALLOW_SEALING` covers it but
  we should verify guest seal calls work as expected (these
  syscalls aren't trapped today; `fcntl` is — would need to
  check).
- **Path canonicalization.** A guest opening
  `/dev/shm/foo` vs `/proc/self/cwd/../../dev/shm/foo` should
  hit the same name. The translator already canonicalizes
  paths before the bind/translate decision; the shm hook would
  sit after that.

## When to do this

Not urgent — disk-backed works for Firefox + the integration
suite. Worth doing when:

- We notice meaningful flash-write volume from Firefox SHM in
  production logs (similar threshold as the proot version).
- We want to be the first rootless Linux-on-Android stack with a
  real tmpfs-equivalent `/dev/shm`. Symbolic value as much as
  practical — proot/Termux ecosystems have been carrying the
  disk-backed bind for years.
- Storage quota grows uncomfortably on long-running installs.

## References

- `server/app/src/main/java/me/phie/tawc/install/TawcrootMethod.kt`
  — the current disk-backed bind setup (`devShmDir`,
  `tawcrootArgv`, `renderEnterScript`).
- `issues/tawcroot-firefox-segfault.md` — the bug that forced
  adding the bind (firefox parent SEGV on `shm_open` ENOENT).
- `issues/proot-dev-shm-disk-backed.md` — the proot-side equivalent
  of this issue, with broader survey of options (Termux's
  `libandroid-shmem`, ashmem, etc.). Most of that survey applies
  unchanged here; tawcroot just has a cleaner intercept point
  (the SIGSYS handler) than proot's seccomp-rewrite gymnastics.
- `notes/tawcroot.md` "Bootstrap & entry" — the section that
  describes the (current) disk-backed bind and the (planned)
  in-handler emulation.
