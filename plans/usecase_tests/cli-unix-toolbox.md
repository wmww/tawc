# Usecase test: classic Unix file toolbox

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user does ordinary file wrangling: archives, compression, checksums, find/grep pipelines, links.

## Prerequisites

- Everything should already be present (coreutils, tar, gzip; install
  `xz` if missing). No or minimal package installs.

## Steps

Work under `/root/usecase-toolbox/`.

1. Generate a ~50 MB file from `/dev/urandom`; `sha256sum` it; `gzip`
   then `gunzip`, `xz` then `unxz`; re-checksum — must match exactly.
2. Build a tree with regular files, a symlink (relative and absolute), a
   hardlink, an executable, and odd permission modes (600, 750, 4755 —
   note whether the setuid bit survives; it has no real effect under
   tawcroot either way). `tar cf` / extract to a second dir / `diff -r`
   and compare `stat` output (modes, link counts, symlink targets).
3. Pipelines over many small files: create ~2000 small files in nested
   dirs; `find . -name '*.txt' | wc -l`, `grep -r` for a planted string,
   an `awk`/`sort`/`uniq -c` frequency count over generated text.
4. `mv` a large file across directories, `cp -a` the whole tree,
   `rm -rf` the copy.
5. `df -h /` and `du -sh .` — output should be sane, not garbage or
   errors.

## Expected results

- All checksums round-trip; tar preserves modes, symlinks, and hardlink
  identity (same inode via `stat`); pipelines return correct counts;
  no tool errors.

## Known issues / caveats

- xattr-preserving flags (`tar --xattrs`, `cp -a` on files carrying
  `security.capability`) may warn — writing that xattr is a documented
  tawcroot divergence (notes/tawcroot/status.md). Warnings there are
  expected; data loss is not.

## Cleanup

Remove `/root/usecase-toolbox/` and uninstall anything you added.

## Run log (2026-07-13, physical, Arch tawcroot) — PROBLEM found

Not passed. One real bug; everything else worked.

Passed:
- Step 1: 50 MB `/dev/urandom` file, `sha256sum` round-trips exactly
  through `gzip`/`gunzip` and `xz`/`unxz`.
- Step 2: tar of a tree with rel+abs symlinks, a hardlink, an
  executable, and modes 600/750/4755. `diff -r` clean; extracted `stat`
  matches modes, link counts, and symlink targets; hardlink pair shares
  one inode after extract. setuid bit (4755) survived tar round-trip.
- Step 3 (via recursion): `find -name '*.txt' | wc -l` = 2000,
  `grep -rl` = 2000, `grep -rho | sort | uniq -c` frequency count
  correct.
- Steps 4/5: `mv` (size preserved), `cp -a` whole tree (2000 files),
  `rm -rf`, `df -h /` and `du -sh .` all sane.
- No packages installed (coreutils/tar/gzip/xz all present). No xattr
  warnings hit in these steps.

FAILED — large argv wall: any exec with more than ~256 arguments is
silently destroyed. The natural Step-3/4 idioms `cat many/dir*/*.txt`
and `grep pattern many/dir*/*.txt` (2000 file args) return empty / 0
matches while looking successful; a direct `grep -q ... <2000 files>`
exits 74. Root cause is a 264-slot `eff_argv[]` in the tawcroot loader's
shebang stage, rejected *after* the execveat commit so the guest gets a
bare exit code, no errno, no stderr — even though `getconf ARG_MAX`
reports 2 MB and the collection layer accepts 4096 args.

See issues/usecase_tests/tawcroot-argv-count-capped-at-256-silently-destroys-exec.md
Re-run and (if fixed) verify the big-argv idioms, then this can pass.
