# tawcroot — testing strategy

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

