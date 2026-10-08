# App collects dups of its IME channel fd

Every window focus change that starts an IME connection leaves one more
fd in the app process pointing at the same `SOCK_SEQPACKET` socket, whose
other end `system_server` holds as the IME session channel for our pid
(`dumpsys input_method` `curSession ... channel=... (server)`). Seen on
the OnePlus 9 (Android 14), 2026-10-08: 12–17 fds on one inode after a
test run, +1 per `rootfs-run` that opens the log screen; the launcher has
1. Two forced GCs (`am dumpheap`) did not drop them, though counts did
fall back at times during a suite run.

Likely `InputMethodManager` receiving the same channel again in an
`InputBindResult` and not disposing the duplicate, but unconfirmed, and
unclear why only TAWC accumulates. Harmless short-term; over a very long
session it could creep toward the fd limit.

Tests: `helpers::app_fd_targets` counts each socket once, so the leak
checks ignore this.

Repro: `ss -xpa` as root, count `fd=` entries of the u_seq socket shared
by `me.phie.tawc.dev` and `system_server`, before and after a few
`scripts/rootfs-run.sh true`.
