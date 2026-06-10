# tawcroot — fast rootless chroot via systrap

`tawcroot/` is a from-scratch C implementation of a proot-style fake
chroot, using **seccomp `RET_TRAP` + in-process `SIGSYS` handling**
(the "systrap" technique gVisor uses for its platform layer) instead
of `ptrace`. The goal is a strict superset of proot's TAWC use case —
same compat envelope at meaningfully lower syscall overhead, fewer
compat hacks, and a codebase shaped for our needs rather than
inherited from proot's ptrace heritage.

**Status: default and only officially supported install method.**
Release builds ship only tawcroot; chroot/proot are dev-only and only
appear in debug builds. See `notes/installation.md` "Install methods"
for the build-time gating.

This doc is the design + implementation plan. The code is being built
fresh. Refer to `deps/proot/` (the Termux fork we currently vendor) as
reference when something proot solves is non-obvious — particularly
the Android-specific quirks in its `src/tracee/seccomp.c` and the
loader/exec dance — but **don't port proot's architecture**. proot
inherited a tracer/tracee design from its ptrace heritage that doesn't
fit a single-process systrap model, and its code quality is uneven in
ways we don't want to inherit.

## What it is, in one paragraph

A small C program that, given a rootfs path and a command, sets up a
seccomp-bpf filter that traps path-bearing and a few other syscalls,
installs a `SIGSYS` handler that emulates those syscalls against the
rootfs, and launches the guest through a re-exec + in-process ELF
loader so the handler remains installed. Path translation, fake-root
uid/stat behavior, and Android-specific syscall fixups all happen
in-process, in the same thread that issued the syscall. No tracer
process, no `ptrace`, no per-syscall context switch.

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

## Architecture overview

One process, one address space. Three layers, in dependency order:

```
+----------------------------------------------------------+
|  guest program (bash, pacman, firefox, …)                |
|  uses normal libc — its syscalls are intercepted          |
+----------------------------------------------------------+
|  trampoline / SIGSYS handler                              |
|  reads syscall args from ucontext, *emulates* the syscall |
|  via the trusted stub, writes only the return value back  |
+----------------------------------------------------------+
|  bootstrap                                                |
|  parses argv, builds seccomp filter, installs handler,    |
|  re-execs itself as --exec-child, then manual-loads guest  |
+----------------------------------------------------------+
```

The "trusted stub" is a small piece of code at a known address that
the seccomp filter `RET_ALLOW`s by an instruction-pointer check (see
"Issuing host syscalls from the handler" for the exact predicate).
The filter allowlists the stub's syscall-instruction address. The
handler issues all its own syscalls through that stub, so the filter
doesn't recurse.

### Why one process is correct here

In ptrace-land you need two processes because the tracer's signal
handlers and memory aren't accessible from the tracee. systrap
collapses that: the handler runs in the tracee thread, with full
access to its own data segment, heap, and the saved register frame.
There's no marshalling, no `process_vm_readv`, no `PTRACE_GETREGS`.
You read `info->si_call_addr`, you write `uc->uc_mcontext.gregs[…]`,
you return.

This also fixes one of proot's longest-standing bugs: the "tracee
forks while the tracer is in the middle of handling its syscall"
race. There's no tracer, so there's no race.

## Process layout

A `tawcroot` invocation is a single Linux process. Its lifecycle:

1. **Parse argv** — `tawcroot [-r <rootfs>] [-w <cwd>] [-b <src>:<dst>]… -- <cmd> <args…>`.
2. **Open the rootfs** with `O_PATH | O_DIRECTORY` and stash the fd in
   global state. All path translation will resolve through this fd
   via `*at` syscalls (no string concatenation, no TOCTOU windows).
3. **Materialize bind-mount table** — each `-b src:dst` becomes a
   pre-opened `O_PATH` fd of `src`, plus the in-rootfs path of
   `dst`. Lookups walk longest-prefix-match first.
4. **Set `PR_SET_NO_NEW_PRIVS`**.
5. **Install the SIGSYS handler** (`SA_SIGINFO`, no `SA_NODEFER` —
   see "Why the handler is async-signal-safe" below).
6. **Install the seccomp filter** via `seccomp(SECCOMP_SET_MODE_FILTER, …)`.
7. **Launch the guest through the exec handler path**: re-exec
   ourselves as `--exec-child`, reinstall the handler, then
   manual-load the guest ELF + interpreter and jump to its entry.
   There is no direct `execve` into the guest because caught signal
   handlers are reset across `execve` (see §"execve handling in
   detail").

After step 7 the process is running guest code in the tawcroot
address space. The handler, the bind-mount table, and the rootfs fd
live in tawcroot's data segment and are referenced from the handler
whenever a TRAP fires.

**FD inheritance rule.** Capability fds tawcroot itself opens
(`rootfs_fd`, bind-source `O_PATH` fds, `our_binary_fd`) are
`O_CLOEXEC`. They get cleared by the `execveat` re-exec into
ourselves, and `--exec-child` re-opens them from serialized state
during its init. The guest's own fds (stdio, whatever it had open)
are *not* tawcroot's to mark — they pass through unchanged, and the
guest's `O_CLOEXEC` discipline determines what survives. Cost of
re-opening tawcroot's fds: ~12 `openat`s per guest exec, well under
a microsecond.

**Internal fd protection rule.** `O_CLOEXEC` is not enough after the
manual-load jump. At that point guest code shares our fd table and can
see, close, duplicate over, or inspect any tawcroot capability fd via
normal fd syscalls (`close`, `close_range`, `dup2`, `dup3`, `fcntl`,
`/proc/self/fd`, etc.). Many runtimes also proactively close all
unknown fds after fork. Therefore tawcroot must treat its own fds as a
reserved fd range, not just as ordinary globals.

Implementation contract:

- Move all tawcroot-owned fds into a high reserved range during init
  (for example `TAWCROOT_FD_BASE + n` via `fcntl(F_DUPFD_CLOEXEC)`),
  store only those numbers in runtime state, and never allocate guest
  fds from that range.
- Trap fd-control syscalls that can affect arbitrary fd numbers:
  `close`, `close_range`, `dup`, `dup2`, `dup3`, `fcntl`, and
  `pidfd_getfd` if present. Guest operations targeting internal fds
  must behave as if the fd does not exist (`-EBADF`) unless the call is
  issued by tawcroot itself through `tawcroot_raw_syscall()`.
- For guest `dup*`/`fcntl(F_DUPFD*)`, choose results below the reserved
  range when possible. If the guest explicitly asks for an fd inside
  the reserved range, return `-EBADF`/`-EINVAL` in the same spirit as a
  protected descriptor, not a live capability fd.
- Hide internal fds from `/proc/self/fd` and `/proc/self/fdinfo` views
  when we add proc synthesis. Until then, tests must at least cover
  that opening or closing internal fd numbers from the guest fails
  harmlessly and does not break later path translation.

The guest fd-provenance table is the one MVP exception to the
"globals are immutable" simplification. It must be a fixed-size,
handler-safe table updated only by trapped fd-creating / fd-destroying
syscalls, with atomic slot publication or another lock-free scheme.
Do not put cleat/STC containers or malloc-backed maps on this path.

This is a correctness requirement, not hardening theatre: a shell or
language runtime that runs `close_range(3, ~0U, 0)` must not destroy the
rootfs fd out from under the next translated `openat`.

**Re-exec state handoff.** The one exception to the `O_CLOEXEC`
rule is a short-lived `exec_state_fd`, created by the `execve`
handler immediately before re-execing tawcroot. It is passed as the
positional argv slot after `--exec-child` (i.e. `argv[2]`,
formatted as a decimal integer), intentionally *without*
`FD_CLOEXEC` for that one host exec. `--exec-child` reads it through
`tawcroot_raw_syscall()`, reconstructs process state, then closes it
before manually jumping to guest code. The fd is never visible to the
guest.

State format is versioned and length-prefixed binary, not ad-hoc
string parsing. It contains:

- rootfs host path;
- bind specs (`src` host path, `dst` guest path), in original order;
- current-root state (empty today; future nested `chroot` adds an
  inner-root prefix) and an optional guest-cwd hint for diagnostics;
- tawcroot flags such as debug logging;
- guest-requested exec path and `argv` strings.

The guest `envp` is not copied into the state fd. The handler passes
the guest's original `envp` to `execveat()` unchanged, so after the
host re-exec it is available as tawcroot's normal environment. That
environment is treated only as guest payload when building the
synthesized stack; `--exec-child` does not read internal config from
it.

Preferred implementation is `memfd_create("tawcroot-exec-state",
MFD_CLOEXEC)` followed by clearing `FD_CLOEXEC` only on the fd passed
to `execveat`. If `memfd_create` is unavailable or blocked on a
target, fall back to a pre-opened app-cache directory fd plus
`openat(O_TMPFILE)` or a pipe only for small smoke tests. Do not
fall back to environment variables for internal state; preserving the
guest environment verbatim is part of the contract.

**Environment rule.** `--exec-child` does not consult `environ` for
tawcroot configuration. Internal state travels through the initial
CLI or through the positional `<state-fd>` slot of `--exec-child`'s
argv on re-exec. This makes "envp passes through verbatim"
practical: the child may enumerate `environ` only to copy the guest
environment onto the synthesized guest stack, not to interpret
tawcroot settings.

## SIGSYS handler

The hot path. Called any time the seccomp filter returns
`SECCOMP_RET_TRAP`. Signature:

```c
static void sigsys_handler(int sig, siginfo_t *info, void *ucontext);
```

`info->si_code == SYS_SECCOMP` distinguishes seccomp-originated
SIGSYS from kernel-fault SIGSYS (we abort if it's not seccomp —
something is very wrong).

`info->si_syscall` and `info->si_arch` give the syscall number and
arch (we'll only ever see one arch per build, but assert it
matches). The arguments are in the saved register state inside
`ucontext`, which is `ucontext_t *`.

The handler structure:

```c
{
    syscall_no = info->si_syscall;
    args = read_args_from_ucontext(uc);   // arch-specific
    pc   = read_pc_from_ucontext(uc);

    handler = dispatch_table[syscall_no];  // small, dense table
    rv = handler(args);                    // long or -errno

    write_return_value_to_ucontext(uc, rv);
    // ucontext is restored by the kernel on sigreturn
}
```

`dispatch_table` is a fixed array of function pointers indexed by
syscall number. Most entries are `NULL`. For a NULL entry we
return `-ENOSYS` — **not** abort. Android's stacked
`untrusted_app` filter can TRAP syscalls we don't handle (TRAP
beats ALLOW across stacked filters; see §"Seccomp filter"), and
future Android versions may TRAP additional syscalls. Aborting on
an unexpected TRAP would make us fragile to those changes.
`-ENOSYS` is what the kernel returns for unsupported syscalls and
is the value programs check to fall back gracefully.

