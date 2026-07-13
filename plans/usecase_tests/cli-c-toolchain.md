# Usecase test: build and run C programs

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a developer uses the rootfs as a native dev box: write C, compile, run, use make and autotools.

## Prerequisites

- Arch installs `base-devel` at bootstrap, so `gcc`/`make` should already
  be present. Verify with `scripts/rootfs-run.sh 'cc --version && make --version'`.

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

Remove `/root/usecase-c/`. Nothing was installed.
