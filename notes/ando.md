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

**Per-distro, default off.** ando is a per-install setting
([Installation.andoEnabled], default `false`; absent in legacy metadata
→ `false`, so upgrades lose ando until re-enabled — opt-in, fail-closed).
Configurable at install time (the install form checkbox / the
`--arg ando=true` exec-broker install action) and toggled later on
[DistroInfoActivity]. See "The setting" below. When disabled, a guest
cannot reach ando through *any* path — the CLI stays installed but
refuses with enable instructions.

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
guest (under tawcroot)                    app process (untrusted_app, no filter)
+--------------------+                    +---------------------------------+
| bash               |   unix socket      | ando broker (Rust thread in the |
|  └─ ando (client)  | /run/tawc-ando/     | compositor .so; one listener   |
|     header+fds ──── ando.sock ────────→  | per ando-ENABLED distro,       |
|     0/1/2/cwd via SCM_RIGHTS,            | reconciled by nativeSyncAndo-  |
|     then: signal msgs →                  | Brokers)                        |
|     ← exit status                        |  SO_PEERCRED uid == getuid()    |
+--------------------+                    |  recvmsg fds → fork/exec child  |
                                          |  waitpid / kill(-pgid)          |
                                          |   └─ child: /system/bin/…, su   |
                                          +---------------------------------+
