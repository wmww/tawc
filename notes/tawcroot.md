# tawcroot — fast rootless chroot via systrap

`tawcroot/` is a from-scratch C implementation of a proot-style fake
chroot, using **seccomp `RET_TRAP` + in-process `SIGSYS` handling**
(the "systrap" technique gVisor uses for its platform layer) instead
of `ptrace`. The goal is a strict superset of proot's TAWC use case —
same compat envelope at meaningfully lower syscall overhead, fewer
compat hacks, and a codebase shaped for our needs rather than
inherited from proot's ptrace heritage.

This doc is the design + implementation plan. The code is being built
fresh. Refer to `proot/` (the Termux fork we currently vendor) as
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

## Language: C + cleat

Plain C (C11), compiled with NDK clang for arm64-v8a and x86_64.
For containers, managed strings, and the test framework we use
[cleat](https://codeberg.org/sphi/cleat) — Sophie's C library that
bundles a unit-test framework and wraps [STC](https://github.com/stclib/STC)'s
generic containers behind cleat's own generics API. cleat is the
*only* third-party dep; we use cleat's vendored STC, not a separate
STC checkout.

cleat lives in `./cleat/`, cloned at a pinned tag and `.gitignore`d
(same pattern as `libhybris/`, `libxkbcommon/`, `proot/`). The
build script clones it on first run; nothing to package separately.

**Use cleat's generics, not raw STC.** When we need a generic
container (vec, map, set, smart pointer), instantiate it through
cleat's wrapper macros, not STC's `#define i_type … #include "stc/vec.h"`
forms directly. The wrappers give us a consistent surface (naming,
iteration, ownership conventions) shared with the rest of Sophie's C
code. Raw STC is fine to *read* through cleat's headers but we don't
write call sites against it.

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
  handler boundary) for benefits cleat/STC can largely replicate.
- **C + cleat** is the simplest thing that works. C is the lingua
  franca of the bits we touch (signal handlers, asm stubs, kernel
  ABI, ELF parsing, BPF). cleat (via STC) fills C's worst gaps —
  managed strings, vecs/maps for the bind table and BPF generator,
  smart-ish pointers for ownership — and gives us a test framework
  in the same dep. We get C's predictability and footprint with a
  meaningful chunk of C++'s ergonomics. The handler avoids cleat
  entirely (no allocation, no hidden control flow); init/parsing/
  tests use it freely.

Constraints baked into the design:
- **In the SIGSYS handler:** no malloc, no cleat/STC, no stdio,
  nothing that takes a lock, and no libc calls at all unless we have
  audited the generated code. Path buffers are fixed-size on the
  handler's stack (`char buf[PATH_MAX]`). The bind table is built
  once at init using cleat containers and converted to a flat C
  array referenced by the handler — the handler walks the array,
  not the cleat-wrapped vec.
- **Outside the handler** (init, argv parsing, ELF reading,
  tests): cleat freely, via its generics surface.
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
mappings show host-side rootfs paths there — **nested
`chroot(2)` tracking** — proot fakes it by tracking an inner-root
prefix and adjusting translation; we currently `-EPERM` it and
will likely revisit when an ALARM/pacman flow trips on it —
full `io_uring` SQE rewriting, perhaps in time binder-fd
rewriting) we'll likely end up wanting too as we feed more
workloads through tawcroot. Add on demand, not proactively.

Specifically for nested `chroot`: don't *implement* it now, but
don't paint the architecture into a corner that makes adding it
hard. Concretely:

- **Path translation must take its "current root" from a per-
  process state snapshot, not scattered statics.** Today that
  snapshot has `rootfs_fd`, bind entries, and an empty current-root
  prefix; a future nested-chroot implementation adds an `inner_root`
  prefix string and the translator consults it before falling through
  to `rootfs_fd`. The `-EPERM` `chroot` handler still exists, but
  flipping it to "publish a new state snapshot with the requested
  inner root and return 0" is a localized change. Because SIGSYS
  handlers can run concurrently in multiple threads, this future
  publish step must use the lock-free snapshot rules in §"Threading
  and `vfork` invariants", not an in-place mutable string.
