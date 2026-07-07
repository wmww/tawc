# tawcroot Landlock — kernel-enforced path containment

Add optional [Landlock](https://docs.kernel.org/userspace-api/landlock.html)
self-restriction to tawcroot so that path containment stops depending
on resolver correctness. Landlock is an unprivileged LSM (kernel 5.13+)
that lets a process irrevocably restrict its own filesystem access to a
set of path hierarchies. tawcroot wraps itself in a ruleset scoped to
{rootfs + bind sources + a small internal support set}; the kernel then
enforces containment on the *resolved inode*, so a resolver escape that
today lets tawcroot issue an `openat` outside the rootfs becomes a plain
`EACCES` instead.

**Status: plan, not started.** Gated on kernel ≥ 5.13; the primary
device (OnePlus 9, kernel 5.4) does not support it, so this is
probe-and-enable-if-present, same shape as the `PR_SET_SYSCALL_USER_DISPATCH`
future-work item in notes/tawcroot/status.md. It ships doing nothing on 5.4 and
lights up on newer devices / the host test machine.

## Why this is the right lever

- **It moves enforcement into the kernel.** Today the only thing keeping
  a guest inside the rootfs is the in-handler resolver's `..`-clamp and
  symlink re-rooting (notes/tawcroot/path-translation.md §"Translation rules"). That code
  has two known escape corners (lexical `..` after a symlink component,
  cross-operand rename) and any future bug is a silent escape, because
  tawcroot is *not* `chroot`ing — the host fs is right there. Landlock
  converts "trust my C" into "trust the kernel": an escaped path resolves
  to an inode outside every granted hierarchy → `EACCES`.
- **It binds the whole process, not just the interception path.** Landlock
  restricts *every* syscall tawcroot's process issues, including raw ones
  from guest code that has neutered the handler (closed the rootfs fd,
  mmap'd over the stub, etc.). So for the specific class "guest opens a
  host path outside the rootfs," Landlock is a real second wall even
  against a guest that defeats wall 1. It does **not** turn tawcroot into
  a general sandbox (memory-level attacks, ptrace, `/proc/self/mem`, and
  non-fs syscalls are all still open — see §"What it does not cover").
- **It fits the architecture.** Like the seccomp filter, a Landlock
  ruleset is (a) installable unprivileged under `PR_SET_NO_NEW_PRIVS`,
  (b) inherited across `fork`/`execve`, and (c) monotonic — you can only
  ever *add* restriction. So it installs **once** at top-level init and
  needs no re-application on the `--exec-child` re-exec, exactly like the
  filter (notes/tawcroot/sigsys-handler.md §"Why non-PIE", notes/tawcroot/seccomp-filter.md §"Filter installation").

## Availability & gating

Probe with `landlock_create_ruleset(NULL, 0,
LANDLOCK_CREATE_RULESET_VERSION)`, which returns the supported ABI
version (≥1) or `-ENOSYS` on pre-5.13 kernels / kernels built without
Landlock. On any error, skip silently and run exactly as today. On the
5.4 device this is one extra syscall returning `-ENOSYS` at init and
nothing else changes.

**Must-verify before relying on it (same discipline as the raw-syscall
smoke in notes/tawcroot/sigsys-handler.md §"Issuing host syscalls"):** on a real
Android ≥ 5.13 device, confirm Android's own zygote/`untrusted_app`
seccomp allowlist does not `KILL`/`TRAP` the three landlock syscalls
(444/445/446). They are new enough that an older allowlist may not
include them. Install Landlock *before* our own seccomp filter, while
only Android's filter is active — same position `seccomp()` itself runs
in today — so if Android permits `seccomp()` it most likely permits
these too, but this is not guaranteed and must be tested, not assumed.
If Android blocks them, Landlock is simply unavailable on that
device/OS and we fall back to today's behavior.

## The allow-set

Landlock enforces on the **host** filesystem namespace (there is no
chroot). So the ruleset must grant every host path tawcroot legitimately
touches. Three sources, all already known at init:

1. **The rootfs** — `tawcroot_rootfs_host_path` (canonicalized in
   `supervisor_init` step 2). The guest's entire world. Grant full
   read/write/exec + all dir ops.
2. **Every bind source** — `tawcroot_binds[i].src` for `active` binds
   (canonical host paths, already stored). This is the whole point:
   binds are the enumerated set of host locations outside the rootfs the
   guest is *meant* to reach (`/system`, `/vendor`, `/dev`, `/proc`,
   `/sys`, app-data passthroughs, the per-distro ando dir, …). Granting
   exactly the bind sources means the allow-set and the reachable-set are
   the same object. Grant full read/write/exec + dir ops (some binds are
   read-only — `tawcroot_binds[i].read_only`, landed; see §"Interaction
   with read-only binds").
3. **tawcroot-internal support paths** — a small fixed list of host paths
   tawcroot itself opens that are *not* necessarily binds: chiefly host
   `/proc` (the handler reads `/proc/self/fd/<n>` for reverse-translation,
   `/proc/self/exe`, proc-shadow synthesis inputs). `/proc` is usually
   already a bind, but tawcroot must not depend on that — grant host
   `/proc` read/exec explicitly. Audit `supervisor_init` and the fs
   handlers for any other host open (the libtawcroot.so path used for
   `/proc/self/exe` is a *readlink target*, not opened; memfds and the
   exec-state fd are anonymous and outside Landlock's scope).

### Access-rights mask — do NOT over-handle

Landlock only restricts the access categories named in the ruleset's
`handled_access_fs` bitmask; anything not handled passes through
unrestricted. **We deliberately handle only the filesystem read / write
/ exec / directory-management rights** and leave the following UNhandled
so nothing GPU/IPC-shaped breaks:

- **`LANDLOCK_ACCESS_FS_IOCTL_DEV`** (ABI 5+) — NOT handled. GPU
  passthrough (`ioctl` on `/dev/kgsl-3d0`, binder, gfxstream) must stay
  unrestricted; those go straight to the host kernel by design
  (notes/tawcroot/overview.md §"What it explicitly is not"). Not naming this right
  leaves all device ioctls unrestricted.
- **`LANDLOCK_ACCESS_NET_*`** (ABI 4+) — NOT handled. This is a
  filesystem-containment feature only; leave sockets alone.
- **AF_UNIX connect** — current Landlock does not gate connecting to a
  filesystem socket, so wayland/kumquat/ando keep working. (Their
  directories are granted anyway as bind sources.)

The handled mask must then be **clamped to what the running ABI version
supports**, or `landlock_create_ruleset` fails with `-EINVAL`. The fs
right set grew over versions (v1: the 13 base rights; v2 adds
`REFER` for cross-dir rename/link; v3 adds `TRUNCATE`; later versions add
ioctl/net which we don't handle). Requesting a bit the kernel doesn't
know is an error. So: build the desired mask, then AND it with the
version-appropriate supported mask. This clamp is the one piece of real
logic and is a **pure function** — extract it and unit-test it under
cleat (`abi_version → handled_fs_mask`), per the maintenance contract.

Grant `REFER` on the hierarchies when ABI ≥ 2 so cross-directory
`rename`/`link` inside the rootfs keeps working (without it, ABI-2+
kernels deny all cross-dir refer even within a granted tree). On ABI 1,
`REFER` doesn't exist and cross-dir rename is unrestricted anyway.

## Integration point

Install in `prod_rootfs_init` (main.c), in the top-level prod path only,
sequenced:

```
tawcroot_supervisor_init(&sa);      // opens rootfs_fd, builds bind table
tawcroot_set_no_new_privs();        // already present; required by Landlock
tawcroot_landlock_apply();          // NEW — probe, build ruleset, restrict_self
tawcroot_install_filter(...);       // existing seccomp install
```

- **After `supervisor_init`** because it needs `tawcroot_rootfs_fd` and
  the materialized `tawcroot_binds[]` (their `src_fd` O_PATH handles are
  exactly the `parent_fd`s `landlock_add_rule` wants — no re-opening).
- **After `NO_NEW_PRIVS`** because `landlock_restrict_self` requires it
  for an unprivileged caller.
- **Before the seccomp filter** so the landlock syscalls run under only
  Android's filter (see §"Must-verify"). Also keeps our own filter from
  having to allowlist them.
- **NOT in `supervisor_init`, NOT on `--exec-child`.** Like the seccomp
  filter and `NO_NEW_PRIVS`, the ruleset is inherited kernel state; the
  child re-exec must not (and need not) re-apply it. Putting it in
  `supervisor_init` would wrongly re-run it every guest exec.

`tawcroot_landlock_apply()` is a no-op returning success when the probe
says unsupported or when disabled by config (§"Config"). A hard failure
*after* the probe says supported (e.g. `restrict_self` errors) should
be fatal (`tawc_exit_group`) — we asked for containment and didn't get
it; failing open would be a silent security downgrade. A clear exit code
in the 9x band (say 96) like the neighbors.

### chroot interaction — none, by construction

Emulated `chroot(2)` (notes/tawcroot/path-translation.md §"chroot emulation") only ever
*narrows* the guest's view: it re-anchors binds within the existing
rootfs and swaps the current-root prefix. It never needs to reach a host
path outside the original rootfs+bind set. Since Landlock already grants
that whole set and rules can't be relaxed, emulated chroot stays a strict
subset of what's allowed. No conflict, no re-application. Add a test that
a post-chroot guest still operates normally under Landlock.

## Syscalls / uapi to add

All three landlock syscall numbers are arch-generic and identical on
aarch64 and x86_64 (they live in the post-5.11 shared block):

- `sysnr.h`: `landlock_create_ruleset` **444**, `landlock_add_rule`
  **445**, `landlock_restrict_self` **446** (same value both arches —
  unlike the older split numbers this file already carries).
- `tawc_uapi.h`: add `struct landlock_ruleset_attr { __u64
  handled_access_fs; __u64 handled_access_net; /* only if we ever handle
  net */ }`, `struct landlock_path_beneath_attr { __u64 allowed_access;
  __s32 parent_fd; } __attribute__((packed))`, the
  `LANDLOCK_ACCESS_FS_*` right constants, `LANDLOCK_CREATE_RULESET_VERSION`,
  and `LANDLOCK_RULE_PATH_BENEATH`.
- Calls go through `TAWC_RAW(...)` like every other raw syscall (bionic
  has no wrappers, same as `seccomp`). The `add_rule` per-hierarchy loop
  opens nothing new: reuse `tawcroot_rootfs_fd` and each bind `src_fd`;
  for the internal support set (`/proc`) open a transient `O_PATH` fd,
  add the rule, close it — these are pre-filter, pre-restrict, so no
  reserved-range juggling needed.

## New files

- `tawcroot/src/landlock.c`, `tawcroot/include/landlock.h` — prod C.
  Exposes `tawcroot_landlock_apply(const struct tawcroot_landlock_cfg *)`
  (or reads globals, matching the supervisor style). Freestanding, raw
  syscalls only. Small.
- The ABI→mask clamp helper is pure (no syscalls): put it somewhere
  cleat-testable (e.g. a `landlock_mask.c`/inline in strings-style unit
  scope, or guard the pure fn so the unit test can link it), and add
  `tawcroot/tests/unit/` coverage. Everything touching the kernel stays
  in `landlock.c` and is exercised by handler/integration tests.

## Config / opt-out

- CLI flag on the **initial** invocation, e.g. `--landlock=auto|off`
  (default `auto` = enable iff probe supports). Plumbed like `-r` through
  `prod_rootfs_init`; it configures the one-shot top-level apply and is
  inherited, so it never needs to travel in the exec-state fd and never
  violates the "`--exec-child` doesn't read config from env" rule.
- **Do not** source this from an env var read by `--exec-child`
  (design rule, notes/tawcroot/architecture.md §"Environment rule"). A top-level CLI
  flag is the correct channel.
- **Kotlin (future, optional):** a per-distro "extra hardening" toggle,
  or just always-`auto`, passed via the install action args like the
  ando toggle (plans/ando-toggle.md). MVP can hardcode `auto` in the
  spawn command and add the UI later — decide when a device that
  actually supports Landlock is in the test loop. Note the escape hatch
  exists (`--landlock=off`) so a workload broken by an over-tight
  allow-set can be unblocked without a rebuild.

## Interaction with read-only binds

Read-only binds **landed first** (`-b SRC:DST:ro`, enforced at the
translation layer — notes/tawcroot/path-translation.md §"Read-only
binds"). Landlock can enforce that *for real*: thread each bind's
`tawcroot_binds[i].read_only` flag into the rule's `allowed_access`,
granting a read-only bind source only the read/exec rights and
omitting the write/create/delete rights. That turns the emulation
into kernel enforcement and closes the RO plan's documented residues
(in-process attacks, `/proc/<pid>/root` escapes, resolver escape
bugs). The RO-root case (`tawcroot_root_ro`, set by chroot into an RO
bind) needs the same treatment on the rootfs grant.

## What it does NOT cover (honesty)

Landlock closes the *filesystem path-escape* hole. It is not a general
sandbox and must not be described as making tawcroot one:

- **Memory-level monitor defeat still works.** A hostile guest can read
  tawcroot's memory via `/proc/self/mem`, `process_vm_writev` on its own
  tid, or mmap over the handler. Landlock is filesystem-only; it doesn't
  stop any of that. `/proc/self/mem` is under the granted `/proc` and is
  reachable. So "wall 1" (monitor shares the guest's address space) is
  unchanged — see the sandbox discussion in the design notes.
- **Non-filesystem syscalls are untouched** — ptrace, network (unless we
  later opt into net rights), ioctls (intentionally, for GPU), signals.
- **`rename`/`link` semantics** need `REFER` handled to keep working on
  ABI 2+, and cross-hierarchy refer between two *different* granted trees
  is still denied by Landlock (matching mount-boundary semantics). Should
  be fine — the rootfs is one tree — but watch for a workload that renames
  across a bind boundary.
- **Resolver bugs are contained, not fixed.** The lexical-`..` and
  cross-rename corners in notes/tawcroot/status.md "Known gaps" still produce
  *wrong* (non-kernel-faithful) results inside the allowed tree; Landlock
  only guarantees they can't produce an *escape*. Keep hardening/fuzzing
  the resolver regardless.

Net: this specifically hardens the ando socket-as-identity guarantee and
every other property that rests on rootfs containment (a resolver escape
can no longer reach another distro's on-disk state), without claiming to
be a boundary against a guest that attacks tawcroot in memory.

## Tests

Per the maintenance contract (notes/tawcroot/status.md §"Maintenance contract"):

- **Unit (cleat):** the pure `abi_version → handled_fs_mask` clamp —
  each ABI version maps to the expected supported-rights subset; unknown
  future version clamps to the highest we know.
- **Handler / integration (host):** the host dev machine runs a modern
  kernel (Landlock-capable), so unlike most device-only features these
  can actually exercise the enforced path:
  - with Landlock active, an escape attempt that the resolver *would*
    let through (construct or simulate the known `/a/sym/../x` case, or a
    deliberately-crafted path against a fixture) returns `EACCES`, not a
    host inode;
  - normal in-rootfs and in-bind operations (read, write, create,
    delete, exec-via-mmap, cross-dir rename) still succeed;
  - a post-emulated-`chroot` guest still operates normally;
  - ando/wayland-style AF_UNIX connect through a bound socket dir still
    works;
  - a device `ioctl` path is unrestricted (guard against accidentally
    handling `IOCTL_DEV`).
  - A/B the suite with `--landlock=off` vs `auto` to prove the escape is
    blocked *only* when active and that `off` is a clean passthrough.
- **Device 5.4 regression:** assert the probe reports unsupported and
  tawcroot behaves exactly as before (no new failures, `pacman`/Firefox
  smoke unaffected). This is the "ships doing nothing on the real device"
  guarantee.
- **Device ≥5.13 (when one is in the loop):** the must-verify that
  Android's filter permits the landlock syscalls, plus the enforcement
  tests from the host list re-run on-device.

## Open questions

- Does Android's `untrusted_app` seccomp allowlist permit landlock
  syscalls on the Android versions we care about? (Blocking must-verify;
  determines whether this is ever live on real devices vs. host-tests
  only.)
- Exact internal support set beyond `/proc` — audit every host `openat`
  tawcroot issues on its own behalf (proc-shadow inputs, self-exe path
  handling) before first enable, so the first real run doesn't
  self-`EACCES`.
- Should `restrict_self` failure-after-probe-success be fatal (chosen
  here) or degrade to a logged warning? Fatal is the honest default for
  a security feature; revisit only if a device probes-supported but
  restricts unreliably.

## Doc updates (same change as implementation)

- notes/tawcroot/overview.md: promote the "sandboxing" open-question item to a
  real §"Optional Landlock containment" describing the probe, allow-set,
  install position, and the honesty boundary; update §"Confirmed
  environment" with the Landlock probe result per kernel.
- This plan is deleted and folded into notes/tawcroot/ when done.