### Guest memory access

The guest's pointers are in our address space, but they are still
untrusted syscall arguments. A bad path pointer, unterminated string,
or invalid output buffer must return the same error the kernel would
return (`-EFAULT` or `-ENAMETOOLONG`), not crash tawcroot from inside
the SIGSYS handler.

All guest-pointer access therefore goes through tiny `copy_from_guest`,
`copy_string_from_guest`, and `copy_to_guest` helpers. They are
handler-safe and libc-free. Prefer raw `process_vm_readv` /
`process_vm_writev` against our own task id: the kernel validates the
guest address and returns `-EFAULT` without delivering SIGSEGV to
tawcroot. The helpers build small stack-local `iovec`s and issue the
syscalls through `tawcroot_raw_syscall()`. Never use `memcpy`/`strlen`
directly on guest pointers.

`process_vm_*` against self always passes the kernel's access check
(`__ptrace_may_access` short-circuits for the same thread group before
Yama/SELinux run), and Android's app seccomp policy allows it, so
production needs no fallback; `supervisor_init` fails fast (exit 92)
if the probe fails. A `-EPERM` probe failure means a seccomp errno
filter on the *host* — e.g. a container sandbox without
`CAP_SYS_PTRACE` (Docker's default profile) — and the host test
harnesses detect this and bail with one clear message instead of
cascading `-EFAULT`s. A SIGSEGV-fixup fallback copy path was
considered and rejected: tawcroot only virtualizes SIGSYS, and taking
ownership of guest-owned SIGSEGV (used by JITs, e.g. Firefox wasm)
isn't worth it for a case production never hits. If a real fallback is
ever needed (sandboxed hosts), prefer the pipe trick — `write(pipe_wr,
guest_src, n)` gets kernel address validation with clean `-EFAULT` and
partial counts, reading back completes the copy — over signal games.

These helpers are used for:

- path strings on every path-bearing syscall;
- `readlink`/`readlinkat`, `getcwd`, and other output buffers;
- `execve`/`execveat` argv and envp pointer arrays and strings.

Do not use plain `memcpy`, `strlen`, or ad-hoc pointer walking on
guest-provided addresses in handler code.

### Reading and writing the saved registers

The siginfo/ucontext shapes are arch-specific. We isolate this in
`arch/<arch>.h` with two inline helpers:

```c
static inline void arch_read_args(ucontext_t *uc, syscall_args *out);
static inline void arch_write_return(ucontext_t *uc, long rv);
```

`x86_64` (`uc->uc_mcontext.gregs`):
- args: RDI, RSI, RDX, R10, R8, R9 → `REG_RDI, REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9`.
- return: RAX.
- pc: RIP.

`aarch64` (`uc->uc_mcontext.regs[]`):
- args: x0..x5.
- return: x0.
- pc: pc.

Both archs are needed (real device is aarch64, emulator is x86_64).
We do not need to handle 32-bit personalities — Android's lp32
filter exists but TAWC is lp64 across the board.

### Issuing host syscalls from the handler

We can't just call `syscall(2)` from glibc because (a) glibc's
`syscall` may go through a vDSO/PLT path the seccomp filter would
trap, and (b) we want a single, allowlistable instruction-pointer
range. Instead, a hand-written stub:

```asm
; arch/x86_64/stub.S
.global tawcroot_raw_syscall
.type   tawcroot_raw_syscall, @function
tawcroot_raw_syscall:
    mov     %rdi, %rax           # syscall number → RAX
    mov     %rsi, %rdi           # shift args left
    mov     %rdx, %rsi
    mov     %rcx, %rdx
    mov     %r8,  %r10
    mov     %r9,  %r8
    mov     8(%rsp), %r9
.global tawcroot_raw_syscall_insn
tawcroot_raw_syscall_insn:
    syscall
.global tawcroot_raw_syscall_ret
tawcroot_raw_syscall_ret:
    ret
```

There are three related addresses, and mixing them up breaks the
whole design:

- The label sitting **on** the SYSCALL/SVC instruction
  (`tawcroot_raw_syscall_insn`) is the address of the trapping
  instruction in the binary.
- `seccomp_data.instruction_pointer` and `siginfo->si_call_addr`
  identify the **post-syscall PC** — the address of the instruction
  immediately *after* the SYSCALL/SVC. Linux populates both fields
  from `pt_regs->ip` / `pt_regs->pc`, which the syscall-entry asm
  sets to the saved-return PC (the next instruction). Confirmed
  empirically on x86_64 and aarch64 — see the smoke driver in
  `tawcroot/src/main.c`. SYSCALL is 2 bytes on x86_64, SVC is 4
  bytes on aarch64; the post-syscall PC is the *insn label + insn
  size*, which is the same address as `tawcroot_raw_syscall_ret`.
- The saved program counter in `ucontext_t` is the resume PC; for
  `SECCOMP_RET_TRAP` it equals `instruction_pointer` (resuming after
  the handler continues at the instruction after `SYSCALL`/`SVC`).

The BPF filter therefore checks
`instruction_pointer == &tawcroot_raw_syscall_ret` (NOT `_insn`) and
`RET_ALLOW`s just that one address. Anything else executing a
SYSCALL/SVC is guest code and follows the normal trap table.

(On aarch64 the equivalent uses `svc #0`; same `_ret`-label pattern.)

The stub symbols (`tawcroot_raw_syscall`,
`tawcroot_raw_syscall_insn`, `tawcroot_raw_syscall_ret`) should be
hidden (`.hidden` directive in the asm) and the asm function placed
in `.text` with no special section attributes. Under static linking
there's no PLT to worry about, but hidden visibility still ensures
the symbols can't be interposed by code the guest later mmaps (some
debug/ptrace tooling does this).

**Must-test before phase 1:** a tiny Android smoke binary installs a
filter that allows exactly `&tawcroot_raw_syscall_ret`, traps a
normal syscall from elsewhere, and records both `si_call_addr` and
the saved resume PC from `ucontext_t` for x86_64 and aarch64. If
the observed filter address is not the post-syscall PC label,
stop and fix the stub/filter contract before implementing path
translation. (Status: passing on x86_64 emulator — see
`tawcroot/tests/handler/test_foundation_smoke.c`,
`tawcroot/tests/testhost/src/smoke.c`, and the comment in `filter.c`
where the `_ret`-vs-`_insn` discrepancy is documented.) The same smoke must issue every raw syscall the handler
or `--exec-child` bootstrap plans to use (`openat`, `read`, `write`,
`close`, `mmap`, `mprotect`, `munmap`, `getcwd`, `readlinkat`,
`fstatat`/`statx`, `fcntl`, `execveat`, state-fd creation, etc.).
Our IP allowlist bypasses only our own filter; it cannot override
Android's already-stacked zygote filter. If Android `TRAP`s or
`KILL`s one of these raw syscalls from the stub, pick a different
primitive before building on it.

### Why non-PIE

The IP-based allowlisting in the BPF filter bakes
`&tawcroot_raw_syscall_ret` as a fixed address literal into the
filter program at install time. The filter is kernel state, inherited
across `execve`, and **cannot be modified or removed** — only new
filters can be stacked on top. Seccomp filter stacking uses a
"most restrictive wins" rule: `TRAP` (0x00030000) has strictly
higher precedence than `ALLOW` (0x7fff0000). If any filter in the
stack says TRAP for a given syscall, the result is TRAP regardless
of what other filters say.

This creates a hard constraint for the re-exec architecture:

1. First tawcroot installs a filter with `IP == X → ALLOW` for
   the stub. After `execve` into the guest, this filter is
   inherited.
2. Guest execs something → handler re-execs tawcroot
   (`--exec-child`). The inherited filter is still active.