- **Per-process state is preserved across `fork` naturally**
  (it's just process memory) and re-established by
  `--exec-child` on every guest exec — so the same plumbing
  that carries the rootfs fd carries the inner-root prefix
  later. The Approach-A re-exec therefore ferries "the per-process
  tawcroot state" forward, not just "the rootfs path", through the
  versioned `exec_state_fd` described below. That blob has room for
  more fields than we use today.
- **`getcwd` reverse-translation already needs a "current root"
  notion** (to decide what prefix to strip); writing it against
  the per-process state struct from the start costs nothing and
  means nested-chroot's `getcwd` story drops in for free.

Future-Claude implementing nested chroot should be able to do it
in one `.c` file plus a couple of fields on the state struct,
not by restructuring the translator.

The architecture is therefore deliberately structured to make adding
those things later cheap, not just to ship MVP fast:

- **Per-syscall handler files** (`src/syscalls/fs.c`, `exec.c`,
  `proc.c`, `deny.c`) so a new feature is "add a `.c` and a
  dispatch entry," not "edit a 2 kLoC file."
- **The dispatch table is a fixed array indexed by syscall number**,
  with `NULL` for unhandled. Adding a syscall means filling a
  slot and adding it to the BPF allowlist — no ordering hazards.
- **The BPF filter is generated from a syscall list** at build
  time, single source of truth shared with the dispatch table.
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
handler immediately before re-execing tawcroot. It is passed as
`--state-fd=<n>` in the child argv, intentionally *without*
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
CLI or through `--state-fd=<n>` on re-exec. This makes "envp passes
through verbatim" practical: the child may enumerate `environ` only
to copy the guest environment onto the synthesized guest stack, not
to interpret tawcroot settings.

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
`process_vm_writev` against our own pid: the kernel validates the
guest address and returns `-EFAULT` without delivering SIGSEGV to
tawcroot. The helpers build small stack-local `iovec`s and issue the
syscalls through `tawcroot_raw_syscall()`. If a target kernel lacks
`process_vm_*` or Android policy blocks it, use an arch-local
fault-recovery path implemented in tawcroot code, not
`memcpy`/`strlen` directly.

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

There are two related addresses, and mixing them up breaks the
whole design:

- `seccomp_data.instruction_pointer` and `siginfo->si_call_addr`
  identify the syscall instruction that triggered seccomp.
- The saved program counter in `ucontext_t` is the resume PC; for
  `SECCOMP_RET_TRAP` it is already positioned as though the syscall
  instruction completed, so resuming after the handler continues at
  the instruction after `SYSCALL`/`SVC`.

The BPF filter therefore checks
`instruction_pointer == &tawcroot_raw_syscall_insn` and `RET_ALLOW`s
just that one instruction address. Anything else executing a
SYSCALL/SVC is guest code and follows the normal trap table.

(On aarch64 the equivalent uses `svc #0`; same label-on-the-insn
pattern.)

The stub symbols (`tawcroot_raw_syscall`,
`tawcroot_raw_syscall_insn`, `tawcroot_raw_syscall_ret`) should be
hidden (`.hidden` directive in the asm) and the asm function placed
in `.text` with no special section attributes. Under static linking
there's no PLT to worry about, but hidden visibility still ensures
the symbols can't be interposed by code the guest later mmaps (some
debug/ptrace tooling does this).

**Must-test before phase 1:** a tiny Android smoke binary installs a
filter that allows exactly `&tawcroot_raw_syscall_insn`, traps a
normal syscall from elsewhere, and records both `si_call_addr` and
the saved resume PC from `ucontext_t` for x86_64 and aarch64. If
the observed filter address is not the syscall-instruction label,
stop and fix the stub/filter contract before implementing path
translation. The same smoke must issue every raw syscall the handler
or `--exec-child` bootstrap plans to use (`openat`, `read`, `write`,
`close`, `mmap`, `mprotect`, `munmap`, `getcwd`, `readlinkat`,
`fstatat`/`statx`, `fcntl`, `execveat`, state-fd creation, etc.).
Our IP allowlist bypasses only our own filter; it cannot override
Android's already-stacked zygote filter. If Android `TRAP`s or
`KILL`s one of these raw syscalls from the stub, pick a different
primitive before building on it.

### Why non-PIE

The IP-based allowlisting in the BPF filter bakes
`&tawcroot_raw_syscall_insn` as a fixed address literal into the
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
  region. Use `-Wl,-Ttext-segment=<addr>`: **0x2000000000** for
  aarch64, **0x600000000000** for x86_64 (same convention proot's
  loader uses — high enough to avoid guest binary + ld.so + shared
  libs, low enough to be in valid user address space).
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

