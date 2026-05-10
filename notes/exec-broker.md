# Dev exec broker

A debug-build-only in-app service that lets the host run commands as the
app uid/domain — same SELinux context (`untrusted_app`) the production
launch path uses. Replaces all `run-as` and most `su` use in dev/test
workflows with a path that genuinely mirrors how things execute when a
user launches the app.

## Why this exists

Without the broker, host-side test/dev scripts had two ways to enter the
app's environment:

1. `adb shell run-as me.phie.tawc <cmd>` — works on debug builds but
   transitions into the `runas_app` SELinux domain, which is **not** the
   domain the running app actually uses (`untrusted_app`). Subtle policy
   differences between the two have bitten us before (e.g. the missing
   `system_file:execmod` on `runas_app` we worked around for libhybris;
   see `notes/proot.md`). PDEATHSIG also doesn't fire across the
   `shell → runas_app` transition, so killing the host script orphans
   any guest processes — fixed by routing through this broker.

2. `adb shell su -c <cmd>` — works on rooted devices, runs as root
   (uid 0, domain `su`). Even further from production.

The broker fixes both: the app process itself fork-execs the requested
command, so it inherits the app's `untrusted_app:s0:cXXX,cYYY` context
exactly as a user-launched run does. PDEATHSIG works (same-domain
parent → child). No `runas_app` weirdness to paper over. No root needed.

## Architecture

```
host                                        device (app process)
+--------------+                            +-----------------------+
| tawc-exec    |       adb forward          | ExecBroker            |
| (Rust bin)   | ─── localabstract:foo ───→ | LocalServerSocket     |
| stdio relay  |       ↔ TCP loopback       | accept loop           |
+--------------+                            |   ↓                   |
       ↑                                    | per-connection thread |
       │                                    |   ↓ ProcessBuilder    |
   user shell,                              | child (any cmd, runs  |
   test scripts                             | in untrusted_app)     |
                                            +-----------------------+
```

The host helper (`tawc-exec`) opens an `adb forward` to the device-side
`LocalServerSocket`, sends a small header describing argv/env/cwd,
multiplexes local stdio over the connection in framed binary form, and
exits with the child's exit code. The device-side accept loop spawns
the child via `ProcessBuilder` with stdio piped, runs three relay
threads, and closes the socket when the child exits.

## Wire protocol

### Header (text, line-oriented)

UTF-8, LF-terminated lines, terminated by an empty line. Three
mutually exclusive header forms — pick one:

**ARGV form** — fork-exec a child process. Used for raw command exec
(file copies, `cat /proc/foo`, etc.).

```
TAWCEXEC 1
ARGV /system/bin/sh
ARGV -c
ARGV echo hello; cat
ENV PATH=/system/bin:/system/xbin
ENV HOME=/data/local/tmp
CWD /data/local/tmp
OP_TITLE Repair distro X

```

`OP_TITLE` is optional and may appear on ARGV or RUNINSIDE forms (not
on ACTION — those handlers manage their own log screen). When set,
the broker registers a [me.phie.tawc.ops.Operation] in
`OperationsRegistry`, opens `LogScreenActivity` so the panel attaches,
and tees every chunk of process stdout/stderr into the op's log flow
(line-buffered) for the in-app surface. The host TTY still sees the
raw byte stream — the mirror is purely additive.

**ACTION form** — invoke an in-process broker action. Used by
`tawc-exec --action install --arg id=arch …` (and the
`scripts/install-distro.sh` / `scripts/uninstall-distro.sh` wrappers
on top). The broker looks the action name up in
`me.phie.tawc.dev.ActionRegistry`; the handler runs in-process and
streams its output back over the same stream-frame protocol. See
`me.phie.tawc.install.InstallActions` for the install/uninstall
handlers.

```
TAWCEXEC 1
ACTION install
ARG id=arch
ARG method=proot
ARG mirrorProxy=http://127.0.0.1:8080/proxy/

```

