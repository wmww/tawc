# tawcroot: Full Hardlink Emulation

Android `untrusted_app` SELinux denies `link` on `app_data_file` (see
`notes/proot.md`). Today's partial fallback (`link_with_symlink_fallback`,
`tawcroot/src/syscalls_fs.c`) renames the real file to the NEW name and
leaves a guest-absolute symlink at the old name. That direction keeps the
two destructive idioms correct (publish: `link(tmp,final);unlink(tmp)`;
backup: `link(f,f.bak);rename(new,f)`) and survives git and full Debian
installs, but it is guest-observably not a hardlink:

1. NOFOLLOW stats report a symlink, not a regular file (tar/rsync/find
   walk it as one).
2. `st_nlink` stays 1; the names report different inodes on NOFOLLOW
   stats. `link()`-then-`st_nlink==2` lock protocols break.
3. `unlink(new)` dangles the old name.
4. `open(old, O_NOFOLLOW)` fails ELOOP.
5. `readlink(old)` succeeds where hardlinks give EINVAL.
6. `rename(tmp, new)` makes the old name read the new bytes.
7. Re-linking builds symlink chains.
8. Bind-mount destinations get wrong (rootfs-rooted) symlink targets.

## Goal

`link`/`linkat` in the rootfs indistinguishable from real hardlinks for
guest-observable behavior — shared inode, live `st_nlink`, any-order
unlink, regular-file NOFOLLOW/readlink semantics, no visible machinery —
except the deviations accepted below. proot/chroot compatibility of the
resulting rootfs is explicitly NOT a goal; only tawcroot needs to
understand the on-disk state.

## Design overview

A hardlinked file's data lives in a **link object** stored *outside the
guest tree*. Every guest name for it is a symlink whose target is an
**opaque token**, not a path. tawcroot intercepts every consumer of those
symlinks (resolution, readlink, NOFOLLOW stats/opens/attrs), so the guest
sees N regular-file names sharing one inode. A per-store **flock**
serializes mutations; a single-slot **intent file** makes every
multi-syscall mutation crash-recoverable, self-healing, with no
full-tree sweep anywhere.

## On-disk layout

```
distros/<id>/
  rootfs/          guest tree (mount root) — guest files only
  metadata.json    (existing, installer-owned)
  tawcroot/        tawcroot-owned emulation state
    version        store format version (ASCII integer; see below)
    lock           mutation lock (flock; created once, never deleted)
    intent         single-slot crash journal (+ intent.new rename twin)
    link/<token>   link objects (hardlink cluster data)
    tmp/<ino>      linkable O_TMPFILE files awaiting publish
```

Why out-of-tree: the rename primitive needs the object on the same
*filesystem* as the guest file, not inside the guest *tree*, and
`distros/<id>/` is on the same fs as `rootfs/`. Path translation already
confines guest paths to the rootfs fd, so the guest structurally cannot
name anything under `tawcroot/` — which deletes the entire hiding
apparatus an in-tree stash would need (reserved names, getdents
filtering, post-resolution lookup blocks, collision policy). Same-fs
bind sources (normal case: other /data dirs) share this one store; the
mount distinction dissolves because rename works across the whole fs.

`tawcroot/` is generic metadata with link emulation as first customer.
`lock` + `intent` are subsystem-neutral, but they are a **paired
unit**: the single-slot intent is sound only because that one lock
admits one mutation at a time across all intent users, so one
serialization domain = one lock + one intent slot, always together.
Future multi-syscall emulations (mknod, whiteouts) join the existing
pair — everything behind it is cold-path. Single-syscall metadata
updates (e.g. ownership persistence as one per-file xattr write) need
neither lock nor intent and must not take them; that keeps the one
plausible warm-path customer outside the lock by construction. If a
hot multi-syscall customer ever appears, it gets its own lock+intent
pair (a second domain) under the standing rule: never hold two domain
locks at once.

**Invariant:** `rootfs/` alone is no longer self-contained — hardlinked
data lives in `tawcroot/link/`. Backup/export/migration must treat
`distros/<id>/` as the unit (already true of install management; must be
documented in notes/ when this ships).

