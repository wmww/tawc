# tmux (and any SO_PEERCRED peer-cred check) fails: getsockopt returns real app uid, not faked 0

**Layer:** tawcroot (uid emulation gap)
**Found by:** usecase test `cli-tmux-curses` (physical, Arch tawcroot, 2026-07-13)
**Severity:** blocks tmux entirely; likely breaks any server that authenticates
unix-socket clients via peer credentials (some display managers, dbus-ish
brokers, gpg-agent-style socket auth, etc.).

## Symptom

Every tmux client command against a freshly-started server prints
`access not allowed` and (annoyingly) exits 0:

```
$ scripts/rootfs-run.sh 'tmux new-session -d -s uc; tmux ls'
access not allowed        # from tmux ls
```

`tmux new-session -d` returns 0 and the server process does start (visible as
`tmux: server` in /proc), but it rejects *every* client, so no session is
usable. `tmux kill-server` is itself rejected — you must kill the server by
pid. Verbose client log (`tmux -vv`) shows the client connects, sends
MSG_COMMAND (200), and immediately receives MSG_EXIT (203) carrying the
`access not allowed` string; no server log is ever written.

## Root cause

tmux 3.7b's server, on `accept()`, checks the connecting client's credentials
via `getpeereid()` → `getsockopt(SO_PEERCRED)` and rejects the client when the
peer uid does not match the server's own `getuid()`.

Under tawcroot the guest sees a faked root identity: `tawcroot/src/identity.c`
installs handlers for `getuid`/`geteuid`/`getgid`/... that return 0. But
tawcroot does **not** intercept `getsockopt`, so `SO_PEERCRED` is filled by the
kernel with the *real* app uid. Result: server `getuid()==0` but peer
`SO_PEERCRED uid==<app uid>` → mismatch → reject.

Confirmed empirically inside the guest:

```
$ scripts/rootfs-run.sh '<python peercred probe>'
getuid()= 0 geteuid()= 0
SO_PEERCRED pid=29958 uid=10250 gid=10250
```

`access not allowed` is a literal string in `/usr/bin/tmux` (`strings` confirms
it is tmux's own message, not a broker/adb artifact).

## Repro

1. Arch tawcroot install, `pacman -S --noconfirm tmux`.
2. `scripts/rootfs-run.sh 'tmux new-session -d -s uc; tmux ls'`
   → prints `access not allowed`.
3. Peer-cred probe (any language): bind a unix `SOCK_STREAM`, connect to it
   from a thread, `accept()`, read `SO_PEERCRED` — uid is the real app uid
   (~10250) while `getuid()` reports 0.

## Fix direction (not simple/minor — left for a real change)

Add a `getsockopt` handler in tawcroot that, when the guest euid is virtual-0,
rewrites the returned credential structs to the faked identity:

- `SOL_SOCKET`/`SO_PEERCRED` → set `ucred.uid`/`ucred.gid` (and arguably keep
  the real pid) to 0.
- Same treatment for `SCM_CREDENTIALS` ancillary data delivered via `recvmsg`
  (`SO_PASSCRED`), for programs that authenticate that way. `syscalls_socket.c`
  already special-cases recvmsg address rewriting, so that path exists to
  extend.

This needs reading/writing the tracee's optval buffer against optlen, mirroring
existing tracee-memory rewrite handlers — more than a one-liner, so it was not
attempted during the usecase test.

## Impact on the usecase test

`cli-tmux-curses` cannot run at all: tmux is the harness for driving vim/htop
through the PTY-less exec broker, and tmux never yields a working session. vim
and htop were therefore never exercised. Test file left in place, not added to
Completed.

## Cleanup note

The rejected server cannot be stopped with `tmux kill-server` (that client is
also rejected). Kill it by pid found via `/proc/<pid>/comm == "tmux: server"`
(the `/proc` cmdline masking issue does not hide `comm`).