- Core tawcroot globals (rootfs_fd, bind table, resolver state) are
  immutable post-init. The MVP deliberately does **not** maintain a
  mutable guest-cwd string; `AT_FDCWD` resolution derives from the
  kernel's current cwd via raw `getcwd` + reverse translation. The
  fd-provenance table and guest-visible `SIGSYS` shadow are explicit
  exceptions: fixed-size, handler-safe, and updated only through
  trapped control syscalls.
- All per-call state is stack-local.
- We never call any libc function with hidden mutable state from
  the handler (no `getenv`, no `errno` read, no stdio).

`vfork` shares VM with the parent until `execve`. The same
immutability/snapshot rule covers it: a `vfork`'d child running our
handler must not mutate ordinary globals that the parent depends on.
Any mutable process-wide state (fd provenance, guest `SIGSYS` shadow,
future nested-chroot tracking) must use an explicitly designed
lock-free snapshot scheme that is safe for concurrent signal handlers
(for example, fixed-size double-buffered state plus an atomic sequence
counter), or it must be re-derived from kernel state per call. Do not
add a plain mutable C string/global that handlers read while another
thread can update it.

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
- `seccomp` and `prctl(PR_SET_SECCOMP, ...)`: reject with `-EPERM` or
  `-ENOSYS` for MVP. Allowing the guest to stack filters means future
  traps may arrive with different `RET_DATA` and different intended
  semantics, and `KILL`/`ERRNO` actions could preempt translation.
- `prctl(PR_GET_SECCOMP)` may return the host truth (`2`) or a
  guest-compatible value if a workload needs it; do not lie in ways
  that encourage a program to install a filter we will reject.

The guest-visible `SIGSYS` shadow state is tiny but still mutable
process state. Store it in fixed-size atomics or a snapshot structure
compatible with the threading rules above; do not protect it with
malloc-backed containers or libc locks from inside the handler.

Tests must cover at least: guest `sigaction(SIGSYS, SIG_DFL)`, guest
blocking `SIGSYS`, guest `prctl(PR_SET_SECCOMP)`, and a path syscall
after each attempt. The expected result is that path translation still
works.

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

1. **`/proc` self-magic.** `/proc/self/{exe,cwd,root}` and
   `/proc/<pid>/{exe,cwd,root}` for our own pid: synthesize from
   the stashed original guest-requested exec path (see
   §"`/proc/self/exe`" — the kernel's view points at the dynamic
   linker after our exec dance, not at anything the guest would
   recognize). Other `/proc` entries pass through unchanged.

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

   **Optional optimization for kernel ≥ 5.6:** for `openat`
   specifically, use `openat2` with `RESOLVE_IN_ROOT` to let the
   kernel do the clamping (kernel treats our rootfs fd as `/`,
   `..` at the top stays at the top, absolute symlinks resolve
   relative to the rootfs fd). Probe for `openat2` at init with
   a single call; if it returns `-ENOSYS`, fall back to manual
   canonicalization for all paths. **Our primary test device
   (OnePlus 9, Android 14, kernel 5.4) predates `openat2`**, so
   manual canonicalization is the primary code path and must be
   correct and fast on its own.

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
  or rebinds. We start with reject (`-EPERM`) and revisit if a
  guest actually needs to chroot inside the chroot.
- `pivot_root`, `mount`, `umount2` — return `-EPERM`. Same logic.
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

- `mknod`, `mknodat` — translate. Will likely fail with `-EPERM`
  for character/block devices because Android `untrusted_app`
  doesn't permit `mknod`. Pass the error through.

- `mount`, `umount`, `setxattr`/`getxattr`/`*xattr` family —
  translate paths. Most will fail at the kernel layer; that's
  fine.

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
                       ["tawcroot", "--exec-child",
                        "--state-fd=NN"],
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

For `/proc/self/exe` *opens* (e.g. `open("/proc/self/exe",
O_RDONLY)` to re-read the binary), we translate to the stashed
guest path through the normal rootfs-relative path: the open
reaches the actual binary on disk.

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

## Seccomp filter

Hand-written cBPF. The list of trapped syscalls is defined at build
time (a single header shared with the dispatch table — one source of
truth); `filter.c` emits the BPF program from that list at runtime
and installs it. Program structure:

```
load syscall_nr from seccomp_data
load arch from seccomp_data
if arch != AUDIT_ARCH_<our-arch>: KILL_PROCESS  // can't happen, defense in depth
// cBPF is 32-bit — compare instruction_pointer in two halves
load instruction_pointer[31:0] from seccomp_data (offset 16)
if lo32 != lo32(&tawcroot_raw_syscall_insn): goto not_stub
load instruction_pointer[63:32] from seccomp_data (offset 20)
if hi32 == hi32(&tawcroot_raw_syscall_insn): ALLOW  // our stub
not_stub:
switch (syscall_nr):
  case openat: TRAP
  case stat: TRAP
  ...
  default: ALLOW
```

**Implementation note:** seccomp cBPF operates on 32-bit words.
`seccomp_data.instruction_pointer` is a `__u64` at offset 16;
you must `BPF_LD_ABS_W` both halves (offsets 16 and 20) and
compare each. On aarch64 with base 0x2000000000 the high 32 bits
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

The Kotlin install side rewrites the wrapper script on every
chroot entry the same way `enter.sh` is rewritten today
(`notes/installation.md` §"Mount lifecycle" — same idea).

## Bootstrap & entry

`tawcroot` is invoked from a small wrapper script that the
installer writes (`enter.sh` analogue). Roughly:

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

**`/dev/shm` is never disk-backed under tawcroot.** The proot
method's app-writable `/dev/shm` directory bind exists because
proot can't intercept `memfd_create`/`shm_open` shape mismatches
cleanly; tawcroot must not inherit that workaround. Instead,
`/dev/shm` is handled in-handler: `shm_open` and any path-bearing
syscall under `/dev/shm/<name>` is emulated via `memfd_create` (or
an equivalent anon-fd primitive) and a small in-process name table,
so guest code sees POSIX shm semantics without any host directory
backing it. See the memfd-extension issue in `issues/` for the
shape; tawcroot is the place that fix lands. No `tawcroot-dev-shm`
directory, no `-b …:/dev/shm` entry, and the installer's
`TawcrootMethod` must not create or reference such a path.

`-w` sets initial CWD (translated). We also export a small set of
env vars (`HOME`, `USER`, `TMPDIR`, `PATH`) before exec.

## Module layout

```
tawcroot/
├── BUILD               # one-line note: built by client/build-tawcroot
├── README.md           # short: "see notes/tawcroot.md"
├── include/
│   ├── tawcroot.h      # public types: bind_entry, syscall_args, etc.
│   ├── arch.h          # selects arch/<arch>.h based on __aarch64__/__x86_64__
│   └── log.h           # tiny logger (write to stderr, gated by argv/state flag)
├── src/
│   ├── main.c          # argv parse, init, exec
│   ├── child.c         # --exec-child re-entry
│   ├── filter.c        # build + install seccomp filter
│   ├── handler.c       # sigsys_handler dispatch, ucontext glue
│   ├── path.c          # translate(), reverse_translate(), bind table
│   ├── elf.c           # read PT_INTERP, execve handling
│   ├── identity.c      # fake-root uid/gid/stat/chown decoration
│   ├── usercopy.c      # guarded guest memory copy helpers
│   ├── syscalls/
│   │   ├── fs.c        # openat, stat family, mkdir, unlink, …
│   │   ├── exec.c      # execve, execveat
│   │   ├── proc.c      # /proc/self/exe synthesis, getcwd reverse
│   │   └── deny.c      # mount, chroot, pivot_root → -EPERM
│   └── arch/
│       ├── x86_64.h    # arch_read_args, arch_write_return
│       ├── x86_64_stub.S
│       ├── aarch64.h
│       └── aarch64_stub.S
├── tests/
│   ├── unit/           # cleat-driven tests for path.c, filter.c, elf.c, …
│   └── integration/    # one binary per scenario, run under tawcroot
└── Makefile            # plain make, no autoconf nonsense
```

Build artifacts: one static binary per ABI (`libtawcroot.so` —
ET_EXEC despite the `.so` name; shipped as a jniLib like
`libproot-loader.so` for the same APK-execve reason), no separate
loader binary.

## Build integration

A `client/build-tawcroot` script alongside `client/build-proot`:

- Clones cleat into `./cleat/` at a pinned tag/commit (gitignored,
  same pattern as `libhybris/`, `libxkbcommon/`, `proot/`) on
  first run. Pin lives in the build script; bumping it is a
  deliberate change. cleat brings its own vendored STC — we do
  *not* clone STC separately, and we use cleat's generics
  wrappers, not raw STC's.