Token = the file's inode number (known before the rename, which
preserves it; unique per fs among live files). Create objects with
RENAME_NOREPLACE and retry with a suffixed token on EEXIST — a
host-level copy of a distro renumbers inodes, so a fresh token can
collide with a stale baked name; plain rename would silently destroy an
existing object.

## Store versioning

`tawcroot/version` (ASCII integer) is written when the store is first
created and read once at session start, alongside the intent check.
It guards not forward migration (new code can always be taught old
formats) but **old code mutating a newer store** — an APK downgrade or
an imported `distros/<id>/` — blind to invariants it doesn't know
(e.g. a new sidecar that must stay consistent with the object being
deleted). That only works if the check ships in the first release:
"absence means v1, add the file at the first bump" fails because v1
binaries never look.

- version == supported → proceed.
- version < supported → migrate under the store lock; bump the file
  last, so a killed migration re-runs (same recheck-then-act
  discipline as intent recovery).
- version > supported → disable store *mutations* only: link fallback
  degrades to raw EPERM, unknown emulated names surface as dangling
  symlinks. Degraded guest behavior, zero corruption, legible in logs.

One number for the whole store — subsystems are cold and migrations
rare. Formats living outside the store are versioned in-band instead:
symlink targets are self-describing (a future format is a new
`tawcroot:` prefix recognized alongside the old — the v1 path-target
symlinks already work this way), and xattr names can do the same. Most
evolutions therefore never bump the store version; it gates the
store's internal invariants.

## Name symlinks: opaque tokens

Target text: `tawcroot:link:<token>`, exact literal. It never resolves
as a path; the resolver detects the prefix and maps the token to
`(link-dir fd, token)` directly, via store fds captured at init.
Consequences, all by construction:

- **Location-independent.** Renaming a name, its ancestors, or whole
  subtrees never breaks links (the v1-style path targets and proot-style
  relative targets both fail some rename shape).
- **Guest-chroot immune.** `chroot(2)` emulation (`chroot.c`) swaps the
  root view; the store fds and token mapping don't route through it.
  pacman 6.x's chroot hop is a live workload, so this is core, not edge.
- **Fails loudly** outside tawcroot (a path-shaped target that
  half-resolves under some other runtime is worse than one that
  obviously dangles). Acceptable per the non-goal above.

Forgery guard: detection-by-target-text is only sound if guests can
never author it — a forged symlink is a phantom referrer whose unlink
decrements a count it never contributed to (the one route to data
loss). So `symlinkat` rejects guest targets beginning with `tawcroot:`
(EPERM). rename preserves target text so can't mint one; archives can't
contain one because `readlink` on emulated names is intercepted (the
literal never appears in guest-readable output, so legit tars are
self-consistently clean).

`readlink` on an emulated name: EINVAL ("it's a regular file") — except
when the object is itself a symlink (hardlink-of-a-symlink, e.g. from
`rsync -H`): then forward the object's real target, and stats report the
object's real mode rather than hardcoding S_IFREG. The resolver likewise
splices the object's target and continues the guest-side walk when a
FOLLOW hop lands on a symlink object.

## Refcounts

Preferred: xattr `user.tawc.nlink` on the link object — atomic with the
file, survives moves. **Spike required:** verify `untrusted_app` may set
`user.*` xattrs on `app_data_file` (ext4 and f2fs, supported Android
versions). Fallback: sidecar `link/<token>.cnt` (out-of-tree, so no
extra hiding cost).

Rules:
- First link always **sets** the count, never trusts a pre-existing
  value — xattr-copying tools (`cp --preserve=xattr`, `tar --xattrs`)
  can plant stale `user.tawc.*` values on ordinary files.
- Guest xattr surface: filter `user.tawc.*` from `listxattr`/`getxattr`;
  reject guest `setxattr`/`removexattr` on it (handlers all exist).
- A count that cannot be maintained degrades to "never delete the
  object" — bounded leak, data-safe.

## Concurrency: the store lock

tawcroot handlers run in-process per guest thread (no proot-style tracer
serialization), and refcount read-modify-write races across processes
can undercount → premature object deletion → data loss. So: one
`flock(LOCK_EX)` on `tawcroot/lock` around every structural mutation.