3. If tawcroot is PIE, ASLR places the stub at a new address Y.
   The inherited filter says `IP == X → ALLOW`, but the stub is
   at Y — no match — inherited filter returns TRAP. Even if
   `--exec-child` installs a new filter saying `IP == Y → ALLOW`,
   the inherited TRAP beats the new ALLOW. **The stub's own
   syscalls are TRAPped.** The handler recurses/dies.

**With non-PIE** (`-static -no-pie`), the binary is ET_EXEC. The
kernel loads it at the link-time-fixed address every time. The stub
is at the same virtual address across all re-execs. The inherited
filter's `IP == X` check matches the new process's stub. ALLOW.
No new filter needed.

Consequences:
- `--exec-child` **must NOT install a new seccomp filter.** The
  inherited filter is correct. It only reinstalls the SIGSYS
  handler (reset by `execve`) and re-opens the rootfs fd / bind
  table.
- Pick base addresses that avoid the guest's normal mapping
  region. Use `-Wl,--image-base=<addr>` (NDK lld does **not**
  support `-Ttext-segment` — it errors out and explicitly
  recommends `--image-base`). Bases:
  **0x2000000000** for aarch64 (matches proot's `LOADER_ADDRESS`),
  **0x40000000** for x86_64. The aarch64 number can be high
  because `R_AARCH64_CALL26` and friends are PC-relative and the
  binary is internally self-consistent. The x86_64 number is
  forced down to ~1 GB by static-bionic's `libc.a`: it contains
  `R_X86_64_PLT32` references to weak-undefined symbols (e.g.
  `__loader_add_thread_local_dtor` from `__cxa_thread_atexit_impl.o`,
  `__scudo_default_options` from scudo's `flags.o`) that the static
  link resolves to address 0. The PLT32 displacement from our `.text`
  to 0 must fit in a signed 32-bit immediate, so any base above ~2 GB
  fails the link with "relocation R_X86_64_PLT32 out of range." 1 GB
  comfortably clears the typical ET_EXEC default base (0x400000) and
  sits far below where Linux/glibc ld.so loads (~0x7f00_0000_0000),
  while leaving 1 GB of headroom under the 2 GB PLT32 ceiling.
- No ASLR for tawcroot itself. Acceptable — tawcroot is not a
  security boundary (§"What it explicitly is not"), and the guest
  can read `/proc/self/maps` to find our address anyway.

Future: `PR_SET_SYSCALL_USER_DISPATCH` (kernel 5.11+) defines
a *range* of instruction pointers from which syscalls are always
allowed, operating at the syscall entry layer (separate from
seccomp). It is not subject to the stacked-filter precedence
problem. If we ever want to re-enable PIE (e.g. for ASLR hardening),
probe for dispatch at init and fall back to BPF IP-check on older
kernels. gVisor uses dispatch on 5.11+ and seccomp on older
kernels. Our device has kernel 5.4, so this is future work.

### Threading and `vfork` invariants

The handler runs in the calling thread's context. With multiple
guest threads, multiple SIGSYS handlers can run concurrently in
their own stacks. This is safe by construction because:

- Core tawcroot globals (rootfs_fd, bind table, well-known-symlink
  memo cache, the rootfs host-path string) are written exactly twice
  in a process's lifetime: once at supervisor init, and again only by
  the `chroot(2)` handler when the guest issues a chroot. The MVP
  deliberately does **not** maintain a mutable guest-cwd string;
  `AT_FDCWD` resolution derives from the kernel's current cwd via
  raw `getcwd` + reverse translation. The fd-provenance table and
  guest-visible `SIGSYS` shadow are explicit exceptions: fixed-size,
  handler-safe, and updated only through trapped control syscalls.
- All per-call state is stack-local.
- We never call any libc function with hidden mutable state from
  the handler (no `getenv`, no `errno` read, no stdio).

The chroot path-mutation race. `chroot` is the one syscall that
re-writes core globals after init. The writes are sequenced (zero
the length, then bytes, then republish length; deactivate-then-clear
for binds; write the new fd last) so concurrent readers fall into
one of two states: they see the OLD coherent view, or they see a
"length zero" / "no binds" intermediate that every reader interprets
as "path outside the rootfs view → -ENOENT". The latter is a
transient ENOENT the guest retries past in microseconds. Without
explicit memory barriers this isn't a hard guarantee on aarch64, but
the failure mode is bounded: a concurrent reader can at worst observe
a transient ENOENT or wrong-route on a single path-bearing syscall.
For the workloads we target (pacman 6.x's chroot is single-threaded
post-startup; container runtimes that chroot from a worker thread
don't run on Android) this is acceptable. If a future workload needs
strict multi-threaded chroot safety, switch to a seqlock-snapshot
protocol (writer increments seq; readers retry on odd-seen or
mismatch).

`vfork` shares VM with the parent until `execve`. A `vfork`'d child
running our handler must not mutate ordinary globals that the parent
depends on. Any mutable process-wide state outside the chroot/init
exception (fd provenance, guest `SIGSYS` shadow, future namespace
tracking) must use an explicitly designed lock-free snapshot scheme
that is safe for concurrent signal handlers (for example, fixed-size
double-buffered state plus an atomic sequence counter), or it must be
re-derived from kernel state per call. Do not add a plain mutable C
string/global that handlers read while another thread can update it
without the bounded-failure analysis the chroot path has.

### Guest signal/seccomp control

tawcroot owns `SIGSYS`. The seccomp filter is permanent kernel state,
and the whole architecture assumes a live `SA_SIGINFO` handler for
seccomp-originated traps. A guest that installs its own `SIGSYS`
handler, blocks `SIGSYS`, resets the disposition to `SIG_DFL`, or
stacks another seccomp filter can break tawcroot even though it is
"just" making normal Linux syscalls.

Therefore these syscalls are part of the runtime surface, not optional
hardening:

- `rt_sigaction`: virtualize `SIGSYS`. Guest attempts to read or write
  the `SIGSYS` disposition see a guest-shadow value, while the real
  kernel disposition remains tawcroot's handler. Other signals pass
  through. If the guest asks for `SIG_DFL`/`SIG_IGN` on `SIGSYS`, store
  that in the shadow table but do not apply it to the kernel.
- `rt_sigprocmask` / `sigprocmask`: prevent guest code from blocking
  real `SIGSYS`. Maintain a guest-visible shadow mask if needed, but
  clear `SIGSYS` before forwarding the real mask to the kernel.
- `seccomp(SECCOMP_SET_MODE_*)` and `prctl(PR_SET_SECCOMP, ...)`:
  return `-EPERM` without installing the guest's filter. We can't
  honestly install it: stacked seccomp can `KILL_PROCESS` our
  raw_syscall stub, return errno before our path-translation trap, or
  `RET_TRAP` into a guest-owned `SIGSYS` path. Firefox 150.0.3 on
  Arch Linux ARM / OnePlus 9 accepts this denial without a UI warning
  or `unregister_tls_module` abort as of 2026-05-19. Read-only
  seccomp ops (`SECCOMP_GET_ACTION_AVAIL` etc.) still pass through
  verbatim.
- `prctl(PR_GET_SECCOMP)` may return the host truth (`2`) or a
  guest-compatible value if a workload needs it; do not lie in ways
  that encourage a program to install a filter we will reject.

The guest-visible `SIGSYS` shadow state is tiny but still mutable
process state. Store it in fixed-size atomics or a snapshot structure
compatible with the threading rules above; do not protect it with
malloc-backed containers or libc locks from inside the handler.

Tests must cover at least: guest `sigaction(SIGSYS, SIG_DFL)`, guest
blocking `SIGSYS`, guest `prctl(PR_SET_SECCOMP)` / `seccomp(2)` (both
must observe success on `SET_MODE_*` without actually installing the
filter), and a path syscall after each attempt. The expected result
is that path translation still works.

### Why the handler is async-signal-safe

Strictly speaking, an `SA_SIGINFO` handler is required to use only
async-signal-safe APIs. Our handler:

- Reads immutable static globals (the rootfs fd, the bind table) and
  fixed-size atomic/snapshot runtime tables (fd provenance, guest
  `SIGSYS` shadow) — fine, as long as those tables follow the
  threading rules above.
- Issues raw syscalls via the stub — fine, `syscall(2)` is
  async-signal-safe by definition.
- Walks a small stack-local string buffer after the path has been
  copied through the guarded guest-copy helpers.

What we **avoid**: malloc, stdio, locale, anything that takes a
lock. Path buffers are fixed-size on the handler's stack
(`PATH_MAX` is 4096; we can afford it). The bind table is built
once at init and immutable thereafter; the few mutable runtime tables
must be lock-free and handler-safe.

Because guest glibc/ld.so is manually loaded into the same address
space as a statically linked bionic tawcroot, the handler/runtime must
also be treated as a small freestanding runtime. Compile handler,
guest-copy, path, syscall-dispatch, and arch-stub objects with flags
that prevent compiler-inserted runtime dependencies from leaking into
the hot path (`-fno-stack-protector`, no sanitizers, no profiling,
no exceptions/unwind assumptions). Do not read bionic `errno` in the
handler; raw syscalls return negative errno values directly. Any libc
helper used outside the handler but before the manual-load jump must
be explicitly audited under static bionic and inherited seccomp.

We deliberately leave `SA_NODEFER` *off*: the kernel masks SIGSYS
for the duration of the handler. With NODEFER set, a stub-syscall
that erroneously trapped (e.g. if the IP-allowlist predicate
regressed) would re-enter the handler synchronously and stack-
overflow under recursion; without it, the second SIGSYS sits
pending until the outer handler returns, at which point it fires
once and we either succeed or crash cleanly with a single nested
frame. Either knob is defensible, but "non-recursive by default"
is the safer one given our handler state is immutable-or-snapshot
only and the handler has no genuine reason to re-enter itself.

### Handler coding conventions

Refactored conventions every fs-handler follows (June 2026 cleanup):

- **Single guest fetch.** A handler copies the guest path string at
  most once (`fetch_guest_path` into the scratch pool), classifies
  intercepts (`classify_shm`, `tawcroot_proc_shadow_open`,
  `tawcroot_is_proc_self_exe`) against that local copy, and then
  translates the *same bytes* via `translate_local`. Never re-read a
  guest pointer after classifying it — a racing guest thread could
  swap the string between the peek and the translate (double-fetch).
  Handlers without intercepts use `translate_at`, which is fetch +
  `translate_local` in one call.
- **`struct fs_path`** is the unit of translation output: `{fd,
  is_root, path}` — pass `fd`/`path` straight to the `*at` syscall;
  `is_root` means the path resolved to the base directory itself and
  each syscall picks its own semantics ("." for most, AT_EMPTY_PATH,
  or a direct errno).
- **Legacy x86_64 syscalls are `via_at` shims** — rebuild the register
  frame and re-enter the modern *at handler (nr is preserved for
  nr-sensitive handlers). Don't write bespoke legacy bodies.
- **Bounded string building** goes through `tawc_str_append` /
  `tawc_str_append_dec` / `tawc_str_copy` / `tawc_proc_fd_path`
  (strings.c, unit-tested) — no hand-rolled bounded copy loops.
- **Trapped-and-denied syscalls** register `tawcroot_deny_enosys` /
  `tawcroot_deny_eperm` (dispatch.c) with the rationale comment at the
  registration site.
- **/proc shadow synthesis** lives in `proc_shadow.c`; adding a shadow
  file means one synthesizer + one line in `tawcroot_proc_shadow_open`.

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

**Fake-root identity and metadata (MVP, because current proot uses
`-0`):**

- `getuid`, `geteuid`, `getgid`, `getegid` → return 0.
- `setuid`, `setgid`, `seteuid`, `setegid`, `setresuid`,
  `setresgid`, `setreuid`, `setregid` → return 0 only for root-like
  transitions we claim to support; otherwise return the kernel-like
  error. We are not granting host privileges, just preserving the
  guest illusion that it is root inside the rootfs.
- `getgroups` / `setgroups` → report a minimal root-compatible group
  set and accept no-op root-compatible updates.
- `stat`, `lstat`, `fstat`, `fstatat`, `newfstatat`, `statx` →
  after the host syscall succeeds, copy the result through a local
  struct, rewrite uid/gid fields to 0 for rootfs-owned paths/fds, and
  copy the decorated result back to the guest via `copy_to_guest`.
  Host errors pass through unchanged.
- `chown`, `lchown`, `fchown`, `fchownat` → translate paths/fds and
  return success for rootfs-owned files when the requested ownership
  is root-compatible. Do not issue a real host `chown`; app uid cannot
  do it and the on-disk rootfs intentionally remains app-owned.
- `capget`/`capset` and related privilege probes: start with the
  minimal behavior needed by Arch/pacman tests. If a probe fails a
  real workload, add a targeted fake-root response rather than trying
  to model Linux capabilities wholesale.

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
  fall back to a relative `symlinkat` when the source and destination
  are both inside the rootfs view. This mirrors proot's
  `--link2symlink` behavior and is required for the same ALARM
  hardlink cases documented in `notes/proot.md`.
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
- Fake-root identity/metadata syscalls (`getuid`, `stat`, `chown`,
  etc.) needed to match proot `-0`.
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

## Seccomp filter

Hand-written cBPF. The list of trapped syscalls is the runtime
dispatch table — `prod_rootfs_init` walks it via
`tawcroot_dispatch_trap_list` to collect every syscall number with a
non-NULL slot, and `filter.c` emits the BPF program from that list
and installs it. The dispatch table is the single source of truth
shared between handler routing and the allowlist. Program structure:

```
load arch from seccomp_data (offset 4)
if arch != AUDIT_ARCH_<our-arch>: KILL_PROCESS  // can't happen, defense in depth
// cBPF is 32-bit — compare instruction_pointer in two halves
load instruction_pointer[31:0] from seccomp_data (offset 8)
if lo32 != lo32(&tawcroot_raw_syscall_ret): goto not_stub
load instruction_pointer[63:32] from seccomp_data (offset 12)
if hi32 == hi32(&tawcroot_raw_syscall_ret): ALLOW  // our stub
not_stub:
load syscall_nr from seccomp_data (offset 0)
switch (syscall_nr):
  case openat: TRAP
  case stat: TRAP
  ...
  default: ALLOW
```

**Implementation note:** seccomp cBPF operates on 32-bit words.
`seccomp_data.instruction_pointer` is a `__u64` at offset 8 (after
`int nr` at 0 and `__u32 arch` at 4); you must `BPF_LD_ABS_W` both
halves (offsets 8 and 12) and compare each. On aarch64 with base 0x2000000000 the high 32 bits
are non-zero — a filter that only checks the low half would
mis-ALLOW guest code at a coincidentally matching low address.

That's ~45 BPF instructions, well within the kernel's filter
limit.

Two filters are active: Android's existing `untrusted_app` filter
(already loaded by zygote before we got control), plus our filter
(installed on top). The kernel evaluates all filters on every
syscall; the result with the highest precedence wins. Precedence is
the kernel-defined seccomp action order, not a rule we invent in
tawcroot:

```
KILL_PROCESS (0x80000000) > KILL_THREAD (0x00000000) > TRAP (0x00030000) >
ERRNO (0x00050000) > USER_NOTIF (0x7fc00000) > TRACE (0x7ff00000) >
LOG (0x7ffc0000) > ALLOW (0x7fff0000)
```

Do not reimplement this as an arbitrary numeric sort. The constants
are encoded so the documented order works today, but the maintenance
contract is "follow Linux's action precedence"; use the constants and
keep the behavior tests below as the source of truth.

Key interactions with Android's filter:
- Android says ERRNO, we say TRAP → **TRAP wins**. This is exactly
  what we want for the lp64-`access`-on-x86_64 case documented in
  `notes/proot.md`.
- Both filters say TRAP for the same syscall → **one SIGSYS**, with
  `RET_DATA` from the most recently installed filter (ours). No
  double delivery.
- We say ALLOW, Android says TRAP → **TRAP wins**. We cannot
  un-TRAP anything a prior filter TRAPs. This is why
  `--exec-child` must not install a new filter — see §"Why
  non-PIE". **This also means we can receive TRAPs for syscalls
  we didn't ask to trap** — the handler must return `-ENOSYS`
  for NULL dispatch entries, not abort (see §"SIGSYS handler").
  Future Android versions may TRAP additional syscalls.
- If Android ever KILL_PROCESSes a syscall, we lose — but that's
  true for any sandbox technique on Android and isn't unique to us.

### Filter installation as non-root

`PR_SET_NO_NEW_PRIVS` is the magic that lets unprivileged processes
install seccomp filters. Set it before `seccomp(SECCOMP_SET_MODE_FILTER, …)`.
No `CAP_SYS_ADMIN` needed, no SELinux restriction — confirmed
working from Android's `untrusted_app` domain.

Note: bionic does **not** provide a `seccomp()` libc wrapper. Use
`syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog)`
directly.

The filter is installed **once**, during the initial tawcroot
invocation. It is inherited across `fork` and `execve` as kernel
state. Children of the guest will be filtered too, which is what we
want — a `bash` that spawns `pacman` should still translate paths.
**`--exec-child` does NOT install a new filter** — the inherited
filter is correct because the stub address is a link-time constant
(non-PIE). See §"Why non-PIE".

The SIGSYS handler, by contrast, is process-local (memory, not
kernel state). It is duplicated across `fork` but **reset to
`SIG_DFL` across `execve`** — the post-execve guest has the filter
(kernel state, survived) but no handler. The default action for
SIGSYS is process termination. This is the central thing we have to
solve. Two known approaches:

#### Approach A: re-exec into ourselves first

Make the guest's `execve` go through us. Specifically: rewrite
every `execve("/bin/foo", argv, envp)` so it re-invokes our binary
with a state fd:
`execveat(our_binary_fd, "", ["tawcroot", "--exec-child",
"--state-fd=NN"], envp, AT_EMPTY_PATH)`. The state fd contains the
original target path, guest argv strings, current-root state, rootfs
path, bind specs, and tawcroot flags. envp is the *guest's* envp,
passed through unchanged — `--exec-child` must not splice
anything of its own into the environment. Our binary, on detecting
`--exec-child`, reads and closes the state fd, reinstalls the SIGSYS
handler (reset by `execve`) and re-opens the rootfs fd / bind table.
It does **NOT** install a new seccomp filter — the inherited filter
is correct because the stub is at the same address (non-PIE, see
§"Why non-PIE"). It then proceeds to launch the real target via the
PT_INTERP dance below.

**Critical bootstrap constraint:** between the `execveat` that
launches `--exec-child` and the point where the handler + rootfs
fd are fully initialized, the inherited seccomp filter is live but
there is no handler. The default SIGSYS action is process
termination. Therefore **`--exec-child`'s entire init path —
state-fd read, handler install, rootfs fd open, bind-table setup,
ELF load — must issue all its own syscalls through
`tawcroot_raw_syscall()`**, which the inherited filter ALLOWs by
IP. No libc wrappers for TRAPped syscalls (`openat`, `stat`,
`readlink`, `mmap` with path args, etc.) until after the jump to
the guest. `sigaction` is safe (not TRAPped). Static bionic's
`memcpy`, `memset`, string functions are safe (no syscalls). This
is the same discipline as the handler itself — the handler uses
the stub to avoid recursion; `--exec-child` init uses it to
survive the no-handler window and the no-rootfs-fd window.

Pros: clean conceptual model, every guest process is a fresh
tawcroot init.

Cons: every guest exec becomes one host re-exec into tawcroot plus a
manual ELF load/jump for the real target. Per-exec overhead is small
individually but stacks up on fork-heavy workloads (`pacman -Qkk`,
configure scripts, Firefox content-process spawn). Worth measuring
once the MVP runs.

This is the approach we start with. It's morally what proot's
loader does, except our "loader" is the same binary as our entry
point — **but only if `libtawcroot.so` is statically linked.**
Otherwise bionic linker opens libtawcroot's `DT_NEEDED` libs
under the inherited filter with no handler installed, and the
process dies before `--exec-child` runs. See "Known gaps" #1.

#### Approach B: in-process trampoline preserved across execve

Use `mmap(... MAP_FIXED ...)` to put our handler + globals at a
fixed address that we then claim is "owned" via something like
preloading. Doesn't actually work — `execve` clears mappings
unconditionally. The only way to survive `execve` is to be in the
new program's text or be re-loaded.

Verdict: Approach A plus manual guest load. Each guest `execve`
becomes a re-exec of tawcroot. The `--exec-child` process
reinstalls the handler, then manual-loads the real target and jumps
to it without another `execve`. The seccomp filter is inherited so
syscalls keep getting trapped; the handler stays live because the
final transfer to guest code is an in-process jump, not an exec.

This means **the binary must be exec'able** from the guest's
context. In Android terms, that's the `nativeLibraryDir`
(`/data/app/~~<hash>/me.phie.tawc-<hash>/lib/<abi>/libtawcroot.so`)
which has the `apk_data_file` SELinux context, same trick proot
uses for `libproot-loader.so`. So we ship as `libtawcroot.so` and
the path stays in `nativeLibraryDir`.

`libtawcroot.so` is therefore a directly-`execve`d non-PIE static
binary *named* as a shared object — same pattern proot's
`libproot-loader.so` uses (also an ET_EXEC static binary renamed
to `.so` for jniLib extraction). Android's jniLib extractor only
matches on `lib*.so` filenames — it doesn't validate ELF type.
Build with `-static -no-pie` and an explicit `_start`; the build
script links it as an executable, with
`-Wl,-Ttext-segment=<base-addr>` per arch (see §"Why non-PIE"
for address choices) and
`-Wl,-soname,libtawcroot.so` so the jniLib packaging step is
happy. The APK **must** set `extractNativeLibs="true"` in the
manifest (already the case — proot's `libproot-loader.so` has the
same constraint). Without extraction, jniLibs are mmap'd directly
from the APK and don't exist as files the kernel can `execve`.

The explicit `_start` matters. Do not rely on opaque static-bionic
startup code before tawcroot has installed or reinstalled the `SIGSYS`
handler, because the inherited seccomp filter is already live in
`--exec-child`. The startup contract is:

- `_start` parses the raw initial stack enough to find `argc`, `argv`,
  `envp`, and auxv without libc.
- If this is `--exec-child`, all syscalls before the handler is live
  go through `tawcroot_raw_syscall()` and only through the raw stub.
- Any bionic/libc initialization we choose to call must be audited for
  syscalls under the inherited filter. If we skip bionic init for the
  runtime path, document which libc facilities are therefore forbidden
  before the manual guest jump.
- Handler/runtime code does not depend on bionic TLS, `errno`, stdio,
  malloc, pthread state, or property-service initialization. Init code
  may use higher-level helpers only before the first filter install, or
  after the `--exec-child` handler is installed.

Side effect: the binary needs to know its own path (to re-exec
itself). Either bake it in via a `-D` at compile time (brittle —
APK paths change on reinstall) or read `/proc/self/exe` at startup
and stash it. **Two separate stashed paths, two separate uses:**

- **`our_binary_path`** — the real on-disk path of
  `libtawcroot.so` (read via `readlinkat` through the raw stub
  against `/proc/self/exe` at initial startup and in each
  `--exec-child` init, before the handler is installed or before
  the guest exe stash is set — so the kernel returns the real
  path, not the synthesized one). Used by the handler to re-exec
  ourselves on guest `execve`.
- **`guest_exe_path`** — the path the guest *asked* us to exec,
  set by `--exec-child` after it translates the guest's requested
  binary path. Used by the `/proc/self/exe` synthesis handler
  (§"`/proc/self/exe`") to return what the guest expects.

These are distinct static globals. Confusing them is an easy bug:
`our_binary_path` is a host path into `nativeLibraryDir`;
`guest_exe_path` is a rootfs-relative path like `/usr/bin/bash`.

The Kotlin install side rebuilds the argv on every entry, the same
way the bind table is rebuilt for proot/chroot (see
`notes/rootfs-sessions.md` for the single-entry-point invariant).

## Bootstrap & entry

`tawcroot` is invoked from `TawcrootMethod.startInside`, which
builds an argv of the rough shape:

```sh
#!/system/bin/sh
exec /data/app/~~<hash>/lib/arm64-v8a/libtawcroot.so \
     -r /data/data/me.phie.tawc/distros/<id>/rootfs \
     -b /system:/system \
     -b /vendor:/vendor \
     -b /apex:/apex \
     -b /system_ext:/system_ext \
     -b /linkerconfig:/linkerconfig \
     -b /dev:/dev \
     -b /proc:/proc \
     -b /sys:/sys \
     -b /dev/binderfs:/dev/binderfs \
     -b /data/data/me.phie.tawc:/data/data/me.phie.tawc \
     -w /root \
     -- /usr/bin/bash -l "$@"
```

The `-b` set mirrors `ChrootMounter.mountScript`'s mount set + the
proot-method's mount set (`notes/proot.md`), including `/proc`,
`/sys`, `/dev`, and the libhybris Android system-library paths.
Since we don't actually *mount* anything, this is just bind-table
entries; the host paths get directly aliased into the rootfs view.
As in `ProotMethod`, bind sources that do not exist on a given
Android version (`/system_ext`, `/linkerconfig`, sometimes
`/dev/binderfs`) are filtered out when rendering the wrapper rather
than making tawcroot reject the whole argv.

**`/dev/shm` is emulated in-handler via `memfd_create`** — no host
directory bound, no flash-write cost. Path-bearing syscalls under
`/dev/shm/<name>` are intercepted by the SIGSYS handler and routed
to a small (name → memfd) table in `src/shm.c`:

- `openat(O_CREAT)` creates a `memfd_create(name, MFD_ALLOW_SEALING)`,
  stores an internal **non-CLOEXEC** dup at the high reserved-fd
  range, and hands a fresh dup to the guest. `O_EXCL` and `O_TRUNC`
  honored.
- `openat` on an existing name returns a fresh dup of the cached fd.
- `unlinkat` drops the name; the segment lives on as long as any
  fd is open (POSIX shm semantics, kernel refcount).
- `fstatat`/`statx`/`faccessat` synthesize sensible answers for both
  `/dev/shm` (synthetic dir) and `/dev/shm/<name>` (regular file
  with `fstat`-derived size, owner uid 0).

Cross-process visibility for fork+execve patterns (Mozilla parent →
content IPC) is preserved via two mechanisms:

1. The internal memfds are non-CLOEXEC, so they survive the
   handler-driven `execveat` re-exec verbatim — same fd numbers,
   same kernel objects.
2. `exec_state` ferries the (name, fd_int) pairs through the memfd
   handed across re-exec; `--exec-child`'s loader calls
   `tawcroot_shm_register` to rebuild the (name → fd) map without
   re-creating any kernel-side memfd.

Mozilla's IPC SHM is not contended in practice (a tiny spinlock
guards the table; held only across the create/unlink syscalls).
`mmap`/`ftruncate`/`mremap`/etc. operate on the returned fd as
real kernel operations — no further handler involvement.

`-w` sets initial CWD (translated). We also export a small set of
env vars (`HOME`, `USER`, `TMPDIR`, `PATH`) before exec.

## Module layout

Actual layout (flat — the `syscalls/` subdir was planned but the file
count never justified it; kept as one `syscalls_fs.c` until execve
adds an `exec.c`).

```
tawcroot/                            # everything tawcroot-specific lives here
├── README.md           # short: "see notes/tawcroot.md"
├── build               # cross-ABI NDK build (also stages into APK jniLibs)
├── build-fixtures.sh   # NDK build for guest fixtures (loader smoke)
├── test.sh             # runs the cleat orchestrator (host) or pushes
│                       #   testhost via adb (device)
├── Makefile            # incremental host build (production + testhost + cleat tests)
├── include/                            # production headers — no test scaffolding
│   ├── tawcroot.h      # entry-point contract (tawcroot_main)
│   ├── arch.h          # syscall_args struct, includes arch/<arch>.h
│   ├── arch/{aarch64,x86_64}.h  # arch_read_args / arch_write_return
│   ├── chroot.h        # chroot(2) emulation registration
│   ├── dirent_filter.h # getdents64 reserved-fd dirent compaction — pure
│   ├── dispatch.h      # syscall→handler table API
│   ├── errno_neg.h     # negated-errno constants (TAWC_EINVAL == -22)
│   ├── exec_handler.h  # execve handler entry (memfd build + execveat)
│   ├── exec_state.h    # serialized re-exec state struct + (de)serialize
│   ├── fdtab.h         # reserved high-fd table + close/dup protection
│   ├── filter.h        # seccomp filter install API
│   ├── filter_build.h  # pure cBPF program builder
│   ├── handler.h       # SIGSYS handler install + observation
│   ├── identity.h      # fake-root uid/gid registration
│   ├── io.h            # libc-free print helpers + tawc_str_* builders
│   ├── loader_elf.h    # phdr parsing, image bounds, interp pointer
│   ├── loader_exec.h   # --exec-child entry + shebang resolution
│   ├── loader_jump.h   # asm-only stack-pivot + final jump to ld.so/_start
│   ├── loader_map.h    # mmap/mprotect of PT_LOADs, AT_PHDR computation
│   ├── loader_stack.h  # synthesize argv/envp/auxv on a fresh stack
│   ├── path.h          # translator, modes, bind table, open_in_view
│   ├── path_oracle.h   # readlink oracle interface used by resolver
│   ├── path_orchestrate.h # fold→bind→memo→resolve→bind ctx + memo struct
│   ├── path_resolve.h  # symlink walker — operates against an oracle
│   ├── path_scratch.h  # handler-safe PATH_MAX scratch-buffer pool
│   ├── proc_rewrite.h  # /proc/self/maps reverse-translation — pure
│   ├── proc_shadow.h   # /proc shadow-fd synthesis + /proc/self classify
│   ├── raw_sys.h       # tawc_<syscall> wrappers
│   ├── shm.h           # /dev/shm emulation (memfd-backed name table)
│   ├── signal_shadow.h # guest SIGSYS sigaction/sigmask virtualization
│   ├── supervisor.h    # shared per-process bootstrap (prod + --exec-child)
│   ├── syscalls_{control,exec,fs,socket}.h # handler registration entries
│   ├── sysnr.h         # per-arch syscall numbers
│   ├── tawc_string.h   # memcpy/memset/... freestanding-vs-hosted switch
│   ├── tawc_uapi.h     # kernel ABI constants (O_*, AT_*, struct stat, ...)
│   └── usercopy.h      # process_vm_readv-based guarded copy
├── src/                                # production sources — no test scaffolding
│   ├── main.c          # production entry: CLI parse, --exec-child dispatch
│   ├── chroot.c        # chroot(2) emulation — root-view swap on guest call
│   ├── dirent_filter.c # getdents64 compaction — pure
│   ├── dispatch.c      # syscall→handler table storage
│   ├── exec_handler.c  # execve guest-side entry (build memfd state + execveat)
│   ├── exec_state.c    # exec-state (de)serialization — pure
│   ├── filter.c        # install BPF program (seccomp + stub address)
│   ├── filter_build.c  # build BPF program — pure
│   ├── handler.c       # sigsys_handler dispatch, ucontext glue
│   ├── identity.c      # fake-root uid/gid handlers
│   ├── io.c            # io.h print impl (via tawc_write)
│   ├── strings.c       # pure libc-free str/mem helpers + bounded builders —
│   │                   #   also linked into the cleat test runner under hosted
│   │                   #   glibc (tawcroot/tests/unit/test_strings.c)
│   ├── path.c          # translate(), reverse-translate, bind table, memo,
│   │                   #   open_in_view
│   ├── path_fold.c     # absolute-path folder (`.`/`..`/empty/`//`) — pure
│   ├── path_orchestrate.c # fold→bind→memo→resolve→bind staging, binds_reanchor — pure
│   ├── path_resolve.c  # symlink walker — pure, oracle-driven
│   ├── path_scratch.c  # scratch-buffer pool (CAS acquire/release)
│   ├── proc_rewrite.c  # /proc/self/maps line rewriter — pure
│   ├── proc_shadow.c   # /proc shadow memfds (maps/overflow/pci) + classify
│   ├── shm.c           # /dev/shm emulation
│   ├── signal_shadow.c # SIGSYS sigaction/sigmask shadow state
│   ├── supervisor.c    # shared bootstrap: rootfs fd, binds, handler, masks
│   ├── loader_elf.c    # ELF phdr parsing
│   ├── loader_map.c    # PT_LOAD mmap/mprotect, AT_PHDR computation
│   ├── loader_stack.c  # synthesize argv/envp/auxv on a fresh stack
│   ├── loader_exec.c   # --exec-child main: load guest + jump, shebang chain
│   ├── loader_io_prod.c # production loader I/O vtable (raw syscalls)
│   ├── syscalls_{fs,fd,control,exec,socket}.c # per-syscall handlers
│   ├── usercopy.c      # process_vm_readv probe + guarded guest copies
│   └── arch/{aarch64,x86_64}_{stub,loader_jump}.S  # _start, raw syscall stub,
│                                                   # sigreturn trampoline, loader jump
└── tests/                              # everything that isn't shipped in production
    ├── testhost/
    │   ├── include/
    │   │   ├── child.h     # --exec-child entry
    │   │   ├── rootfs_smoke.h # rootfs syscall smoke entry
    │   │   └── smoke.h        # foundation smoke harness
    │   └── src/
    │       ├── testhost_main.c  # argv dispatch (--exec-child / -r ROOTFS / foundation)
    │       ├── child.c          # --exec-child re-entry
    │       ├── rootfs_smoke.c   # rootfs syscall smoke (inline-asm probes)
    │       └── smoke.c          # foundation smoke (trap-contract, raw-syscall exercise)
    ├── unit/                # cleat-direct pure-function tests (no fork)
    │   └── test_strings.c   # tawc_strlen/streq/starts_with/parse_long/int_to_str
    ├── handler/             # cleat tests that fork tawcroot-testhost
    │   ├── steps.{c,h}      # parse [ok ]/[FAIL] lines from testhost stdout and
    │   │                    #   register one cleat test per check (dynamic)
    │   ├── test_foundation_smoke.c      # one dynamic test per foundation step
    │   └── test_rootfs_syscalls_smoke.c # builds fake rootfs, then rootfs smokes
    └── integration/         # cleat tests that fork production tawcroot
        └── programs/        # tiny C guests built by the runner and exec'd under tawcroot
```

The directory is laid out so it could be lifted into its own repo (no
external paths inside `tawcroot/`). The one tawc-app coupling is in
`tawcroot/build.sh`, which stages the production binary into
`app/src/main/jniLibs/<abi>/libtawcroot.so` for APK packaging,
and in `tawcroot/test.sh --device` which sources `scripts/lib/select-device.sh`
to pick the adb target — strip those if splitting.

Build artifacts (per `tawcroot/build.sh`):
- **Production tawcroot** — one static non-PIE ET_EXEC per ABI:
  `libtawcroot.so` for arm64-v8a / x86_64, `tawcroot` for host.
  Shipped as a jniLib like `libproot-loader.so` for the same
  APK-execve reason. No test scaffolding, no `--run-test`, no
  smoke driver, no third-party deps.
- **`tawcroot-testhost`** — same source set + `tawcroot/tests/testhost/src/`,
  compiled with `-DTAWCROOT_TESTHOST`. Built by default for
  `--abi=host`; built for cross-ABIs only with `--testhost`. Not
  packaged into the APK.
- **`tests`** — cleat orchestrator. Built by default for `--abi=host`
  (hosted glibc); cross-compiled for `aarch64`/`x86_64` against bionic
  on demand via `tawcroot/build.sh --abi=<abi> --tests`. The Android
  variant is what `tawcroot/test.sh --device` pushes and runs as adb
  shell; same four-layer suite, same filter syntax, same exit code as
  host mode — only the orchestrator binary changes.

## Build integration

A `tawcroot/build.sh` script alongside `scripts/build-proot.sh`:

- Cross-compile with the NDK's clang for `arm64-v8a` and `x86_64`.
- Pure C11 + a couple of `.S` files for production sources. No
  talloc, no autoconf, no config.h.
- **Statically linked, non-PIE, freestanding**
  (`-static -nostdlib -nostartfiles -no-pie -ffreestanding`).
  Required by both the re-exec architecture (no bionic linker
  before our `_start`) and the seccomp filter's IP-based stub
  allowlisting (stable address across re-execs — see §"Why non-PIE").
  Handler/runtime objects also use `-fno-stack-protector` and no
  sanitizer/profiling instrumentation so guest glibc and
  tawcroot's runtime cannot collide through compiler-inserted
  helper paths. No `dlopen`, no Android property/IPC features
  (we don't use these). Expect ~30 KB binary today; ~1–2 MB once
  the manual ELF loader and full handler land.
- **Fixed base addresses** per arch via `-Wl,--image-base=`:
  0x2000000000 (aarch64), 0x40000000 (x86_64). NDK lld rejects
  `-Ttext-segment` outright; use `--image-base`. The x86_64 base
  is constrained by static-bionic PLT32 weak-undef relocations —
  see §"Why non-PIE" for the full story. The aarch64 base mirrors
  proot's `LOADER_ADDRESS` convention.
- Output `build/tawcroot-<abi>/libtawcroot.so` + strip; testhost
  variant at `build/tawcroot-<abi>/libtawcroot-testhost.so` or
  `build/tawcroot-host/tawcroot-testhost`.
- Gradle `packTawcroot` task copies the production binary into
  `app/src/main/jniLibs/<abi>/libtawcroot.so`. Testhost is
  never copied — it's only used by the cleat orchestrator on host
  and by `tawcroot/test.sh --device` via adb push.
- Clones cleat into `./deps/cleat/` at a pinned commit (gitignored,
  same pattern as `deps/libhybris/`, `deps/libxkbcommon/`, `deps/proot/`) on
  first `--abi=host` run. Pin lives in the build script; bumping
  it is a deliberate change. cleat brings its own vendored STC —
  we do *not* clone STC separately. **cleat is built into the
  test orchestrator only**, not into either tawcroot binary.
- Builds the cleat orchestrator (`build/tawcroot-host/tests`)
  natively with the host toolchain (hosted glibc binary). Also
  cross-builds it for `aarch64`/`x86_64` against bionic when
  `--tests` is passed, landing at `build/tawcroot-<abi>/tests`. The
  Android variant is what `tawcroot/test.sh --device` runs on the
  device — same source set, same filter syntax, same exit code
  semantics as the host build (cleat is plain POSIX C + vendored
  STC; no glibc-only deps). See §"Testing strategy".
- **Host build is incremental** via `tawcroot/Makefile`:
  `gcc -MMD -MP` for header dep tracking, `-j$(nproc)` for
  parallel compile. Warm rebuilds (no source changes) take ~30 ms;
  touching one production source rebuilds + relinks both tawcroot
  binaries in ~300 ms; touching a cleat header rebuilds just the
  test files that include it. For tight inner loops you can call
  `make -C tawcroot` directly — `tawcroot/build.sh --abi=host`
  adds the cleat-clone step on top. Cross-ABI NDK builds stay in
  the bash flow (NDK setup is bash-shaped, and they're not in
  the inner loop).

This keeps it consistent with the existing proot build (proot
ships as `libproot.so` + `libproot-loader.so`; tawcroot ships as
`libtawcroot.so` for production, with `tawcroot-testhost` and
`tests` as host-only test artifacts).

### Source list lives in two places

The production `.c` set is duplicated between `tawcroot/build.sh`
(`SRC_C_PROD`, used for the NDK cross-builds and on-device tests)
and `tawcroot/Makefile` (`PROD_C`, used for the host build that
`tawcroot/test.sh --host` exercises). **Adding a new `.c` file means
editing both.**

The split exists for a reason — the host Makefile uses gcc with
header-dep tracking for fast incremental builds; the cross-build
needs NDK-flavoured bash that the Makefile would clutter — but it's
a correctness trap. The chroot.c regression was exactly this: it
was added to `PROD_C` but missed in `SRC_C_PROD`, so `tawcroot/test.sh
--host` passed (host build was complete) while the device shipped a
binary that didn't even include the chroot handler. (There would
have been a linker error if the cross-build had run, but Gradle's
`buildTawcroot` task didn't list source dirs as inputs and skipped
the rebuild — so the stale pre-fix binary stayed staged in jniLibs.)

Mitigations: Gradle's `buildTawcroot$abi` now lists `tawcroot/src`
and `tawcroot/include` as inputs (so source-only edits invalidate
the cache), and the cross-build *does* fail loudly on link if a
referenced symbol is missing — once it actually runs. If the two
lists drift again, `tawcroot/test.sh --device` is the canonical way to
catch it: running the on-device tests forces a cross-build for the
target ABI and any link error surfaces immediately.

## Installer integration

A new `TawcrootMethod.kt` next to `ProotMethod.kt`. Same shape:

- Build the argv (bind table + chroot exec) in `startInside`.
- Apply the same bootstrap-cache + tar-extract pipeline as
  `ProotMethod`.
- No `/dev/shm` host bind — the SIGSYS handler emulates POSIX shm
  via `memfd_create` (`src/shm.c`). See §"Bootstrap & entry".
- Run `TawcInstaller.installInto` to copy libhybris (and the glvnd
  vendor JSON) into the rootfs as real files at `/usr/lib/hybris/`.

The Kotlin `InstallationMethod` enum already has an `extra` slot
(`metadata.json`); we add `TAWCROOT` as a value and the radio in
`InstallActivity` defaults to it on rootless devices once we're
confident.

`scripts/rootfs-run.sh` reads `metadata.json` to decide between
chroot/proot/tawcroot (today it's just chroot/proot). One more
case in the `case method` switch.

## Testing strategy

**Tests run on the host. Every feature is tested as it is
implemented.** A change without its tests is not a change.

### The two-binary split

Production tawcroot must not contain test scaffolding or diagnostic
argv branches — no smoke driver, no `--run-test` hook, no loader
diagnostics on the production CLI. The production argv surface is
exactly two entries:

- `tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]` — the
  rootfs-mode launch path.
- `tawcroot --exec-child <fd>` — re-entry from the SIGSYS execve
  handler dance (`exec_handler.c` writes exec_state into a memfd
  and re-execs `/proc/self/exe` with this argv). Not test-only —
  the handler uses it during normal guest exec, so it must stay
  reachable in production.

Testhost-only diagnostic flags live behind `#ifdef TAWCROOT_TESTHOST`
in `main.c`:

- `--exec PATH [ARGS...]` — invoke the manual ELF loader directly,
  no path translation, no handler/filter. Lets integration tests
  exercise the loader vtable in isolation.
- `--exec-via-handler PATH [ARGS...]` — drive
  `tawcroot_exec_handler_perform()` end-to-end (memfd write →
  `/proc/self/exe` re-exec → `--exec-child`). When testhost is the
  re-exec target, its own `--exec-child` dispatch checks whether
  `argv[2]` is a bare integer (loader-child path, production
  semantics) or `--state-fd=<n>` (foundation-smoke path).

The test layer lives in two places:

1. **`tawcroot-testhost`** — a *second* binary built from the
   same production sources plus `tawcroot/tests/testhost/src/{testhost_main,
   smoke,child,rootfs_smoke}.c`, compiled with `-DTAWCROOT_TESTHOST`. Same
   `_start`, same raw-syscall stub, same filter, same handler — the
   only difference is `main.c` routes argv-dispatch into
   `tawcroot_testhost_main()` (which lives outside `tawcroot/src/`)
   for non-loader argv, plus the diagnostic loader flags above.
   The smoke driver issues inline-asm syscalls *from inside the
   testhost* so each one's IP is outside the stub allowlist and the
   filter TRAPs into the real handler. Handler-level tests cannot
   live anywhere else: the IP allowlist couples them to the binary
   under test. Testhost is **never** packaged into the APK; it
   exists only so the cleat orchestrator can fork it.

2. **`tests` (cleat orchestrator)** — a glibc/bionic binary at
   `build/tawcroot-{host,aarch64,x86_64}/tests`, built from
   `tawcroot/tests/{unit,handler,integration}/*.c` linked against
   cleat + STC. The host build runs locally; the cross-builds run
   on the device under `tawcroot/test.sh --device`. This is the *only*
   place cleat / STC code ever runs in the project. It owns:
   - filter syntax (full-match regexes against `module`, `name`,
     or `module::name`; multiple args OR'd; see cleat
     docs/test_framework.md),
   - exit code (0 = pass, 1 = fail),
   - the unified report.

   The orchestrator does not share an address space with tawcroot.
   For handler-layer cases it `fork`+`exec`s `tawcroot-testhost`
   with the appropriate argv (e.g. `-r <rootfs>`), captures
   stdout/stderr/exit, and asserts on the result. For
   integration cases it forks the real production `tawcroot` (once
   that binary grows enough of the manual ELF loader to jump to a
   guest). cleat's `test_capture { ... }` is also available for
   self-forking tests when handy.

`tawcroot/test.sh` is a thin wrapper around the cleat
orchestrator; `tawcroot/test.sh` runs everything,
positional args become cleat filters. `--device` mode pushes the
NDK-cross-built orchestrator (plus tawcroot, tawcroot-testhost,
fixtures, and the androidfilter wrap) to the canonical on-device
scratch dir `$TAWC_SCRATCH` (`/data/local/tmp/tawc-dev/`, see
`scripts/lib/tawc-scratch.sh`), then exec's it as adb shell. Same
binary shape as host mode — just a different ABI and a different
`TAWCROOT_TEST_TMPDIR` (`$TAWC_SCRATCH/tt-rootless` vs `/tmp`).
PASSTHROUGH filters propagate to the orchestrator verbatim, exit
code is the orchestrator's own (captured via an `__exit=$?` sentinel
because adb shell isn't always a clean exit-code passthrough).

### Cleat / STC are never linked into production

This is a hard rule:

- `tawcroot/src/` (production) — no cleat, no STC, no glibc, no
  libc at all. Production stays `-static -nostdlib -nostartfiles
  -ffreestanding -no-pie`.
- `tawcroot/tests/testhost/src/` (testhost-only) — same constraints. The
  smoke driver uses the same `tawc_io_*` raw-syscall helpers as
  production. cleat / STC are not reachable here either; this
  binary is freestanding.
- `tawcroot/tests/{unit,hosted,handler,integration}/` (cleat
  orchestrator) — cleat / STC freely. Hosted glibc binary. The
  hosted layer shares an address space with the production code
  under test (that's the point — ASan sees it); the handler /
  integration layers fork the real freestanding binaries.

Every production source compiles under both regimes (the whole
`PROD_C` set is in `PROD_C_FOR_TESTS`); the freestanding-only parts
are exactly `src/arch/*.S`. The raw-syscall extern resolves to the
asm stub in the freestanding binaries and to
`tests/hosted/raw_syscall_host.c` in the orchestrator.

### The five test layers

Every PR-equivalent unit of work should land tests in at least one
of these layers, and ideally a layer-1/1.5 logic test plus a layer-2
or layer-3 functional test.

**Sanitizers**: the host `tests` orchestrator is always built with
ASan+UBSan (`TESTS_SANFLAGS` in `tawcroot/Makefile`,
`-fno-sanitize-recover=all` so any hit fails the run; `test.sh` sets
`ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1`).
The host `tawcroot` / `tawcroot-testhost` binaries (test artifacts
only — the shipped binaries are the uninstrumented NDK cross-builds)
get trap-mode UBSan (`TAWC_UBSAN`): no runtime, UB becomes SIGILL and
the forked test fails. The whole production C tree compiles into the
hosted tests binary: `PROD_C_FOR_TESTS = PROD_C` plus the hosted
raw-syscall shim `tawcroot/tests/hosted/raw_syscall_host.c`, which
provides `tawcroot_raw_syscall` (inline asm, exact `-errno`
convention) since the asm stub carries `_start` and can't link
against glibc. The shim also exposes `tawcroot_test_raw_hook` — a
test-installable interceptor for fault injection and syscall
observation. Only `src/arch/*.S` stays freestanding-only.

1. **Unit (`tawcroot/tests/unit/`)** — pure-function tests,
   cleat-direct, no fixture state: string helpers, path fold/resolve/
   orchestrate (vtable-mocked), loader ELF/map/stack, BPF program
   generation, dirent filter, signal shadow, proc rewrite. Pure C,
   no fork. Sub-second. Runs on every save.
1.5. **Hosted (`tawcroot/tests/hosted/`)** — handler-LOGIC tests
   against the real production handlers, in-process under ASan: the
   harness (`hosted.{h,c}`) builds a tmpdir rootfs, opens+reserves it
   as the current root (supervisor-init steps 1–6, no filter/SIGSYS),
   and `th_sys()` calls dispatch handlers directly with synthesized
   args. "Guest memory" is plain test buffers — usercopy self-targets
   via `process_vm_readv`. The teardown diffs `/proc/self/fd` so fd
   leaks fail tests (the fd table is the product's main leakable
   resource; sanitizers don't track it). `tawcroot_test_raw_hook`
   injects mid-handler faults (EINTR/ENOSPC/EMFILE) and observes the
   translated syscalls the handlers actually issue. New handler-logic
   coverage defaults to this layer; the fork layers below keep what
   genuinely needs the real artifact (filter install, SIGSYS
   delivery, stub IP allowlisting, `_start`/bootstrap).
2. **Handler (`tawcroot/tests/handler/`)** — fork `tawcroot-testhost` with
   chosen argv, capture stdout, parse every `[ok ]` / `[FAIL]` line
   that `tawc_io_step` emits, register one cleat test per check.
   The testhost itself has the inline-asm probes that drive the
   SIGSYS handler; the cleat case sets up fixtures (e.g. fake
   rootfs), execs, and surfaces individual checks via cleat's
   `register_dynamic_tests`. Each parsed pass becomes a no-op cleat
   test; each fail becomes a `register_test_problem` whose message
   contains the step line plus its trailing kv-context. The shared
   helper is `tawcroot/tests/handler/steps.{c,h}`.

   Today's modules: `handler/test_foundation_smoke` (trap-contract +
   raw-syscall sweep + state-fd handoff + `--exec-child` re-exec),
   `handler/test_rootfs_syscalls_smoke` (path translation +
   runtime invariants). Together they
   produce 100+ individually-named cleat tests; one testhost exec
   per file. Adding a new check is a one-line `tawc_io_step()` call
   in the testhost — the cleat side picks it up automatically next
   run.

   Test names are derived from the `tawc_io_step` label. Keep
   labels stable (no embedded runtime values, no unicode arrows /
   dashes); see `tawcroot/tests/testhost/src/smoke.c::exercise_one` for the
   convention (rv goes on a separate kv-line so the label stays
   stable).
3. **Integration (`tawcroot/tests/integration/`)** — full `tawcroot
   -r /tmp/fake-rootfs -- <child>` runs against a host-built fake
   rootfs (a tmpdir tree with well-known files and a static
   `dash`/`busybox` for the `execve` paths). `<child>` programs
   are tiny C binaries built by the test suite from
   `tawcroot/tests/integration/programs/` that exercise specific behaviors:
   "cat /etc/foo returns rootfs content", "openat with absolute
   path translates", "relative openat with `..` is clamped at the
   rootfs root", "bad path pointers return `EFAULT` rather than
   crashing", "getuid/stat/chown preserve the fake-root illusion",
   "guest `close_range` cannot kill tawcroot's internal fds",
   "guest `sigaction(SIGSYS)` / `sigprocmask` cannot disable
   translation", "guest seccomp installation is denied", "linkat
   falls back to symlink on Android-style `EPERM`", "execve of a
   dynamically linked binary reaches the loader through PT_INTERP",
   etc. Today's modules: `test_prod_rootfs` / `test_prod_features` /
   `test_prod_fork` / `test_exec_child` / `test_exec_via_handler` /
   `test_diag_exec`.
4. **Differential (`tawcroot/tests/diff/`, future)** — same `<child>`
   programs as layer 3, run under both proot and tawcroot, diff
   stdout/exit. Cheap once layer 3 exists; high signal because
   proot's behavior is the spec for everything except the
   handful of places we deliberately diverge (fast path,
   `/dev/shm` via memfd, etc.). Not built yet.
- **Synthesized Android-filter tests** (`tawcroot/tests/handler/
  androidfilter/wrap.c` + `tawcroot/tests/handler/test_androidfilter.c`):
  a pre-filter wrapper that installs an Android-`untrusted_app`-shaped
  seccomp filter (RET_TRAP on `openat2`/`faccessat2`/`clone3`, plus
  the lp64 legacy set on x86_64) before exec'ing the testhost. Reuses
  the entire rootfs syscall step list via the `--include-legacy-x86_64` /
  default split — produces the `androidfilter` and (on x86_64)
  `androidfilter_legacy` modules, ~104 cleat tests on x86_64. Catches
  the probe-ordering / handler-recursion / dispatch-coverage bugs
  documented in "Bugs found and fixed during install pipeline
  validation" without needing a device. The wrapper does NOT install a
  SIGSYS handler — default disposition for any trapped syscall before
  tawcroot's handler is up is process termination, mirroring the
  device behavior. See the wrapper's header comment for the
  deliberate-not-trapped exclusions (`execve`/`execveat` so the
  wrapper can launch the child; `close_range` because tawcroot's
  handler re-emits NR 436 internally and Android's filter doesn't
  appear to trap it). Found one bug: the testhost's `prod_rootfs_init`
  ordered `probe_openat2` before `install_handler`, mirroring the same
  bug fixed in production main.c months ago. See
  `tawcroot/tests/testhost/src/rootfs_smoke.c`.
- **Real-workload smoke** (`tawcroot/tests/integration/workload/`): drive
  a `pacstrap`'d Arch chroot through tawcroot on the dev box,
  including a `pacman -Syu`. This is where we measure the perf
  win as a regression test, not just a bench number.

### What needs Android, end to end (small list)

Not part of the standing test loop — these run by hand once when
wiring things up, plus periodically as smoke:

1. APK plumbing (`TawcrootMethod`, jniLib packaging, wrapper
   script generation, dispatch in `rootfs-run`).
2. SELinux execve-from-`nativeLibraryDir` smoke.
3. Final perf comparison vs proot in the real deployment.
4. libhybris/AHB syscall coverage check on a real device.

That's the entirety of the Android-only test surface. Everything
else lives on the dev box.

### On-Android smoke checks

The four Android-only items above are wired into the existing
`scripts/run-integration-tests.sh` harness, which already supports
`TAWC_TARGET=physical|emulator` and abstracts methods via
`RootfsRunner`. Add tawcroot variants of the existing proot/chroot
integration tests — they should be pure dispatch additions, not
new test logic. The emulator covers the lp64 `access`-on-x86_64
case under the synthesized Android filter, and the device covers
libhybris/AHB syscall coverage.

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
   devices. `openat2` is an optional fast path for newer kernels
   (probe at init, use when available). The well-known-directory
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

Deferred tawcroot syscall, proc-shadow, and performance ideas live in [tawcroot-future-work.md](../plans/tawcroot-future-work.md).

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
