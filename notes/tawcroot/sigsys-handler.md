# tawcroot — SIGSYS handler

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

