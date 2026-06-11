# ando — run Android commands from inside the rootfs

`ando [flags] <cmd> [args…]` (named like sudo, but for Android) runs
`<cmd>` as a plain Android process: no tawcroot seccomp filter, no
path translation, no fake-root identity, no rootfs env / libhybris
`LD_PRELOAD`/`LD_LIBRARY_PATH` baggage. stdio stays on the caller's
fds, exit status propagates. On a rooted phone `ando -r <cmd>` (or the
explicit `ando su -c '…'`) gets device root with no special handling —
the spawned child is an ordinary app-uid process, so the normal Magisk
su client/daemon flow applies (the Magisk app must grant
me.phie.tawc).

Production feature, all build types and install methods.

## Why a broker, not an in-process escape

tawcroot's seccomp filter is permanent kernel state: it is inherited
across fork/exec by every guest descendant and can never be removed,
only stacked on (tawcroot.md §"Why non-PIE"). Any process forked from
inside the rootfs carries the filter forever. So escape = ask a
process that **never had the filter** to spawn the command. The app
process is that process; ando is a production sibling of the debug
exec broker (exec-broker.md) with one big upgrade: instead of relaying
stdio through frames, the client passes its real stdin/stdout/stderr
fds over the socket (SCM_RIGHTS), so the Android child writes to the
guest's pty/pipes directly — `isatty` is true in a terminal session,
window size is inherent (same pty), no relay threads.

```
guest (under tawcroot)                 app process (untrusted_app, no filter)
+--------------------+                 +---------------------------------+
| bash               |   unix socket   | ando broker (Rust thread in the |
|  └─ ando (client)  | /usr/share/tawc | compositor .so, started by one  |
|     header+fds ──── /ando.sock ────→ | JNI call at app startup)        |
|     0/1/2/cwd via SCM_RIGHTS,        |  SO_PEERCRED uid == getuid()    |
|     then: signal msgs →              |  recvmsg fds → fork/exec child  |
|     ← exit status                    |  waitpid / kill(-pgid)          |
+--------------------+                 |   └─ child: /system/bin/…, su   |
                                       +---------------------------------+
```

The rootfs methods already support everything the client needs: the
filesystem `sun_path` in `connect(2)` is translated through the share
bind (tawcroot `syscalls_socket.c::translate_unix_sockaddr`, proot's
path translation; chroot resolves it natively, same as the wayland and
kumquat sockets in the same dir), `sendmsg` on a connected socket
forwards `msg_control` verbatim (SCM_RIGHTS works from the guest), and
tawcroot doesn't trap `recvmsg` at all.

## Components

- **Broker**: `compositor/src/ando.rs`. Standalone `std::thread` in
  the app process (not part of the compositor event loop — same shape
  as the kumquat server thread), listening on the filesystem socket
  `<appData>/share/ando.sock` (`AppPaths.andoSocket`; stale node from
  a previous app process is unlinked before bind). A share-dir socket
  is naturally multi-user-safe — each Android user's app instance has
  its own data dir — and needs no name computation on either side.
  Started by `NativeBridge.nativeStartAndoBroker` from
  `TawcApplication.onCreate`, so it's alive whenever the app process
  is, independent of compositor startup. One thread per connection,
  body in `catch_unwind`; the panic hook in `lib.rs` exempts `ando-*`
  threads from its abort so a protocol bug kills the connection, not
  the app. This is the one app-side service in Rust rather than
  Kotlin: the whole data path is unix-syscall-shaped (peercred,
  SCM_RIGHTS, setpgid/fchdir, waitid, kill), which Kotlin's
  LocalSocket handles poorly.
- **Client**: `tawcroot/ando/` → static bionic binary, shipped as
  `jniLibs/<abi>/libando.so`, installed into each rootfs at
  `/usr/local/bin/ando` by `AndoInstallProvider` (rides
  `TawcInstaller`, so APK upgrades re-stamp it). It connects to the
  fixed guest path `/usr/share/tawc/ando.sock`; `TAWC_ANDO_SOCKET`
  overrides the path (test hook — no production flow sets it).

