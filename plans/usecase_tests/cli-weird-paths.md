# Usecase test: hostile filenames and path edge cases

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** real user files have spaces, unicode, and weird nesting; tawcroot rewrites every path, so this is where translation bugs would surface.

## Prerequisites

- None beyond a READY Arch install. No packages needed.

## Steps

Work under `/root/usecase-paths/`.

1. Create, write, list, `cat`, rename, and delete files named with:
   spaces, leading `-`, single/double quotes, `äöü™日本語` (UTF-8),
   emoji, a literal newline (`$'a\nb'`), a 255-byte name, and `...`.
   Use both shell tools and a tiny python/perl script if available.
2. Symlinks: chains (a→b→c→file), a broken link, a self-loop (must give
   ELOOP, not hang), absolute links pointing inside the rootfs.
3. Documented divergence check — `..` after a symlink resolves
   *lexically* under tawcroot (notes/tawcroot/status.md): build
   `dir/link -> /some/other/dir`, then access `dir/link/../x` and record
   which parent it resolves against. Confirm behavior matches the
   documented divergence; this is expected, not a bug.
4. Depth: build a directory chain ~200 components deep (tawcroot caps at
   256 components); create/read a file at the bottom; then push past the
   cap (~300) and confirm the failure is a clean errno
   (ENAMETOOLONG-style), not a crash or hang.
5. `getcwd` sanity from deep inside; `find` over the whole test tree.

## Expected results

- All names round-trip byte-exact (verify with `ls -b` / checksums);
  symlink semantics are normal except the documented `..` divergence;
  the depth cap fails cleanly.

## Known issues / caveats

- The `..`-after-symlink and 256-component behaviors are accepted
  divergences (notes/tawcroot/status.md). The test verifies they fail the
  *documented* way; anything nastier (crash, wrong file silently
  accessed) is issue-worthy.

## Cleanup

Remove `/root/usecase-paths/` (mind the newline-named file — `rm -rf`
the directory).
