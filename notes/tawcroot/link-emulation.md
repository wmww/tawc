# Hardlink emulation (the link store)

Android `untrusted_app` SELinux denies `link(2)` on `app_data_file` —
both the `file` and `lnk_file` classes (verified on the enforcing
emulator, Android 16 / kernel 6.6). tawcroot emulates hardlinks fully:
shared inode, live `st_nlink`, any-order unlink, regular-file
NOFOLLOW/readlink semantics. This note records the shipped design,
format, and behavior (distilled from the completed — and since
deleted — plans/tawcroot-full-link-emulation.md).
Code: `src/linkstore.c`, `include/linkstore.h`,
handler integration in `src/syscalls_fs.c` (linkat/unlinkat/renameat2/
stats/openat/readlinkat/symlinkat/utimensat/fchownat) and
`src/syscalls_fd.c` (getdents64 d_type rewrite).

## On-disk format (store version 1)

```
distros/<id>/
  rootfs/           guest tree (mount root) — guest files only
  tawcroot/         tawcroot-owned emulation state ("the store")
    version         store format version, ASCII integer
    lock            fcntl-record-lock file (created once, never deleted)
    intent          single-slot crash journal (+ intent.new rename twin)
    link/<token>    link objects (hardlink cluster data; may be symlinks
                    — hardlink-of-a-symlink stores the symlink itself)
    link/<token>.cnt  refcount sidecars (fixed-width "NNNNNNNNNN\n",
                    one pwrite, the only count store)
    work/<token>.<role>  transient staged/parked entries; roles
                    .add/.del/.src/.dst per mutation type
    tmp/<ino>       linkable O_TMPFILE files awaiting publish
```

Every guest name of a hardlink cluster is a symlink whose target is the
opaque literal `tawcroot:link:<token>`. Token = object inode in decimal
(`-<k>` suffix on collision after host-level copies renumber inodes).
The resolver detects the prefix and re-bases at the store's `link/` fd;
`symlinkat` refuses guest targets starting with `tawcroot:` (forgery
guard — a forged token symlink would be a phantom referrer). The store
is discovered ONLY at top-level prod entry (`<rootfs>/../tawcroot`) and
ferried to `--exec-child` via exec_state (v5 `store_host_off`), because
a guest chroot changes the rootfs view but never the store.

**Backup/export invariant: `distros/<id>/` is the unit.** `rootfs/`
alone is no longer self-contained — hardlinked data lives in
`tawcroot/link/`. Install management already treats the distro dir as
the unit; anything new (export, migration, host-side copying) must too.
Corollary: **bind sources must be distro-exclusive** — tokens name
objects in one distro's store, so a host dir bound into two distros
would dangle in the other, and deleting distro A would destroy data a
shared dir still references.

Version gate: `version` is read at configure and re-read at every lock
acquisition (guests outlive sessions). Newer-than-supported → DEGRADED:
token *detection* stays on, all store *mutations* refuse (link falls
back to raw EPERM), zero corruption. v1 fallback symlinks from the
pre-store emulation (guest-absolute path targets) don't match the token
form and keep behaving as plain symlinks; no migration.

## Concurrency + crash safety (summary)

- One whole-file `fcntl(F_SETLKW)` lock (kernel-released on SIGKILL;
  fork/exec escape it in the safe direction) + an in-process futex
  mutex, around every structural mutation. Readers are lock-free.
- Monotone invariant: stored count ≥ live referrers — increment before
  a name appears, decrement after it's gone. Crash windows leak
  (overcount), never lose data.
- Single-slot intent journal (host-real paths), written via rename;
  recovery keys on store-internal state only (`work/`, `link/`,
  intent), verifies any guest entry is the matching token symlink
  before touching it, never recreates names from recorded paths, and is
  idempotent (recovery can be killed and re-run). Runs at every lock
  acquisition plus an O(1) session-start check. CLOBBER (rename onto an
  emulated name) is deliberately intent-free; sidecar writes are single
  fixed-width pwrites so lock-free readers and SIGKILL can't tear them.
- Missing/garbled sidecar: report `st_nlink` 2, never delete the
  object (degraded, data-safe).
- Kill-matrix coverage: `tests/hosted/test_linkstore_kill.c` kills every
  mutation before every k-th syscall, re-runs recovery (also killed at
  every j), and interleaves lock-free guest renames.

## O_TMPFILE

- `O_TMPFILE|O_EXCL` ("will never link"): pure passthrough — anon file
  creation is allowed, fstat nlink 0 exact, publish attempts fail
  in-kernel (ENOENT) like the real thing.
- `O_TMPFILE` without O_EXCL: the file is created at `tmp/<ino>` (a
  letter-prefixed unique name renamed to the inode decimal) and its fd
  returned. Publish linkat — either `linkat(fd, "", dst, AT_EMPTY_PATH)`
  or `linkat(AT_FDCWD, "/proc/self/fd/N", dst, AT_SYMLINK_FOLLOW)` —
  detects the tmp-resident source by host path and becomes one atomic
  `renameat2(NOREPLACE)` (linkat's EEXIST for free; the still-open fd
  keeps referencing the inode at its new home). No store → passthrough
  (scratch works; publish degrades to EXDEV).
- Stray cleanup: `tawcroot_linkstore_tmp_sweep()` at TOP-LEVEL session
  start only, age-gated at 7 days — the session model does not
  guarantee guest teardown (init-reparented exec-broker descendants
  survive; concurrent sessions exist; spike-verified), so eager
  sweeping would unlink a live guest's temp under it.

