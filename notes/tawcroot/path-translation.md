# tawcroot — path translation

## Path translation

The core of the value-add. For each syscall that takes a path, we:

1. Read the path string out of the guest address space through the
   guarded guest-copy helpers, bounded by `PATH_MAX`.
2. Decide if it needs translation:
   - Absolute path inside our rootfs view? → rewrite.
   - Absolute path that matches a bind-mount prefix? → rewrite to
     the bind source.
   - Relative path? → resolve relative to the kernel cwd (for
     `AT_FDCWD`) or the supplied `dirfd`, reverse-translate that base
     into the guest view, then canonicalize inside that root/bind view.
     Do **not** blindly pass relative paths through.
3. Construct the translated, canonicalized path in a per-call stack
   buffer.
4. Issue the *at-flavoured equivalent of the syscall via the stub,
   passing our rootfs fd as `dirfd` and the translated suffix as the
   path. Almost every modern Linux syscall has an `*at` form; for
   the few that don't (e.g. `chroot`), we open + fchdir/fchmod/etc.
5. Return the syscall's return value (positive or `-errno`).

We don't write back into the tracee's memory — the syscall is
*emulated*, not forwarded. The guest never sees the translated
path, only the result. This sidesteps the whole class of bugs proot
has around "the tracee modifies the path buffer between argument-
read and syscall-entry."

### Translation rules

The ordering matters. First reduce the guest request to a
guest-absolute path:

- If the syscall path is absolute, use it as-is.
- If the syscall path is relative and `dirfd == AT_FDCWD`, call raw
  `getcwd`, reverse-translate the returned host path into a guest path,
  then append the relative path. The kernel cwd is the source of truth;
  tawcroot does not keep a mutable cwd cache in the MVP. If `getcwd`
  fails because the cwd was unlinked, return the kernel-like error for
  now; add an fd-based cwd tracker only if a real workload needs
  deleted-cwd compatibility.
- If the syscall path is relative and `dirfd` is a guest fd, resolve
  the fd's current location through `/proc/self/fd/<dirfd>` (or a
  lightweight fd table if we add one later), reverse-translate that
  host path into a guest prefix, then append the relative path. If the
  fd does not map back into the rootfs/bind view, return `-ENOENT` or
  the kernel-equivalent error rather than leaking host paths.

This fd-relative resolution needs fd provenance, not just string
translation. MVP may use `/proc/self/fd/<dirfd>` for ordinary
directory fds, but the architecture must keep room for a small fd table
that records whether an fd came from the rootfs, a bind source,
`/proc`, or outside the guest view. That same table is also needed for
`fstat`, `fchown`, `fchmod`, `fchdir`, deleted-directory behavior, and
internal fd protection. Anonymous fds and fds outside the guest view
should not be reverse-translated into host paths.

Then apply these rules to the guest-absolute path `P`:

1. **`/proc` self-magic.** `/proc/self/<x>` and `/proc/<pid>/<x>`
   for our own pid (any of our tids, optional `task/<tid>/`
   segment), readlink side: `exe` is synthesized from the stashed
   original guest-requested exec path (see §"`/proc/self/exe`" —
   the kernel's view points at libtawcroot.so after our exec
   dance); `cwd` is reverse-translated through the same
   longest-prefix walk as `getcwd` (outside-view cwd → -ENOENT,
   same no-host-leak stance); `root` passes through — we never
   kernel-chroot, so the kernel's "/" answer already matches what
   a guest expects, emulated chroot included. Other `/proc`
   entries — and ALL of another process's — pass through unchanged
   (see §"Accepted syscall-fidelity divergences").

2. **Bind-mount longest-prefix match.** If `P` starts with a bound
   `dst`, rewrite to `<src><P after dst>` and resolve from the
   bind's pre-opened fd via the `*at` form.

3. **Default rootfs.** Everything else: strip the leading `/` and
   resolve relative to the rootfs fd via the `*at` form.

