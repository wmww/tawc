# tawcroot — overview & scope

## Language: C, with cleat for tests only

Plain C (C11), compiled with NDK clang for arm64-v8a and x86_64. The
production tawcroot binary has **no third-party deps**. It links
nothing but the kernel ABI: hand-rolled raw-syscall stubs, hand-rolled
io helpers (`tawc_io_*`), no libc, no cleat, no STC.

[cleat](https://codeberg.org/sphi/cleat) — Sophie's C library that
bundles a unit-test framework and wraps [STC](https://github.com/stclib/STC)'s
generic containers — is used **only by the host-side test orchestrator**
(`build/tawcroot-host/tests`). It is a hosted glibc binary that forks
the tawcroot binaries as subprocesses; cleat / STC code never runs
inside `libtawcroot.so` or `tawcroot-testhost`.

cleat lives in `./deps/cleat/`, cloned at a pinned commit and `.gitignore`d
(same pattern as `deps/libhybris/`, `deps/libxkbcommon/`, `deps/proot/`). The
build script clones it on first run for `--abi=host`; nothing to
package separately. Cross-builds (aarch64/x86_64) never reference
cleat.

**Containers in production code:** if/when production needs a
container (bind table, BPF generator's program buffer, …), use
hand-rolled fixed-size C arrays / open-coded structures. The
constraints (`-static -nostdlib -no-pie -ffreestanding`, no malloc,
no glibc) mean STC isn't reachable anyway. The bind table is built
once at init from a flat C array; the handler walks the array
directly. Anything that requires growing-on-demand allocation
belongs out-of-handler and out-of-production: extract a pure
function, test it under cleat, encode the result as a flat C array
in the runtime.

Why C, not C++ or Rust:
- **Rust** is the worst fit for the hot path. The SIGSYS handler
  must be async-signal-safe (no allocation, no unwinding, no Drop
  impls firing on shared state). Rust's hot-path subset is the
  tightest of the three — panics unwind into Drop, the whole
  handler ends up `unsafe`. We also get little of Rust's
  memory-safety value because we already trust the guest. Build
  overhead (cargo + cargo-ndk for one small `.so`) is real friction
  for a ~3 kLoC project.
- **C++** would give us nicer toys outside the handler (RAII,
  templates, `string_view`) but adds toolchain weight (libc++
  static-link, `-fno-exceptions -fno-rtti -fno-unwind-tables`,
  watching for hidden allocations / destructors crossing the
  handler boundary) for not much in return. Production tawcroot
  doesn't have a libc to begin with; the bits where C++ would help
  (test scaffolding) live in the cleat-orchestrator process anyway.
- **Plain C** is the simplest thing that works. C is the lingua
  franca of the bits we touch (signal handlers, asm stubs, kernel
  ABI, ELF parsing, BPF), it links cleanly with `-nostdlib`, and
  the things C is bad at (managed strings, regex, generic
  containers) we don't actually need *inside production* — the
  bind table is a flat array, paths are fixed-size buffers, the BPF
  program is a static `struct sock_filter[]`. Where we *do* want
  those niceties (test setup, path-translation unit tests once
  extracted, BPF generator validation), they live in the cleat
  test runner outside `libtawcroot.so`.

Constraints baked into the design:
- **Production binary (`libtawcroot.so` / host `tawcroot`):** no
  libc, no malloc, no cleat, no STC, no stdio, no third-party deps
  at all. `-static -nostdlib -nostartfiles -no-pie -ffreestanding`.
  All syscalls go through the raw stub in `arch/<arch>_stub.S`;
  all formatted output goes through `tawc_io_*`. The seccomp filter
  + handler structure relies on this — see §"Why non-PIE".
- **In the SIGSYS handler specifically:** no allocation, no stdio,
  nothing that takes a lock, no libc calls at all unless we have
  audited the generated code. Path buffers are fixed-size on the
  handler's stack (`char buf[PATH_MAX]`). The bind table is a flat
  C array built at init.
- **Testhost binary (`tawcroot-testhost`):** same constraints as
  production — it's the same `_start`, same raw-syscall stub, same
  filter, same handler. The only difference is `-DTAWCROOT_TESTHOST`
  routes argv-dispatch in `main.c` to `tawcroot_testhost_main()`
  (defined under `tawcroot/tests/testhost/src/`). cleat / STC are NOT linked
  here either; the smoke driver uses the same `tawc_io_*` helpers.
- **Cleat test runner (`tests`):** hosted glibc binary, fully
  cleat-using. Forks the tawcroot binaries as subprocesses; never
  shares an address space with them. This is the *only* place cleat
  / STC code ever runs in this project.
- Compile with `-O2 -fno-strict-aliasing -fvisibility=hidden
  -fno-stack-protector -static -no-pie`. Strip hard. No global
  constructors, sanitizers, profiling instrumentation, or implicit
  runtime hooks in handler/runtime objects. Static linking is
  mandatory — see §"Build integration" and "Known gaps" #1 for why.
  **Non-PIE is mandatory** — see §"Why non-PIE" for the
  filter-stacking constraint that forces this.
  Under static non-PIE linking, `-Wl,-z,now` and PLT-related
  flags are moot (no PLT exists), but `-fvisibility=hidden`
  still helps the linker drop unused symbols.

If we outgrow C — e.g. tawcroot balloons past ~10 kLoC, or we want
real ADTs for syscall results — revisit. The "Language" decision
is reviewable, the "C handler core" decision less so (the handler
shape is what it is).

## Why not just keep proot

Several reasons, all pulling in the same direction:

- **Perf, everywhere.** `notes/proot.md` covers proot's ~5–10× native
  cost on syscall-heavy work. systrap collapses each intercepted
  syscall from "tracee stops, tracer wakes, two `ptrace` syscalls,
  context switch back" to "kernel delivers SIGSYS in-thread, handler
  returns" — typically 1.5–3× native. This shows up as snappier
  shells, faster `pacman`, faster builds, less latency in
  syscall-heavy app paths (Firefox startup, font scans, GTK theme
  loads). It is not a single-workload optimization.
- **Fewer compat hacks.** In-process syscall emulation sidesteps
  several proot corners we currently work around or live with:
  the separate loader-extract binary and the tracer/tracee fork race
  documented in proot's bug tracker. Some compatibility behavior is
  still required — notably `/proc/self/exe` synthesis,
  hardlink-as-symlink fallback, and fake-root uid/stat behavior —
  but those become local syscall handlers rather than ptrace-era
  machinery. The handler runs in the tracee thread with full access
  to its saved registers, so a lot of proot's accidental complexity
  just isn't needed.
- **A codebase we own.** proot is a vendored Termux fork of an
  upstream that doesn't fit Android well; its code quality is uneven
  and its architecture is shaped by ptrace constraints we don't
  share. A small, idiomatic C implementation built around the
  systrap model is easier to reason about, debug, and extend (e.g.
  if we later want better `/proc` synthesis, optional sandboxing, or
  binder-fd rewriting).
- **No surprise pessimization on hot paths.** ptrace traps *every*
  syscall the tracer asks to see; even with seccomp pre-filtering,
  there's a per-trap fixed cost. RET_TRAP only fires for the ~30
  syscalls we actually care about, and the BPF evaluation on the
  ~320 we don't is sub-microsecond. There's no equivalent of "proot
  got slower because we taught it about a new syscall."

`pacman -Syu` is the most legible benchmark — ~7 minutes today on a
fresh ALARM install on the emulator, almost all of it syscall-bound —
so it's the headline number we'll quote when measuring. But the win
is general; pacman is just where it's most obvious.

## Designed for expansion

Today's scope is the proot-replacement minimum: path translation,
fake-root uid/stat behavior, hardlink-as-symlink fallback,
`/proc/self/exe` synthesis, `getcwd` reverse-translate, ELF
loader-injection. Most of what mature proot does beyond that
(fake-`mknod`, mount-namespace shims, finer `/proc` synthesis — including
`/proc/<pid>/maps` reverse-translation since post-manual-load
mappings show host-side rootfs paths there — full `io_uring` SQE
rewriting, perhaps in time binder-fd rewriting) we'll likely end
up wanting too as we feed more workloads through tawcroot. Add on
demand, not proactively.

`chroot(2)` is implemented (see §"chroot emulation"). The
emulation uses `tawcroot_rootfs_fd` + `tawcroot_rootfs_host_path`
+ `active`-flagged bind entries as the "current root view";
chroot swaps all of them in place and rebuilds the symlink memo
cache. The handler lives in one .c file (`src/chroot.c`) plus a
pure helper (`tawcroot_path_binds_reanchor` in
`path_orchestrate.c`).

The architecture is therefore deliberately structured to make adding
those things later cheap, not just to ship MVP fast:

- **Per-syscall handler files** (`src/syscalls/fs.c`, `exec.c`,
  `proc.c`, `deny.c`) so a new feature is "add a `.c` and a
  dispatch entry," not "edit a 2 kLoC file."
- **The dispatch table is a fixed array indexed by syscall number**,
  with `NULL` for unhandled. Adding a syscall means filling a
  slot and adding it to the BPF allowlist — no ordering hazards.
- **The BPF filter is generated from the dispatch table** at
  runtime (in `prod_rootfs_init`, via
  `tawcroot_dispatch_trap_list`). The dispatch table — populated by
  per-subsystem `..._register()` calls — is the single source of
  truth shared between handler routing and the BPF allowlist.
- **Path translation, ELF parsing, bind-table lookup, and fake-root
  metadata decoration are separate from the handler dispatch**, so
  new policy (e.g. more complete xattr or device-node emulation)
  drops in as another decoration on the dispatch entry, not a
  rewrite.
- **The "Open questions" section below is a living list.** Items
  there (sandboxing, `io_uring`, USER_NOTIF fallback) get promoted
  to real work when the need shows up.

The bar for "do we add this now" is "is it needed for the workloads
we're shipping?" The bar for design decisions is "would this make
the future-needed thing harder?" Don't bake in assumptions that
foreclose future expansion.

## What it explicitly is not

- **Not a sandbox.** No security boundary. Like proot, the rewriter
  trusts the guest. The filter is a routing mechanism, not a
  containment one. Real isolation comes from Android's existing
  `untrusted_app` SELinux domain (which already wraps the whole app
  process).
- **Not a Linux ABI emulator.** Unlike gVisor's Sentry, the host
  kernel still implements almost every syscall. We rewrite arguments
  for syscalls that take rootfs-relative paths, synthesize the small
  fake-root identity surface TAWC needs, and fix up a few syscalls
  Android's stacked filter rejects. Everything else flows straight
  through.
- **Not a full userland-namespace replacement.** We do fake the
  small root identity surface that TAWC already depends on from
  proot's `-0`: `getuid`/`geteuid`/`getgid`/`getegid` report 0,
  `stat`-family results are decorated to look root-owned where
  appropriate, and ownership-changing syscalls are treated as
  compatibility operations rather than real privilege changes. We
  do **not** provide pid namespaces, mount namespaces, user
  namespace semantics, or a general-purpose privilege model.
- **Not a 32-bit emulator.** Android lp32 is being phased out and
  our distros are lp64 only. The seccomp filter `KILL_PROCESS`es
  any 32-bit personality syscall as defense-in-depth.
- **Not a tracer process.** We considered `SECCOMP_USER_NOTIF`
  (kernel delivers traps to a separate supervisor process via
  fd) and rejected it — that puts us back in proot's tracer/
  tracee model and loses the "single process, direct memory
  access, no marshalling" win. If signal-safety bites, the answer
  is "fix the signal-safety bug," not "switch architectures."

So the libhybris / GPU / binder story is unchanged. ioctls on
`/dev/kgsl-3d0` etc. are `RET_ALLOW`'d and reach the host kernel
directly, which is exactly what we want.

