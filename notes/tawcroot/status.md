# tawcroot — known gaps, divergences, future work & maintenance

## Known gaps to address before MVP

These are unresolved problems the design hasn't pinned down yet.
Each needs an answer before the corresponding phase lands; flagged
here so we don't ship MVP and discover them at runtime.

1. **~~`libtawcroot.so` must be statically linked~~ — RESOLVED.**
   Static linking against bionic's `libc.a` is confirmed viable.
   NDK r27 ships `libc.a` for all architectures. `-static -no-pie`
   produces an ET_EXEC binary with no `PT_INTERP`, no `DT_NEEDED`
   — the kernel loads it directly without invoking the bionic
   linker. No traps before our `_start`.

   Static linking is also **required for the IP-based seccomp
   filter to work across re-execs** — see §"Why non-PIE". This
   is a stronger constraint than the bionic-linker avoidance
   that originally motivated it.

   Implementation notes:
   - `seccomp()` and `execveat()` have **no bionic libc wrappers**.
     Use raw `syscall(__NR_seccomp, ...)` and
     `syscall(__NR_execveat, ...)`. The syscall numbers and
     constants (`SECCOMP_SET_MODE_FILTER`, `SECCOMP_RET_TRAP`,
     `struct sock_fprog`, etc.) are available via
     `<linux/seccomp.h>` and `<linux/filter.h>`.
   - `sigaction`, `mmap`, `mprotect`, `memcpy`, string functions
     all work in static bionic.
   - `errno` is TLS-based, works correctly in static bionic.
   - `pthread` works from API 23+ (`pthread_atfork` missing on
     21-22; irrelevant, we target API 28+).
   - Static bionic binaries **only run on Android** — they
     segfault on desktop Linux due to bionic-specific kernel
     init expectations. Host-side tests must use the host
     toolchain, not NDK.
   - Binary size: ~450 KB stripped for trivial program; full
     tawcroot likely 1–2 MB.