Why this exact shape:
- **flock, because the threat model is SIGKILL** (LMK, force-stop) —
  the kernel releases the lock when the holder dies. Shared-memory
  futexes wedge forever; create/delete sentinel files wedge on a stale
  sentinel.
- **Not on the object's own inode** — the guest sees that inode as the
  linked file and may legitimately flock it; flock excludes across fds
  even within one process, so the emulation would self-deadlock against
  the guest's own lock. The lock file is an inode the guest can never
  address.
- **Coarse (per store), because mutations are cold** (dpkg/pacman
  bursts, git gc — never hot loops), critical sections are bounded
  handler code that never returns to guest code while holding, and a
  single lock needs no ordering analysis. Per-cluster locks are the
  escalation if contention ever measures, at the cost of a lock-file
  lifecycle problem; don't start there.
- **Readers are lock-free.** Stats/opens tolerate racing a teardown; an
  ENOENT there is indistinguishable from the unlink serializing first.
  Locks serialize; they don't make sequences atomic — that's the intent
  slot's job.

## Crash safety: the intent slot

No POSIX primitive updates a directory entry and a count atomically, and
a process can die between any two syscalls. Two mechanisms make every
reachable state safe and self-healing, with recovery O(one operation),
never O(tree):

**Monotone ordering invariant:** the stored count is always ≥ live
referrers. Increment *before* a name appears; decrement *after* a name
is gone. Every kill window therefore errs toward a leak (object kept too
long), never a deficit (object deleted while referenced).

**Single-slot intent file.** Because the lock admits one mutation at a
time, and every lock holder repairs any leftover intent before writing
its own, at most one pending intent can ever exist — so the "journal" is
one small file, not a log. Protocol per mutation:

1. `flock(LOCK_EX)`.
2. Read `intent`; non-empty → a holder died mid-operation → run
   recovery, clear the slot.
3. Write own intent (op, token, guest name paths, **count-before** —
   authoritative because the lock excludes concurrent movement) via
   `intent.new` + rename, so the slot is never torn.
4. Execute the operation's syscalls in monotone order.
5. Clear the slot, unlock.

Recovery is **recheck-then-act with absolute values** — it never asks
"did the crashed process reach the decrement?" (unanswerable); it forces
the recorded end state, and is idempotent so recovery itself can be
killed and re-run:

- `NEW tok src dst` — object absent → roll back (remove the dst symlink
  if present); object present → roll forward (create missing name
  symlinks, set count 2).
- `ADD tok name n` — name exists as token symlink → count := n+1; else
  count := n.
- `DEL tok name n` — name still exists → count := n (op never started);
  name gone → count := n−1, and at 0 unlink the object (+sidecar).
- `CLOBBER tok dst n` (rename onto an emulated name) — dst still the
  token symlink → count := n; else count := n−1, at 0 unlink object.

Single-syscall mutations (RENAME_EXCHANGE of name symlinks, O_TMPFILE
publish rename) are atomic in the kernel and need no intent.

Self-healing hooks: recovery runs at every lock acquisition, plus an
O(1) session-start check (read `intent`; non-empty → lock and repair).
No sweep, no user action; the worst interim state is one object with a
count one too high, guest-visible only as a transiently wrong
`st_nlink` until the next mutation or session start heals it.

No fsync: the threat is process death, and the page cache outlives the
process, so the intent write is reliably ordered before the operation.
True power loss can reorder them; the monotone invariant still holds
independently, so the worst case is one permanently leaked object —
accepted (an optional fsync of the intent would close even that).

First-link sequence (under lock + intent): create the dst symlink first
(natural EEXIST semantics before anything is mutated), set count on the
source file, rename source → `link/<token>` (NOREPLACE), create the
src-name symlink. The one-syscall window where the old name is absent is
the accepted residue of emulating an atomic syscall with several.

## Syscall surface

Emulated-name detection is reactive: NOFOLLOW handlers check the leaf
only when it stats as a symlink (+1 readlinkat), so non-symlink hot
paths pay nothing; FOLLOW paths pay nothing at all (the resolver already
readlink-probes every component and just learns one new target form).

