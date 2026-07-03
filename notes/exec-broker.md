# Dev exec broker

A debug-build-only in-app service that lets the host run commands as the
app uid/domain — same SELinux context (`untrusted_app`) the production
launch path uses. Replaces all `run-as` and most `su` use in dev/test
workflows with a path that genuinely mirrors how things execute when a
user launches the app. Its production sibling for rootfs guests (not
the host) is the ando broker — see [ando.md](ando.md).

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
`tawc-exec --foreground-app --action install --arg id=arch ...`.
The broker looks the action name up in
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

The install action also takes `--arg externalBinds=<json>` — a JSON
`ExternalBind` array (omitted = none; there is no default bind set).
See notes/external-binds.md.

**RUNINSIDE form** — run a command inside an installed chroot. The
broker reads the install's recorded method from `metadata.json` and
calls `InstallationMethod.startInside`, the single Kotlin entry point
for "enter the chroot" (notes/rootfs-sessions.md). Used by
`rootfs-run.sh`, `run-integration-tests.sh`, and the integration
test crate. Omit `CMD` for interactive `bash -l`. `OP_TITLE` is
optional and behaves the same as on ARGV form. `GRAPHICS` is
optional and overrides the in-rootfs `GraphicsBackend` for this one
spawn (libhybris / gfxstream / cpu); without it `Settings.graphicsBackend`
(the user's UI pick) is used. Integration tests pin a backend per
spawn here so the suite exercises every backend without flipping the
persisted pref.

```
TAWCEXEC 1
RUNINSIDE arch
CMD pacman -Syu
GRAPHICS libhybris
OP_TITLE arch: pacman -Syu

```

- `TAWCEXEC 1` — magic + version. Must be the first line.
- `ARGV <s>` (ARGV-form only) — one per arg. At least one required.
  `argv[0]` is the program path; the broker passes it through to
  ProcessBuilder unchanged after decoding. Strings are UTF-8.
- `ENV <KEY=VALUE>` (ARGV-form only) — zero or more. **Replaces** the
  inherited environment entirely (no merge). Omit for an empty env.
  `KEY` is split off the first `=`; `VALUE` is decoded.
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
- `GRAPHICS <key>` (RUNINSIDE-form only) — optional. One of
  `libhybris` / `gfxstream` / `cpu`; unknown keys are rejected. When
  set, the broker passes this through to `InstallationMethod.startInside`
  and `RootfsEnv` uses it instead of `Settings.graphicsBackend` for
  this spawn only. Tests use it to run a single client under a
  specific backend without touching the persisted pref.
- `OP_TITLE <title>` (ARGV-form / RUNINSIDE-form only) — optional. When
  set, mirrors process stdio into a `LogScreenActivity` panel titled
  with `<title>`. See [BrokerOpMirror].
- ARGV / ACTION / RUNINSIDE are mutually exclusive in one header;
  combining is a protocol error.
- The empty line terminates the header. Frame stream begins
  immediately after.

#### Value encoding

Every value-bearing field — `ARGV` arg, `ENV` value half (after the
`=`), `CWD`, `ARG` value, `CMD`, `OP_TITLE` — is encoded so a `\n` in
user-supplied data doesn't end the header line early. The encoding is
small and reversible:

```
\\  ⇒ \\\\
\n  ⇒ \\n
\r  ⇒ \\r
```

It's a no-op for any value that contains none of those three chars
(the common case for ASCII shell commands), so normal text passes
through unchanged. See `tests/integration/src/exec_broker.rs::encode_value`
(host) and `ExecBrokerSession.kt::decodeValue` (device).

Programmatic identifiers — `ACTION` name, `RUNINSIDE` install id, and
the `KEY` half of `ENV K=V` / `ARG k=v` — are not encoded; they're
matched against registered handlers / regex-validated slugs / shell
identifier rules and never carry control chars.

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
`me.phie.tawc.install.InstallActions.registerAll()` (install /
uninstall), `me.phie.tawc.dev.InputActions.registerAll()` (the test
input handlers below), and `me.phie.tawc.dev.SettingsActions.registerAll()`
(the settings get/set actions below) to populate `ActionRegistry`
before any client connection arrives.

## Registered actions

| Action | Source | Purpose |
|--------|--------|---------|
| `install` | InstallActions | Run the install state machine; mirrors the [Operation] log + progress to host stdout/stderr; cancels on disconnect. Use `--foreground-app`. |
| `uninstall` | InstallActions | Same shape, opposite direction. Use `--foreground-app`. |
| `query-state` | InputActions | Calls `NativeBridge.nativeQueryState()` and prints a one-line `key=value` compositor-thread snapshot on stdout: client/toplevel/surface counts, frame + wlegl debug counters, output geometry, `xwayland_running`, and `xwayland_pids` (comma-separated live uid-owned Xwayland pids, zombies excluded). Schema lives in the format string in `compositor/src/event_loop.rs` and the parser in `tests/integration/src/compositor.rs`; unknown keys are ignored, so adding fields is backward-compatible. Observational only — doesn't change input state. Needs no focused activity. |
| `test-init` | InputActions | Per-test reset: swap `Settings` to an in-memory factory-default store, push live runtime settings, swap `NativeBridge.imeOutput` to a fresh `RecordingImeOutput`, clear the active IC, and ask attached Wayland/XWayland client windows to close. Prints `closed=N`; the Rust harness waits for a clean compositor only when `N > 0`, so the normal no-client path stays fast. Does not write `SharedPreferences`; app process death discards it. |
| `input-ready` | InputActions | Succeeds only when the focused `CompositorActivity` has an active `TawcInputConnection` for its own `SurfaceView`. Used by tests after `onShowKeyboard` so the first `ic-*` action cannot race IC creation. |
| `focused-editor-info` | InputActions | Test-mode observation of the last `EditorInfo` produced by `RecordingImeOutput` when it created/restarted the IC. Used for activity-scoped content-type coverage. |
| `focused-activity-id` / `focus-activity` | InputActions | Test navigation helpers for multi-document compositor tasks. The first returns the focused compositor Activity id; the second brings that existing `tawc://activity/<id>` document task forward through Android's normal Activity path, then fails unless the target gains window focus within 5s — `startActivity` silently no-ops when Android 10+ blocks background starts, so a tawc activity must already be foreground (from another app, use `su -c 'am start -n me.phie.tawc/.compositor.CompositorActivity -a android.intent.action.VIEW -d tawc://activity/<id>'`). |
| `ic-commit-text` (`text`) | InputActions | `TawcInputConnection.commitText(text, 1)`. |
| `ic-commit-completion` (`text`) | InputActions | `TawcInputConnection.commitCompletion(CompletionInfo(..., text))`. |
| `ic-commit-correction` (`offset`, `old`, `new`) | InputActions | `TawcInputConnection.commitCorrection(CorrectionInfo(...))`. |
| `ic-replace-text` (`start`, `end`, `text`) | InputActions | `TawcInputConnection.replaceText(start, end, text, 1, null)`. |
| `ic-set-composing-text` (`text`) | InputActions | `TawcInputConnection.setComposingText(text, 1)`. |
| `ic-set-composing-region` (`start`, `end`) | InputActions | `TawcInputConnection.setComposingRegion(start, end)`. |
| `ic-finish-composing` | InputActions | `TawcInputConnection.finishComposingText()`. |
| `ic-set-selection` (`start`, `end`) | InputActions | `TawcInputConnection.setSelection(start, end)`; currently succeeds only for no-op selection requests because text-input-v3 cannot move the client cursor. |
| `ic-delete-surrounding-text` (`before`, `after`) | InputActions | `TawcInputConnection.deleteSurroundingText(before, after)`. |
| `ic-delete-surrounding-text-codepoints` (`before`, `after`) | InputActions | `TawcInputConnection.deleteSurroundingTextInCodePoints(before, after)`. |
| `ic-send-key-event` (`keycode`) | InputActions | `TawcInputConnection.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode))`. |
| `ic-send-modified-key-event` (`keycode`, `ctrl`, `alt`, `shift`) | InputActions | `TawcInputConnection.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode, metaState))`. |
| `ic-finish-hidden-composing` | InputActions | Test-only stale-callback hook: calls `finishComposingText()` on the hidden test IC retained by `RecordingImeOutput` after keyboard hide. Normal `ic-*` actions still require the current focused IC. |
| `hardware-key` (`keycode`, optional `action`, `repeat`) | InputActions | Dispatch a `KeyEvent` through the focused Activity/view path; `action` is `press` (default), `down`, or `up`. |
| `back` | InputActions | Dispatch Android Back through the focused activity's back-press path (the entry the system OnBackInvoked callback routes into). Activity-level rather than system input dispatch — compositor Back handling lives below that boundary either way. |
| `set-graphics-backend` (`value`) | SettingsActions | Write `Settings.graphicsBackend` to the given `GraphicsBackend.key` (`libhybris` / `gfxstream` / `cpu`). In test mode this only mutates the in-memory store. Tests normally pass `--graphics` on each RUNINSIDE spawn instead. |
| `get-graphics-backend` | SettingsActions | Print the current backend key on stdout. |
| `set-output-scale` (`value`) | SettingsActions | Snap to the 0.25x grid, save `Settings.outputScale`, and push the live compositor output scale. In test mode this only mutates the in-memory store. |
| `get-output-scale` | SettingsActions | Print the current output scale. |
| `set-gtk3-broken-menus-workaround` (`enabled`) | SettingsActions | Save and push the live GTK3 broken menus workaround toggle. In test mode this only mutates the in-memory store. |
| `get-gtk3-broken-menus-workaround` | SettingsActions | Print the current GTK3 broken menus workaround setting. |

**Rule for input actions: every driver goes through `TawcInputConnection`.**
There is intentionally no broker action that calls `NativeBridge.native*`
trampolines directly. Tests act as a keyboard (the IC) or as an app
(observing `wayland-debug-app` events on the wayland side); never as
something poking the compositor in the middle. Earlier revisions had
bypass actions (`inject-text`, `set-composing`, …) that skipped the IC
— they were deleted because text-input-v3's done-ordering produces
correct GTK observables on the wayland side regardless of what the IC
computed, so a buggy IC could pass bypass tests and a wayland-side
assertion became a redundant proof of text-input-v3. Driving every
scenario through IC closes that. See `notes/text-input.md`
"Test infrastructure note" for the rationale.

All normal `InputActions` `ic-*` handlers require a focused
`CompositorActivity` and an active `TawcInputConnection` targeting that
activity's `SurfaceView`, then post the call to the main looper. The
handler resolves the activity via `CompositorService.focusedActivity()`
(walking `activities: Map<String, WeakReference<…>>` for
`hasWindowFocus()`). No focused activity, no matching IC, or an IC method
returning `false` becomes a non-zero broker exit instead of a silent skip.

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

`tawc-exec` is a small Rust binary target in `tests/integration/`. Host
scripts call `scripts/tawc-exec.sh`, which rebuilds the helper when
sources are newer, caches it at `build/tawc-exec/tawc-exec`, and then
execs the cached binary. Use `scripts/tawc-exec.sh --clean` to force a
rebuild.

Usage:

```
tawc-exec [--foreground-app] [--cwd DIR] [--env K=V ...] [--op-title TITLE] -- ARGV0 ARGV1 ...
tawc-exec [--foreground-app] --action NAME [--arg K=V ...]
tawc-exec [--foreground-app] --in-rootfs ID [--graphics KEY] [--op-title TITLE] [-- CMD ...]
```

`--foreground-app` starts `MainActivity` even when the app process is
already running. Install/uninstall actions need it because they start
`InstallationService` as a foreground service. `RUNINSIDE` does this
implicitly in CLI mode because the app may need to start the lazy
compositor foreground service before entering the rootfs; suite mode
honors only the explicit flag (see "Connect modes" below).

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

### Connect modes: suite port vs CLI

The host side has two connect modes, decided by `TAWC_EXEC_BROKER_PORT`:

- **Suite mode** (env var set): `scripts/run-integration-tests.sh` starts
  the app once, opens a single `adb forward` for the whole run, and
  exports the port. Every broker request just connects to it — no
  per-request `adb forward`, no `pidof` app-running probe, no implicit
  `am start` foregrounding on RUNINSIDE. An explicit `foreground_app`
  request (install/uninstall helpers needing the foreground-app BAL
  allowance) still runs `am start`. A refused connection or header-write
  failure is a loud error ("re-run scripts/run-integration-tests.sh"),
  not a recovery path: no test force-stops the app, so a dead app
  mid-suite is a bug. (adb accepts the host TCP connection before
  dialing the device socket, so a dead app usually surfaces on the
  header write rather than at connect.)
- **CLI mode** (env var unset; `scripts/tawc-exec.sh` from a shell):
  must work against a cold app, so each request probes
  `pidof me.phie.tawc`, starts `MainActivity` if needed (and always for
  RUNINSIDE, which may need the foreground app to launch the lazy
  compositor service), and opens its own short-lived forward.

Host-transport rule for integration tests: per-request host process
spawns are banned; everything app- or compositor-facing goes through the
broker (`query-state` carries the debug counters and `xwayland_pids`,
`compositor::is_running` is a query-state round-trip, Back is the `back`
action). Allowed `Command::new("adb")` exceptions: `screencap_raw` for
pixel tests, the CLI/fallback paths inside `exec_broker.rs`, suite
setup/teardown in shell scripts, the wrapped tawcroot suite
(`tawcroot/test.sh --device`), and `adb::shell` for genuinely shell- or
su-side work that cannot run as the app uid (`ando` process counting,
`uninstall_wipe` su sweeps).

Deliberately rejected while killing the old per-request spawns:
force-stopping the Xwayland wayland connection in `test-init` (the
rootfs kill sweep already ends X11 clients; Xwayland restarts add churn
without isolation value) and a multiplexed long-lived broker protocol
(per-request local TCP connects are cheap).

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
- **`scripts/emulator.sh` setup**: `setenforce 0`, Magisk policy.
  One-time emulator bootstrap, irrelevant once the AVD exists.

`tawcroot/test.sh --device` runs as adb shell. The FIFO/mknod handler
checks are host-only because Android SELinux denies mknod on
`shell_data_file`; the rest of the device cleat suite stays rootless.

Everything else — fixture installs, integration tests, log probes,
chroot file inspection, the readiness check, the install-id probe,
host-driven shell sessions inside the chroot — runs as the app uid
through the broker.

## See also

- `app/src/main/java/me/phie/tawc/dev/ExecBroker.kt` — the listener.
- `app/src/main/java/me/phie/tawc/dev/ExecBrokerSession.kt` — per-
  connection logic (header parsing, frame routing, descendant kill).
- `tests/integration/src/exec_broker.rs` — host helper library.
- `tests/integration/src/bin/tawc-exec.rs` — CLI wrapper for scripts.
- `scripts/tawc-exec.sh` — host-side wrapper that auto-builds the helper.
