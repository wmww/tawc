# pgrep/pkill/ps can't identify guest processes — /proc name fields show tawcroot, not the guest program

Found by the `android-serve-http-to-browser` usecase test on the physical
OnePlus (Android 14, device 50f4ca18), 2026-07-13. The usecase itself
passed; this is a process-management snag hit during shutdown.

## Symptom

Inside an Arch tawcroot guest, for **every** guest process (including
the current shell):

- `/proc/<pid>/cmdline` → `tawcroot --exec-child 3`
- `/proc/<pid>/comm` → `4`
- `/proc/<pid>/exe` → `.../lib/arm64/libtawcroot.so`

So the standard daemon-stop flow silently no-ops:

```
$ pkill -f 'http.server 8000'   # matches nothing, exit 1, no output
$ pgrep -a python               # no match
$ ps aux                        # "ps: Unable to get system boot time"
```

A user who backgrounds a server (nohup pattern) and later runs
`pkill python` gets exit 1 and — worse — `pgrep` showing nothing reads
as "already dead" while the server keeps serving. The only working stop
path is remembering the `$!` pid (or bisecting `/proc` by hand) and
`kill <pid>`, which does work.

`ps` additionally errors with "Unable to get system boot time"
(procps reads `btime` from `/proc/stat`), though it still prints
partial output.

## Layer

tawcroot. `notes/tawcroot/status.md` already lists
`/proc/<pid>/cmdline` and `/proc/<pid>/auxv` under "More `/proc`
shadows" as extend-when-needed candidates; this is a concrete workload
that needs it. `comm` (and `/proc/stat` btime for `ps`) should join
that list. `exe` masking matters less but affects `pgrep -x`-style
matching too.

## Impact

Any daemon-lifecycle usecase (this test, cli-background-daemon.md,
cli-ssh-server.md, anything using pkill/pgrep/killall/ps/top) can't
find its processes by name. cli-background-daemon.md step 2 ("`ps aux`
shows the loop") will fail as written.

Confirmed by the `cli-background-daemon.md` usecase test on the same
physical OnePlus, 2026-07-13: `pgrep -f uc-daemon` returned no match
(exit 1) for a live `nohup` loop, and `ps aux` printed
"Unable to get system boot time" plus a masked `COMMAND` column. That
test's daemon-lifecycle steps were exercised by tracking the loop's
`$!` pid directly (pidfile / `/proc/<pid>`), which works. No new
failure modes beyond this issue.

## Repro

```
TAWC_INSTALL_ID=arch scripts/rootfs-run.sh \
  'sleep 30 & p=$!; sleep 0.2; cat /proc/$p/comm; \
   tr "\0" " " < /proc/$p/cmdline; echo; readlink /proc/$p/exe; \
   pgrep sleep || echo no-match; kill $p'
```

→ `4`, `tawcroot --exec-child 3`, `libtawcroot.so` path, `no-match`.