- `linkat` — host linkat first; on EACCES/EPERM emulate. Directory
  sources still pass the kernel's EPERM through. Source resolving to an
  emulated name → point the new name at the same object, `ADD` (no
  chains). Source with no name (O_TMPFILE fd via `/proc/self/fd/N` or
  AT_EMPTY_PATH, not one of our `tmp/` objects) → deliberate EXDEV
  (see O_TMPFILE).
- `unlinkat` — leaf emulated → `DEL`.
- `renameat2` — dst clobbers an emulated name → `CLOBBER` + rename.
  Src emulated → plain rename IS correct (opaque targets are
  location-independent). RENAME_EXCHANGE → atomic, no count change.
- `newfstatat`/`statx` — NOFOLLOW leaf emulated → stat the object:
  real mode, shared `st_ino`, `st_nlink` from the count. FOLLOW stats
  landing in the store (result base fd == link-dir fd) → same nlink fix.
- `openat` `O_NOFOLLOW` (incl. `O_PATH`) — leaf emulated → open the
  object instead of ELOOP. O_CREAT|O_EXCL on an emulated name gets
  EEXIST naturally.
- `readlinkat` — EINVAL / forward-if-symlink-object (above).
- `utimensat`/`fchownat`/`faccessat2` with NOFOLLOW — apply to the
  object. (`fchmodat` has no NOFOLLOW flag at the syscall level;
  `fchmodat2` stays ENOSYS.)
- `symlinkat` — forgery guard (above).
- `getdents64` — **no changes**; nothing hidden lives in-tree.

## O_TMPFILE

The publish idiom (`open(O_TMPFILE)` … `linkat` the fd its first name)
is a hardlink of a *nameless* inode: SELinux denies it identically, and
rename-based emulation needs a name. Split on O_EXCL:

- `O_TMPFILE|O_EXCL` ("will never link"): **pure passthrough** — anon
  file creation is allowed (only `link` is denied), fstat nlink 0 is
  exact, and a linkat attempt fails ENOENT in-kernel just like the real
  thing. Zero new code.
- `O_TMPFILE` without O_EXCL (linkable): create `tawcroot/tmp/<ino>`
  and return its fd. Publish linkat (source resolves through
  `/proc/self/fd/N` to a `tmp/` path, or AT_EMPTY_PATH + fstat →
  inode-keyed O(1) lookup) = `renameat2(tmp → dst, NOREPLACE)`: atomic,
  reproduces linkat's EEXIST, and the still-open fd references the same
  inode so post-publish writes land at the destination (which
  copy-based emulation gets wrong). A second link is then a normal
  `ADD`. Never-linked strays are cleaned at session start by listing
  `tmp/` (at session start no guest processes exist, so every entry is
  dead) — O(one directory).

Prevalence check (informs priority): scratch dominates — `tmpfile(3)`
and Python `tempfile` use O_TMPFILE and never link; publish users are
ostree/flatpak, systemd components, casync. git/dpkg/apt/pacman publish
via named temp + rename and never hit this. So until the tmp/ stage
lands, the deliberate-EXDEV fallback for anonymous sources is
low-impact, and denying O_TMPFILE at open (breaking scratch to fix
publish) is firmly rejected.

## Bind mounts and guest chroot

Same-fs binds (the normal case): covered by the one store; gap 8
disappears because nothing is path-addressed. True cross-fs binds: link
within them cannot reach the store (EXDEV on rename) — unsupported
initially, degrading to today's behavior; the documented escalation is a
per-fs in-tree `/.tawcroot` stash, which drags the full hiding apparatus
back for that mount only, so don't build it speculatively. Guest
`chroot(2)`: immune via opaque tokens + init-captured store fds (above).

## Legacy v1 artifacts

Existing rootfses contain v1 fallback symlinks (guest-absolute path
targets). They don't match the token form, so they keep behaving exactly
as today — ordinary symlinks, no migration, no misdetection. New links
on top of them treat them as plain symlink sources.

## Accepted deviations