## linkat source detection (ordering matters)

1. Emulated-name sources are detected BEFORE the host attempt
   (symlinks are class `lnk_file`; a host policy that allows linking
   them would hardlink the token symlink itself — phantom referrer).
2. Store-resident sources are store-aware, mandatory backstop for every
   spelling: the resolver catches token names in the rootfs view;
   FOLLOW-mode sources are additionally O_PATH-opened and their
   `/proc/self/fd` host path prefix-compared against `<store>/link/`
   (→ ADD, token parsed from the path — works for `-k` suffixed
   tokens) and `<store>/tmp/` (→ publish). This covers
   `/proc/self/fd/N` spellings, which route through the /proc bind and
   skip the resolver. AT_EMPTY_PATH sources resolve the fd's host path
   the same way BEFORE the host attempt. NEW must never rename a
   store-resident source — it would rename the object itself out of
   `link/`, dangling the whole cluster.
3. Otherwise host linkat first; EACCES/EPERM engages emulation (plus
   ENOENT for AT_EMPTY_PATH — the kernel's unprivileged
   CAP_DAC_READ_SEARCH refusal). Directory sources keep the kernel's
   EPERM. Named in-view AT_EMPTY_PATH sources → NEW via
   `(AT_FDCWD, host_path)` after a dev/ino re-verify; nameless sources
   (memfd, O_EXCL temps, fully unlinked) → deliberate EXDEV.
4. Hardlink-of-a-symlink: NEW stores the symlink itself as the object.
   The resolver splices a symlink object's target back into the guest
   walk (`readlink_store` oracle member) — relative targets resolve
   against each NAME's parent dir, absolute targets through the guest
   view, exactly like real hardlinked symlinks. NOFOLLOW stats report
   the object's real S_IFLNK mode; readlink forwards the object's
   target; plain O_NOFOLLOW opens still ELOOP.

## Accepted deviations (beyond plans/, observed in the field)

- fd-based stats (fstat / AT_EMPTY_PATH) of an open object DO report
  the sidecar nlink — the plan first accepted kernel nlink 1 there,
  but GNU tar CREATE registers hardlinks from the fstat of the fd it
  opens, which silently dissolved hardlink structure at archive time
  (found in the stage-5 tar round-trip). Cost gate: only
  S_ISREG/S_ISLNK fds with kernel nlink 1 while a store is open pay
  one `/proc/self/fd` readlink.
- Non-O_EXCL O_TMPFILE: fstat nlink 1 (not 0); `/proc/self/fd` shows a
  live `tmp/` path (no " (deleted)").
- Emulated names are never planted inside binds: the resolver
  deliberately skips bind-routed paths, so a token symlink there would
  be unresolvable on FOLLOW opens (data marooned). linkat degrades
  instead — NEW with either operand on a bind takes the v1 fallback
  (both names real/openable), ADD with a bind destination returns
  EXDEV (tools fall back to copy). Legacy token names inside binds
  (from before this gate) still kernel-chase the literal → ENOENT;
  their d_type is rewritten and NOFOLLOW stats fixed up like rootfs
  names.
- A process started before the store existed (LATENT) opens it the
  first time it MEETS a token: FOLLOW walks upgrade via the resolver,
  linkat upgrades before source detection, mutations via `store_lock`.
  The remaining gap is NOFOLLOW-only surfaces (lstat of an emulated
  name in a stale-LATENT process reports a symlink until any of the
  above fires).
- Dangling token names (object lost: partial host copy, crashed-NEW
  window) behave as plain symlinks on NOFOLLOW surfaces — visible and
  unlinkable for cleanup — and ENOENT on FOLLOW/publish surfaces.
- `d_ino` in directory entries is the name symlink's inode; `d_type`
  for EVERY symlink in rootfs-view dirs is rewritten DT_LNK→DT_UNKNOWN
  (forces type-trusting walkers to stat into the fixed-up handlers).
- fd-path introspection (`readlink /proc/self/fd/N`, `/proc/self/maps`)
  of emulated hardlinks leaks raw store paths (the store is outside the
  reverse-translation prefixes). `/proc/self/exe` is unaffected.
- Cross-fs binds: no emulation (v1 fallback).
- One-syscall old-name-missing window during first link; crash windows
  leave counts high (never low); power loss can leak one object.

## Testing

`tawcroot/test.sh --host 'linkstore.*'` (fidelity matrix + kill matrix,
also in the device suite). Handlers self-degrade with no store
configured — testhost/hosted default to OFF so pre-store tests keep
exercising the v1 fallback; linkstore tests opt in via
`tawcroot_linkstore_configure`.

E2E-verified on the emulator (Debian sid rootfs via the exec broker):
ln semantics incl. hardlinked symlinks, `cp -al` farm + `rm -rf`
source, tar round-trip of a hardlinked tree (both directions preserve
structure), git local clone (hardlinked objects) + gc + fsck, apt/dpkg
package installs, hardlink inside a guest `chroot(2)`, 6-process
link/unlink race stress (store clean after: no intent, no `work/`
strands, clusters reclaimed), O_TMPFILE publish via the magic-link
spelling with post-publish writes. Perf spot-check (emulator): `find
/usr/lib -type f` over 3772 entries — every entry lstat'd due to the
DT_UNKNOWN rewrite — 34 ms (~9 µs/entry incl. traps); not
pathological.
