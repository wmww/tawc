# Usecase test: everyday git

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user keeps real work in git inside the rootfs: clone over HTTPS, commit, branch, merge, diff.

## Prerequisites

- `git` is in the Arch bootstrap package set; verify with
  `scripts/rootfs-run.sh 'git --version'`.

## Steps

Work under `/root/usecase-git/`.

1. Clone a tiny repo over HTTPS (direct network, not proxied):
   `git clone https://github.com/octocat/Hello-World.git`. Check `git log`
   works.
2. Local workflow in a fresh `git init` repo: set `user.name`/`user.email`
   locally, commit a few files, create a branch, edit on both branches,
   merge (produce and resolve one real conflict), `git diff`, `git log
   --graph --oneline`, tag, `git gc`.
3. `git status` timing sanity: with ~1000 small generated files committed,
   `git status` should return in a few seconds at most (tawcroot syscall
   overhead is claimed at ~1.5–3× native; a pathological slowdown here
   would be a finding).

## Expected results

- Clone, full local workflow, and gc all behave normally; no index/lock
  errors, no corruption reported by `git fsck`.

## Known issues / caveats

- git is heavy on `stat`/`openat`/rename — exactly the syscalls tawcroot
  path-translates. Weird failures here are more likely tawcroot than git.

## Cleanup

Remove `/root/usecase-git/`. Nothing was installed.