- `fstat(fd)` / `statx(fd, "", AT_EMPTY_PATH)` report the object's real
  `st_nlink` (1). Fixing it costs an fgetxattr on essentially every
  regular-file fstat (hot); no known consumer compares fd-nlink to
  path-nlink. Revisit only on evidence.
- Non-O_EXCL O_TMPFILE: fstat nlink 1 (not 0); `/proc/self/fd` readlink
  shows a live `tmp/` path (no " (deleted)"); churn-heavy never-linking
  users accumulate invisible temps until session end (best-effort eager
  cleanup via a high-fd close-trap dup is a documented refinement, not
  initial scope).
- One-syscall old-name-missing window during first-link.
- Transiently high `st_nlink` after a crash, until healed.
- Power loss (not process death) can leak one object permanently.
- Anonymous-source linkat without a `tmp/` object: EXDEV.
- Cross-fs binds: no emulation.

## Rejected alternatives

- **Copy-on-link:** breaks shared-inode writes, doubles I/O and peak
  disk (git packs), still lies about `st_nlink`.
- **Raw EPERM:** libarchive/pacman hardlink extraction hard-fails — the
  original motivation (`notes/proot.md`).
- **FUSE / mount tricks:** need privileges `untrusted_app` lacks.
- **proot-style same-directory hidden files** (this plan's previous
  shape, from proot's link2symlink): strands still-referenced hidden
  files in guest directories (`rm -rf` of a `cp -al` source hits
  ENOTEMPTY on an apparently-empty dir), ancestor renames dangle
  cross-dir targets, its same-directory-scoped GC deletes live
  cross-directory data, and hiding scattered names needs per-entry
  getdents filtering everywhere.
- **In-tree stash at the mount root:** fixed the above but still needed
  the full hiding/reservation apparatus; superseded by out-of-tree
  `tawcroot/` once proot/chroot compat was dropped as a goal.
- **Path-shaped symlink targets:** root-view-dependent (break under
  guest chroot — a shipped workload) or depth-dependent (relative);
  opaque tokens are immune to both.
- **Full-sweep GC:** O(tree), needs quiescence, and scoped variants are
  unsound; replaced by the intent slot (O(1), self-healing).
- **Coordinator daemon / central metadata DB:** the fs must stay the
  source of truth (guest processes die mid-op, so any index drifts and
  must be rebuilt from disk — the daemon is just a cache with a
  lifecycle, an availability dependency, and fakeroot/pseudo's
  db-corruption fragility). Serialization, the only other thing it
  offers, is one flock.

## Staging

1. **Spike (½ day):** `user.*` xattr writability under `untrusted_app`
   on app data (ext4 + f2fs); confirm flock and O_TMPFILE-passthrough
   assumptions on device. Decide count store.
2. **Core:** `tawcroot/` layout + store fds at init, version file +
   refuse-if-newer check, lock, intent slot + recovery, opaque-token
   detection in the resolver,
   linkat/unlinkat/renameat2/fstatat/statx, symlinkat forgery guard,
   `user.tawc.*` xattr filtering. Hosted tests: fault-injection fidelity
   matrix (publish, backup, farm, any-order unlink, st_ino/st_nlink,
   rename-over) plus a **kill-matrix harness** — run every mutation,
   kill after syscall k for every k, run recovery, assert invariants.
   Closes gaps 1–3 and 6–8; the bulk of the value.
3. **Edge surface:** O_NOFOLLOW/O_PATH opens, readlink semantics,
   NOFOLLOW utimens/chown/faccessat2, CLOBBER + RENAME_EXCHANGE,
   hardlink-of-symlink, deliberate EXDEV for anonymous linkat sources.
4. **O_TMPFILE:** `tmp/` objects, publish rename, session-start sweep.
5. **Integration + hygiene:** session-start recovery hook wiring; device
   tests (git clone, tar round-trip of a hardlinked tree, pacman package
   with hardlinks, `cp -al` farm, hardlinks inside a guest chroot);
   cross-process race stress test (hosted kill-matrix can't exercise
   real races); perf spot-check (NOFOLLOW stats on symlink-heavy trees);
   document the `distros/<id>/`-is-the-unit invariant and the store
   format in notes/.

Each stage is shippable.
