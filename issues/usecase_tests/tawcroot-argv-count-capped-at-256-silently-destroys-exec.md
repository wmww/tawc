# tawcroot: exec argv count silently capped at ~256, destroys the process past it

## Summary

Any `execve` whose argument vector exceeds ~256 entries is silently
broken under tawcroot: the process is destroyed with a bare exit code
(74 or 75), no `errno` returned to the caller, and no stderr. Common
shell idioms over a few hundred files (`cat *`, `grep foo *.txt`,
`rm many...`, `tar cf a.tar dir/*`, `cp files... dest`) return empty
output / do nothing while looking like they succeeded.

`getconf ARG_MAX` inside the guest reports `2097152` (2 MB), so the
guest is told it has a normal Linux argv budget and then hits an
undocumented ~256-entry wall.

Found by the `cli-unix-toolbox` usecase test (Step 3, awk/sort/uniq
frequency count over ~2000 files). Everything else in that test passed.

## Reproduce (Arch tawcroot, physical)

```
scripts/rootfs-run.sh 'mkdir -p /root/t && cd /root/t &&
  printf "#!/bin/sh\necho argc=\$#\n" > argc.sh && chmod +x argc.sh &&
  for k in 260 261 262 263 300; do
    set --; for i in $(seq 1 $k); do set -- "$@" a; done;
    ./argc.sh "$@" >o 2>e; echo "k=$k rc=$? out=[$(cat o)] err=[$(cat e)]";
  done'
```

Observed:

```
k=260 rc=0  out=[argc=260] err=[]
k=261 rc=0  out=[argc=261] err=[]
k=262 rc=75 out=[]         err=[]
k=263 rc=74 out=[]         err=[]
k=300 rc=74 out=[]         err=[]
```

Same wall for a real ELF (`cat`), not just scripts:

```
scripts/rootfs-run.sh 'cd /root/t && cat X/*.txt | wc -l'   # >256 file args -> 0
grep -q needle many/dir*/*.txt ; echo $?                     # -> 74 (loader exit leaks as guest status)
```

The cutoff depends on argument *count*, not total bytes: 20 args of
500 B each (10 KB) succeed; 262 one-byte args fail. Threshold is one
lower for scripts (shebang resolution prepends the interpreter, so it
overflows the same buffer one entry sooner).

## Root cause

`tawcroot/src/loader_exec.c` (`tawcroot_loader_exec`, shebang-resolution
stage), around line 258:

```c
static const char *eff_argv[TAWC_SHEBANG_MAX_DEPTH * 2 + 256];
int eff_argc = 0;
if (args->argc > (int)(sizeof eff_argv / sizeof eff_argv[0]) - 1)
        LOADER_FAIL(74);              // line 261
...
long resolved_fd = resolve_shebangs(..., sizeof eff_argv / sizeof eff_argv[0], ...);
if (resolved_fd < 0) LOADER_FAIL(75); // line 285
```

With `TAWC_SHEBANG_MAX_DEPTH == 4` (`include/loader_exec.h`), `eff_argv`
has `4*2 + 256 = 264` slots, so total argc is capped at 263 (fewer once
shebang prepends consume slots). This is far below the documented
`TAWCROOT_EXEC_STATE_MAX_ARGS == 4096` that the collection layer
(`syscalls_exec.c` `collect_array`, `argv_ptrs[MAX_ARGS+1]`,
`argv_strings[64*1024]`) already accepts.

Because the argv is collected and the `execveat`-into-self commit
happens *before* the loader runs, the loader's rejection lands after the
point of no return: the original guest image is already gone, so
`LOADER_FAIL(74/75)` (`tawc_exit_group`) just terminates the process
instead of returning `-E2BIG` to the caller. This is exactly the
post-commit-rejection hazard the comment in
`tawcroot/src/loader_stack.c:78-84` already documents for a *different*
cap — that one was brought into lockstep with the 4096 collection cap;
`eff_argv` was not.

## Impact

- Silent data loss / no-op for ordinary multi-hundred-file shell globs.
  The failure is invisible: exit status is often a downstream pipe
  component's (e.g. `... | wc -l` reports `0` at rc 0), or the raw
  loader exit code 74/75 leaks out with no message.
- The guest advertises `ARG_MAX = 2 MB`, so no program self-limits
  below the real wall.

## Suggested fix

Size `eff_argv` to hold what the collection layer accepts, keeping it in
lockstep with `TAWCROOT_EXEC_STATE_MAX_ARGS` the way `loader_stack.c`
already does — e.g.

```c
static const char *eff_argv[TAWCROOT_EXEC_STATE_MAX_ARGS
                            + TAWC_SHEBANG_MAX_DEPTH * 2 + 1];
```

It is `static` (BSS), so the extra ~32 KB is free. Ideally also reject
oversized argv at the *collection* layer (before the execveat commit) so
the guest gets a real `-E2BIG` from `execve(2)` instead of a destroyed
process, matching the loader_stack lockstep discipline.

## Layer

tawcroot (exec/loader). Not distro packaging, not compositor/app.

## Notes / not yet done

- Not fixed here (core exec hot-path change; wants a rebuild +
  on-device re-verify, beyond a usecase-test-agent's "simple and minor"
  remit). Filed for a tawcroot owner.
- Everything else in `cli-unix-toolbox` passed: 50 MB gzip/xz checksum
  round-trips, tar mode/symlink/hardlink-inode preservation, find/grep
  pipelines done via recursion, mv/cp -a/rm -rf, df/du.