**RUNINSIDE form** — run a command inside an installed chroot. The
broker reads the install's recorded method from `metadata.json` and
calls `InstallationMethod.startInside`, the single Kotlin entry point
for "enter the chroot" (notes/rootfs-sessions.md). Used by
`rootfs-run.sh`, `install-test-deps.sh`, and the integration
test crate. Omit `CMD` for interactive `bash -l`. `OP_TITLE` is
optional and behaves the same as on ARGV form.

```
TAWCEXEC 1
RUNINSIDE arch
CMD pacman -Syu
OP_TITLE arch: pacman -Syu

```

- `TAWCEXEC 1` — magic + version. Must be the first line.
- `ARGV <s>` (ARGV-form only) — one per arg. At least one required.
  `argv[0]` is the program path; the broker passes it through to
  ProcessBuilder unchanged. Strings are UTF-8 and may not contain LF.
- `ENV <KEY=VALUE>` (ARGV-form only) — zero or more. **Replaces** the
  inherited environment entirely (no merge). Omit for an empty env.
- `CWD <path>` (ARGV-form only) — optional. Default: the app process's
  cwd.
- `ACTION <name>` (ACTION-form only) — exactly one. Must be a name
  registered at app startup; unknown names get a STREAM_ERR + exit
  −1.
- `ARG <key>=<value>` (ACTION-form only) — zero or more. Per-action
  semantics; see the handler's docstring.
- `RUNINSIDE <install-id>` (RUNINSIDE-form only) — exactly one.
  Resolves to `<distros>/<id>/`; unknown id gets STREAM_ERR + exit −1.
- `CMD <command>` (RUNINSIDE-form only) — optional. The command runs
  via `bash -lc <command>` inside the rootfs. Omit for interactive
  `bash -l`.
- `OP_TITLE <title>` (ARGV-form / RUNINSIDE-form only) — optional. When
  set, mirrors process stdio into a `LogScreenActivity` panel titled
  with `<title>`. See [BrokerOpMirror].
- ARGV / ACTION / RUNINSIDE are mutually exclusive in one header;
  combining is a protocol error.
- The empty line terminates the header. Frame stream begins
  immediately after.

### Frames (binary, multiplexed)

Each frame: `[u8 stream] [u32 length BE] [length bytes...]`.

Streams used **client → server**:

| ID | Meaning              | Length     |
|----|----------------------|------------|
| 0  | stdin data           | 1..=65536  |
| 4  | stdin EOF            | 0          |

Streams used **server → client**:

| ID | Meaning              | Length     |
|----|----------------------|------------|
| 1  | stdout data          | 1..=65536  |
| 2  | stderr data          | 1..=65536  |
| 3  | exit                 | 4 (i32 BE) |
| 5  | broker error string  | UTF-8      |

Exit payload is a signed 32-bit integer big-endian: `>=0` for normal
exit, `<0` for `-signal_number` if killed by a signal. (Java's
`Process.exitValue()` doesn't distinguish; we always report normal
exit unless we can confirm a signal — current implementation: always
the value `Process.exitValue()` returned, signed.)

After sending the exit frame the server closes the socket.

If the broker hits an internal error (e.g. ProcessBuilder failed to
launch), it sends a `5` frame with the error message, then a `3` frame
with `-1`, then closes.

### Cancellation

If the client closes the socket without sending a stdin EOF + exit
handshake, the server interprets it as cancellation and
`destroyForcibly()`s the child (SIGKILL on Android).

This is the property that fixes orphan-on-parent-death: `tawc-exec`
running on the host gets SIGPIPE'd / Ctrl-C'd / killed → the TCP
forward closes → adbd closes the device-side abstract socket → the
broker thread's read returns EOF → SIGKILL goes to the child. Same
domain, no PDEATHSIG quirks.

## Security model

The broker exists in dev builds and exposes execution-as-the-app to
anything that can connect to its `LocalServerSocket`. We have to make
sure "anything" is just the dev workflow, not random apps on the device.

Two layers of defense:

### 1. Build-type gate

`ExecBrokerService` is only instantiated when `BuildConfig.DEBUG`. The
`MainActivity` startup code has an `if (BuildConfig.DEBUG)` guard
around `startForegroundService`, and the service's `onStartCommand`
double-checks. **Release APKs never bind the socket.** A user phone
that ships a release build has zero exposure even if the per-connection
auth had a bug.