4. **`..` and symlink escapes are blocked for both absolute and
   relative requests.** Without intervention, the kernel's `*at`
   resolution walks `..` through real on-disk directories — so a
   guest path like `/etc/../../host-secret` would escape our rootfs
   view, since we aren't actually `chroot()`ing. The same is true
   for relative paths and for absolute symlink targets reached while
   resolving a relative path.

   We block this via **in-handler path resolution**, but it must be
   parameterized by syscall semantics. There is no single
   "canonicalize path" behavior that is correct for all syscalls:

   - **Follow-final** (`open` without `O_NOFOLLOW`, `stat`, `access`,
     `chmod`, `chdir`, exec target): resolve every component including
     the final component, follow symlinks via `readlinkat`, treat
     absolute symlink targets as guest-absolute paths, and clamp `..`
     at the selected root/bind boundary.
   - **No-follow-final** (`lstat`, `readlink`, `open(O_NOFOLLOW)`,
     `fstatat(..., AT_SYMLINK_NOFOLLOW)`, `fchownat(...,
     AT_SYMLINK_NOFOLLOW)`): resolve parent components only; the final
     component is passed through as a leaf and must not be followed.
   - **Parent-for-create** (`mkdir`, `mknod`, `symlink` destination,
     `open(O_CREAT)` when the file does not exist): resolve and clamp
     the parent directory, but do not require the leaf to exist and do
     not follow a non-existent leaf.
   - **Parent-for-remove/rename** (`unlink`, `rmdir`, `rename`
     destination): resolve the parent and preserve the leaf operation's
     kernel semantics (`unlink` removes the symlink itself; `rmdir`
     requires a directory; `rename` has its own overwrite rules).
   - **Two-path operations** (`link`, `linkat`, `rename`, `renameat2`):
     resolve each operand with its own mode. Source and destination may
     use different selected roots/binds; reject cross-root cases only
     where the host syscall or our compatibility fallback cannot model
     the guest-visible behavior.

   The resolver returns a selected base fd plus a root-relative suffix
   and a mode-specific leaf decision. The syscall handler, not the
   generic resolver, decides which host syscall and flags to issue.
   This avoids bugs like following the target of `readlink`, resolving
   the source string of `symlink(2)`, or converting `lstat` into
   `stat`.

   **Trailing-slash semantics ride on top of the modes.** The kernel
   treats leftover bytes after the final component (`/` runs, `/.`)
   as "follow the final symlink, require a directory" — even under
   `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW` — while parent-mode ops keep
   the leaf verbatim and apply the slash to it (`unlink("l/")` →
   ENOTDIR, `mkdir("l/")` → EEXIST). The lexical fold erases those
   bytes, so translate detects them on the raw guest string
   (`has_trailing_dir_marker`, path_orchestrate.c), upgrades the
   leaf walk from NOFOLLOW to FOLLOW (keeping absolute targets
   clamped by the resolver), and re-appends one `/` to the final
   suffix so the kernel-side syscall enforces the directory
   requirement with its native errno shapes.

   **`AT_EMPTY_PATH` on plain `openat` is NOT a portable shortcut.**
   It only landed in kernel 6.6 (`openat2` got it earlier, but plain
   `openat(dirfd, "", AT_EMPTY_PATH, ...)` returns `-ENOENT` on
   anything older). When the translated suffix is empty (guest asked
   for "/"), pass `"."` instead — that resolves to the dir `dirfd`
   refers to and works on every kernel we target. For `chdir`, just
   `fchdir(base_fd)` directly when the suffix is empty. For
   `*statat`/`readlinkat`/etc the flag is well-supported and fine.
   (Confirmed empirically on Android 16 / kernel 6.6 emulator.)

   **No `openat2(RESOLVE_IN_ROOT)` shortcut.** An earlier design
   conditionally used `openat2` with `RESOLVE_IN_ROOT` inside
   `handle_openat` on kernel ≥ 5.6 to let the kernel re-root
   absolute symlink targets at `base_fd`. That broke cross-bind
   absolute symlinks: when `base_fd` is a bind src dirfd, an
   absolute symlink target (e.g. Android's `/system/lib64/libc.so
   → /apex/com.android.runtime/lib64/bionic/libc.so`) gets
   re-rooted at the bind src instead of the host root, so
   `<bind_src>/apex/...` is opened, fails with `ENOENT`, and the
   guest sees "library not found". Specifically: this silently
   broke libhybris's bionic-libc load on Pixel 10 Pro Fold (kernel
   ≥ 5.6) while older devices like the Pixel 4a (kernel 4.14, no
   `openat2`) worked. The manual resolver is now the single
   contract regardless of kernel version, and `handle_openat`
   always uses plain `tawc_openat`. Regression test:
   `prod_rootfs_cross_bind_abs_symlink` in
   `tests/integration/test_prod_rootfs.c`.

   This is not a security boundary — see §"What it explicitly is
   not" — but it stops accidental escapes (build scripts, config
   files, tarball extraction with relative paths) from silently
   reaching host content, which is a *correctness* concern even
   for a trusted guest. proot does the same canonicalize-and-clamp;
   we want feature parity.

   **Symlink-aware canonicalization cost.** Modern glibc rootfs
   has `/lib`, `/lib64`, `/bin`, `/sbin` as symlinks (typically
   into `/usr`); ld.so opens libraries through these on every
   program startup. Naively each `openat` does several
   `readlinkat`s walking the symlink chain. **Memoize the
   canonical form of well-known directory paths at init**
   (`/lib`, `/lib64`, `/usr/lib`, `/bin`, `/sbin`, `/etc` is the
   typical hit set), so the per-syscall canonicalization cost
   stays near zero on the hot path. This memoization is essential
   for performance — it's not optional. The cache has no
   invalidation — if rootfs symlinks change mid-session (e.g.
   `pacman` replaces `/lib`), the stale entry will silently
   misdirect opens. Acceptable for now: these symlinks are
   effectively immutable in ALARM. If it bites, invalidate on
   `symlinkat`/`rename`/`unlinkat` of cached prefixes.

### Which syscalls need trapping

The minimal set that covers our usage. Numbers are arch-specific;
the dispatch table is generated from a syscall list at build time.

**Virtual identity and metadata (stateful, like proot's `fake_id0`):**

Identity is a tracked process-wide struct (`identity.c`:
`tawc_identity` — r/e/s/fs uid+gid plus a 32-entry supplementary-group
shadow), initialised to root (all 0, groups `{0}`), guarded by a
seqlock (the signal-shadow pattern: fixed-size state + atomic sequence
counter; multi-writer safe via CAS-claim). It exists so daemons that
drop privileges and *verify the drop* (OpenSSH `permanently_set_uid`)
behave: after a drop, restoring old ids EPERMs and the getters report
the target user. The privilege predicate everywhere is virtual
`euid == 0` — fake root has all caps, everyone else none.

- `getuid`, `geteuid`, `getgid`, `getegid`, `getresuid`, `getresgid`,
  `getgroups` → report tracked values. `getgroups` is trapped so the
  kernel doesn't leak the app's Android gids (3003/9997/…) into `id`.
- `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`,
  `setresgid`, `setfsuid`, `setfsgid`, `setgroups` → Linux semantics
  applied to the virtual state (including the setreuid saved-id update
  rule and setfsuid's return-previous-never-error contract).
  `setgroups` beyond the 32-entry shadow returns kernel-plausible
  `-ENOMEM`. There is no `seteuid` syscall; libc routes via setres*id.
- `fork` inherits identity for free (address-space copy); `execve`
  ferries it through exec_state (v4, `has_identity` + embedded
  struct), restored in `--exec-child` after supervisor init.
- `stat`, `lstat`, `fstat`, `fstatat`, `newfstatat`, `statx` →
  after the host syscall succeeds, copy the result through a local
  struct, rewrite uid/gid fields to 0 for rootfs-owned paths/fds, and
  copy the decorated result back to the guest via `copy_to_guest`.
  Host errors pass through unchanged. Decoration is NOT
  identity-aware (a dropped process still sees root-owned files and
  can still open anything the app uid can) — same divergence proot
  has; enforcement is a non-goal.
- `/proc/self/status` is NOT synthesized: its `Uid:`/`Gid:`/`Groups:`
  lines show the real app ids, diverging from the getters. Known
  divergence; nothing we target parses it for identity.
- `chown`, `lchown`, `fchown`, `fchownat` → translate paths/fds and,
  when virtual euid == 0, validate existence then return success
  without a real host `chown` (app uid cannot chown; the on-disk
  rootfs intentionally remains app-owned). When dropped, forward the
  real host syscall so genuine permission errors surface.
- `chmod`, `fchmodat` → translate and ATTEMPT the host chmod (modes
  matter inside the rootfs and normally apply for real — the app uid
  owns the files), but when virtual euid == 0 swallow a host
  `EPERM`/`EACCES` into success: root doesn't get permission errors
  from chmod. Concrete consumer: sshd's `pty_setowner()` chmod on
  `/dev/pts/N`, which Android SELinux denies and sshd treats as fatal
  for every TTY login. Non-permission errors pass through; dropped
  processes get the real result. fd-based `fchmod` stays untrapped
  (kernel resolves the fd itself; revisit if a workload fatals on it
  the way sshd did on fchmodat).
- A consequence of identity-blind decoration worth knowing: after a
  drop, files the guest itself creates still stat as uid 0, so
  `test -O` / ownership checks in dropped sessions misreport.
- Out of scope: no privilege *enforcement* (identity is
  cosmetic-consistent only), no setuid-bit honoring on exec (a
  dropped process cannot re-elevate via `su`/`sudo`, same as proot),
  no capget/capset modeling beyond the euid predicate, no per-user
  NSS awareness (ids are numbers). `chroot` and `mknod` stay
  un-gated (the kernel would demand CAP_SYS_CHROOT/CAP_MKNOD): a
  dropped guest can still virtually chroot / attempt mknod. sshd's
  privsep chroot happens *before* its drop so gating isn't needed;
  revisit if a workload depends on the denial.
- Known bounded failures (same stance as the chroot globals /
  signal shadow): a vfork child calling set*id before exec corrupts
  the parent's view; a set*id from a guest signal handler that
  interrupted another set*id on the same thread would self-deadlock
  on the writer claim; an identity *reader* from such a handler —
  get*id, or any handler consulting `tawcroot_identity_euid()`
  (chmod/chown privilege gates) — spins on the odd seqlock forever;
  fork while another thread is mid-set*id leaves the child's seqlock
  odd. All require a guest signal or fork landing inside a set*id's
  few-dozen-instruction write window; no known workload does any of
  these.

**Runtime control surface (protect tawcroot's own invariants):**

- `close`, `close_range`, `dup`, `dup2`, `dup3`, `fcntl` → hide and
  protect tawcroot's reserved internal fd range; guest-visible fds
  behave normally.
- `rt_sigaction`, `rt_sigprocmask` → virtualize `SIGSYS` so the guest
  cannot remove or block tawcroot's real trap handler.
- `seccomp`, `prctl(PR_SET_SECCOMP)`, relevant `prctl` seccomp queries
  → deny guest filter installation for MVP; expose only compatible
  query behavior.

**Path-bearing (rewrite path arg, switch to `*at` form):**

- `open` → `openat(AT_FDCWD, …)` is already at; the kernel `open`
  doesn't take a dirfd, so we route through `openat` with our
  rootfs fd.
- `openat` → re-`openat` with the selected root/bind fd after
  canonicalization. Absolute paths are rooted at the guest `/`;
  relative paths are first resolved against the reverse-translated
  kernel cwd or the supplied guest dirfd, then clamped inside the
  same root/bind view.
- `openat2` (kernel ≥ 5.6) — same.
- `stat`, `lstat`, `fstatat`, `newfstatat`, `statx`.
- `access`, `faccessat`, `faccessat2`.
- `chmod`, `fchmodat`.
- `chown`, `lchown`, `fchownat`.
- `mkdir`, `mkdirat`.
- `rmdir`, `unlink`, `unlinkat`.
- `symlink`, `symlinkat`.
- `link`, `linkat` — translate both paths. If the host `linkat`
  fails with `EACCES`/`EPERM` under Android app-data SELinux policy,
  emulate the link: `renameat2(src → dst, RENAME_NOREPLACE)` moves
  the real file to the new name, then a guest-absolute symlink is
  left at the old name (rolled back if the symlink fails). In the
  spirit of proot's `--link2symlink`, required for the ALARM hardlink
  cases documented in `notes/proot.md`. The direction (real file at
  the NEW name) matters: git finalizes objects/packs with
  `link(tmp, final); unlink(tmp)`, and a symlink at `final` would
  dangle after the `unlink` — every fetched object vanished and
  clones died with `fatal: bad object <HEAD>`. Directory sources are
  stat-checked and keep the kernel's own EPERM (never rename a
  directory away). The emulation is deliberately partial (st_nlink
  stays 1, NOFOLLOW stats see a symlink, unlink-of-destination
  dangles the source); gaps and the full design live in
  `plans/tawcroot-full-link-emulation.md`. Hosted fault-injection
  coverage: `hosted_linkat_fallback_*` in `test_hook_faults.c`.
- `rename`, `renameat`, `renameat2`.
- `readlink`, `readlinkat`.
- `truncate`.
- `chdir` — translate, open the target directory, then `fchdir` so
  the kernel's CWD matches. We intentionally do not update a separate
  guest-cwd global; later relative `AT_FDCWD` paths and `getcwd` both
  derive from the kernel cwd and reverse-translate it.
- `getcwd` — call the host, then reverse-translate (strip the
  rootfs prefix, prepend `/`, or look up a bind source and emit
  the bound destination).
- `chroot` — guest-issued chroot is interesting. proot rejects it
  or rebinds. We reject (`-EPERM`, `fake_eperm` in
  `syscalls_control.c`) and revisit if a guest actually needs to
  chroot inside the chroot.
- `pivot_root`, `mount`, `umount2`, `unshare`, `setns` — same
  `fake_eperm` denial. Trapped so they can't desync our path
  bookkeeping or tear down setup binds.
- `execve`, `execveat` — the path arg needs translation, AND the
  loader/interpreter path inside the ELF (`PT_INTERP`,
  `/lib64/ld-linux-x86-64.so.2`) needs to resolve correctly. The
  kernel will do that itself once we've translated the binary's
  own path, because the inner `openat` calls the kernel makes
  during `execve` go through… wait, no, the kernel issues those
  itself in kernel space, they don't go through our seccomp
  filter. Which means if `/lib64/ld-linux-x86-64.so.2` doesn't
  exist on the host, the exec fails. We must therefore ensure that
  ELF `PT_INTERP` paths resolve. Two options:
   - **Bind-mount approach** (proot does this for some platforms):
     pre-bind `/lib64`, `/lib`, `/usr/lib` etc. in the host so the
     kernel finds the interpreter. Awkward — leaks rootfs paths
     into the host tree.
   - **Loader injection** (proot's preferred approach): rewrite
     the `execve` to run our own loader stub which then re-execs
     with `LD_TRACE_LOADED_OBJECTS`-style indirection… no, simpler:
     read the ELF, find `PT_INTERP`, manually `mmap`+`execve` via
     the interpreter at the rootfs path. proot's loader does
     exactly this. We implement the same idea but cleaner.

  Plan: read the ELF header, extract `PT_INTERP`, translate it, then
  re-exec tawcroot as `--exec-child`. The child reinstalls the handler,
  opens the translated binary/interpreter, `mmap`s their PT_LOAD
  segments, synthesizes the initial stack/auxv, and jumps to ld.so's
  entry point without a second `execve`. See "execve handling" below.

- `mknod`, `mknodat`, `statfs` — trapped, translated, dispatched.
  `mknod`/`mknodat` route to the modern `mknodat` against
  `(base_fd, suffix)`; FIFO/socket nodes succeed on tmpfs; char/
  block devices typically fail `-EPERM` because Android
  `untrusted_app` doesn't permit `mknod`. `statfs` dispatches via
  `/proc/self/fd/<base_fd>/<suffix>` (no `*at` form, and `fstatfs`
  on an `O_PATH` fd is unreliable on Android-shipped 5.4 kernels).
  `fstatfs` (fd-based) stays untrapped.

- `setxattr`/`getxattr`/`listxattr`/`removexattr` and the `l*`
  symlink-NOFOLLOW variants — trapped, translated, dispatched
  through `/proc/self/fd/<base_fd>/<suffix>`. `f*xattr` (fd-based)
  stay untrapped — kernel resolution against an open fd is already
  correct. Most calls return `-EOPNOTSUPP` from Android app-private
  storage; the value of trapping is that the failure is against the
  guest-visible path, not a host-relative one.

- AF_UNIX sockaddr translation (`syscalls_socket.c`): `bind`/
  `connect`/`sendto`/`sendmsg` forward-translate a pathname
  `sun_path`; `getsockname`/`getpeername`/`accept`/`accept4`
  reverse-translate the out-param address back into the guest view.
  The `recvmsg` `msg_name` / `recvfrom` `src_addr` datagram source
  address is **deliberately not** reverse-translated: `msg_name`
  lives inside the guest msghdr, so the BPF filter can't trap
  conditionally, and `recvmsg` is the hottest receive syscall in
  the system (every Wayland/X11/dbus message) — trapping it taxes
  all guest receive traffic to cover a case with no known consumer.
  A datagram source address only carries a path when the sender
  explicitly `bind()`s a pathname socket; glibc `syslog(3)` and
  sd_notify don't. Revisit if a workload does path-addressed
  datagram request/reply (symptom: replies vanish because the
  host-path source address gets forward-translated again on the
  way back out through `sendto`).

- `socket` (`syscalls_socket.c`): `AF_NETLINK` + `NETLINK_AUDIT`
  returns `-EPROTONOSUPPORT` — what a kernel without `CONFIG_AUDIT`
  says (netlink family present, protocol unregistered). Android
  SELinux denies the audit socket with `EACCES`, which libaudit
  consumers (Debian libpam, sshd's linux-audit support) escalate to
  hard failures: `pam_acct_mgmt` → `PAM_SYSTEM_ERR` (breaking
  `su`/`login`/sshd-with-PAM rootfs-wide) and sshd `fatal()`s on TTY
  logins. Every other family/protocol passes through.

- `mount`, `umount` — translate paths. Most will fail at the
  kernel layer; that's fine.

- `io_uring_setup` — return `-ENOSYS`. This isn't a feature we
  defer; leaving io_uring un-trapped lets the guest submit
  path-bearing SQEs (the kernel reads SQEs from app-shared
  memory, not via syscalls our filter can see) that operate on
  *host* files. Denying setup makes programs fall back to
  syscall-based I/O, which we do translate. Full interception
  is sketched in §"Open questions".

**Non-path quirks (Android's stacked filter):**

The `untrusted_app` filter on lp64 RET_TRAPs syscalls that exist on
some arches but not others (this is the issue documented in
`notes/proot.md` §"Why upstream proot doesn't work on Android
x86_64"). Concretely on x86_64: `access`, `open`, `chmod`, `chown`,
`mkdir`, `rmdir`, `unlink`, `symlink`, `link`, `rename`. We TRAP
these in our own filter too, and our handlers always rewrite to the
`*at` equivalent — which means we silently fix Android's gap as a
side effect of doing path translation. This is the cleanest version
of the kludge proot's `src/tracee/seccomp.c` carries.

On aarch64, the listed syscalls don't exist as separate numbers in
the first place — glibc routes through `*at` natively — so the
Android-filter side of this is a no-op there, but we still trap
them in our filter for safety (in case a static binary issues them
directly).

**Why we trap so few syscalls:**

The seccomp filter should be as narrow as possible. Every TRAP is a
SIGSYS round-trip, and modern programs make a *lot* of `read`,
`write`, `futex`, `epoll_wait`, `mmap`, `clock_gettime` calls. None
of those need translation. The TRAP set should be strictly:

- The path-bearing list above.
- `getcwd` (because we have to reverse-translate).
- Virtual-identity/metadata syscalls (`getuid`, `set*id`, `stat`,
  `chown`, etc.) — the stateful analogue of proot `-0`/`fake_id0`.
- Runtime-control syscalls needed to preserve tawcroot's invariants
  (`close*`/`dup*`/`fcntl` for internal fd protection,
  `rt_sigaction`/`rt_sigprocmask` for `SIGSYS`, and
  `seccomp`/`prctl` for guest filter denial).
- `io_uring_setup` so path-bearing SQEs cannot bypass translation.
- Any of the Android-stacked-filter set we want to silently rewrite.

That's still a small syscall subset out of ~350. The rest stay
`RET_ALLOW`, including all the hot ones. Per-call overhead on
non-path syscalls: zero (just the kernel evaluating the BPF, which
is nanoseconds). There's no kernel knob to skip BPF evaluation on
RET_ALLOW; that nanosecond per syscall is unavoidable and far
below proot's ptrace round-trip.

**Per-fd / per-arg BPF fast-paths.** Some trapped syscalls only do
real work for a small subset of their argument values. The seccomp
filter can `RET_ALLOW` the uninteresting calls so they never hit the
handler. Today only `close` has one (JEQ ladder against
`tawcroot_reserved_fds[]`; landed to fix the gpgme/`closefrom` death
spiral, see history below). The same shape applies to others:

- `fcntl`: `F_DUPFD`/`F_DUPFD_CLOEXEC` only need handling when arg2
  ≥ `TAWCROOT_RESERVED_FD_BASE`; every other op only needs handling
  when arg0 (fd) is in `tawcroot_reserved_fds[]`. Today's handler in
  `syscalls_fd.c` already encodes this discrimination correctly; the
  BPF doesn't yet, so glibc's TLS/locking, GnuTLS' fd-flag scrubbing,
  and stdlib hot paths all trap unnecessarily on `F_GETFD`/`F_SETFD`.

These are pure perf optimizations — add them when a workload is
measurably handler-bound on the syscall in question (count
`tawcroot_dispatch_*` invocations by syscall number under a stress
run). Mechanical extension to `tawcroot_build_filter`: same shape as
the close special case.

### execve handling in detail

This is the trickiest part of the design and the place proot has
the most accidental complexity. There are two interleaved problems:

1. **The handler doesn't survive `execve`.** Per `execve(2)`,
   caught signal dispositions are reset to `SIG_DFL` across
   `execve`. So while our seccomp filter survives (kernel state,
   inherited via `PR_SET_NO_NEW_PRIVS`), the SIGSYS handler does
   not — and the default action for SIGSYS is "terminate the
   process." If we `execve` into ld.so directly, ld.so's first
   path-bearing syscall (`openat` for `libc.so.6`) traps with no
   handler installed and the program dies before main().
2. **The kernel needs to find a real on-disk dynamic linker** for
   the new program. The PT_INTERP path baked into the ELF
   (`/lib64/ld-linux-x86-64.so.2`) doesn't exist on the host; we
   have to redirect it.

Approach A solves problem 1 for the *first* exec by re-exec'ing
ourselves so a fresh `--exec-child` reinstalls the handler. But
the second exec — `--exec-child` launching the actual program —
re-opens the same wound: anything that does `execve(ld.so, …)` to
hand control to the dynamic linker again clears the handler.

**The fix is to *not* `execve` into ld.so.** `--exec-child`
manually loads the binary and ld.so into its own address space
(mmap PT_LOAD segments, set up the stack with argv/envp/auxv per
the SysV ABI, jump to ld.so's entry point), proot-loader-style.
Our handler text and globals stay where they are; ld.so runs
"on top of" them in unused parts of the address space; ld.so's
syscalls (path-bearing or otherwise) keep going through our
handler because the handler is still installed. This is what
proot's `libproot-loader.so` does; we do the same dance in-process
in `--exec-child`'s code rather than via a separate ELF.

(There is no `execve`-the-second-time short-cut. We considered
`execveat`'ing into ld.so and re-installing the handler from a
small LD_PRELOAD-injected constructor, but ld.so has to make
syscalls — including path-bearing `openat` for the LD_PRELOAD
library itself — *before* any user constructor runs, so the
handler reset is fatal. Manual load is the only mechanism that
keeps the handler live.)

End-to-end flow:

```
guest:        execve("/bin/bash", argv, envp)
   │
   ▼  SIGSYS, our handler
handler:      execveat(our_binary_fd, "",
                       ["tawcroot", "--exec-child", "<NN>"],
                       envp, AT_EMPTY_PATH)
   │
   ▼  kernel runs us; seccomp filter inherited, handler is gone
--exec-child: read+close state fd, re-open rootfs fd / bind table,
              install SIGSYS
              handler, then manually load /bin/bash + its
              PT_INTERP into our address space and jump to
              ld.so's entry point — NO further execve.
   │
   ▼  in-process jump (no syscall, handler stays live)
ld.so:        opens /lib/libc.so.6 etc. via openat(AT_FDCWD, …)
              → those traps come back through our handler
              → handler translates to rootfs-relative *at calls
              → recursive case is self-healing
```

The `--exec-child` manual-load step (replaces the rejected
"execveat into ld.so" path):

1. Translate `/bin/bash` → `<rootfs>/bin/bash`.
2. Open the binary with `openat(rootfs_fd, "bin/bash", O_RDONLY)`.
3. Read the ELF header.
4. If it's dynamically linked, read `PT_INTERP` (typically
   `/lib64/ld-linux-x86-64.so.2` or `/lib/ld-linux-aarch64.so.1`),
   translate, and open it via the rootfs fd. For static binaries,
   skip this step.
5. `mmap` the PT_LOAD segments of binary and (if applicable)
   ld.so into the address space. Use kernel-chosen addresses
   (PIE) plus `MAP_FIXED_NOREPLACE` to avoid clobbering tawcroot's
   own text/data/heap. Set permissions per segment via `mprotect`.
6. Build the SysV ABI initial stack: argv (with original argv[0]
   restored — no `--argv0` trick needed since we're not running
   ld.so as a CLI), envp (guest's verbatim), auxv (synthesize
   `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`,
   `AT_EXECFN`, etc. against the binary we just loaded).
7. Set the stack pointer and jump to ld.so's entry (or the
   binary's `e_entry` for static ELFs). No syscall.

The cost of one guest `execve` is *one* host exec (the re-exec of
ourselves) plus an in-process load. The in-process load is
roughly equivalent to what the kernel's `execve` does internally,
so total wall-cost is higher than a native exec but avoids a second
host exec into ld.so. The big win is that the handler stays live with
no signal-restoration trickery.

This is conceptually what proot's loader does, modulo proot ships
a separate `libproot-loader.so` for ptrace-land reasons. We have
a different reason to fold loader and runtime into one binary:
we want the SIGSYS handler installed before any path-bearing
syscall fires in the new process, which means no `DT_NEEDED` libs
to load before our `_start` runs. **`libtawcroot.so` is therefore
statically linked and non-PIE** (see "Known gaps" #1 and §"Why
non-PIE") and contains both the filter/handler runtime and the
manual-load program loader — one binary, a static ET_EXEC
executable (despite the `.so` filename).

### `/proc/self/exe`

When the guest reads `/proc/self/exe`, the kernel returns the path
of whatever was actually `execve`d. With manual-load
(§"execve handling in detail") that's *libtawcroot.so itself* —
the only `execveat` between the guest's intended binary and now
was the re-exec into ourselves; ld.so was loaded by us via mmap,
not via `execve`. So `/proc/self/exe` points at
`<nativeLibraryDir>/libtawcroot.so` regardless of whether the
guest binary is static or dynamic, and it's never the path the
guest expects. Many programs assume this path is the program they
think they're running (Firefox is a notable offender; glibc's
`dlopen` also relies on it for `$ORIGIN` resolution).

The fix isn't a translation — there's no host-prefix to strip back
off — it's a *substitution*. `--exec-child` stashes the
guest-requested exec path during its init (the handler's
`execveat` replaces the process on success, so the stash can't
happen in the handler itself — it happens in the fresh
`--exec-child` process). The readlink/readlinkat handler returns
that stashed `guest_exe_path` value when it sees:

- `/proc/self/exe`
- `/proc/<our-pid>/exe`
- `/proc/<our-tid>/exe` (tids equal pids in single-threaded init,
  but a multithreaded guest will see different tids; we recognize
  any tid in our process)

proot does the same; the key insight is that the value the guest
expects is the path it *originally* asked us to execve, before any
of our rewriting.

A second, result-side substitution catches readlinks that reach the
same answer without a recognizable request path — glibc realpath's
`readlinkat(O_PATH-fd, "")` on an fd opened from `/proc/self/exe`,
and `readlink("/proc/self/fd/<n>")` on such an fd: when the kernel's
result equals libtawcroot.so's own host path, the stashed guest path
is returned instead. This check is suppressed when the *request*
named a different process's exe link (`/proc/<other-pid>/exe`):
every tawcroot guest's kernel exe is the same libtawcroot.so, so the
equality test alone would hand the *caller's* guest exe path to a
readlink of someone else's link (observed: `ls` reading bash's link
got `/usr/bin/ls`). Cross-process exe links pass the kernel bytes
through verbatim, like the rest of cross-process /proc.

For `/proc/self/exe` *opens* (e.g. `open("/proc/self/exe",
O_RDONLY)` to re-read the binary), we translate to the stashed
guest path through the normal rootfs-relative path: the open
reaches the actual binary on disk.

### `/proc/<pid>/fd/<n>` reverse translation

readlink of an fd magic link returns the kernel's HOST path for the
fd's target. When the *request* path classifies as
`/proc/<pid>/fd/<n>` (any pid — sibling tawcroot processes share the
view; `self`, numeric, and `task/<tid>/` forms; the fd-relative
`readlinkat(<fd of /proc/self/fd>, "<n>")` shape `ls -l` uses is
caught via the same compose-through-dirfd step as exe/cwd), the
result is reverse-translated through the rootfs/bind longest-prefix
walk — the same one `getcwd` uses. Outside-view results
(`socket:[…]`, `pipe:[…]`, `/memfd:… (deleted)`, app-private host
files) pass through verbatim: fd links legitimately point outside
the view, so unlike `getcwd` we don't ENOENT them.

Field reproducer: Bun-compiled binaries (Claude Code's native build)
canonicalize their cwd via `open(dir)` +
`readlink(/proc/self/fd/<n>)`, got the host rootfs path back, and
died with "Can't access working directory" when the follow-up stat
of that host path translated to nothing inside the view.

### `getcwd` reverse translation

After a successful `chdir("<rootfs-rel>/foo/bar")` the kernel's CWD
is `<host-prefix>/<rootfs-rel>/foo/bar`. `getcwd` returns that. We
intercept, strip the host prefix, return the rootfs-relative form.
If the path matches a bind-source prefix, we map back to the
bind-destination.

If the host prefix doesn't match (the guest somehow `chdir`'d
outside the rootfs), we return `-ENOENT` rather than leaking host
paths. proot returns the host path unchanged in that case which is
a small info leak.

`readlink("/proc/self/cwd")` synthesizes its answer through the same
function (`tawcroot_cwd_to_guest_abs`), including the -ENOENT
outside-view stance, so the two surfaces can't drift apart.

### chroot emulation

We don't have CAP_SYS_CHROOT inside the Android app sandbox, so
`chroot(2)` can't be forwarded to the kernel. Returning `-EPERM`
(the original posture) breaks pacman 6.x's `_alpm_run_chroot`,
which calls `chroot(handle->root)` unconditionally — every
post-install scriptlet and post-transaction hook fails on a fresh
Manjaro install (~80 errors per `pacman -Syyu`); the rootfs is left
without GTK icon caches, GSettings schemas, mime caches, etc. So
we *emulate* the syscall instead.

The emulation lives in `src/chroot.c`. Per-process state that
defines "the current root view":

- `tawcroot_rootfs_fd` — O_PATH dirfd. Initially the rootfs the
  supervisor opened; replaced atomically(-ish) on each successful
  `chroot()`. The *previous* fd stays in the reserved-fd list so
  the guest can't close it; it leaks until process exit (one fd
  per chroot, capped by the typical "chroot at startup" idiom).
- `tawcroot_rootfs_host_path[+_len]` — kernel-canonical host path
  of the current root, recovered via `readlink("/proc/self/fd/<n>")`
  on the new fd. `getcwd` reverse-translation, `/proc/self/maps`
  rewriting, and the relative-path resolver all read this.
- `tawcroot_binds[]` — every bind has an `active` flag and a `dst`
  field that's stored **current-root-relative**. On chroot, each
  bind's host coordinate (`old_root_host + "/" + dst`) is checked
  against the new root: inside → strip prefix → re-anchor; sibling
  or outside → mark inactive (the src_fd stays reserved); exact
  match for the new root → mark inactive (the rootfs_fd swap
  covers it). The pure helper is
  `tawcroot_path_binds_reanchor` in `path_orchestrate.c`.
- The well-known-symlink memo cache (`g_memo` in `path.c`) gets
  rebuilt against the new rootfs_fd. Old entries memoized symlinks
  in the *outer* root and would silently route inside the chroot
  to wrong targets if not refreshed.

`tawcroot_guest_exe_path` (used to synthesize `/proc/self/exe`)
and `tawcroot_self_host_path` (host path of libtawcroot.so) are
**unchanged** by chroot. Real Linux behaves the same: the kernel's
`/proc/<pid>/exe` is fixed at exec time and survives chroot
verbatim — even if the chroot makes it unreachable inside the new
view.

Edge cases the design handles cleanly:

- **chroot("/")** (the pacman 6.x case): all path translation
  surfaces collapse to identity. binds_reanchor leaves every dst
  unchanged. The memo rebuild re-reads the same symlinks.
- **chroot to a non-existent / non-directory target**: the openat
  with `O_DIRECTORY` returns the kernel's errno; we propagate.
  No state mutates.
- **chroot through a symlink**: the path translator's symlink
  resolver runs in `FOLLOW` mode, so chroot("/foo") where /foo is
  a symlink lands at the resolved target.
- **chroot with `..` chains**: folded by `tawcroot_path_fold_absolute`;
  clamped at the *current* root before openat.
- **chroot into a bind dst**: the path translator returns the
  bind's `src_fd` as `base_fd`; openat lands at the bind src's
  host path; the canonical `/proc/self/fd/<new>` readlink picks
  that up — this is the new root. binds_reanchor sees the new
  root host path bears no relationship to any existing bind's
  composed `cur_root + "/" + dst` (the bind src lives outside
  the rootfs by construction), so every bind — including the one
  we just chrooted into — falls through to the "outside" branch
  and gets deactivated. **Limitation**: binds NESTED under the
  chrooted-into bind dst (a rare configuration: bind A at
  `usr/sub` *and* bind B at `usr/sub/inner`) are also
  deactivated rather than re-anchored to "inner". Fixing this
  would require comparing the new root host path against each
  bind src's host path in addition to the rootfs prefix; not in
  scope for current workloads.
- **Repeated chroot**: each call uses the *current* state as the
  outer root, so chroot("/usr") then chroot("/lib") inside that
  goes one level deeper. (Real chroot is not strictly nestable
  for non-root, but our emulation mirrors the kernel-permissive
  posture — same as a process with CAP_SYS_CHROOT.)
- **Kernel cwd outside the new view**: `getcwd` returns `-ENOENT`
  (existing posture for any cwd outside the rootfs view, just
  newly relevant after chroot moves the goalposts).

`pivot_root` continues to return `-EPERM`. Real pivot_root requires
two separate mounts and exchanges them; we don't model mounts at
all (binds aren't kernel mounts), so there's nothing useful to
emulate. No targeted workload hits it.