```

The socket a connection arrives on *is* the distro identity: each
enabled install has its own listener on its own socket, so the broker
needs no per-connection distro check beyond the peercred gate.

The rootfs methods already support everything the client needs: the
filesystem `sun_path` in `connect(2)` is translated through the share
bind (tawcroot `syscalls_socket.c::translate_unix_sockaddr`, proot's
path translation; chroot resolves it natively, same as the wayland and
kumquat sockets in the same dir), `sendmsg` on a connected socket
forwards `msg_control` verbatim (SCM_RIGHTS works from the guest), and
tawcroot doesn't trap `recvmsg` at all.

## Components

- **Broker**: `compositor/src/ando.rs`. Standalone `std::thread`s in
  the app process (not part of the compositor event loop — same shape
  as the kumquat server thread). There is **one listener per
  ando-enabled install**, each on that install's own filesystem socket
  `<appData>/distros/<id>/ando/ando.sock` (`InstallationStore.andoSocket`;
  a sibling of `rootfs/` — deleted explicitly by `RootfsCleaner`'s
  pass 2 at uninstall — and outside the wholesale-bound share dir so
  no other distro's guest can enumerate it). A per-app-data-dir socket
  is naturally multi-user-safe.
  - **Sync API.** `ando::sync(ids, paths)` reconciles the live listener
    set to exactly the given (id, path) pairs: it starts missing
    listeners (mkdir + unlink stale + bind) and stops removed ones. It
    is idempotent, driven from Kotlin's `AndoBrokers.refresh` (below)
    via `NativeBridge.nativeSyncAndoBrokers`.
  - **Stopping a listener** (disable) is immediate: close the socket,
    unlink the node, and SIGKILL the pgids of any in-flight children
    spawned through it (each connection registers its child pgid in a
    shared set). An `eventfd` polled alongside the listener wakes the
    accept loop so the stop is prompt. Disable means disabled, not
    "drains eventually".
  - Per-connection handling, protocol, and the peercred check are
    unchanged. One thread per connection, body in `catch_unwind`; the
    panic hook in `lib.rs` exempts `ando-*` threads from its abort so a
    protocol bug kills the connection, not the app. This is the one
    app-side service in Rust rather than Kotlin: the whole data path is
    unix-syscall-shaped (peercred, SCM_RIGHTS, setpgid/fchdir, waitid,
    kill), which Kotlin's LocalSocket handles poorly.
- **Lifecycle wiring**: `me.phie.tawc.AndoBrokers.refresh(context)`
  lists installs, filters `InstallationStore.andoEnabled` (metadata OR
  the test override), and calls the native sync; for *disabled*
  installs it also unlinks any stale socket node an unclean app
  shutdown left behind (else a still-bound guest session would get
  ECONNREFUSED — "is the tawc app alive?" — instead of ENOENT's
  ando-disabled instructions). Called from `TawcApplication.onCreate`'s
  startup thread (which also unlinks the legacy
  `<appData>/share/ando.sock` node from older versions), from
  `Installer` right after the initial metadata write and after
  uninstall (which also drops the id's test override), and from every
  toggle commit path (install form, distro settings, `set-ando`).
- **Client**: `tawcroot/ando/` → static bionic binary, shipped as
  `jniLibs/<abi>/libando.so`, installed into each rootfs at
  `/usr/local/bin/ando` by `AndoInstallProvider` (rides
  `TawcInstaller`, so APK upgrades re-stamp it) — **unconditionally**,
  enabled or not: the stamp machinery is global and a present-but-
  refusing CLI is the disabled-state error surface. It connects to the
  fixed guest path `/run/tawc-ando/ando.sock`
  (`TawcrootMethod.GUEST_ANDO_DIR`), reachable only through the
  per-distro ando bind that the spawn's bind builder emits **only when
  ando is enabled** (see "The setting"). On connect **ENOENT** (no bind
  → disabled) the client prints multi-line enable instructions before
  exit 127; other errnos, including **ECONNREFUSED** (node present, no
  listener → an *enabled* distro whose broker/app died), keep the
  "broker not running — is the tawc app alive?" diagnosis.
  `TAWC_ANDO_SOCKET` overrides the path (test hook — no production flow
  sets it).

An earlier revision used an abstract socket named after the app uid.
That spiraled: the guest can't compute the uid (`getuid()` is faked to
0, `/proc/net/unix` is SELinux-denied to apps, and reading
`/proc/self/status` would exploit a hole in tawcroot's identity
shadowing that may close someday), so it needed a `RootfsEnv` var plus
a fallback alias bind. The share-dir socket needs none of that.

A yet-earlier revision used a single shared socket at
`<appData>/share/ando.sock`, bound into every rootfs via the share
bind. That couldn't be gated per-distro: the capability *is* the
socket, the share dir is bound into every rootfs, and the peercred
gate can't tell distros apart (every guest is the app uid). Making
ando per-distro required both the socket and its reachability to
become per-distro — see below.

## The setting

`Installation.andoEnabled: Boolean`, default `false`; absent in legacy
`metadata.json` parses as `false` (additive field, safe default, no
schema bump). Existing installs lose ando on upgrade until the user
re-enables it — intended: opt-in, fail-closed. Two independent layers
enforce a disabled distro, so a bug in one still leaves the other:

1. **No listener.** The broker only listens on sockets of
   ando-enabled distros (`AndoBrokers.refresh` → `ando::sync`). A
   disabled distro has no socket node and no accepting listener
   anywhere.
2. **No path.** The per-distro socket is reachable only through a
   per-distro bind — host `distros/<id>/ando/` → guest
   `/run/tawc-ando/` (`GUEST_ANDO_DIR`) — that each method's bind
   builder (tawcroot `bindSpecs`, `ProotMethod.prootArgv`,
   `ChrootMounter.mountScript`) includes **only when `andoEnabled`**,
   read fresh per spawn (like `externalBindsFor`, via
   `InstallationStore.andoHostDir`). A disabled guest has no path that
   translates to *any* ando socket.

The guest path is deliberately a **dedicated top-level dir, not nested
under the shared `/usr/share/tawc` bind**. That shared dir is bound
into every rootfs and is guest-writable, so if the ando path fell
through it when unbound, a disabled distro's client would resolve into
`<appData>/share/ando/` — where a co-tenant distro could `mkdir` and
plant a fake listening socket and capture the disabled distro's ando
stdio/cwd fds (all same-uid, no escape needed). With its own top-level
bind there is no fall-through: a disabled guest's `/run/tawc-ando/`
resolves inside its *own* rootfs (nothing created it) → ENOENT. (An
earlier revision nested it at `/usr/share/tawc/ando/` and leaned on
longest-prefix bind precedence; that both exposed the fall-through and
made a chroot `mkdir` of the mount point pollute the shared dir.)

### Take-effect semantics

- **Disable: immediate.** Listener closed, socket unlinked, in-flight
  ando children SIGKILLed. An already-running guest keeps the (now
  dead) path in its bind table; connects fail.
- **Enable: next spawn.** The listener comes up immediately, but a
  session spawned while disabled has no bind entry; only processes
  started after the toggle get it. Same per-spawn semantics binds
  already have. (This is why the client's disabled error tells the
  user to open a *new* terminal.)

### Configuring it

- **Install time:** the install form checkbox ("Allow running Android
  commands (ando)", default off, all methods) → intent extra →
  `InstallationService.startInstall` → `Installer` → initial metadata.
  Exec-broker install action: `--arg ando=true|false` (default false).
- **Post-install:** a READY/FAILED-gated toggle on `DistroInfoActivity`
  (state-gated like `ManageBindsActivity.commit` to avoid racing
  service writes). Gate + write go through `InstallationStore.update`,
  which re-reads and applies the edit under a per-id lock so a
  concurrent writer (installer manifest refresh, binds edit) can't
  revert the toggle; then `AndoBrokers.refresh`.
- **Tests:** an in-memory per-id override in `InstallationStore`
  (`setAndoOverride`, mirrors `Settings.enterTestMode`) that the
  `set-ando` broker action writes and spawn paths / `AndoBrokers.refresh`
  read through; discarded on app-process death and cleared by
  `test-init`. Never a durable metadata write.

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
(`/data/data/me.phie.tawc/distros/<id>/ando/`), unreachable to other
apps even before peercred.

### Per-distro gating (what the toggle actually protects)

The per-distro toggle closes every **supported** access path for a
disabled distro: the CLI, hand-rolled socket clients, and cross-distro
socket reach. No bind covers another distro's `distros/<id>/ando/`, and
under chroot host paths aren't visible at all, so an enabled distro
still can't reach a disabled (or another) distro's socket. The guest
ando path is a dedicated top-level dir (not nested under the shared
`/usr/share/tawc` bind), so a disabled distro's default socket path
does not fall through into the guest-writable shared dir where a
co-tenant could plant a decoy socket — see "The setting".

It is **not** a boundary against a same-uid process that escapes the
virtualization layer. tawcroot/proot are not sandboxes; an escaped
guest is the app uid and could equally ptrace or kill the app, or read
another distro's socket node directly off disk — no per-distro design
can fix that, only real process isolation could. This is the same
stance the peercred model takes: ando grants "run as the app uid" to
processes that already are the app uid.

ando's socket is the only guest→Android exec surface: the debug
ExecBroker is not guest-reachable (peercred gate `{0, 2000}`; guests
are 10xxx) and debug-only, and the wayland/kumquat sockets carry no
exec capability.

Rejected alternatives (folded from the design): removing the CLI when
disabled (the socket stays reachable — theater); a single socket plus
peercred-pid → `/proc/<pid>/cwd`/`root` distro identification
(unreliable and racy); a per-distro token handshake (equivalent power
to per-distro sockets with more protocol); and a global toggle in
`Settings` (doesn't meet the per-distro requirement).

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
- **chroot: ando has never actually worked.** Chroot guests run as
  real root (`su` → `unshare -m` → chroot), and the broker's peercred
  gate rejects any uid != app uid — including 0. This predates the
  per-distro toggle (the old shared socket was equally gated). The
  chroot bind plumbing is kept for shape-consistency with the other
  methods, but an enabled chroot distro's `ando` fails after connect.
  Accepted: chroot is a barely-supported debug-only dev loop. Don't
  "fix" this by admitting uid 0 at the gate without thought — root can
  reach every method's listener, not just chroot's.

## See also

- `compositor/src/ando.rs` — broker + sync API (protocol docs in module header).
- `tawcroot/ando/src/ando.c` — guest client.
- `tawcroot/ando/build.sh` — static bionic build → jniLibs staging.
- `app/src/main/java/me/phie/tawc/AndoBrokers.kt` — listener-set reconcile.
- `app/src/main/java/me/phie/tawc/install/InstallationStore.kt` —
  `andoSocket`/`andoDir` paths, `andoEnabled`, test override.
- `app/src/main/java/me/phie/tawc/install/Installation.kt` — `andoEnabled` field.
- `app/src/main/java/me/phie/tawc/install/AndoInstallProvider.kt`.
- `tests/integration/tests/ando.rs` — integration coverage.
