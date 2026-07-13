# Usecase test: background processes that outlive the session

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user starts a long-running process (server, download, build), closes the shell, and expects it to keep running.

Relevant mechanics: on *normal* session exit, orphaned children reparent
and survive under the app uid; on host-side *cancellation* (Ctrl-C /
killed `tawc-exec`) the broker BFS-kills descendants; `am force-stop`
kills everything under the uid (notes/exec-broker.md "Cancellation:
descendant kill").

## Prerequisites

- None beyond a READY Arch install.

## Steps

1. Start a marker daemon and exit normally:
   `scripts/rootfs-run.sh 'nohup sh -c "while :; do date >> /root/uc-daemon.log; sleep 5; done" >/dev/null 2>&1 & disown; echo started'`.
2. From a new session, confirm it survives: `ps aux` shows the loop, and
   the log keeps growing over ~30 s.
3. Cancellation path: start a second daemon the same way but kill the
   host-side `rootfs-run.sh` (Ctrl-C / SIGKILL the tawc-exec process)
   *while the command is still running* (e.g. make the foreground command
   `sleep 60` after spawning). Verify the broker's descendant kill
   reaped it — or record exactly what survives.
4. Longevity under Android: leave daemon #1 running ~10 minutes, press
   Home on the device (app to background), then re-check the log grew.
   (Do not force-stop the app; do not reboot.)
5. Kill daemon #1 from a new session with `pkill -f uc-daemon` (or by
   pid) and confirm it's gone.

## Expected results

- Normal-exit daemons survive across sessions and keep running while the
  app is backgrounded (at least on this timescale — Android may kill the
  app under memory pressure; that's platform reality, not a bug).
- Host-cancelled sessions get their descendants killed per the
  documented broker behavior.

## Known issues / caveats

- If the backgrounded-app daemon dies, distinguish Android killing the
  app process (check logcat, `pidof me.phie.tawc`) from a tawc bug
  before filing.

## Cleanup

Kill all test daemons, remove `/root/uc-daemon.log`.
