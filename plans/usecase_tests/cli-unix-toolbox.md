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
