# tawcroot — phasing

## Phasing

- **Phase 0 — Foundation smoke**: ✓ DONE on x86_64 emulator (kernel
   6.6, Android 16). Static non-PIE binary, raw syscall stub, BPF IP
   allowlist against `tawcroot_raw_syscall_ret` (NOT `_insn` — the
   kernel reports post-syscall PC; see §"Issuing host syscalls from
   the handler"), inherited seccomp across self-exec, non-`CLOEXEC`
   `exec_state_fd` handoff via memfd, SIGSYS handler reinstall, and
   raw-syscall set exercised through the stub under our smoke filter.
   aarch64 cross-build clean. Outstanding: real Android `untrusted_app`
   zygote-filter validation (requires APK-level deployment — `run-as`
   doesn't inherit zygote filter; folded into Phase 4).
- **Phase 0.5 — Runtime invariant protection**: ✓ DONE on x86_64
   emulator (Android 16, kernel 6.6); aarch64 cross-build clean.
     • Internal fds (`rootfs_fd`, bind src_fds) reserved into a
       high-numbered range (`TAWCROOT_RESERVED_FD_BASE = 1000`) at
       init via `fcntl(F_DUPFD_CLOEXEC, base)`. From the guest's
       perspective every fd ≥ 1000 returns -EBADF.
     • Trapped + handled: `close`, `close_range` (clamps at the
       reserved boundary so `close_range(0, ~0u)` only closes
       guest-visible fds), `dup`, `dup2` (x86_64 only; aarch64 has
       only dup3), `dup3`, `fcntl` (with F_DUPFD/F_DUPFD_CLOEXEC
       capping the requested minimum at base-1).
     • SIGSYS shadow: `rt_sigaction(SIGSYS, ...)` round-trips a
       guest-side `struct kernel_sigaction` shadow — the kernel
       disposition stays our handler. `rt_sigaction` for any other
       signal passes through verbatim.
     • Signal-mask shadow: `rt_sigprocmask` strips the SIGSYS bit
       from any new mask before forwarding to the kernel and
       OR-injects it into oldset based on a shadow `g_guest_sigsys_blocked`
       flag. Guest reads back what it wrote; kernel never blocks SIGSYS.
     • Seccomp denial: `seccomp(2)` and `prctl(PR_SET_SECCOMP)`
       return -EPERM; `prctl(PR_GET_SECCOMP)` and other prctl ops
       pass through.
     • Acid tests: after guest does `close_range(3, ~0u)` AND
       `rt_sigaction(SIGSYS, SIG_DFL)` AND `rt_sigprocmask(SIG_BLOCK,
       {SIGSYS})` AND `seccomp(SET_MODE_FILTER, ...)`, openat
       through our handler still resolves inside the rootfs.
     Lives in `src/syscalls_fd.c` + `src/syscalls_control.c`;
     reservation entry point is `tawcroot_fd_reserve` in
     `include/fdtab.h`. Guest-multi-thread mask state (per-thread
     shadow) is a phase-2 follow-up — the rootfs syscall smoke is single-threaded.
- **Phase 1 — MVP path translation (host-side)**: ✓ DONE on x86_64
   emulator (Android 16, kernel 6.6) **and validated on aarch64
   device** (OnePlus 9, Android 14, kernel 5.4.284) via
   `tawcroot/test.sh --device`; Android-filter-specific coverage is
   synthesized by `tests/handler/androidfilter`.
   Path translation, fd reservation, SIGSYS/sigprocmask shadow,
   seccomp/prctl denial, well-known-symlink memo, fake-root identity,
   `renameat2`, `truncate`, and the mode-aware lstat-vs-stat memo all
   pass on the device. With this run, Phase-0's outstanding
   "real-`untrusted_app`-zygote-filter validation" item is also closed.
   The `faccessat2` (kernel ≥5.8) and `close_range` (kernel ≥5.9)
   handler-suite cases skip on 5.4 — testhost detects -ENOSYS from the
   raw syscall and emits `[skip]` (parsed by `tawcroot/tests/handler/steps.c`,
   registered as passing); polyfilling new syscalls with older ones is
   intentionally out of scope for tawcroot, real workloads on 5.4 see
   the same -ENOSYS without us in the path.
   Comprehensive handler set:
     • argv parse (`-r <rootfs>`, `-b src:dst` repeatable)
     • dispatch table; BPF trap set generated from the same handler list
     • per-arch ucontext glue (read args, write return)
     • absolute path translation with `..` clamp; escape attempts
       (`/../../host-secret`) provably clamp at rootfs root
     • relative path reverse-translation via raw `getcwd` + rootfs
       host-prefix strip
     • bind-mount table with longest-prefix match
     • well-known-symlink memoization (`/lib`, `/lib64`, `/bin`,
       `/sbin`, `/usr/sbin`, `/usr/lib64`, `/var/run` — the typical
       glibc-rootfs symlink hit set) — required for ld.so library
       opens at any program startup
     • fake-root identity (`getuid`/`geteuid`/`getgid`/`getegid` → 0)
     • metadata decoration: `fstatat` and `statx` with `st_uid`/
       `st_gid`/`stx_uid`/`stx_gid` rewritten to 0
     • path-bearing handlers: `openat`, `readlinkat`, `faccessat2`,
       `chdir`, `getcwd` (reverse-translates), `mkdirat`, `unlinkat`,
       `symlinkat`, `linkat` (with EACCES/EPERM → symlink fallback
       per proot's `--link2symlink`), `fchmodat`, `fchownat`
       (fake-root no-op success — host uid can't chown rootfs files
       and the on-disk owner stays app-uid)
     • legacy x86_64 wrappers (`stat`/`lstat`/`access`/`readlink`/
       `chmod`/`chown`/`lchown`/`mkdir`/`rmdir`/`unlink`) routed
       through *at variants — the kludge that closes Android's
       lp64-`access`-on-x86_64 gap as a side-effect of path
       translation. Also `poll`→`ppoll` and `epoll_wait`→`epoll_pwait`
       on x86_64 for the same reason: Android's untrusted_app filter
       RET_TRAPs the legacy variants so we just forward them. Without
       the `epoll_wait` shim, mio's epoll backend (used by wezterm,
       and probably others) sees -ENOSYS from the empty dispatch slot
       and aborts with "polling for events: ENOSYS; terminating".
     • `ioctl` `TCGETS2`/`TCSETS{,W,F}2`: try-native-first, fall
       back to the legacy `TCGETS`/`TCSETS{,W,F}` family on -EACCES.
       Android's `untrusted_app_all` sepolicy `allowxperm` set
       `unpriv_tty_ioctls` whitelists the legacy four but on at
       least the Android-15 emulator does NOT include the termios2
       variants, so the kernel SELinux gate returns -EACCES on
       every TCGETS2. Glibc's `tcgetattr` issues TCGETS2 first and
       only falls back on -EINVAL, NOT -EACCES — so `bash`,
       `lxterminal`, `wezterm` all see the EACCES, conclude stdin
       isn't a tty, and skip both the prompt and readline (the
       "renders but no prompt or input" symptom). The fallback
       sidesteps the xperm gate; the kernel-ABI structs share the
       first 36 bytes (4*tcflag_t + c_line + c_cc[19]), and the
       speed_t tail is zeroed (irrelevant for pty workloads — CBAUD
       bits in c_cflag carry the symbolic baud unchanged).
       Try-native-first rather than always-translate because the
       xperm gap is Android-version- and vendor-specific (the
       OnePlus 9 honours TCGETS2 fine); on permissive policies we
       want the kernel's full struct termios2 with the real
       c_ispeed/c_ospeed, not a synthetic legacy view. Other errors
       (-ENOTTY, -EFAULT, -EINVAL, …) pass through unmodified.
       All other ioctl numbers pass through to the kernel via
       TAWC_RAW; the trap is unconditional on `ioctl(2)` because
       BPF-arg matching on the cmd is more involved than today's
       filter generator supports — perf-wise terminal apps issue
       O(1) ioctls per frame so the SIGSYS round-trip cost is
       invisible.
     • EFAULT-safe guest-pointer copies via `process_vm_readv`
       (probed at init with `tawc_usercopy_init`); `openat(NULL)`
       and `openat(<unmapped>)` cleanly return -EFAULT, no handler
       crashes
     • Four-mode resolution API (FOLLOW / NOFOLLOW / PARENT_CREATE /
       PARENT_REMOVE) plumbed through every path-bearing handler.
       The well-known-symlink memoizer is mode-aware: a sole-component
       match is rewritten only under FOLLOW, so `lstat("/lib")`
       returns the symlink's `S_IFLNK` while `stat("/lib")` returns
       the target dir's `S_IFDIR`. Verified on x86_64 emulator.
     • `rename` / `renameat` / `renameat2` (two-path translation
       w/ PARENT_REMOVE + PARENT_CREATE), `truncate` (translate path
       then `openat` + `ftruncate` + `close`). Legacy x86_64
       `link`/`symlink`/`rename` route through their `*at` cousins.
     • Generic non-final-component symlink resolution via
       `openat2(2)` with `RESOLVE_IN_ROOT` on kernel ≥5.6. The
       openat handler probes at init (`tawcroot_path_probe_openat2`)
       and routes through openat2 when available, letting the
       kernel handle arbitrary in-rootfs symlinks (including
       absolute targets that would otherwise escape) by re-rooting
       resolution at our rootfs fd. Verified: a fake rootfs with
       `/etc/host-secret → /etc/passwd` (absolute) opens to ENOENT
       (clamped) instead of leaking the host's `/etc/passwd`.
     • Manual symlink-aware canonicalization (`src/path_resolve.c`
       + `src/path_fold.c`, oracle interface in `include/path_oracle.h`).
       Walks each component, calls a filesystem oracle's `readlink`,
       splices the target back into the suffix, re-folds, and bounds
       the walk at `SYMLOOP_MAX = 40`. Mode-aware: NOFOLLOW /
       PARENT_CREATE / PARENT_REMOVE leave the leaf untouched. This
       runs unconditionally inside `tawcroot_path_translate` after
       fold + memo and before bind routing, so every path-bearing
       handler (not just `openat`) gets the same clamping discipline
       on every kernel. Marked `LEGACY-5.4` in `path.c`: the resolver
       exists because kernel <5.6 has no `openat2(RESOLVE_IN_ROOT)`,
       and is well-contained for future deletion (banner comment in
       `include/path_resolve.h` documents the drop procedure).
       Tested two ways: cleat unit tests against a mock oracle
       (`tawcroot/tests/unit/test_path_resolve.c`: chain, self-loop, depth
       bomb, NOFOLLOW leaf, absolute-target clamp, `..`-target clamp)
       and end-to-end against a real fake rootfs on host + device
       (`tawcroot/tests/handler/test_rootfs_syscalls_smoke.c` rootfs adds `/altpath`,
       `/chain1..3`, `/loop`).
   Phase-1 outstanding: the openat2 fast path in `handle_openat`
   stays for the tiny perf win on 5.6+, but is now redundant — the
   resolver has already canonicalized the path. Cheap to drop later.
- **Phase 2 — execve handling**: split into sub-stages because the
   work landed incrementally and the doc was lying about what's done.
   The original "manual loader is the long pole" framing is obsolete:
   the loader works end-to-end on host for both static and dynamic
   guests with full argv/envp/auxv (`tawcroot/tests/unit/test_loader_smoke*`,
   `tawcroot/tests/integration/test_prod_exec`, `tawcroot/tests/integration/test_exec_child`,
   `tawcroot/tests/integration/test_exec_via_handler`).

   **All sub-stages 2a-2g pass on host x86_64. Cross-builds (aarch64,
   x86_64) clean. Real-Android validation pending phase 4 (APK
   plumbing).**

   Sub-stages:
   - **2a — In-process loader (`--exec`)**: ✓ DONE on host x86_64.
     `tawcroot_loader_exec` parses ELF, maps PT_LOADs (with BSS
     partial-page zero + anonymous extension), reads PT_INTERP and
     maps ld.so for dynamic guests, allocates a fresh stack, builds
     argc/argv/envp + full auxv (incl. `AT_RANDOM` from `getrandom`),
     and jumps. `/bin/true`, `/bin/ls /dev/null`, dynamic exit-42 all
     pass.
   - **2b — `--exec-child` memfd handoff**: ✓ DONE on host. Handler
     writes versioned exec_state into a non-CLOEXEC memfd; `--exec-child`
     mmaps it, parses path/argv/envp, hands off to the loader.
     Round-trips static + dynamic guests; bad-fd / corrupt-magic /
     short-buffer cases all return cleanly.
   - **2c — SIGSYS-handler-side `execve` interception**: ✓ DONE.
     `tawcroot_exec_handler_perform` builds the memfd, opens
     `/proc/self/exe`, and `execveat`s into `--exec-child`. In
     rootfs mode it now translates the guest path through
     `tawcroot_path_translate` for the existence probe, and
     captures rootfs+binds+guest_exe into the exec_state's optional
     extras (v2 format) so `--exec-child` can re-establish state.
     Validated by the `prod_rootfs_guest_does_execve` integration
     test.
   - **2d — Production CLI `-r/-b/--` + path translation in the
     loader**: ✓ DONE. `tawcroot -r ROOTFS [-b SRC:DST]... -- CMD
     [ARGS...]` parses argv in `main.c`, opens the rootfs O_PATH
     fd, reserves it into the high range, builds the bind table,
     installs handler+filter, and calls `tawcroot_loader_exec`.
     The loader detects `tawcroot_rootfs_fd >= 0` and routes the
     guest binary's path AND PT_INTERP through
     `tawcroot_path_translate`. Validated by 8 cases in
     `tawcroot/tests/integration/test_prod_rootfs.c` (static binary inside
     rootfs, bind dst routing, `..`-clamping, missing/bad arg
     shapes, unreachable guest).
   - **2e — `/proc/self/exe` synthesis**: ✓ DONE.
     `tawcroot_set_guest_exe_path` (in `path.c`) stashes the
     guest's requested exec path; `handle_readlinkat` matches
     `/proc/self/exe` and `/proc/<our-pid>/exe` and returns the
     stash. Production `main.c` sets it from `argv[cmd_start]`
     after `prod_rootfs_init`; `tawcroot_loader_exec_child`
     re-sets it from the carried `exec_state.guest_exe` (or the
     new exec path if absent). Validated in `rootfs_smoke.c`.
   - **2f — Handler reinstall in `--exec-child`**: ✓ DONE.
     `tawcroot_loader_exec_child` calls `tawcroot_supervisor_init`
     (in `src/supervisor.c`) when `exec_state.rootfs_host` is present.
     supervisor_init does: open rootfs O_PATH, reserve, set host
     path, usercopy probe, add binds, re-register inherited shm
     name table, memoize symlinks, dispatch_init, install SIGSYS
     handler, reset signal mask, probe `openat2`, stash
     `/proc/self/exe`. The seccomp filter is NOT re-installed (it's
     inherited as kernel state — see "Why non-PIE"). The same
     supervisor_init is called from `prod_rootfs_init` for the
     top-level entry, so the bootstrap stays in one place. Without
     this the post-exec guest's first path-bearing syscall would
     either route to host paths (no rootfs view) or kill the
     process (no handler).
   - **2g — Multi-process correctness**: ✓ DONE for static execve.
     `prod_rootfs_guest_does_execve` validates the full chain:
     filter trap → handler dispatch → memfd write with extras →
     execveat self → `--exec-child` re-init → manual-load target
     inside rootfs → exit 42. `tawcroot/tests/integration/test_prod_fork.c`
     extends the surface to cover a guest **forking** (clone(SIGCHLD),
     separate-VM child) before doing the trapping syscall:
       * `prod_fork_child_opens_marker_in_rootfs` — child openat
         exercises path translation against a different PID/tid than
         the parent, regression-guarding the cached-tid bug
         (usercopy.c:14-22; "More phase-5b bugs" below).
       * `prod_fork_then_execve_in_child` (+ `..._with_bind`) — the
         bash-style fork+execve chain, exercising exec_handler from a
         fork-child PID and the bind-table re-export.
       * `prod_fork_closefrom_then_execve_in_child`
         (+ `..._with_bind`) — gpgme/closefrom shape: child
         close(1000..1003) + close_range(0, ~0u, 0) before execve,
         catching reserved-fd-survival regressions ("Phase 5c — full
         integration suite" below).
       * `prod_fork_exec_proc_self_exe_correct_in_child` — Firefox
         libxul.so regression: descendant's /proc/self/exe must
         resolve to its own path, not the original guest binary
         (exec_handler.c:91-101; "Phase 5c").
     `tawcroot/tests/integration/test_prod_features.c` covers
     production-binary feature paths that used to live only at the
     rootfs-smoke testhost handler layer:
       * `prod_unix_bind_translates_sun_path` — AF_UNIX bind() sun_path
         translation (gpg-agent regression). Was untested anywhere
         before this.
       * `prod_proc_self_fd_hides_reserved` — getdents64 dirent filter
         hides reserved fd 1000 (gpgme closefrom death-spiral).
       * `prod_inherited_sigsys_block_unblocked_by_init` — orchestrator
         blocks SIGSYS, forks tawcroot; supervisor_init's SIG_SETMASK
         reset is what keeps the guest's first trapping syscall alive
         (JVM-spawned-shell regression, "Phase 4" fix).
     fork+exec for *dynamic* guests, and `bash -c 'ls'` style flows,
     still need a glibc-on-rootfs fixture but the static surface that
     past regressions actually fired through is now covered.

   Exit gate (was "dynamically linked `/bin/true` and
   `/bin/sh -c "ls /"` run from inside a fake rootfs"): partially
   met. Static binaries inside the rootfs run end-to-end including
   guest-issued execve. Dynamic binaries inside a fake rootfs need
   a fixture rootfs that contains the dynamic linker — that's an
   integration-test scaffolding task, tracked alongside the dynamic
   fork+exec follow-up.
- **Phase 3 — Full trapped syscall surface**: every syscall in "Which
   syscalls need trapping" above.
- **Fast iteration loop (debugging tawcroot on a real Android target).**
   The full install pipeline (download + extract + pacman -Syu) takes
   ~7 minutes; restarting it every code change is too slow. The
   working iteration loop:

   1. One-time setup: install the APK once with `--es method tawcroot`,
      let it run far enough that the bootstrap is extracted (PKG_KEYRING
      stage). Even if the install errors out at pacman-key, the rootfs
      under `/data/data/me.phie.tawc/distros/arch/rootfs/` is intact.
   2. Per-iteration: `tawcroot/build.sh --abi=<abi>`
      then `adb push app/src/main/jniLibs/<abi>/libtawcroot.so
      /data/local/tmp/tawc-dev/libtawcroot.so` then `adb shell 'su -c "cp
      /data/local/tmp/tawc-dev/libtawcroot.so <apk-lib-path>/libtawcroot.so"'`.
      Total ~3 seconds.
   3. Test: `adb shell 'run-as me.phie.tawc sh -c "cd <rootfs> && \
      <libtawcroot> -r <rootfs> <binds> -- <cmd>"'`.

   This bypasses the APK install AND the Arch install. Only Kotlin
   changes (TawcrootMethod, etc.) require a full APK rebuild +
   `adb install -r`.

- **PHASE 5 COMPLETE on x86_64 emulator.** A full `tawcroot`-method
   Arch install via the in-app `InstallActivity` reaches `state: READY`,
   and `scripts/rootfs-run.sh "uname -a; id; pacman --version"`
   produces clean Arch output (`Linux localhost ...`, `uid=0(root)`,
   `Pacman v7.1.0`) on the host shell. End-to-end app launch through
   the APK + run-as + tawcroot chain works.

- **Additional bugs found and fixed during install pipeline validation:**
   - **`openat2` and `faccessat2` killed handler-host processes via
     Android's stacked seccomp filter.** Android 16's `untrusted_app`
     domain RET_TRAPs both syscalls (NR 437 and 439). Two flavours of
     fix:
     1. `tawcroot_path_probe_openat2()` was running BEFORE
        `install_handler` in `prod_rootfs_init`. Without the handler,
        Android's TRAP went to default disposition and killed the
        process. Reordered: install_handler first, then probe (the
        handler now catches the trap and routes to "no slot →
        -ENOSYS", which the probe interprets as "openat2 unavailable,
        fall back to manual canonicalization").
     2. `handle_access` and `handle_faccessat` issued `faccessat2`
        from inside our SIGSYS handler. Android's filter then trapped
        again, causing recursive SIGSYS that the kernel routes
        past-mask via force_sig and kills with default action. Fix:
        use `faccessat` (NR 269) — older but unrestricted by Android's
        filter — and drop the flags (our common callers don't pass
        AT_-style flags through access).
     The `bash -c "exec uname"` test that worked earlier didn't hit
     this because uname doesn't call access; pacman-key does as part
     of its very first command, which is why it surfaced only during
     real install validation.
   - **Inherited signal mask in the JVM-spawned shell chain:** the
     `prod_rootfs_init` path (running from `/system/bin/sh` via
     ProcessBuilder) didn't unblock SIGSYS, mirroring the unblock
     `--exec-child` already does. Mask was actually 0x80000000 (bit 31
     = signal 32, NOT SIGSYS) so this turned out not to be the
     actual blocker — but the unblock is now defensive at every
     entry point regardless.

- **Bugs found and fixed during emulator validation:**
   - `#!` shebang scripts: the manual loader didn't handle them at all
     (treated `#!/usr/bin/bash` files as ELF, failed with -EINVAL).
     Added `resolve_shebangs` in `loader_exec.c` that mirrors Linux
     `binfmt_script.c`: reads up to 4 levels of shebang indirection,
     rewrites argv to `[interp, [shebang_arg,] script_path,
     orig_argv[1..]]`, opens the interpreter as the actual ELF to load.
     Subtle gotcha: don't share storage between `path_buf` and
     `argv_out[0]` — overwriting one silently rewrites the other and
     bash sees `argv[1] == /usr/bin/bash` instead of the intended
     script path. Without this, every shell-script entry-point in Arch
     (pacman-key, gpg, etc.) failed with the loader's exit 61.
   - `getresuid` / `getresgid` not in the fake-root trap set: bash
     reads its `$UID` / `$EUID` via `getresuid(2)`, not `getuid(2)`.
     Without the trap the kernel returns the real app uid (10222) and
     scripts like pacman-key that gate on `[[ $EUID -eq 0 ]]` reject
     the call. Added handlers in `identity.c` that copy zero into all
     three out-pointers via `tawc_copy_to_guest`.
   - `AT_EMPTY_PATH` with a NON-NULL empty string: glibc's `fstat()`
     calls `fstatat(fd, "", &st, AT_EMPTY_PATH)` with a real but-empty
     pointer. The handler only short-circuited `gpath == 0` (NULL) and
     fell through to translate `""` against the kernel cwd, routing
     `fstat` to the wrong inode. wc, gpg-agent, etc. would then read
     stale stat data and segfault. Added a one-byte peek via the
     EFAULT-safe usercopy helper in both `handle_newfstatat` and
     `handle_statx`. Test in `rootfs_smoke.c::fstatat(fd, "", AT_EMPTY_PATH)`.

   These fixes are why static binaries worked early in phase 5 but
   anything script-based (pacman-key) and anything that did `bash $UID`
   stayed broken until they landed.

- **Bugs found and fixed during phase-5b (aarch64 OnePlus 9, Android
  14, kernel 5.4.284):**
   - **`execve` (NR 221) was missing from the aarch64 dispatch.** The
     prior comment in `syscalls_exec.c` claimed "aarch64 has no
     execve(2); glibc routes everything through execveat(2) there" —
     this was wrong. aarch64 has both NR 221 (execve) and NR 281
     (execveat). Glibc's `execve()` wrapper goes through NR 221, only
     `fexecve()` and `execveat()` use NR 281. Without an NR 221
     handler, `bash -c '/bin/true'` (and any other plain
     fork+execve) issued NR 221 untrapped: our filter said ALLOW,
     Android's stacked filter killed the process with SIGSYS, and the
     guest died with "Bad system call" the moment it tried to exec a
     child. Fix: register `handle_execve` for `TAWC_SYS_execve = 221`
     on aarch64 too (wrappers `do_exec` work identically on both
     arches). Symptom on x86_64 emulator: nothing — Android's filter
     either allows execve there or doesn't apply the same restriction,
     so the bug was silent until the first real-device run.
   - **`clone3` (NR 435) untrapped on aarch64.** Glibc 2.34+ tries
     clone3 first and falls back to clone(NR 220) on -ENOSYS. Without
     a handler, our filter ALLOWed clone3 and Android's filter
     intercepted — depending on policy version this was either a
     silent ENOSYS (good — fallback fires) or a kill. Adding
     `handle_clone3` returning -ENOSYS keeps the fallback path
     deterministic and removes one unknown-behavior surface. The
     comment in `syscalls_control.c::handle_clone3` documents the
     filter-precedence reasoning: this fix only works if Android
     RET_TRAPs (or RET_ERRNOs) clone3, not RET_KILLs it. Empirically
     bash-fork on Android 14 worked once clone3 was -ENOSYS'd from
     our handler.
   - **`loader_exec_child` ordered `probe_openat2` before
     `install_handler`** on the post-execveat re-entry path, in
     mirror of the Phase 4 emulator bug that was already fixed in
     `prod_rootfs_init` (main.c). On Android 14 this turned out not
     to be the live blocker (Android 14's filter doesn't TRAP
     openat2 — same as x86_64 emulator never tripped this in
     practice), but the ordering inversion is still a bug:
     `prod_rootfs_init` → install_handler before probe; the re-init
     in `loader_exec_child` should match. Fix is one line; reordered
     to install_handler then probe_openat2.

   These fixes converted "every bash-fork dies after 6 sigactions"
   into clean execution of `uname -a`, `id`, `pacman --version`,
   shell pipelines (`ls /etc | head`), all through the bash → fork
   → execve → handler → exec_handler_perform → execveat self →
   --exec-child → loader chain on the OnePlus 9.

- **More phase-5b bugs found while debugging pacman-key/gpg-agent on
  aarch64 device:**
   - **AF_UNIX bind/connect didn't translate `sun_path`.** The path
     in `bind(fd, &sockaddr_un, len)` lives inside the userspace
     struct, not as a separate syscall argument the kernel resolves
     through *at-style APIs. Without translation, gpg-agent's
     `bind(/root/.gnupg/S.gpg-agent)` looked for `/root/` on the host
     filesystem and got -ENOENT, exiting status 2 and breaking every
     pacman-key flow. Fix: trap `bind` (NR 200 aarch64 / 49 x86_64)
     and `connect` (NR 203 / 42), copy the sockaddr_un from guest,
     translate sun_path through `tawcroot_path_translate`, and
     forward with a rewritten `/proc/self/fd/<base_fd>/<suffix>` form
     on the handler stack. Lives in `src/syscalls_socket.c`.
     Confirmed: `gpg-agent --daemon` now exits 0 with the keyring
     sockets created at the right host-filesystem location.
   - **Fd-relative path resolution in *at handlers.** Every *at
     handler (openat, fstatat, statx, readlinkat, faccessat, mkdirat,
     unlinkat, fchmodat, symlinkat, linkat, renameat, renameat2)
     was passing the guest's `dirfd` to `fetch_and_translate`, which
     ignored it and resolved relative paths against the kernel CWD
     instead. gpg opens its homedir then issues
     `openat(homedir_fd, "pubring.gpg", ...)` — translating
     "pubring.gpg" via cwd produced ENOENT even though `ls` and
     `stat` of the file work. Same shape breaks `find -delete`
     mid-walk and any tool that uses `*fd_dir(); openat(fd, …)`
     patterns (which is most modern fs traversal). Fix: new
     `fetch_and_translate_at(dirfd, …)` variant — when dirfd ≠
     AT_FDCWD and path is relative, pass through to the kernel's
     fd-relative resolution. The dirfd is itself one we previously
     handed back from a translated openat, so the inode is already
     inside the rootfs view. Caveat: a `..` chain from the dirfd
     can still escape (kernel walks `..` past the dirfd freely);
     proot has the same gap on kernels without RESOLVE_BENEATH.
   - **Per-string env buffer was too small.** `do_exec` collected
     argv/envp into static buffers with `MAX_STR = 4096` per string.
     Bash's `LS_COLORS`, exported function bodies, and a few
     /etc/profile.d additions easily blow past 4 KB; a single
     overflowing env entry made `tawc_copy_string_from_guest` return
     -ENAMETOOLONG, which `do_exec` propagated as the execve(2)
     return — bash printed "File name too long" for every external
     command. Bumped MAX_STR to 16 KB and the envp_strings buffer to
     256 KB.

   With the bind/dirfd/env-buffer trio in place, the original
   pacman-key/gpg-agent reproducer (`gpg --quick-gen-key` against a
   fresh homedir) now passes end-to-end on aarch64 device and
   x86_64 emulator: pubring.kbx is created, the master key is
   generated, the self-signature is written, and pacman-key --init
   runs to completion. The earlier diagnosis ("gpg never tries
   `O_CREAT` on pubring.kbx") was wrong — the apparent missing-
   `O_CREAT` was a downstream symptom of an earlier-failing
   socket/dirfd path that we'd already untangled by the time the
   reproducer was retested. Issue closed.

   pacman-key --populate previously failed because of an unrelated
   `wc` segfault (its `secret_keys_available` helper pipes gpg's
   --with-colons output through `wc -l`). Root cause: the loader
   in `loader_exec.c` allocated a 256 KiB anonymous stack for the
   guest. wc 9.11's `wc_lines`/`wc_bytes` are compiled with
   `-fstack-clash-protection` and pre-allocate a 256 KiB I/O
   buffer in the stack frame, then page-probe (`orq $0,(%rsp)`)
   on the way down — which walked one page off the bottom of our
   region and SIGSEGV'd with SEGV_MAPERR at exactly `rsp`. Fixed
   by bumping the loader's stack to 8 MiB to match Linux's
   default RLIMIT_STACK; anonymous pages stay demand-zeroed so
   the cost is reservation, not RSS.

   x86_64 had also drifted out of buildable shape while aarch64
   work landed: `handle_rename_legacy` was passing path pointers
   where `do_renameat` expected dirfd ints (left over from before
   the renameat refactor that added explicit dirfds), and
   `TAWC_SYS_execve` (NR 59) was missing from the x86_64 sysnr
   block. Without those fixes the x86_64 cross build never
   produced a binary with the bind/connect translator in it, so
   the emulator was running a stale .so where bind() bypassed
   tawcroot entirely and gpg-agent crashed on the host-side path.
   Both fixes are tiny and obvious in hindsight; they're called
   out so a future bisect doesn't get confused.

   Also fixed a latent stdout-vs-stderr bug while debugging:
   `tawc_io_str` was writing trace and error output to fd 1
   (stdout), but tawcroot is supposed to leave stdout untouched
   for the guest. Output going to stdout gets captured by shell
   `$(...)` command substitution and turns into argv for downstream
   commands; reproduces hilariously in pacman-key, where any future
   trace would silently corrupt arg parsing. Now writes to fd 2
   (stderr), matching the `tawcroot: …` error-message convention
   used at init sites in main.c.

- **Bugs found while bringing up the gtk4 input integration tests on
  the OnePlus 9 (Android 14, kernel 5.4):**
   - **`apply_memo` shift-loop aliasing bug for shrinking targets.**
     The well-known-symlink memoizer's in-place shift wrote the trailing
     remainder right-to-left for both growing and shrinking rewrites.
     For a shrink (`m->target_len < m->src_len`), the very-last shift
     iteration read from a position the very-first iteration had
     already overwritten — truncating the path at `target_len`. The
     production-relevant case is `usr/sbin -> bin` (relative) on Arch:
     translating `/usr/sbin/bash` produced `bin` instead of `bin/bash`,
     which the second memo pass then expanded back to `/usr/bin` (a
     directory), so `lstat /usr/sbin/bash` returned the symlink at
     `/bin` and `bash` builtin PATH lookup failed for any binary that
     glibc resolved through /usr/sbin. Fix: copy left-to-right when
     shrinking, right-to-left when growing. `tawcroot/src/path.c`.
   - **`faccessat` (NR 269 aarch64 / 48 x86_64) was untrapped.** Only
     `faccessat2` (NR 439) and legacy x86_64 `access` (NR 21) had
     dispatch entries. Glibc's `access(2)` wrapper issues NR 269 on
     all kernels and only probes NR 439 opportunistically (5.8+ via
     dlopen-style fallback), so on the OnePlus 9 (5.4) every libc
     access check on a guest path went straight to the kernel, which
     resolved against the host filesystem and returned -ENOENT.
     Symptom: fontconfig couldn't find `/etc/fonts/fonts.conf`
     (`Cannot load default config file: No such file: (null)`),
     gcc's `find_a_file` couldn't find `cc1` and fell back to bare
     `posix_spawnp("cc1")` which then failed with
     `posix_spawnp: No such file or directory`. Fix: register
     `handle_faccessat` for `TAWC_SYS_faccessat` alongside
     `TAWC_SYS_faccessat2`. The handler already drops flags for the
     inner `TAWC_RAW(faccessat, ...)` call (we issue NR 269 internally
     to avoid Android's RET_TRAP on faccessat2), so the same
     implementation serves both numbers.

   With those two fixes, the then-named `test_input_dispatch` suite
   against `--es method tawcroot` passed all 13 input-dispatch
   scenarios on the OnePlus 9 in ~22 s.
   This is the first integration-test suite running entirely under
   tawcroot on the device — the then-current debug app built in the
   chroot install (gcc-on-tawcroot is fine post-faccessat fix; the
   harness supported `TAWC_BUILD_INSTALL_ID` to build in a sibling
   install if desired) and ran from the tawcroot rootfs against the in-app
   compositor over the shared `/data/data/me.phie.tawc/wayland-0`
   socket.

- **Fixed: GNU `wc` 9.11 segfault.** A core dump (with `ulimit -c
  unlimited` under root) showed `si_code=SEGV_MAPERR`, fault address
  ≡ rsp, and rip inside a `orq $0,(%rsp); cmp %r11,%rsp; jne loop`
  page-probing loop — GCC's `-fstack-clash-protection` instrumentation.
  `wc_lines`/`wc_bytes` reserve a 256 KiB I/O buffer in the stack
  frame (offset `-0x4008c(%rbp)`) and walk it down one page at a
  time. The loader's stack mmap was exactly 256 KiB, so the probe
  loop fell off the bottom and SIGSEGV'd. wc was unique among
  coreutils because no other tool happened to allocate a frame ≥
  the loader-stack size. Fix: bump `STACK_SZ` in `loader_exec.c` to
  8 MiB (matches Linux's default RLIMIT_STACK).

- **Phase 4 — Emulator integration**: ✓ APK plumbing landed; ✓
   x86_64 emulator end-to-end validation done. Real Arch glibc binaries
   (uname, id, pacman --version, bash with pipes) run through the full
   tawcroot stack from app context (run-as, real `untrusted_app` zygote
   filter active) on the Android 16 / kernel 6.6 emulator. Including
   the SIGSYS-handler-driven re-exec dance: `bash -c "exec uname"` and
   `bash -c "ls /etc | head"` both run end-to-end, traversing fork →
   trapped execve → handler → exec_handler_perform → execveat into self
   → `--exec-child` re-init → manual-load → ld.so → glibc init → guest
   main → exit.

   One on-device fix needed beyond the host validation: the inherited
   signal mask from the bash child has SIGSYS blocked (bit 30, plus
   bit 31 — bash's normal pre-exec signal-discipline setup), and the
   mask persists across `execveat`. Linux's seccomp `force_sig_seccomp`
   does NOT bypass the thread mask on this kernel, so SIGSYS stays
   pending and the kernel kills with default action when our newly-
   installed handler should have fired. Fix: `tawcroot_loader_exec_child`
   force-unblocks SIGSYS via `rt_sigprocmask(SIG_UNBLOCK, {SIGSYS})`
   immediately after install_handler, before the manual-load jump
   (`tawcroot/src/loader_exec.c`). The runtime sigprocmask shadow in
   `syscalls_control.c` already strips SIGSYS from any guest-issued
   mask change, so this only matters for the inherited initial mask
   in the post-re-exec process.
   `tawcroot/build.sh` cross-builds for aarch64 + x86_64 and stages
   `libtawcroot.so` into `app/src/main/jniLibs/<abi>/`; the APK
   ships it like `libproot.so`. `TawcrootMethod.kt` mirrors
   `ProotMethod.kt` (rootless, app-uid-owned rootfs, same bind set, same
   pure-Kotlin tar extractor) but drops the proot-only workarounds:
   no separate loader stub, no `/dev/shm` host bind (memfd-emulated
   in-handler), no `--link2symlink` (built into the linkat handler), no
   `MOZ_DISABLE_*_SANDBOX` envs (no ptrace tracer for Firefox's
   sandbox to fight). The install activity has a third radio button;
   `scripts/rootfs-run.sh` dispatches `tawcroot` alongside chroot/proot.
   Outstanding: real-device run (phase 5), `pacman -Syu` to completion
   (phase 6).
- **Phase 5 — emulator end-to-end**: ✓ DONE on x86_64. Install via APK
   succeeds (`state: READY`, `method: tawcroot`). `rootfs-run`
   from the host shell runs `uname -a`, `id`, `pacman --version`,
   bash pipelines (`ls /etc | head`), bash fork+exec, manual-load of
   real Arch glibc binaries — all working.

- **Phase 5b — aarch64 port**: ✓ DONE on OnePlus 9 (Android 14, kernel
   5.4.284) for the smoke-command criterion. Install via APK reaches
   `state: READY` (`--es method tawcroot --es id arch-tawcroot`),
   and `rootfs-run "uname -a; id; pacman --version | head -1;
   ls /etc | head"` produces clean Arch glibc output through the bash
   → fork → execve → handler → exec_handler_perform → execveat self
   → --exec-child → loader chain. `pacman-key --init` and
   `gpg --quick-gen-key` both run cleanly on the aarch64 device and
   x86_64 emulator after the bind/connect/dirfd/env-buffer fixes.
   `pacman-key --populate` was previously blocked on a `wc` segfault
   (loader stack too small for wc's stack-clash-probed 256 KiB
   frame); fixed by bumping the loader stack to 8 MiB. Full
   `pacman -Syu` to completion is Phase 6 perf work.

   Three aarch64-only bugs found and fixed during this run; see
   "Bugs found and fixed during phase-5b" above for details. Most
   important: registering `handle_execve` for NR 221 — aarch64 has
   both `execve` (221) and `execveat` (281) and the prior code only
   trapped 281, so any plain `execve()` (which is what bash's `exec`
   builtin and most fork+exec paths use) sailed past our filter and
   got SIGSYS-killed by Android's stacked filter.

   Subsequent gtk4 integration-test bringup surfaced two more
   aarch64-relevant bugs (apply_memo shift aliasing and untrapped
   `faccessat` NR 269); see "Bugs found while bringing up the gtk4
   input integration tests" above. With those fixed, the
   then-named `test_input_dispatch` integration suite (13 input-dispatch
   scenarios driving gtk4 through the compositor over Wayland) runs
   entirely under tawcroot on the OnePlus 9.

- **Phase 5c — full integration suite, OnePlus 9** (2026-05-02):
   pacman package install and the wider chroot-test surface come
   online. **12 of 12 integration tests pass** through tawcroot
   on the OnePlus 9 with no `MOZ_DISABLE_*_SANDBOX` workaround env
   vars. Firefox-side fixes landed: in-handler `/dev/shm` memfd
   emulation (`tawcroot/src/shm.c`) so Mozilla's `shm_open(3)`
   doesn't hard-assert; guest `seccomp(2)` /
   `prctl(PR_SET_SECCOMP)` denial so Mozilla cannot stack a filter
   that would bypass tawcroot's translation invariants;
   legacy x86_64 `readlink(2)` /proc/self/exe synthesis (the
   `readlinkat` handler had it but NR 89 didn't); host-auxv
   passthrough so the synthesized guest stack carries HWCAP /
   HWCAP2 / SYSINFO_EHDR / CLKTCK / FLAGS; the test_firefox
   steady-state AHB assertion rewritten from a fragile
   `wlegl: imported` log-grep (only true while the WebRender
   buffer ring is still growing) to a compositor-state check
   (`surfaces_wlegl >= 1 && surfaces_shm == 0 && frames > before`)
   which catches the same regressions without false-failing on
   settled rings. Three aarch64-relevant
   bugs found and fixed:
     1. **close-loop death-spiral via gpgme.** glibc's
        `closefrom()` (called by gpgme between `fork()` and
        `execve()` for fd hygiene) enumerates `/proc/self/fd` via
        `getdents64` in a loop, closing each fd until the list
        is empty. Earlier rev returned `-EBADF` from the close
        handler for any fd ≥ `TAWCROOT_RESERVED_FD_BASE`, but the
        kernel-side fd was never actually closed — `/proc/self/fd`
        kept showing 1000-1009 forever, glibc's loop never
        terminated, pacman/gpg-agent hung at 100% CPU for tens of
        minutes per scriptlet. Two-part fix:
          a. **BPF close fast-path.** The seccomp filter now
             special-cases `close(fd)`: only `RET_TRAP`s when `fd`
             matches an actual reserved slot baked into the
             filter at install time (rootfs_fd + each bind src_fd),
             and `RET_ALLOW`s otherwise. Removes
             ~1M unnecessary SIGSYS round-trips per gpgme child.
             The reserved-fd list is a new
             `tawcroot_reserved_fds[]` (in `fdtab.h` /
             `syscalls_fd.c`) populated by `tawcroot_fd_reserve()`.
          b. **Handler actually closes.** `handle_close` for a
             reserved fd now calls real `close()` (no more lying
             with `-EBADF`). Lets glibc's closefrom loop terminate.
             A guest fork-child losing its reserved fds is fine
             because the child is about to `execve`, and our
             exec_handler re-establishes them in `--exec-child`.
        Plus a third piece in `path.c`:
          c. **Lazy re-open of reserved fds.** `path_translate`
             validates `tawcroot_rootfs_fd` and each
             `tawcroot_binds[i].src_fd` via a one-syscall
             `fcntl(F_GETFD)` probe at the top of every translation,
             and re-opens from the stashed host path
             (`tawcroot_rootfs_host_path`, new
             `tawcroot_binds[i].src[256]` field) on `-EBADF`.
             Costs one extra syscall per translation; required so
             post-closefrom path syscalls in the parent process
             don't break.
     2. **AT_EXECFN sticks to original argv across guest exec.**
        `tawcroot_exec_handler_perform` was forwarding the parent
        process's stashed `tawcroot_guest_exe_path` through the
        re-exec memfd into the child's `loader_exec_child`, which
        meant `/proc/self/exe` resolved to the *first*-ever guest
        binary (typically `/bin/bash`) for every fork-and-exec'd
        descendant. Firefox's stub binary (which uses
        `/proc/self/exe` to find its installation directory and
        then `dlopen`s `libxul.so` relative to it) saw `/bin/bash`,
        couldn't find libxul, printed
        "Couldn't load XPCOM." and exited. Fix: pass
        `extras.guest_exe = NULL` from the exec handler so
        `--exec-child` falls back to `st.path` (the actual exec
        target). A second pass was needed (regression caught
        2026-05-04): `st.path` is whatever the guest passed to
        `execve`, not the post-symlink real-path. Firefox launched
        via `/usr/sbin/firefox` (→ `/usr/bin/firefox` →
        `/usr/lib/firefox/firefox`) put `$ORIGIN` at `/usr/sbin`,
        re-breaking the libxul.so dlopen. `tawcroot_set_guest_exe_path`
        now runs the path through `tawcroot_path_translate` with
        `PATH_FOLLOW` so `/proc/self/exe` returns the canonical
        guest-absolute path, matching what the kernel would have
        produced under a real chroot.
     3. **Bind src host paths weren't tracked.** The bind table
        previously stored only `src_fd`, recovering the host path
        via `readlinkat /proc/self/fd/<src_fd>` at exec-handler
        time. Once gpgme's closefrom started actually closing those
        fds (per fix #1), the readlink failed, so `--exec-child`
        couldn't re-establish the bind table. Added a `src[256]`
        field to `struct tawcroot_bind` populated at
        `tawcroot_path_add_bind` time; exec_handler now copies
        directly from there.

   With those fixes pacman installs packages cleanly (`pacman -S`
   completed pkgconf in 1.5s, the full test-deps set in ~5 minutes),
   gtk3/gtk4 demos, weston, Vulkan clients, supertuxkart, and the
   then-named `test_input_dispatch` flow all pass on the OnePlus 9.
   Keyring init (`pacman-key --init && --populate archlinux`) also
   completes end-to-end after the wc-segfault loader-stack fix.

- **Phase 6 — Hardening + perf**: stacked-filter weird cases,
   measure and tune.

