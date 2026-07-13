# Usecase test: build and run C programs

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a developer uses the rootfs as a native dev box: write C, compile, run, use make and autotools.

## Prerequisites

- A fresh Arch install ships **without** a toolchain (base packages are
  just `inetutils`; `base-devel`/`gcc`/`make` are absent). Install them
  the way a real user would first:
  `scripts/rootfs-run.sh 'pacman -Syu --needed --noconfirm base-devel'`
  (needs the cache proxy). Then verify with
  `scripts/rootfs-run.sh 'cc --version && make --version'`.
- An earlier revision of this file (and `notes/installation.md`) wrongly
  claimed `base-devel` was present at bootstrap. See
  `issues/usecase_tests/arch-guest-missing-base-devel-toolchain.md`.

## Steps

Work under `/root/usecase-c/`.

1. Hello world: compile with `cc -O2`, run, check output and exit code.
2. Small multi-file project (two `.c` + a header + `Makefile`); build with
   `make`, run it, `make clean`.
3. Runtime coverage: compile and run
   - a pthreads program (8 threads incrementing an atomic, join, print
     total),
   - a program that `fork()`s, `execve()`s `/bin/echo`, and reaps the child
     with `waitpid`.
4. Real-world autotools: download GNU hello
   (`curl -LO https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz`, ~1 MB,
   direct network — not proxied), extract, `./configure && make`, run
   `./hello`. `configure` probes the system heavily, which is a good
   tawcroot exerciser.

## Expected results

- Everything compiles and runs cleanly; exit codes and output are correct.
- `./configure` completes without hangs or spurious failures.

## Known issues / caveats

- `io_uring` is deliberately ENOSYS under tawcroot (notes/tawcroot/status.md);
  tools fall back silently. Not a bug.
- Anything trying to write `security.capability` xattrs fails (documented
  divergence); plain builds should never hit this.

## Cleanup

Remove `/root/usecase-c/`. If you installed `base-devel` for the test,
uninstall exactly the packages the transaction added (grab them from
`/var/log/pacman.log`, not `pacman -Qgq base-devel` — the group name
overlaps pre-existing `base` members like grep/sed and removing those
would wreck the rootfs).

## Run result (2026-07-13, physical, Arch tawcroot)

Steps 1-4 all passed once `base-devel` was installed: builds, exit
codes, output, pthreads (`total=800000`), fork/execve/waitpid, and GNU
hello autotools all correct; `./configure` had no hangs/spurious
failures. Only divergence: `base-devel` was not present at bootstrap,
contradicting the old prerequisite —
`issues/usecase_tests/arch-guest-missing-base-devel-toolchain.md`.
Not marked Completed because of that docs divergence.
