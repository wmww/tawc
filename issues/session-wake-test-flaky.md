# Exit's kill sweep kills processes started right after it

`lazy_compositor::test_session_wake_follows_toggle_and_exit` fails
intermittently at its last step: the `true` run after Exit dies with
SIGKILL (exit 137).

Cause: `SessionExit.killEverything` runs `ProcessScanner.killAllInRootfs`
on a background thread — kill, sleep 250 ms, rescan, up to 8 passes.
Pass 1's kills release the holds, so `SessionService` stops while the
sweep is still going; anything launched in that window (the test's
`true`, or a user relaunching an app right after Exit) is caught by the
next rescan and killed.

Possible fixes: stop the sweep once a new session hold is taken, or only
kill pids that already existed (start time before the Exit) when the
sweep began.
