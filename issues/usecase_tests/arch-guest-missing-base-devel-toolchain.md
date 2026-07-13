# Arch guest ships without base-devel/gcc/make — cli-c-toolchain prereq (and notes) were wrong

Found by the `cli-c-toolchain` usecase test on the physical OnePlus
(Android 14, device 50f4ca18), Arch tawcroot install, 2026-07-13.

## Symptom

The `cli-c-toolchain.md` prerequisite claims:

> Arch installs `base-devel` at bootstrap, so `gcc`/`make` should
> already be present. Verify with `cc --version && make --version`.

On a fresh Arch (ALARM aarch64) tawcroot install this is false:

```
$ cc --version     → cc: command not found
$ make --version   → make: command not found
$ pacman -Qg base-devel → error: group 'base-devel' was not found
$ pacman -Q gcc make    → error: package 'gcc'/'make' was not found
```

The guest ships only the minimal base set. This is **intended
product behaviour**, not a regression:
`ArchPacmanCommon.DEFAULT_BASE_PACKAGES = listOf("inetutils")`, and
`installBasePackages` runs `pacman -Syyu --needed <basePackages>`
with nothing else. The comment there explicitly says the set is "kept
minimal". So the *code* is correct/deliberate — the docs were stale.

## Layer

Docs / test-plan assumption (not distro packaging or tawcroot).

Two stale docs pointed the other way and were corrected in the same
commit as this issue:

- `notes/installation.md` PKG_INSTALL step (was: `pacman -Syu ...
  base-devel git libtool wayland ... weston gtk3 gtk3-demos`) — that
  matched an *old* larger basePackages list; the code now installs
  only `inetutils`. Rewritten to say so.
- `plans/usecase_tests/cli-c-toolchain.md` prerequisite — rewritten to
  install `base-devel` first (the normal Arch workflow) and reference
  this issue.

## Does the usecase actually work?

Yes — once `base-devel` is installed (`pacman -S base-devel`, ~25 pkgs
via the dev cache proxy), the whole C-toolchain usecase passes cleanly
under tawcroot:

- `cc -O2` hello world: compiles, runs, correct output + exit code.
- Multi-file `make` project: builds, runs (`2+3=5`), `make clean`.
- pthreads (8 threads, atomic): `total=800000` as expected, exit 0.
- `fork()`+`execve("/bin/echo")`+`waitpid`: child reaped, status 0.
- GNU hello 2.12.1 autotools: `./configure` completes with no hangs or
  spurious failures, `make` builds, `./hello` → `Hello, world!`,
  `--greeting=` flag works.

gcc 16.1.1, make 4.4.1. Only benign noise: systemd tmpfiles/journal
post-transaction hook warnings ("Current root is not booted", journal
xattr) during pacman install/remove — expected in the systemd-less
rootfs, no functional impact.

## Fix options

1. (Done) Fix the docs so users/tests know to `pacman -S base-devel`.
2. (Optional, product decision) If a "dev box out of the box" is
   desired, add `base-devel` to the Arch basePackages — but that grows
   every install ~200 MB+ and contradicts the deliberate minimalism, so
   the docs fix is the right default. Not doing this.
