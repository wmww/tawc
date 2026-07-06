# tawcroot — architecture & process layout

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