- Cross-compile with the NDK's clang for `arm64-v8a` and `x86_64`.
- Pure C11 + `-Icleat/include` (which transitively exposes the
  bundled STC) + a couple of `.S` files. No talloc, no autoconf,
  no config.h.
- **Statically linked, non-PIE** against bionic's `libc.a`
  (`-static -no-pie`). Required by both the re-exec architecture
  (no bionic linker before our `_start`) and the seccomp filter's
  IP-based stub allowlisting (stable address across re-execs —
  see §"Why non-PIE"). Handler/runtime objects also use
  `-fno-stack-protector` and no sanitizer/profiling instrumentation
  so guest glibc and tawcroot's static bionic runtime cannot collide
  through compiler-inserted helper paths. No `dlopen`, no Android
  property/IPC features (we don't use these). Expect ~1–2 MB binary.
- **Fixed base addresses** per arch via `-Wl,-Ttext-segment=`:
  0x2000000000 (aarch64), 0x600000000000 (x86_64). These mirror
  proot's `LOADER_ADDRESS` convention — high enough to avoid guest
  binary + ld.so + shared lib mappings, within the valid user
  address space.
- Output `build/tawcroot-<abi>/libtawcroot.so` + strip.
- Gradle `packTawcroot` task copies into
  `server/app/src/main/jniLibs/<abi>/libtawcroot.so`.
- Builds the host-side test binary (`build/tawcroot-host/tests`)
  natively **with the host toolchain** (not NDK — static bionic
  binaries don't run on desktop Linux). Runs it as part of the
  script's default target — see §"Testing strategy". Never
  cross-compiled; tests don't ship on-device.

This keeps it consistent with the existing proot build (proot
ships as `libproot.so` + `libproot-loader.so`; tawcroot is a single
`.so`).

## Installer integration

A new `TawcrootMethod.kt` next to `ProotMethod.kt`. Same shape:

- Generate the wrapper script (`enter.sh` equivalent).
- Apply the same bootstrap-cache + tar-extract pipeline as
  `ProotMethod`.
- **Do not** create a disk-backed `/dev/shm` directory or bind one
  in. tawcroot emulates `/dev/shm` in-handler via `memfd_create`
  (see §"Bootstrap & entry"). Skipping the directory is part of the
  contract — if Firefox or anything else trips on `/dev/shm` under
  tawcroot, fix it in the handler, not by reintroducing a host
  directory.
- Apply the libhybris symlinks (`LibhybrisLinker.link` works
  unchanged — the rootfs layout is the same).

The Kotlin `InstallationMethod` enum already has an `extra` slot
(`metadata.json`); we add `TAWCROOT` as a value and the radio in
`InstallActivity` defaults to it on rootless devices once we're
confident.

`client/tawc-chroot-run` reads `metadata.json` to decide between
chroot/proot/tawcroot (today it's just chroot/proot). One more
case in the `case method` switch.

## Testing strategy

**Tests run on the host. Every feature is tested as it is
implemented.** A change without its tests is not a change. The
test framework is cleat's, the same dep that gives us containers
— one less thing to bring in.

The test binary is built natively for the dev box (x86_64 Linux),
not cross-compiled. It does not ship on-device. `client/build-tawcroot`
runs it as part of its default target; CI runs it on every push.
Failing tests fail the build.

What "tested as implemented" means in practice — *every* PR-equivalent
unit of work touches at least:

1. A unit test for the new pure-function logic (path translation
   case, BPF generator entry, ELF helper, bind-table lookup, …).
2. A handler-level integration test that runs `tawcroot -r
   /tmp/fake-rootfs -- /some/test-program`, where `test-program`
   is a small built-by-the-test-suite C binary that exercises the
   syscall(s) via direct `syscall(2)` calls (so we don't depend
   on libc's choice of which `*at` form to use), checks the
   results, and exits 0/non-0.
3. For trapped syscalls: a coverage assertion that the syscall
   appears in both the BPF allowlist *and* the dispatch table
   (the "without all four it's not done" rule from §"Maintenance
   contract").

### Layers, in order of how often they run

- **Pure unit tests** (cleat-driven, in `tests/unit/`): path
  parsing/translation/reverse-translation, bind-table longest-prefix
  match, ELF `PT_INTERP` extraction, BPF program generation
  (cross-checked against libseccomp's `seccomp_export_pfc` for
  human-readable diff). Pure C functions, no syscalls, no fork.
  Sub-second. Runs on every save in a `make watch` loop.
- **Filter unit tests** (`tests/unit/filter_*`): build the BPF
  program in-process, install it via `seccomp(SECCOMP_SET_MODE_FILTER, …)`
  in a child, fire each interesting syscall, observe the action
  via `SIGSYS` siginfo. Validates per-syscall decisions for real,
  not just statically.
- **Integration tests** (`tests/integration/`): full `tawcroot
  -r /tmp/fake-rootfs -- <child>` runs against a host-built fake
  rootfs (a tmpdir tree with well-known files and a static
  `dash`/`busybox` for the `execve` paths). `<child>` programs
  are tiny C binaries built by the test suite that exercise
  specific behaviors: "cat /etc/foo returns rootfs content",
  "openat with absolute path translates", "relative openat with
  `..` is clamped at the rootfs root", "bad path pointers return
  `EFAULT` rather than crashing", "getuid/stat/chown preserve the
  fake-root illusion", "guest `close_range` cannot kill tawcroot's
  internal fds", "guest `sigaction(SIGSYS)` / `sigprocmask` cannot
  disable translation", "guest seccomp installation is denied",
  "linkat falls back to symlink on Android-style `EPERM`", "execve
  of a dynamically linked binary reaches the loader through PT_INTERP",
  etc.
- **Synthesized Android-filter tests**: a pre-filter wrapper that
  installs an Android-`untrusted_app`-shaped seccomp filter
  (RET_ERRNO on a few syscalls per arch) before exec'ing
  tawcroot. Validates the lp64 `access`-on-x86_64 quirk and the
  TRAP-vs-ERRNO precedence rule on plain Linux, no emulator
  required.
- **Real-workload smoke** (`tests/integration/workload/`): drive
  a `pacstrap`'d Arch chroot through tawcroot on the dev box,
  including a `pacman -Syu`. This is where we measure the perf
  win as a regression test, not just a bench number.

### What needs Android, end to end (small list)

Not part of the standing test loop — these run by hand once when
wiring things up, plus periodically as smoke:

1. APK plumbing (`TawcrootMethod`, jniLib packaging, wrapper
   script generation, dispatch in `tawc-chroot-run`).
2. SELinux execve-from-`nativeLibraryDir` smoke.
3. Final perf comparison vs proot in the real deployment.
4. libhybris/AHB syscall coverage check on a real device.

That's the entirety of the Android-only test surface. Everything
else lives on the dev box.

### On-Android smoke checks

The four Android-only items above are wired into the existing
`testing/run-integration-tests.sh` harness, which already supports
`TAWC_TARGET=device|emulator` and abstracts methods via
`ChrootRunner`. Add tawcroot variants of the existing proot/chroot
integration tests — they should be pure dispatch additions, not
new test logic. The emulator covers the lp64 `access`-on-x86_64
case under the *real* zygote filter (vs the synthesized version in
the host suite), and the device covers libhybris/AHB syscall
coverage.

## Phasing

- **Phase 0 — Foundation smoke**: static non-PIE binary, raw syscall stub,
   BPF IP allowlist against `tawcroot_raw_syscall_insn`, inherited
   seccomp across self-exec, non-`CLOEXEC` `exec_state_fd` handoff,
   SIGSYS handler reinstall, and Android-filter compatibility for
   every raw syscall used by the handler/bootstrap. This must pass
   on Android x86_64 and aarch64 before path translation work starts.
- **Phase 0.5 — Runtime invariant protection**: reserve and protect internal
   fds, trap `close*`/`dup*`/`fcntl`, virtualize guest `SIGSYS`
   disposition/mask, deny guest seccomp installation, and prove a path
   syscall still works after guest attempts to close all fds, reset
   `SIGSYS`, block `SIGSYS`, and install its own seccomp filter.
- **Phase 1 — MVP path translation (host-side)**: argv parse, rootfs fd,
   filter for openat/stat/access/getuid/chown/linkat, SIGSYS
   handler, guarded guest-copy helpers, `arch_*` helpers for x86_64,
   absolute + relative path translation with `..`/symlink clamping,
   fake-root uid/stat/chown behavior, hardlink-as-symlink fallback,
   and handler-level tests that issue direct syscalls from a static
   test binary already running under the filter. No dynamic guest
   shell yet; phase 2 is what makes normal `execve` work.
- **Phase 2 — execve handling**: ELF/PT_INTERP, re-exec dance, in-process
   manual loader (PT_LOAD mmap + auxv synth + entry jump — see
   "Known gaps" #2), multi-process correctness, `/proc/self/exe`,
   `getcwd` reverse. Exit criteria: dynamically linked `/bin/true`
   and `/bin/sh -c "ls /"` run from inside a fake rootfs. The manual
   loader is the long pole.
- **Phase 3 — Full trapped syscall surface**: every syscall in "Which
   syscalls need trapping" above.
- **Phase 4 — Emulator integration**: `client/build-tawcroot`, jniLib
   packaging, `TawcrootMethod.kt`, dispatch in `tawc-chroot-run`,
   wrapper script. Run `pacman -Syu` to completion.
- **Phase 5 — aarch64 port**: arch/aarch64 files, run on device, libhybris
   smoke test.
- **Phase 6 — Hardening + perf**: stacked-filter weird cases, Firefox-
   specific stuff, measure and tune.

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

2. **In-process program loader for `--exec-child`** (the *guest*
   loader, distinct from gap #1's static-link decision for
   tawcroot itself). §"execve handling in detail" explains
   *why* we can't `execve` into ld.so (signal handlers reset,
   our SIGSYS handler dies, ld.so's first path-bearing syscall
   kills the process). The fix is to manually load the binary
   + ld.so via `mmap` and jump to ld.so's entry,
   proot-loader-style. Concretely we need:

   This is the highest-risk part of tawcroot. The rest of the design
   is ordinary syscall rewriting; this part has to reproduce enough of
   kernel `execve`'s ELF setup that glibc's dynamic linker believes it
   started normally. Treat it as a separate milestone with its own
   smoke binary before broadening the syscall surface. If we cannot
   make a dynamically linked `/bin/true` and `/bin/sh -c true` run
   under this loader on both host Linux and Android, tawcroot is not a
   viable proot replacement in this form.

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
     stack lives on a freshly `mmap`'d region (~128 KB +
     `PROT_NONE` guard page), not on our existing stack.
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
     `proot/src/loader/assembly-{arm64,x86_64}.h` for the exact
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

   Reference: `proot/src/loader/loader.c` (~150 lines of logic)
   does the mmap+auxv-patch+jump dance; the ELF parsing and
   load-script generation lives in `proot/src/execve/enter.c`
   + `exit.c` (~1400 lines). Our version is simpler because
   we do everything in-process (no ptrace, no cross-process
   memory writes, no load-script serialization). Plan for
   ~500–800 lines in a dedicated `src/loader.c`. This is phase
   2 work — nothing in the rest of the design works without it
   for dynamically linked guests.

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

## Open questions / things to revisit

1. **`io_uring` MVP behavior: deny, not pass-through.** If we
   leave io_uring un-trapped, the guest can submit path-bearing
   SQEs (`OPENAT`, `STATX`, etc.), the kernel reads them from
   shared memory, sees host-relative paths, and silently opens
   *host* files — bypassing every translation rule. That's a
   correctness footgun, not just missing-feature. Trap
   `io_uring_setup` and return `-ENOSYS`; programs that probe
   for io_uring fall back to their non-uring paths cleanly. Full
   interception (the rough design below) is the *next* step,
   not the only step. Adding the deny is ~5 lines.

   Rough design for full interception when something we ship
   actually exercises it:

   - **Chokepoint:** every submission still flows through
     `io_uring_enter` (or a ring `mmap` followed by the kernel
     reading SQEs at enter-time), *unless* the ring was created
     with `IORING_SETUP_SQPOLL`. SQPOLL needs `CAP_SYS_NICE`,
     which Android's `untrusted_app` domain doesn't grant — so
     we reject SQPOLL in our `io_uring_setup` handler and rely on
     the enter-time chokepoint for everything else.
   - **Interception:** trap `io_uring_setup`/`io_uring_register`/
     `io_uring_enter` and the ring's `mmap`. On enter, walk SQEs
     from cached head to current tail; for each path-bearing op
     (`OPENAT`, `OPENAT2`, `STATX`, `RENAMEAT`, `UNLINKAT`,
     `LINKAT`, `SYMLINKAT`, `MKDIRAT`, …), allocate a translated-
     path buffer we own, rewrite the SQE's path pointer to it,
     stash the buffer in a pending list keyed by `user_data`.
     Forward enter to the kernel; drain CQEs to free pending
     buffers as ops complete. The spec guarantees the app doesn't
     mutate SQEs after advancing the SQ tail, so the rewrite
     window between "tail advance" and "kernel reads SQE" is
     clean.
   - **Per-op shapes:** each path-bearing opcode needs a small
     translator (mostly reusing `path_translate()`); `OPENAT2`
     and `RENAMEAT2` have extra pointer hops. New `IORING_OP_*`
     show up each kernel release, so we keep an explicit allowlist
     and pass through unknowns with a warning.
   - **Size:** likely ~500–1000 lines as a self-contained
     `src/uring.c` module. Doesn't disturb the per-syscall
     dispatch; one-time chunk of work, not a continuous tax.

   Why we still defer: nothing we ship today exercises it, the
   "shape it like our other modules" expansion structure (§"Designed
   for expansion") absorbs it cleanly when prioritized, and getting
   the buffer-lifetime tracking right deserves dedicated focus
   rather than being mixed into MVP work.

2. **Small-thread-stack overflow risk.** A path-bearing TRAP from
   a thread with a tiny stack (Go runtime M threads, Firefox
   sandbox children, glib worker pools) runs our handler on that
   stack. The handler's ~4 KB `PATH_MAX` buffer plus a couple
   frames is 5–6 KB; on an 8 KB worker stack with frames already
   used, this can overflow. We're shipping without `sigaltstack`
   (it's per-thread, doesn't inherit across `clone`, and would
   need per-thread-creation interception). Realistic risk is low
   — programs with tiny worker stacks tend not to issue
   path-bearing syscalls from those threads — but if a real
   workload trips this, the right fix is *not* sigaltstack: it's
   shrink the on-stack path buffer to 512 bytes and fall back to
   an init-allocated arena for longer paths. Document the escape
   route; don't pre-emptively design around it.

3. **`/proc/<pid>/maps` reverse-translation.** After manual-load,
   `/proc/self/maps` shows ld.so + libraries with their
   *host*-side rootfs paths (`<rootfs>/lib64/ld-linux…`). Programs
   that grep their own maps for library locations (some sandboxes,
   some tracers) see paths that don't exist in their world view.
   proot wraps the read to rewrite paths; we don't, in MVP. Add to
   the "expand on demand" inventory.

4. **`PR_SET_SYSCALL_USER_DISPATCH` (kernel 5.11+).** This kernel
   mechanism defines a contiguous code address range from which
   syscalls pass through without seccomp evaluation, plus a
   per-thread userspace selector byte to toggle interception
   on/off. gVisor uses it on 5.11+ kernels in preference to
   seccomp BPF for syscall trapping. Benefits for tawcroot:
   - Eliminates BPF evaluation overhead on every syscall.
   - Operates at a different kernel layer than seccomp — not
     subject to the stacked-filter precedence problem, which
     would allow PIE builds with ASLR.
   - Per-thread selector byte means we can disable interception
     while the handler issues syscalls, without the IP-based
     BPF check.

   Our primary device has kernel 5.4, so this is future work.
   Probe at init; fall back to the BPF IP-check approach on
   older kernels. When we do implement it, the non-PIE
   requirement could be relaxed on 5.11+ devices (build two
   variants, or ship non-PIE and accept that dispatch is just a
   perf win without the ASLR benefit).

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

- The C is ours. Keep it small, idiomatic. The only third-party
  dep is cleat (vendored at a pinned tag), which transitively
  brings STC. No autoconf, no plugin systems, no second
  container library.
- For generic containers, use cleat's generics surface, not raw
  STC `i_type`/`#include` forms. We want a consistent style with
  the rest of Sophie's C.
- Match the project's existing conventions: scripts in `client/`,
  installer code in `me.phie.tawc.install`, notes here.
- When adding a new trapped syscall, add it to (1) the BPF filter
  generator, (2) the dispatch table, (3) a cleat unit test in
  `tests/unit/`, (4) an integration test in `tests/integration/`.
  Without all four it's not done. The host-side `client/build-tawcroot`
  default target runs the suite; CI runs it on every push.
- The SIGSYS handler stays cleat-free, STC-free, and malloc-free.
  If a feature needs containers in the handler, the design is
  wrong — restructure so the container is built at init and the
  handler reads a flat immutable view.
- Update this note when the design shifts. Future-Sophie will
  thank you.
