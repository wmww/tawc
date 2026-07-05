# tawcroot: Full Hardlink Emulation (link2symlink v2)

**Status:** design only, not implemented. The current partial fallback
(below) is believed sufficient for every workload we ship; implement
this only when a real workload hits one of the listed gaps.

## Current state

Android `untrusted_app` SELinux denies `link` on `app_data_file` (see
`notes/proot.md`), so `handle_linkat` falls back to emulation when the
host `linkat` returns EACCES/EPERM (`link_with_symlink_fallback`,
`tawcroot/src/syscalls_fs.c`):

- `renameat2(src → dst, RENAME_NOREPLACE)` moves the real file to the
  new name (NOREPLACE preserves `link()`'s EEXIST), then a
  guest-absolute symlink is created at the old name, rolled back if
  the symlink fails.
- Directory sources stat-checked and passed through as the kernel's
  own EPERM.
- The direction (real file at the NEW name) makes the two idioms that
  pair `link` with a destructive follow-up correct:
  - publish: `link(tmp, final); unlink(tmp)` — git object/pack
    finalize, maildir. The old symlink-at-dst scheme dangled here.
  - backup: `link(f, f.bak); rename(new, f)` — `f.bak` keeps the old
    bytes; the rename clobbers only the leftover symlink.
- Coverage: hosted fault-injection tests (`test_hook_faults.c`,
  `hosted_linkat_fallback_*`) force the EPERM path deterministically;
  device smoke test `test_linkat_publish_then_unlink_source`; verified
  end-to-end via git clone and a full Debian install (dpkg) in-app.

### Known gaps (guest-observable differences from real hardlinks)

1. `lstat`/`fstatat(NOFOLLOW)`/`statx` on the old name reports a
   symlink, not a regular file. Archivers and sync tools walking the
   tree (`tar`, `rsync`, `find -type f`) see and preserve a symlink.
2. `st_nlink` stays 1 and the two names report different inodes on
   NOFOLLOW stats. Lock protocols that `link()` then check
   `st_nlink == 2` break.
3. `unlink(new)` leaves the old name dangling (mirror image of the
   old scheme's publish failure, but for a much rarer pattern —
   link-based lock release is the only known idiom).
4. `open(old, O_NOFOLLOW)` fails ELOOP where a real hardlink opens.
5. `readlink(old)` succeeds where a real hardlink gives EINVAL.
6. Replacing the NEW name via `rename(tmp, new)` makes the old name
   read the new bytes; real hardlinks keep the old bytes at the old
   name. (Replacing the OLD name behaves correctly.)
7. Re-linking a fallback symlink builds symlink chains; correct until
   the kernel's ELOOP walk limit (~40).
8. The symlink target is `"/" + <translated suffix>`, which assumes
   the destination resolved against the rootfs root. A hardlink whose
   destination lands inside a bind mount gets a wrong (rootfs-rooted)
   target. Cross-filesystem binds already fail correctly with EXDEV
   from the rename; same-filesystem binds are the edge.

## Goal

`link`/`linkat` inside the rootfs indistinguishable from real
hardlinks for guest-observable behavior: NOFOLLOW stats report a
regular file with shared inode and correct `st_nlink`, any name can be
unlinked in any order, `O_NOFOLLOW`/`readlink` behave as for a regular
file, and no emulation machinery is visible to the guest.

## Design

proot's `--link2symlink` shape, with explicit refcounts and rollback:

### On-disk scheme

First fallback link for a file `A`:

1. `rename(A → <A-dir>/.l2s.<token>)` — the hidden inode file. Token
   derived from the hidden file's inode number after the rename
   (stable, unique per filesystem, no RNG needed).
2. `symlink(.l2s.<token> → A)` (relative, same directory).
3. `symlink(<path-to-hidden> → B)` — relative when B is in the same
   directory, guest-absolute otherwise.
4. Refcount 2 recorded (see below).

Further `link(X, C)` where `X` resolves to an `.l2s.` symlink: point
`C` at the same hidden file, increment the refcount. No chains.

Refcount storage, in preference order (spike required):

- **xattr** `user.tawc.l2s_nlink` on the hidden file. Atomic with the
  file, survives moves. Must verify `untrusted_app` may set `user.*`
  xattrs on `app_data_file` on all supported Android versions.
- **sidecar** `.l2s.<token>.cnt` next to the hidden file if xattrs are
  denied anywhere. Crash window between data ops and count updates —
  acceptable: drift only delays hidden-file deletion (leak), never
  loses data.

A count that cannot be maintained degrades to "never delete the hidden
file" (bounded leak, data-safe).

### Syscall interception (beyond today's linkat)

- `unlinkat` — leaf is an `.l2s.` symlink: unlink the symlink,
  decrement; on 0 unlink the hidden file. Leaf IS a hidden file
  (guest names it directly): ENOENT (hidden names don't exist for the
  guest).
- `renameat2` — destination clobbers an `.l2s.` symlink: implicit
  unlink, decrement. Source is an `.l2s.` symlink: plain rename of
  the symlink is already correct. `RENAME_EXCHANGE` with either side
  an `.l2s.` symlink: exchange the symlinks, no count change.
- `newfstatat`/`statx` (NOFOLLOW leaf = `.l2s.` symlink) — stat the
  hidden file instead; report `S_IFREG`, the hidden file's `st_ino`
  (shared across all names for free), `st_nlink` from the refcount.
  FOLLOW stats only need the `st_nlink` fix.
- `openat`/`openat2` with `O_NOFOLLOW` — leaf is an `.l2s.` symlink:
  open the hidden file instead of failing ELOOP. `O_NOFOLLOW|O_PATH`
  needs the same treatment for fd-relative follow-ups.
- `readlinkat` — on an `.l2s.` symlink: EINVAL (it's "a regular
  file"). On a guest path naming a hidden file directly: ENOENT.
- `getdents64` — filter `.l2s.*` entries from directory listings.
  tawcroot already has a dirent filter layer (`dirent_filter.c`);
  extend it. Perf: the filter must inspect every entry of every
  readdir in the rootfs — measure; if hot, keep a per-directory "has
  l2s entries" hint cached on first miss.
- `utimensat`/`fchmodat`/`fchownat` with NOFOLLOW semantics on an
  `.l2s.` symlink — apply to the hidden file.

### Crash safety / GC

Multi-step sequences (rename+symlink+count) can be interrupted. Every
intermediate state must be data-safe; the reachable failure states are
(a) hidden file with fewer symlinks than its count — bounded leak,
(b) symlink to a vanished hidden file — surfaces as ENOENT, same as a
half-finished real link. Optional: an install-time or debug-action
sweep that deletes `.l2s.` files with zero referring symlinks in the
same directory (cross-directory referrers make a full sweep unsound;
scope the GC to same-directory references, which covers the publish
and tar cases that dominate).

### Explicitly rejected alternatives

- **Copy-on-link:** breaks shared-inode writes, doubles I/O and peak
  disk for large files (git packs), still lies about `st_nlink`.
- **Returning the raw EPERM:** git survives (falls back to rename)
  but libarchive/pacman hardlink extraction hard-fails — the original
  ALARM motivation in `notes/proot.md`.

## Staging

1. **Spike:** xattr writability under `untrusted_app`; decide count
   store. Half a day.
2. **Core fidelity:** state format + linkat/unlinkat/stat/statx.
   Hosted fault-injection matrix (publish, farm, unlink-any-order,
   st_nlink/st_ino fidelity, chain replacement by shared hidden
   file). This alone closes gaps 1–3, 6, 7.
3. **Edge surface:** O_NOFOLLOW opens, readlink, NOFOLLOW
   chmod/chown/utimens, rename-clobber decrement, RENAME_EXCHANGE.
4. **Invisibility + hygiene:** getdents filtering, GC sweep, perf
   measurement, device integration tests (git clone, `tar` round-trip
   of a hardlinked tree, pacman package with hardlinks, `cp -al`
   farm).

Each stage is shippable; stage 2 is the bulk of the value.

## Trigger criteria

Implement when any of: a package hook or archiver visibly
mis-handles a fallback symlink (gap 1), a lock protocol checking
`st_nlink` appears in a supported workload (gap 2), or a workload
unlinks the destination name while keeping the source (gap 3).