An earlier revision used an abstract socket named after the app uid.
That spiraled: the guest can't compute the uid (`getuid()` is faked to
0, `/proc/net/unix` is SELinux-denied to apps, and reading
`/proc/self/status` would exploit a hole in tawcroot's identity
shadowing that may close someday), so it needed a `RootfsEnv` var plus
a fallback alias bind. The share-dir socket needs none of that.

## CLI

```
ando [-E | --preserve-env[=LIST]] [-D dir] [-s] [-u user | -r]
     [-e K=V]… [--] [cmd [args…]]
```

sudo-shaped flags, all client-side (env lines, a chdir before the
cwd-fd open, argv rewrites); the broker and wire protocol are
untouched. Parsing is `getopt_long` with a leading `+`: options stop
at the first non-option, so `ando ls -la` passes `-la` to `ls`. A
command is required unless `-s` is given.

- `-e, --env K=V` — extra env var for the child.
- `-E, --preserve-env` — forward the guest environment, minus a fixed
  blocklist: `PATH`, `LD_PRELOAD`, `LD_LIBRARY_PATH`. Guest values of
  those are rootfs paths that break Android-side (guest `PATH` has no
  `/system/bin`, so the broker child's exec search would fail; guest
  `LD_*` is libhybris baggage the bionic linker would honor and die
  on). Explicit naming wins over policy: `-e PATH=…` or
  `--preserve-env=LD_LIBRARY_PATH` still forwards them.
  `--preserve-env=LIST` (comma-separated, `=` form only — a separate
  word is never consumed, like sudo) sends exactly the named vars,
  blocklist not applied; unset names are silently skipped. ENV send
  order is forwarded-env, then the `TERM` default, then `-e` extras —
  the broker applies lines last-wins, so `-e` always beats `-E`. A var
  whose encoded line would exceed the broker's 64K `MAX_LINE` is
  skipped with a warning instead of killing the connection.
- `-D, --chdir dir` — client-side `chdir` before the `open(".",
  O_PATH)`, so path translation and bind resolution come for free;
  failure is one error line + exit 125.
- `-s, --shell` — run `/system/bin/sh`, with `sh -c <JOINED>` if args
  remain. The shell is fixed (sudo uses `$SHELL`, but the guest's
  `$SHELL` is a rootfs path that doesn't exist Android-side). JOINED
  is the sudo-style join: every byte outside `[A-Za-z0-9_./=:,+@%^-]`
  backslash-escaped, args joined with single spaces.
- `-u user, --user user` / `-r` (alias for `--user=root`) — argv
  rewrite to Android su: `["su", USER, "-c", JOINED]`, or bare
  `["su", USER]` for `-u USER -s` with no command (su's default action
  is an interactive shell). Repeated `-u`/`-r`: last wins. Without
  root, `su` is absent and the broker's normal 127 "not found" path
  reports it. `TAWC_ANDO_SU` overrides the rewrite's argv[0] (test
  hook like `TAWC_ANDO_SOCKET`; lets unrooted tests assert the
  constructed argv).

Verified on the rooted phone (Magisk): `su <user> -c <str>` ordering
is accepted, and Magisk su preserves both the su client's environment
(`-E`/`-e` values reach the root shell) and its cwd (`-D` propagates).

## Wire protocol

Fd passing first, then a text header (LF lines, UTF-8), then a
signal/exit conversation. Header values use the ExecBroker `\n`-escape
encoding (exec-broker.md "Value encoding" — applied to the value half
only, keys are plain); the Rust broker reimplements it
wire-compatibly. Header strings must be UTF-8 — non-UTF-8 argv bytes
(e.g. latin-1 filenames) are rejected; quote/wrap such args in a shell
instead.

```
client → broker:  1 byte, SCM_RIGHTS = exactly [stdin, stdout, stderr, cwdfd]
client → broker:  TAWCANDO 1
                  ARGV <v>          (≥1; argv[0] gets a PATH search if bare)
                  ENV K=<v>         (×n; extras on top of the app env)
                  (blank line)
client → broker:  SIG <n>\n         (×n; forwarded as kill(-pgid, n))
broker → client:  EXIT <code>\n     (then close; 128+sig for signal deaths)
```

The fd message comes first so everything after it is plain stream data
(one buffered reader broker-side); ancillary data would be discarded
by the kernel if its carrier byte were consumed by a buffered read.