### 2. `SO_PEERCRED` on every connection

Even on debug builds, the abstract socket name is in the kernel's
namespace and any process *could* try to connect. The accept loop
rejects every connection whose peer uid isn't in `{0 (root), 2000
(shell)}`:

```kotlin
val cred = client.peerCredentials
if (cred.uid !in allowedUids) { client.close(); continue }
```

`SO_PEERCRED` is populated by the kernel from the connecting task's
real credentials at connect time. **An app cannot spoof its uid** —
no setuid trick, no namespace trick. The kernel just reports
`current->cred->uid` from inside the connect path.

Why these uids:

- `0` (root) — covers `adb shell su -c` and rooted-emulator adbd.
- `2000` (shell) — covers `adb shell` and standard adbd. This is the
  uid `adb forward` connections come in as on user/userdebug builds.

Other apps (`untrusted_app`, uid 10xxx) get rejected. So do isolated
processes, system apps, anything else.

The auth boundary is therefore "anyone with adb access to the device,"
which is exactly the same boundary every other adb-driven dev workflow
already has. The broker doesn't expand the attack surface — it just
gives that pre-existing trust boundary a clean way to drive the app
without the `run-as`/`su` detours.

### What if SELinux blocks the connection?

Android's SELinux policy gates which domains can `connectto` an
abstract socket bound by `untrusted_app`. On userdebug/eng builds and
debug-built apps, `adbd` is generally allowed to connect to abstract
sockets exposed by debuggable apps (the same mechanism LLDB and
profiling tools use). On user builds + release apps the connection
typically wouldn't go through, but we don't ship the broker on
release builds anyway.

## Lifecycle

The broker is a plain background thread spawned from
`TawcApplication.onCreate` (debug builds only) — *not* a Service. We
went through a foreground-service version first, but
`startForegroundService` from `Application.onCreate` is unreliable on
Android 12+ when the cold-start was driven by something other than a
foreground activity (e.g. `am start ...InstallActivity` directly), and
the broker has no UI / notification needs of its own. A daemon thread
plus the kernel keeping the process alive while it has user threads is
enough.

`TawcApplication.onCreate` also calls
`me.phie.tawc.install.InstallActions.registerAll()` to populate
`ActionRegistry` with the `install` / `uninstall` handlers before any
client connection arrives.

Cold-starting any of the app's entry points (MainActivity,
InstallActivity, CompositorActivity) brings up the broker. Stopping the
app (`am force-stop me.phie.tawc`) tears down the broker thread and
any in-flight children. Children run in `untrusted_app` (same domain
as the app) and PDEATHSIG-respect their parent on exit, so force-stop
kills everything cleanly.

### Cancellation: descendant kill

When the host disconnects mid-command, `destroyForcibly()` SIGKILLs
the immediate child but doesn't propagate to its descendants — bash
running `sleep 600` would leave the sleep behind as an init orphan.
The broker handles this by:

1. Walking `/proc/<pid>/status` to build a pid → ppid map.
2. BFS-ing from the immediate child to enumerate every descendant.
3. SIGKILL-ing each captured pid (via `Os.kill`) before
   `destroyForcibly` runs the immediate child.

We can't use `/proc/<pid>/task/<pid>/children`: it requires kernel
`CONFIG_PROC_CHILDREN`, which Android stock kernels (including the
emulator's) don't enable.

The BFS catches descendants whose `ppid` still chains back to the
immediate child. It does **not** catch processes that have been
deliberately reparented to init (`ppid == 1`) — most commonly the
gpgme/libgpg-error `posix_spawn` "double-fork-prevent-zombies" dance
that pacman's signature verify uses on every package. Those are
caught instead by the **whole-UID kill** that fires when the app
itself dies (e.g. `am force-stop me.phie.tawc`): Android SIGKILLs
every process running under the app's uid regardless of parent
chain. Any cleanup that depends on PPid chains alone is incomplete;
the UID-wide kill is the actual safety net for orphaned descendants.

## Host helper

`tawc-exec` is a small Rust binary at `tools/tawc-exec/`. Build it once
with `bash scripts/build-tawc-exec.sh`; the output binary is cached at
`build/tawc-exec/tawc-exec`. Scripts source `scripts/lib/tawc-exec.sh`
to get a `TAWC_EXEC_BIN` env var that auto-builds if missing. The
sourced lib also defines a `tawc_exec` shell function for ergonomics —
note that shell functions can't be `exec`'d, so when the caller wants
exec semantics it should use `"$TAWC_EXEC_BIN"` directly.

Usage:

```
tawc-exec [--cwd DIR] [--env K=V ...] [--op-title TITLE] -- ARGV0 ARGV1 ...
tawc-exec --action NAME [--arg K=V ...]
tawc-exec --in-rootfs ID [--op-title TITLE] [-- CMD ...]
```

`--op-title TITLE` opts into the in-app log-screen mirror — the broker
posts an Operation, opens `LogScreenActivity`, and streams stdout /
stderr lines into it as they come in. `rootfs-run.sh` sets a
sensible default title for non-interactive command invocations
(scriptable callers can override via `TAWC_OP_TITLE=<title>` or set
`TAWC_OP_TITLE=` to suppress); integration tests don't set it (they'd
flicker the screen open hundreds of times per run).

It:

1. Picks a free TCP port.
2. Runs `adb forward tcp:<port> localabstract:me.phie.tawc.exec`.
3. Connects to `127.0.0.1:<port>`.
4. Sends the header.
5. Multiplexes local stdin (frame 0) ↔ socket; demultiplexes socket
   frames into local stdout/stderr.
6. On socket close, reads the exit code from the last frame received
   and exits with it. (If no exit frame: exit 255.)
7. Tears down the `adb forward` on exit.

The TCP port is bound to `127.0.0.1` only and lives just long enough
for the one connection.

## What's not yet done

- **No PTY support.** Stdio is plain pipes. `bash`'s readline detects
  `!isatty(0)` and falls back to the simpler line-buffered REPL, which
  works fine for entering commands but doesn't give arrow keys / line
  editing / job control. Add a PTY mode if/when interactive shell UX
  becomes a complaint. The protocol has room for a `USE_PTY` header
  line.
- **No window-size propagation.** Same — protocol can carry a
  `WINSIZE rows cols` header line later.
- **No multi-command pipelining over one connection.** Each connection
  runs exactly one command. This keeps the protocol simple and matches
  every current use case.

## What still uses `su` / `run-as`

After the broker rollout, the only remaining privileged paths in dev
workflows are:

- **`chroot` install method** (`scripts/rootfs-run.sh`,
  `tests/integration/src/adb.rs`). `chroot(2)` requires
  `CAP_SYS_CHROOT` — fundamental, not a workaround. tawcroot and proot
  installs all go through the broker.
- **`tawcroot/test --device`**. The handler tests call `mknod()` to
  verify the seccomp handler routes mknod correctly, and SELinux
  denies `shell:shell_exec → shell_data_file:file mknod`. App uid
  can mknod in its own data dir, but `untrusted_app` can't `execute`
  files there (W^X policy), so we'd have to ship every freshly-built
  test binary as a jniLib. Easier: the test runner uses `su -c`,
  while production tawcroot stays rootless. (`tawcroot/test --host`
  unprivileged covers most iteration anyway.)
- **`scripts/emulator.sh` setup**: `setenforce 0`, Magisk policy.
  One-time emulator bootstrap, irrelevant once the AVD exists.

Everything else — fixture installs, integration tests, log probes,
chroot file inspection, the readiness check, the install-id probe,
host-driven shell sessions inside the chroot — runs as the app uid
through the broker.

## See also

- `app/src/main/java/me/phie/tawc/dev/ExecBroker.kt` — the listener.
- `app/src/main/java/me/phie/tawc/dev/ExecBrokerSession.kt` — per-
  connection logic (header parsing, frame routing, descendant kill).
- `tools/tawc-exec/` — host helper.
- `scripts/lib/tawc-exec.sh` — sourced by host scripts.
