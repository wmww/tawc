# Usecase test: man pages and documentation after slimming

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a new user types `man bash`. The install slims docs/man/locales aggressively (notes/installation.md "Slimming policy", including pacman `NoExtract` rules that prevent regrowth) — this test checks what that user actually experiences and whether there is any recovery path.

This is deliberately a UX probe: the "expected result" is mostly the
*documented* behavior; the question is whether the failure modes are
sane and whether a determined user can get man pages at all.

## Prerequisites

- Cache proxy up (README step 6). Read the slimming policy section of
  notes/installation.md first so you judge against intent, not
  assumption.

## Steps

1. Fresh-state observations: does `man` exist? What do `man bash`,
   `bash --help`, `ls /usr/share/man`, `ls /usr/share/doc` show?
2. Recovery attempt: `pacman -S --noconfirm man-db man-pages`, then
   `man bash` again. Because of `NoExtract`, the bash package's own man
   page likely still won't exist — check whether `man-pages`' content
   (e.g. `man 2 open`) works while package-owned pages (e.g. `man
   pacman`) stay missing.
3. Try the realistic power-user fix: does removing/overriding the
   `NoExtract` lines in `/etc/pacman.conf` plus
   `pacman -S --noconfirm bash` bring `man bash` back? (Restore
   `pacman.conf` afterwards.)
4. `--help` output and error messages throughout should be functional —
   no crashes, no misleading errors like file corruption.

## Expected results

- Behavior matches the slimming policy: docs absent by design, tools
  fail gracefully (`No manual entry for ...`), and the pacman.conf
  override path works for users who want docs back.
- If there is *no* workable recovery path, or errors are
  confusing/broken (man crashing, mandb erroring loudly), file an issue
  proposing either a fix or a documented recovery recipe.

## Known issues / caveats

- Locales are also stripped; `LANG` complaints from tools are related
  fallout worth noting but not separately failing.

## Cleanup

Restore `/etc/pacman.conf` exactly, `pacman -Rns man-db man-pages`,
reinstall-nothing state (a re-`pacman -S bash` with NoExtract restored
is fine to leave — pacman treats it as a normal reinstall).