Child spawn: `std::process::Command` (inherited app env — zygote PATH,
`ANDROID_ROOT`, … — plus a PATH fallback and the client `ENV` extras;
the client sends `TERM` by default), stdio wired to the received fds,
with a `pre_exec` doing `setpgid(0,0)` (own process group so signal
forwarding and disconnect-kill reap the whole tree) and `fchdir(cwdfd)`
(stays in the app process's cwd on failure). Spawn errors are reported
on the guest's stderr with `EXIT 127`/`126`. The waiter observes the
exit via `waitid(WNOWAIT)` — the zombie keeps the pid/pgid reserved
until the connection teardown reaps it, so `kill(-pgid, …)` can never
hit a recycled process group. Client EOF before child exit → `SIGKILL`
to the child's process group (same orphan-prevention contract as the
debug broker; the uid-wide kill on app death is the backstop for
double-fork escapees).

ART's `ProcessBuilder` reaps per-pid (no `waitpid(-1)` to steal our
child's status); every integration test spawn already runs an ando
child concurrently with ProcessBuilder children (the guest session
itself is one), so a regression would show up as ando hangs/127s.

### cwd as an fd

The child should start where the caller is standing, but the client
can only name its cwd in guest terms — tawcroot reverse-translates
`getcwd()`, and strings can't cross the boundary. Fds can: fd contents
aren't virtualized, so the client's `open(".", O_PATH | O_DIRECTORY)`
yields a real host fd for the directory its cwd resolved to. The child
`fchdir`s to it. No install-id plumbing, no duplicated bind-prefix
tables; bind-mounted cwds (`/usr/share/tawc`, `/system`, …) resolve
correctly for free, and the guest can only hand over directories
reachable through its own view.

## Security model

`SO_PEERCRED` uid must equal the app's uid; everyone else is closed.
That is the entire auth model: the broker grants "run a command as the
app uid" to processes that are already the app uid (every rootfs guest
is — tawcroot is not a sandbox), so it adds no privilege. Other apps
and adb get rejected; the debug ExecBroker's `{0, 2000}` gate stays
separate and debug-only. The socket node sits in app-private data
(`/data/data/me.phie.tawc/share/`), unreachable to other apps even
before peercred. A guest *can* delete the node through the share bind
— that breaks ando until the next app start, but a guest is the same
uid and could just as well kill the app; not a security boundary.

## Semantics and known limits

- **Interactive use** (`ando -s`, `ando -r -s`): the child holds the
  guest's pty fd, so reads/writes/winsize work. It can never make that
  pty its controlling terminal (the rootfs session leader owns it), so
  full job control inside an ando shell — `tcsetpgrp`, Ctrl-Z of inner
  pipelines — won't work; shells detect this and run without job
  control. Ctrl-C works via the client's SIG forwarding. If this ever
  matters, a later `--pty` mode can have the broker allocate a fresh
  pty pair and relay — the protocol has room.
- **fake-root vs real uid**: inside the rootfs you look like root;
  `ando id` truthfully reports the app uid (10xxx). That asymmetry is
  the feature.
- **su descendants**: the Magisk daemon spawns the actual root process
  outside our process tree; disconnect-kill only reaches our direct
  child (the su client). Acceptable — same as any app using su.
- The child is **not** under `setsid` (only setpgid), deliberately: it
  must keep using the caller's tty fds, and the rootfs-session
  invariant (rootfs-sessions.md) governs rootfs entries, not
  Android-side spawns. The child never shares the guest's pgrp, so
  guest-side pgrp kills can't hit it.
- Client exit codes: child's code verbatim (128+sig for signal
  deaths); 127 = broker not reachable or command not found; 126 =
  spawn failure; 125 = usage/protocol errors.

## See also

- `compositor/src/ando.rs` — broker (protocol docs in module header).
- `tawcroot/ando/src/ando.c` — guest client.
- `tawcroot/ando/build.sh` — static bionic build → jniLibs staging.
- `app/src/main/java/me/phie/tawc/AppPaths.kt` — `andoSocket` path.
- `app/src/main/java/me/phie/tawc/install/AndoInstallProvider.kt`.
- `tests/integration/tests/ando.rs` — integration coverage.