2. **~~In-process program loader for `--exec-child`~~ — RESOLVED on
   host.** The manual loader (parser, PT_LOAD mapper with BSS partial-
   page zero and anonymous extension, stack synth with full auxv +
   `AT_RANDOM`, per-arch trampoline) is implemented and runs static
   and dynamic guests through to exit. `tawcroot/tests/integration/test_prod_exec`
   exercises `/bin/true`, `tawcroot/tests/unit/test_loader_smoke_dynamic`
   exercises `/bin/ls`, `tawcroot/tests/integration/test_exec_child` and
   `test_exec_via_handler` round-trip the full memfd handoff. argv,
   envp, and auxv all survive intact (verified in
   `dynamic_argv_check.c`).

   Outstanding pieces (to validate on Android + under translation):
   - Run the same loader against a fake rootfs (depends on phase 2d).
   - Run on the aarch64 device under the real `untrusted_app` zygote
     filter (depends on phase 4 APK plumbing).
   - Verify `MAP_FIXED_NOREPLACE` placement against tawcroot's high
     non-PIE base (`0x2000000000` aarch64 / `0x40000000` x86_64) on a
     real device — host glibc tests don't stress the address-space
     discipline the same way.

   Reference material below stays valid for any future maintenance
   (BSS rules, AT_RANDOM, kernel auxv ordering); the "highest-risk
   part of tawcroot" framing is retired.

   - **ELF header reader** (~50 lines): read `Elf64_Ehdr`,
     validate magic, extract `e_type`/`e_entry`/`e_phoff`/
     `e_phnum`/`e_phentsize`.
   - **PT_LOAD segment mapper** (~100 lines): iterate program
     headers; for each `PT_LOAD`, compute page-aligned
     addresses, mmap file-backed portion with
     `MAP_FIXED_NOREPLACE`, handle BSS. For **PIE (ET_DYN)**
     guest binaries: first do a single large
     `mmap(NULL, total_span, PROT_NONE, MAP_ANON, ...)` to
     reserve the address range, then MAP_FIXED each segment
     within the reservation (kernel-chosen base, free ASLR).
     For **ET_EXEC** (non-PIE) guest binaries: use `p_vaddr`
     directly with `MAP_FIXED_NOREPLACE`.
   - **BSS partial-page zero-fill** — the tricky part. The last
     file-backed page of a segment may contain both initialized
     data and BSS. Must mmap the full page from the file, then
     `memset` to zero the bytes from `p_vaddr + p_filesz` to
     the page boundary. The page must be temporarily writable
     for this if the segment is read-only. Additional full BSS
     pages (where `p_memsz > p_filesz` spans past the file-
     backed extent) are mapped anonymously (pre-zeroed by
     kernel). Omitting the partial-page zero-fill is a common
     manual-loader bug; linkers usually pad with zeros but it's
     not guaranteed.
   - **Stack synthesizer** (~150 lines). Unlike proot's loader
     (which reuses the kernel-built stack and patches auxv
     in-place), we must build a fresh stack because there's no
     kernel exec for the guest. Layout per SysV ABI, bottom-up:
     `argc`, argv pointers, NULL, envp pointers, NULL, auxv
     entries, then string data above. Copy our own auxv and
     override the program-specific fields:
     - `AT_PHDR` (binary base + `e_phoff`)
     - `AT_PHENT`, `AT_PHNUM`
     - `AT_BASE` (ld.so load address; 0 for static binaries)
     - `AT_ENTRY` (binary's `e_entry`)
     - `AT_EXECFN` (guest's requested path — must be a string
       on the new stack, pointer updated)
     - `AT_UID`, `AT_GID`, `AT_EUID`, `AT_EGID` (set all to 0 to
       match fake-root `-0` syscall behavior; do not inherit the
       Android app uid here)
     - **`AT_RANDOM`** (**critical** — must point to 16 fresh
       random bytes on the new stack. glibc reads these at
       startup to initialize `__stack_chk_guard` for stack
       canaries. Without valid `AT_RANDOM`, programs `abort()`
       immediately. Use `getrandom(2)` to fill.)
     - `AT_SECURE` (set 0)
     - `AT_PLATFORM` (copy the *string* to the new stack and
       update the pointer — the inherited pointer is into the
       old stack which we're about to abandon)

     Inherit unchanged from our own auxv: `AT_PAGESZ`,
     `AT_CLKTCK`, `AT_HWCAP`/`AT_HWCAP2`, `AT_SYSINFO_EHDR` (the
     vDSO base — the vDSO mapping persists across mmap of new
     segments, safe to inherit; without it, `clock_gettime` falls
     back to real syscalls, measurable perf regression). The initial
     stack lives on a freshly `mmap`'d region (8 MiB, matching the
     kernel's default RLIMIT_STACK, + `PROT_NONE` guard page at the
     low end), not on our existing stack.
     **`SP` must be 16-byte aligned on entry** — hardware
     requirement on aarch64 (unaligned SP faults), ABI
     requirement on x86_64 (SSE assumes it).
   - **TLS**: handled entirely by ld.so, not by the loader. ld.so
     reads `PT_TLS` headers from the loaded binary and DT_NEEDED
     libs, allocates the Static TLS Block, and sets the thread
     pointer register. We just need correct `AT_PHDR`/`AT_PHNUM`
     so ld.so finds the headers.
   - **brk / process heap**: manual load does not reset the kernel
     program break the way `execve` would. Guest glibc may use `brk`
     for early malloc after ld.so starts. Decide deliberately: either
     reserve a fresh guest heap region and set the kernel break with
     raw `brk(2)` before jumping, or force guest allocation down the
     `mmap` path by giving it a sane but isolated break value. After
     the guest jump, tawcroot runtime must not use malloc or any libc
     path that assumes ownership of the old bionic heap. Add a smoke
     that runs a dynamically linked program doing many small mallocs
     before broadening the syscall surface.
   - **Per-arch entry trampoline** (~15 lines of asm per arch:
     set `rsp`/`sp` to top of synthesized stack, zero
     `rdx`/`x0` (rtld_fini), jump to ld.so's entry — or the
     binary's `e_entry` for static ELFs). See
     `deps/proot/src/loader/assembly-{arm64,x86_64}.h` for the exact
     register conventions.
   - **Address-space layout discipline** (~50 lines).
     libtawcroot.so is non-PIE at a high fixed address (see
     §"Why non-PIE"); we choose the guest binary's and ld.so's
     addresses below it. Read `/proc/self/maps` at `--exec-child`
     startup. Reserve `PROT_NONE` guard regions (~256 MB) around
     tawcroot's own mappings so the kernel can't grow ld.so's
     later `mmap(NULL, …)` allocations (for DT_NEEDED libs)
     into our text/data. Use `MAP_FIXED_NOREPLACE` for all
     explicit placements — it errors cleanly (`-EEXIST`) on
     overlap instead of silently clobbering.
     `MAP_FIXED_NOREPLACE` is available on all relevant Android
     kernels (introduced in 4.17; Android 11+ minimum is 5.4).

   **Implementation policy: write it ourselves with references.**
   We do not vendor proot's loader (GPLv2 — would license-encumber
   tawcroot) and we do not lift musl wholesale (its loader is half
   the problem; see below). Instead we write a fresh `src/loader_*.c`
   set under tawcroot's own license, using these as oracles to
   diff our behaviour against:

   - **musl `ldso/dynlink.c::map_library`** (MIT) — canonical
     reference for PT_LOAD mapping math: addr_min/addr_max walk,
     ET_DYN reservation+`MAP_FIXED` dance, BSS partial-page
     zero-fill, anonymous extension for `p_memsz > p_filesz`.
     Re-read this file when our PT_LOAD mapper disagrees with
     reality, but rewrite it against our fdtab/`tawc_raw_*` API
     rather than copy it verbatim. Musl does **not** synthesize
     a stack — it patches the kernel-built one — so the auxv
     and stack-layout pieces have no musl analogue.
   - **proot `src/loader/loader.c`** (GPLv2, reference only) —
     proof-of-existence for the in-process map+jump pattern on
     Android. Useful as a runtime oracle (run a binary under
     proot, dump its initial stack, diff against ours) but no
     code is copied.
   - **Linux kernel `fs/binfmt_elf.c::create_elf_tables`** — the
     authoritative spec for what userspace sees on the initial
     stack. The auxv list, ordering, and `AT_RANDOM`/`AT_PLATFORM`
     string-copy semantics come from here. Kernel headers and
     the ABI itself aren't copyrightable interfaces; behaviour
     is.
   - **glibc `csu/libc-start.c`** + glibc's
     `elf/dl-sysdep.c::_dl_sysdep_start` — what the guest
     program *expects* to find. Useful when debugging "binary
     starts then immediately aborts/segfaults" — usually a
     missing or wrong auxv entry.

   Plan for ~500–800 lines in `src/loader_elf.c` (parser + phdr
   geometry, ~200 lines), `src/loader_map.c` (PT_LOAD mapper +
   address-space discipline, ~250 lines), `src/loader_stack.c`
   (initial-stack synth + auxv, ~250 lines), and
   `src/arch/<arch>_loader_jump.S` (~15 lines/arch).

   **Test-first staging.** Each piece ships with cleat tests
   before the next builds on it:
   1. parser/geometry: synthetic ELF buffers + real `/bin/true`
      headers, asserted byte-for-byte
   2. mapper: map a real ELF on host, walk `/proc/self/maps`,
      assert prot bits + BSS bytes-zero
   3. stack: build a stack, walk it as if we were the kernel,
      compare against `getauxval` output for every entry
   4. trampoline + static-binary smoke: load and run a freshly
      built `static-hello`, assert exit code + captured stdout
   5. dynamic smoke: dynamically linked `/bin/true`, then
      `/bin/sh -c "ls /"` inside a fake rootfs (the phase-2
      exit gate)

   Until step 4 passes nothing else in phase 2 starts; until
   step 5 passes phase 3 doesn't start.

3. **Path canonicalization is the primary mechanism, not a
   fallback.** `openat2(RESOLVE_IN_ROOT)` requires kernel 5.6;
   our primary test device (OnePlus 9, Android 14) runs kernel
   5.4 and doesn't have it. Manual symlink-aware
   canonicalization in the handler is therefore the main code
   path for *all* path-bearing syscalls on *all* current
   devices. (An `openat2` fast path on newer kernels was later
   tried and reverted — it breaks cross-bind absolute symlinks;
   see `include/path_resolve.h`. Guests calling `openat2` get
   `-ENOSYS` and fall back.) The well-known-directory
   memoization cache (`/lib` → `usr/lib`, `/lib64` → `usr/lib`,
   `/bin` → `usr/bin`, `/sbin` → `usr/bin`, etc.) described in
   §"Translation rules" is essential for hot-path performance,
   not optional — ld.so's library opens on every program startup
   walk these symlinks.

4. **~~Bionic static-linking caveats~~ — CONFIRMED VIABLE.**
   Tested with NDK r27. Static linking against bionic's `libc.a`
   works. Confirmed:
   - No `dlopen`/`dlsym` — we don't use these.
   - No netd-routed DNS resolution — we don't do DNS.
   - No properties (`__system_property_get`) — we don't need
     these.
   - `pthread` works in static bionic from API 23+.
   - `errno` is TLS-based, works correctly.
   - `sigaction`, `mmap`, `mprotect`, `memcpy`, string functions
     all present and working.
   - Static bionic binary only runs on Android (segfaults on
     desktop Linux). Not a problem — on-device binary is NDK-
     cross-compiled; host test binary uses host toolchain.

   Still smoke-test early in phase 1 — link a trivial NDK
   `-static -no-pie` binary that installs a seccomp filter +
   SIGSYS handler, verifies that the raw-syscall BPF allowlist
   matches the syscall-instruction label, execs itself with a
   non-`CLOEXEC` state fd, and verifies the inherited filter +
   reinstalled handler + state handoff work. It must also exercise
   each raw syscall the handler/bootstrap depends on under the real
   Android app zygote filter. This validates the entire re-exec chain
   before writing any path translation code.

5. **Internal fd table and reserved range.** The design now requires
   hidden tawcroot-owned fds, but the exact policy still needs to be
   implemented before MVP path translation can be trusted. Define
   `TAWCROOT_FD_BASE`, move `rootfs_fd`, bind fds, `our_binary_fd`,
   proc/cache helper fds, and state helper fds into that range, and
   make fd-returning guest syscalls avoid it. Trap `close`,
   `close_range`, `dup`, `dup2`, `dup3`, and `fcntl` from phase 0.5.
   Exit criteria: a guest can run `close_range(3, ~0U, 0)` and later
   `open("/etc/passwd")` still translates through the rootfs.

6. **Guest `SIGSYS` and seccomp virtualization.** The real `SIGSYS`
   disposition and mask are tawcroot-owned process state. Implement a
   minimal shadow signal table for guest-visible `SIGSYS` state, keep
   the real handler installed and unblocked, and deny guest seccomp
   installation. Exit criteria: after guest attempts to reset
   `SIGSYS`, block it, and install a seccomp filter, a path syscall
   still traps into tawcroot and succeeds.

7. **Path resolver modes and fd provenance.** Manual path resolution
   must be syscall-aware (`follow-final`, `no-follow-final`,
   `parent-for-create`, `parent-for-remove/rename`, and two-path
   operations). Build this as an explicit enum/API, not a boolean
   "canonicalize" helper. Keep enough fd provenance to handle
   fd-relative calls and fake-root metadata without leaking host paths.
   Exit criteria: tests for `stat` vs `lstat`, `readlink`, `symlink`,
   `unlink` of a symlink, `open(O_NOFOLLOW)`, and fd-relative
   `openat`.

## Accepted syscall-fidelity divergences

Known, deliberate departures from exact kernel semantics, collected
during the 2026-06 syscall-fidelity review. Each was judged not worth
the complexity (or the rework) a faithful emulation would cost;
re-litigate only if a real guest trips over one. (The field-relevant
findings from that review — trailing-slash semantics, reserved-dirfd
EBADF, chown existence probes, getdents64 mid-directory EOF, /proc
shadow CLOEXEC, unlink/rmdir/linkat errno shapes, `/proc/self/cwd`
synthesis, cross-process exe substitution — were all fixed and are
covered by unit/hosted/smoke tests.)

- **`execveat(AT_SYMLINK_NOFOLLOW)` → `-ENOSYS`.** Honest placeholder
  in syscalls_exec.c. Nothing we run uses the flag (`fexecve(3)` is
  AT_EMPTY_PATH), and -ENOSYS is safer than silently following the
  symlink. If a caller ever appears: NOFOLLOW-translate, leaf
  `fstatat`, symlink → -ELOOP, else proceed as the follow case.
- **Signal-shadow staleness across fork.** A forked child inherits
  the parent's tid-keyed `blocked` table (COW); kernel tid reuse
  inside the child can read one stale "SIGSYS blocked" bit until that
  thread's first `rt_sigprocmask`. Bounded to a single wrong
  shadow-mask read; fixing it means resetting the table on the
  fork/clone return path for a race nobody has observed.
- **Path-scratch pool can in principle livelock.** One handler chain
  holds 3–5 of the 128 slots while acquiring more, and acquire spins
  forever on exhaustion — a few dozen threads all mid-chain could
  wedge the pool. Theoretical at realistic guest thread counts; a
  `TAWCROOT_SCRATCH_DEBUG_SPIN_TRAP` build traps the spin if it ever
  needs debugging.
- **`..` after a symlink component resolves lexically.** The fold
  collapses `..` before the resolver can ask whether the preceding
  component is a symlink, so `/a/sym/../x` (sym → /b/c) hits `/a/x`
  where the kernel hits `/b/x` (demonstrated on the host build). This
  is a structural tradeoff, not an oversight: post-fold suffixes
  contain no `..`, which is what makes rootfs-escape containment
  trivially auditable. Kernel-faithful `..` means the resolver must
  handle it mid-walk with containment re-checked per step — a
  fold/resolver rework, not a patch. (The related trailing-slash
  erasure WAS fixed, since it only concerned the final component; see
  `has_trailing_dir_marker` in path_orchestrate.c.)
- **Deep paths cap at 256 components** (-ENAMETOOLONG from the fold);
  the kernel accepts ~2040 single-byte components in its 4096-byte
  limit. No real layout comes close to 256.
- **Cross-process `/proc/<pid>/*` is not reverse-translated.** Only
  the calling process's own /proc views get shadow/synthesis
  treatment; another guest process's maps/cwd/exe/fd show host paths
  verbatim. Same-uid processes can already read each other's /proc
  wholesale, so this leaks nothing the kernel doesn't. Includes
  `/proc/<pid>/root` of an emulated-chroot'd guest, which an outside
  observer sees as "/" where a real kernel would show the chroot dir.
- **`security.capability` xattrs cannot be written.** SELinux denies
  `untrusted_app` CAP_SETFCAP, so setting that xattr returns EPERM;
  during pacman installs libarchive degrades it to a per-file warning
  (`newuidmap`, `gst-ptp-helper`, …) and the file lands without the
  capability bit. `setcap`-running scriptlets fail the same way via
  `cap_set_proc` (separate path, same root denial) and pacman reports
  `error: command failed to execute correctly`. Do **not** make
  `setxattr` lie success: the bit genuinely isn't on disk, exec-time
  capability grants genuinely don't happen, and the warning is the
  user's only accurate signal. We also can't fake it — there is no
  exec-time capability application layer to back the lie. In practice
  it doesn't matter: `newuidmap`/`newgidmap` are irrelevant under
  fake-root (no user namespaces), and `gst-ptp-helper` is an optional
  niche plugin. A workload that truly needs file caps must use the
  debug-only `chroot` install method (su can set the xattr) or be
  declared unsupported under tawcroot.
- **x86_64-only legacy syscalls missing from the dispatch table fall
  through to `-ENOSYS`.** Bionic never issues them, so Android's
  seccomp allowlist RET_TRAPs them; x86_64 glibc occasionally does.
  Observed instance: `getpgrp` (NR 111) broke bash job control —
  fixed by forwarding to `getpgid(0)` in syscalls_control.c.
  Speculative same-class candidates, flagged in review but never
  observed: `pause` (34) (an -ENOSYS pause() returns immediately —
  busy loops, broken signal waits) and `alarm` (37) (silently dropped
  SIGALRM timers). Emulator-only; aarch64 never allocated these
  numbers. If odd timing/signal bugs appear on the x86_64 emulator,
  check TAWCROOT_TRACE `[t] nr=...` output for unhandled legacy
  numbers first; the fix pattern is a small forwarding handler plus a
  hosted test.

## Future work

Deferred ideas; none are scheduled. Project policy: no alternative
code paths keyed on kernel version unless required for correctness or
backed by a very large profiled win.

- **io_uring full interception.** Today `io_uring_setup`/`enter`/
  `register` are denied with `-ENOSYS` (syscalls_control.c) so guests
  fall back to plain syscalls — passing io_uring through would let
  path-bearing SQEs (`OPENAT`, `STATX`, …) bypass path translation.
  If a real workload ever needs io_uring throughput: trap
  setup/register/enter plus ring mmap; reject `IORING_SETUP_SQPOLL`
  (`untrusted_app` lacks `CAP_SYS_NICE`, and rejecting it keeps
  `io_uring_enter` the chokepoint); on enter, rewrite path-bearing SQE
  operands to owned translated buffers freed when matching CQEs
  complete; treat unknown opcodes as explicit allow/warn/deny
  decisions. Roughly 500–1000 LOC in a self-contained `src/uring.c`.
- **More `/proc` shadows.** Extend the existing memfd-shadow pattern
  (proc_shadow.c) only when a workload needs it. Likely candidates:
  `/proc/<pid>/cmdline`, `/proc/<pid>/auxv`,
  `/proc/<pid>/task/<tid>/maps`.
- **Path-component negative cache** (perf; profile first). Cache
  recent "not a symlink" prefix components so the resolver skips
  repeated `readlinkat` calls. Bounded table, invalidated on root-view
  changes and relevant `symlinkat` calls.
- **fd-provenance table** (perf; profile first). Track
  fd → rootfs/bind/host provenance at fd creation/dup sites to avoid
  `/proc/self/fd/<n>` readlinks on fd-relative path operations.

Considered and rejected — don't re-propose without new evidence:

- **`PR_SET_SYSCALL_USER_DISPATCH` (kernel 5.11+)** as a seccomp-BPF
  replacement: a whole parallel trapping path for a modest win —
  per-syscall BPF evaluation is cheap next to SIGSYS delivery on
  trapped calls. (Would also have let newer kernels drop the non-PIE
  requirement.)
- **`openat2(RESOLVE_IN_ROOT)` (kernel 5.6+) resolver fast path**:
  tried and reverted — it re-roots cross-bind absolute symlink
  targets at the bind src dirfd (broke `/system/lib64` → `/apex`
  bionic). See the history note atop `include/path_resolve.h`.

## Confirmed environment

Empirically verified on the primary test device (OnePlus 9,
Android 14, API 34, kernel 5.4.284):

- **`Seccomp:2`** — app process has zygote-installed filter active
- **`execveat`** — syscall present in kernel (`__arm64_sys_execveat`)
- **`MAP_FIXED_NOREPLACE`** — available (kernel 4.17+, we have 5.4)
- **`openat2`** — **NOT available** (requires 5.6). Fall back to
  manual path canonicalization
- **`PR_SET_SYSCALL_USER_DISPATCH`** — **NOT available** (requires
  5.11). Use BPF IP-check
- **`PR_SET_NO_NEW_PRIVS`** — works from `untrusted_app` SELinux
  domain, no `CAP_SYS_ADMIN` needed
- **`seccomp(SECCOMP_SET_MODE_FILTER)`** — works from
  `untrusted_app` (no SELinux hook in the seccomp installation path)
- **`apk_data_file` exec** — confirmed working (proot's
  `libproot-loader.so` already proves this path)
- **NDK static linking** — `libc.a` present for all ABIs in
  NDK r27. `-static -no-pie` produces ET_EXEC with no PT_INTERP

## Maintenance contract

- The C is ours. Keep it small, idiomatic. **Production has no
  third-party deps** — no libc, no cleat, no STC, nothing. cleat
  (and its vendored STC) lives in the host-side test orchestrator
  only; bumping the cleat pin in `tawcroot/build.sh` is a
  deliberate change that affects tests and tests only.
- Don't add a libc, runtime, or container library to production.
  If something needs containers, build the structure at init from
  a flat C array, or extract a pure helper and test it under
  cleat. The bind table is the canonical example.
- Match the project's existing conventions: scripts in `scripts/`,
  installer code in `me.phie.tawc.install`, notes here.
- When adding a new trapped syscall, add it to (1) the BPF filter
  generator, (2) the dispatch table, (3) a `tawcroot/tests/unit/` test
  for any new pure helper, (4) a `tawcroot/tests/handler/` test that
  drives the syscall through `tawcroot-testhost` against a
  fake rootfs, and (5) a `tawcroot/tests/integration/` test once
  production gains a working ELF-load + jump path.
  `tawcroot/test.sh` runs all of them; CI runs it on every push.
- Do NOT add `--run-test`, smoke-driver, or other test argv
  branches to `tawcroot/src/main.c`. Test-only entry code lives
  under `tawcroot/tests/testhost/src/` and is gated behind
  `-DTAWCROOT_TESTHOST`. Production must be reachable via real
  CLI only.
- The SIGSYS handler stays freestanding, allocation-free, and
  libc-free. If a feature needs containers in the handler, the
  design is wrong — restructure so the container is built at
  init and the handler reads a flat immutable view.
- Update this note when the design shifts. Future-Sophie will
  thank you.
