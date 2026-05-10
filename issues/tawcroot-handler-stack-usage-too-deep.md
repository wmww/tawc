# tawcroot: SIGSYS handler chain uses 30-50 KB of stack on the trapping thread

The SIGSYS handler runs on the trapping thread's user stack (no
`sigaltstack`). The path-translation chain piles up multiple
`TAWC_PATH_MAX` (4096-byte) buffers across the call chain, plus
recursion-bounded extra frames in the symlink resolver.

`notes/tawcroot.md` "Open questions" #2 estimates *"~4 KB
PATH_MAX buffer plus a couple frames is 5–6 KB"*. Building tawcroot
with `gcc -Wstack-usage=1024` says otherwise.

## Measurement

Compiled `cc -O2 -ffreestanding -Wstack-usage=1024 -c <file>`:

| file                    | worst single-frame stack usage |
|-------------------------|--------------------------------|
| `syscalls_fs.c`         | **16 448 bytes** (handle_renameat / handle_renameat2 / handle_linkat) |
| `syscalls_fs.c`         | 12 320 bytes (× 9 handlers — utimensat, fchownat, statx, …) |
| `path_orchestrate.c`    | 8 304 bytes (`tawcroot_path_translate_with_ctx`) |
| `path_resolve.c`        | 8 288 bytes (`tawcroot_path_resolve_symlinks`) |
| `path_fold.c`           | 8 136 bytes (`tawcroot_path_fold_absolute`) |
| `chroot.c`              | 8 240 bytes |
| `exec_handler.c`        | 4 864 bytes |
| `path.c`                | 4 144 bytes |

Worst-case path: an unlucky `renameat` from a thread with a tiny stack:

    handle_renameat                        16 448
      ↳ fetch_and_translate (×2 — old, new)
      ↳ tawcroot_path_translate_with_ctx    8 304
        ↳ tawcroot_path_fold_absolute       8 136
        ↳ tawcroot_path_resolve_symlinks    8 288

— ≈ **41 KB peak**, plus the kernel's signal-frame allocation (siginfo_t
+ ucontext_t ≈ 600 bytes on x86_64), plus whatever the trapping
thread had already used.

## Why this is more than the notes claim

The notes count one PATH_MAX buffer. The actual chain has several
that don't share storage because they hold different intermediate
forms simultaneously:

- caller (`handle_*`): `path_buf` + `suffix` (8 KB) — and on two-path
  syscalls `old_buf`/`old_suf`/`new_buf`/`new_suf` = 16 KB.
- `tawcroot_path_translate_with_ctx`: `cwd_abs` + `joined` + `tmp` =
  12 KB during a relative-path-with-symlink-rewrite call.
- `tawcroot_path_resolve_symlinks`: `target` + `tmp` = 8 KB inside
  the per-component loop. (The compiler does sometimes fold these
  into one slot, but `-Wstack-usage` says 8 288 bytes is what gcc
  emits at -O2.)
- `tawcroot_path_fold_absolute`: 8 KB scratch.

## Where it bites

- **Go runtime M threads** start with tiny system stacks (~8 KB) and
  don't auto-grow under a signal handler. Any path-bearing syscall
  from such a thread overflows immediately. (Go's normal goroutines
  run on the M thread's system stack for syscalls, so this is the
  realistic exposure.)
- **glibc `pthread_create` with explicit small stack** (some Mozilla
  IPC helpers, glib worker pools using GThread with reduced size).
  PTHREAD_STACK_MIN on glibc is 16 KB — a 41 KB handler frame can't
  fit at all.
- **musl pthreads** default to 128 KB — handler fits but a deep guest
  call already 60+ KB into its stack will overflow.
- **Default 8 MB main-thread stack** is fine.

The notes claim the realistic risk is low because *"programs with
tiny worker stacks tend not to issue path-bearing syscalls from those
threads."* That's true for Go's idiomatic patterns but breaks for any
goroutine that does file I/O or path-relative operations from inside
a CGO call — common for tools that wrap libc through Go.

## Detection

Overflow lands as a `SIGSEGV` with `si_code = SEGV_MAPERR` at an
address one page below the trapping thread's stack base. Same
signature as the wc segfault we already chased
(`notes/tawcroot.md` "Phase 5b — wc segfault"), but on a guest
thread rather than the loader's stack. Easy to mistake for a guest
bug.

## Fix options, in order of intrusiveness

1. **Shrink PATH_MAX buffers in the hottest chain.** Most filesystem
   paths fit in 256 bytes. Drop `TAWC_PATH_MAX` to 1024 in the
   handler chain (paired with -E2BIG / -ENAMETOOLONG returns for
   over-budget paths) and worst-case drops to ≈ 10 KB. Cheap, safe;
   loses the ability to translate pathologically long paths but
   those are rare and Linux's own `PATH_MAX` is the same 4096 (so
   programs are already supposed to handle ENAMETOOLONG).

2. **Init-allocated arena for path translation.** Put the
   working buffers in a process-wide arena pinned at supervisor_init
   time. Trade: needs per-thread sub-arena because the handler can
   re-enter the path-translation chain from a nested SIGSYS (we
   block in the outer handler, but a recursive case from a sibling
   thread is still possible). The notes already gesture at this as
   the right fix.

3. **`sigaltstack` per-thread.** Notes already explain why this is
   the wrong shape: per-thread, doesn't inherit across `clone`,
   would need per-thread-creation interception. Avoid.

## Severity

Low *today* (no shipped workload trips it). Medium when the first
Go-bridging tool or Mozilla IPC helper inside the chroot does a
path-relative syscall. The latent failure mode is silent
SIGSEGV-on-trap that looks like a guest bug — that's the part that
concerns me most.

The cheap mitigation (option 1) is ~30 minutes of work and would
cap the risk.
