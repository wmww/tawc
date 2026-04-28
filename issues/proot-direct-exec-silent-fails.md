# `ProcessBuilder` exec of proot fails silently — workaround in place

`ProotMethod.runInside` cannot exec the proot binary directly via
`ProcessBuilder`. Per the existing comment:

> Direct ProcessBuilder exec of the proot binary from app context
> produces a silent exit-255 (process forks, the loader stub fails to
> execve_no_trans through SELinux+seccomp gauntlet, but with
> redirectErrorStream and no shell wrapping we can't see why). Going
> through `/system/bin/sh -c '<proot args>'` makes the shell handle
> exec for us — Android's app seccomp policy allows that path…

Today's workaround:

1. Build a long shell command string
2. Base64-encode the user command (already needed for chroot quoting,
   but here it survives an extra shell layer)
3. Spawn `/system/bin/sh` with `-c <built command>`
4. Tee proot's output through a side log to recover the diagnostic
   tail when proot dies before the merged-fd reader drains

Costs:

- An extra `/system/bin/sh` process per chroot entry
- The command-string construction is brittle (single-quoting nested
  through `eval "$(printf %s … | base64 -d)"`)
- The "lose the last lines" failure mode that the side-log tee
  papers over is still a real bug in the I/O handling — see the
  `set -e` comment in `runShell` ("races the merged-stderr drain")
- Diagnostic noise: every proot run logs through one extra shell
  layer

## What to investigate

The exit-255 needs an actual root cause:

1. Repro with `strace -f` (or `simpleperf record` on a Magisk device)
   on a *direct-exec* invocation to see what `execve` of `libproot.so`
   from an app process actually does. Likely candidates: SELinux
   denial logged by audit (check `dmesg | grep avc` after the
   failure), missing `LD_LIBRARY_PATH` for proot's own libtalloc
   (we link static, so this should not apply), proot's own
   `prctl(PR_SET_NO_NEW_PRIVS)` setup tripping on the absence of
   the shell as parent, or `nativeLibraryDir`'s `apk_data_file`
   context behaving differently when the parent isn't a shell.
2. If it's an SELinux thing, see whether `Process.start()` in Java
   sets the right context vs. `sh -c` shelling out.

If we find a clean direct-exec path, drop the shell wrapper, the
base64 dance, and the side-log tee. The expected shape is:

```kotlin
val pb = ProcessBuilder(prootArgv(rootfs) + listOf("/bin/bash", "-lc", command))
    .redirectErrorStream(true)
pb.environment()["PROOT_TMP_DIR"] = prootTmpDir
pb.environment()["PROOT_LOADER"] = prootLoader
val proc = pb.start()
collectOutput(proc, onLine)
```

i.e. roughly what `ChrootMethod.runInside` does today via `Su.run`.

## Why we didn't fix it inline

The shell-wrapper workaround unblocked the proot install path; the
exit-255 is a pre-existing-on-Android quirk with no clear repro
context yet. Filed for someone to dig in with `strace`/`audit2allow`
when there's time.

## References

- `server/app/src/main/java/me/phie/tawc/install/ProotMethod.kt` —
  the `// We invoke proot via the system shell rather than direct
  ProcessBuilder argv.` block in `runInside` and the
  `// No `set -e` prefix here.` block in `runShell`.
