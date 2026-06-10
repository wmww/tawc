# ando — run Android commands from inside the rootfs

`ando <cmd> [args…]` (named like sudo, but for Android) runs `<cmd>`
as a plain Android process: no tawcroot seccomp filter, no path
translation, no fake-root identity, no rootfs env / libhybris
`LD_PRELOAD`/`LD_LIBRARY_PATH` baggage. stdio stays on the caller's
fds, exit status propagates. On a rooted phone `ando su -c '…'` (or
`ando sudo` where that exists) gets device root with no special
handling — the spawned child is an ordinary app-uid process, so the
normal Magisk su client/daemon flow applies (the Magisk app must
grant me.phie.tawc).

## Why this must be a broker, not an in-process escape

tawcroot's seccomp filter is permanent kernel state: it is inherited
across fork/exec by every guest descendant and can never be removed,
only stacked on (notes/tawcroot.md §"Why non-PIE"). Any process
forked from inside the rootfs carries the filter forever, and once
the trapped syscalls fire without tawcroot's SIGSYS handler the
process dies. A hypothetical tawcroot "passthrough mode" would also
keep the per-syscall trap cost, keep the exec-dance requirement for
every child, and still leave the guest env in the way.

So escape = ask a process that **never had the filter** to spawn the
command. The app process is that process, and it already demonstrates
the exact pattern in the debug exec broker (notes/exec-broker.md):
socket listener, header, spawn, relay, descendant kill. ando is a
production sibling of that broker with one big upgrade: instead of
relaying stdio through frames, the client passes its real stdin/
stdout/stderr fds over the socket (SCM_RIGHTS), so the Android child
writes to the guest's pty/pipes directly — `isatty` is true in a
terminal session, window size is inherent (same pty), no relay
threads, no latency.

```
guest (under tawcroot)                 app process (untrusted_app, no filter)
+--------------------+                 +---------------------------------+
| bash               |  abstract unix  | ando broker (Rust thread in the |
|  └─ ando (client)  | ──── socket ──→ | compositor .so, started by one  |
|     header+fds 0/1/2/cwd, SCM_RIGHTS | JNI call at app startup)        |
|     then: signal msgs →              |  SO_PEERCRED uid == getuid()    |
|     ← exit status                    |  recvmsg fds → fork/exec child  |
+--------------------+                 |  waitpid / kill(-pgid)          |
                                       |   └─ child: /system/bin/…, su   |
                                       +---------------------------------+
```

## What tawcroot already supports (verified in source)

- `connect(2)` to an **abstract** AF_UNIX socket passes through
  untranslated (`syscalls_socket.c::translate_unix_sockaddr` returns
  0 for `sun_path[0] == '\0'`). Same netns, same uid — reachable.
- `sendmsg` with `msg_name == NULL` (connected socket) is forwarded
  verbatim, `msg_control` untouched — SCM_RIGHTS fd passing works
  from the guest.
- `recvmsg` is deliberately not trapped — receiving the exit message
  is native speed.

## Components

### 1. Broker — Rust module `compositor/src/ando.rs`

Production (all build types). A standalone `std::thread` in the app
process, living in the compositor `.so` but **not** part of the
compositor event loop — same shape as the kumquat server thread
(notes/gfxstream-bridge.md "Kumquat in-process") and the compositor
thread itself (`lib.rs:191`). Binds an abstract AF_UNIX listener
`me.phie.tawc.ando.<uid>` (uid suffix so multi-user Android installs
don't collide in the kernel-global abstract namespace). One
connection-handler thread per accept, body wrapped in
`catch_unwind` so a parsing bug kills the connection, not the app
(the crate uses the default `panic = "unwind"`; keep it that way).
Logs under the existing `tawc-native` tag. Per-connection:

1. Check `SO_PEERCRED` uid == `getuid()`; else close. This is the
   entire auth model: only this app (which includes every rootfs
   guest — tawcroot is not a sandbox, CLAUDE.md/notes already say
   so) may spawn. Other apps/uids are rejected; the debug broker's
   `{0, 2000}` gate stays separate and debug-only.
2. Read the text header (same `\n`-escape value encoding as the
   ExecBroker protocol — reimplemented in Rust, wire-compatible):
   `TAWCANDO 1`, `ARGV …`×n, `ENV K=V`×n (extras, not a
   replacement), blank line.
3. `recvmsg` 1 protocol byte whose SCM_RIGHTS payload is exactly
   4 fds: stdin/stdout/stderr plus an `O_PATH` fd of the client's
   cwd (see "cwd handling" below).
4. Build env, do the `PATH` search for a bare `argv[0]`, spawn the
   child (below), close the broker's copies of the 4 fds.
5. The connection thread loops reading `SIG <n>` lines →
   `kill(-pgid, n)`. A waiter thread blocks in `waitpid(pid)`; on
   exit it writes `EXIT <code>` (`128+sig` for signal deaths) and
   closes the socket. Socket EOF before exit (client killed, session
   torn down) → SIGKILL the child's process group — same
   orphan-prevention contract as the debug broker; the uid-wide kill
   on app death remains the backstop.

Spawn: child setup is `dup2(fdN→N)`, `fchdir(cwdFd)` (fall back to
the app files dir on failure), `setpgid(0, 0)` (child leads its own
group so `kill(-pgid, …)` reaps the whole tree), then exec. Either
`posix_spawn` with file_actions (`addfchdir_np` needs a minSdk
check) or a small fork+exec like ART's own `ProcessImpl.forkAndExec`,
which does exactly this dance in this same process type — no new
fork-safety risk either way.

Verify once during implementation: nothing else in the app process
does a `waitpid(-1)` that could steal our child's status (modern ART
ProcessImpl waits per-pid; confirm with a test that runs an ando
spawn and a `ProcessBuilder` spawn concurrently).

Env for the child: the app process's own environment (it carries the
zygote-provided `PATH`, `ANDROID_ROOT`, `ANDROID_DATA`, …) plus a
sane `PATH` fallback (`/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin`),
plus client `ENV` extras (client sends `TERM` by default). Nothing
from the guest env leaks unless explicitly forwarded.

cwd handling: the child should start in the directory the caller is
standing in, but the client can only name it in guest terms —
tawcroot reverse-translates `getcwd()`/`/proc/self/cwd`, so guests
never see host paths. Strings can't cross the boundary; **fds can**:
fd contents aren't virtualized, so the client's
`open(".", O_PATH | O_DIRECTORY)` yields a real host fd for the
directory its cwd actually resolved to. The client passes that as
the 4th SCM_RIGHTS fd; the child fchdir()s to it. This needs no
install-id plumbing, no `InstallationStore` lookup, no duplicated
`TawcrootMethod.bindSpecs()` prefix table — bind-mounted cwds
(`/usr/share/tawc`, `/system`, …) resolve correctly for free, and
the guest can only hand over directories reachable through its own
view. Fd unusable (cwd deleted, fchdir fails) → fall back to the app
files dir. This makes `cd ~/Downloads && ando cp file /sdcard/…`
-shaped flows work (subject to Android's own storage rules).

### Why a Rust broker thread (not Kotlin + a JNI spawn helper)

An earlier revision of this plan put the listener in Kotlin
(mirroring the debug ExecBroker) with one JNI call for the
fd-wiring spawn that `ProcessBuilder` can't do. Re-examined, that
split kept the worst of both worlds, and two of its three
justifications didn't survive scrutiny:

- **App-state access**: evaporated when cwd became an fd. The broker
  needs nothing from the Kotlin world — no `InstallationStore`, no
  settings, no UI.
- **Crash containment**: overstated. With the crate's default
  `panic = "unwind"`, a panic in a pure-Rust thread that doesn't
  cross FFI kills only that thread (the compositor thread already
  relies on this shape), and per-connection `catch_unwind` makes the
  boundary explicit. Kotlin's only residual edge is the absence of a
  memory-unsafety class — but the broker is safe Rust, and the
  hybrid design kept a native spawn helper anyway.
- **Reuse**: real but trivial — peercred is one line in Kotlin vs
  ~ten in Rust; the header codec is ~50 lines either way.

Meanwhile the entire data path is unix-syscall-shaped — accept,
SO_PEERCRED, recvmsg/SCM_RIGHTS, dup2/fchdir/setpgid/exec, waitpid,
kill — and in Rust it is one coherent piece in the same idiom as the
C client on the other end of the protocol. The Kotlin route's
flakiest seams (`LocalSocket.getAncillaryFileDescriptors()` delivery
quirks, extracting raw ints from `FileDescriptor` private fields,
per-connection fd marshalling across JNI) simply don't exist. The
JNI surface shrinks to a single startup call.

Cost accepted: future app-side niceties (ops/log-screen mirroring,
settings gating) would need a reverse-JNI upcall — `NativeBridge`
already does plenty of those — and app-side services in this repo
are otherwise Kotlin, so this is a precedent deviation worth one
docstring line.

### 2. Startup hook — one JNI call

`NativeBridge.nativeStartAndoBroker(socketName: String)`, called
from `TawcApplication.onCreate` (all build types; touching
`NativeBridge` also triggers its `System.loadLibrary`). Kotlin
computes the socket name (single source shared with `RootfsEnv`'s
`TAWC_ANDO_SOCKET`); Rust spawns the listener thread and returns.
The broker is alive whenever the app process is, independent of
compositor startup.

### 3. Guest client — `tools/ando/` → `/usr/local/bin/ando`

Small static bionic C program (NDK, both ABIs). It runs as a guest
under tawcroot, so it needs no glibc/per-distro builds. Behavior:

1. Parse `ando [-e K=V]… [--] cmd [args…]`. No args → usage.
2. Socket name from `$TAWC_ANDO_SOCKET` (fallback: built-in default).
3. `open(".", O_PATH | O_DIRECTORY)` — tawcroot translates the path,
   so the fd points at the real host directory. Connect, send
   header, send the protocol byte with SCM_RIGHTS carrying fds
   0/1/2 + the cwd fd.
4. Install handlers for INT/TERM/HUP/QUIT that `write()` `SIG <n>`
   lines (async-signal-safe); block in `read()` until `EXIT <code>`;
   exit with that code. Connection refused → one clear error line
   ("ando broker not running — is the tawc app alive?").

Build: a new `tools/ando/build.sh` (or a target inside
`tawcroot/build.sh`, which already has the NDK toolchain plumbing),
staged as `app/src/main/jniLibs/<abi>/libando.so` — same
jniLib-extractor trick as tawcroot. Update notes/building.md.

Install into the rootfs: a new `AndoInstallProvider : TawcInstallProvider`
that copies from `applicationInfo.nativeLibraryDir/libando.so` to
`/usr/local/bin/ando` (mode 0755). Riding `TawcInstaller` gets
upgrade re-stamping (`tawcStamp`) for free, and nativeLibraryDir
already contains only the device's ABI — no asset tar needed.
`/usr/local/bin` is on `RootfsEnv`'s PATH and every distro profile's
PATH; flag in the provider docstring that this is a tawc-owned file
in user-namespace territory (alternative `/usr/lib/tawc/bin` +
symlink if it ever conflicts).

### 4. RootfsEnv addition

One new var in `RootfsEnv.build()` (all methods, all backends):
`TAWC_ANDO_SOCKET=me.phie.tawc.ando.<uid>`. The env map stays
install-agnostic (cwd travels as an fd, not an id). If the user
wipes the env, ando falls back to the built-in default socket name.

## Semantics and known limits

- **Interactive use** (`ando sh`, `ando su`): the child holds the
  guest's pty fd, so reads/writes/winsize work (a different-session
  process may freely read/write a pty it doesn't control). It can
  never make that pty its controlling terminal (the rootfs session
  leader owns it), so full job control inside an ando shell —
  `tcsetpgrp`, Ctrl-Z suspend of inner pipelines — won't work;
  shells detect this and run without job control. Ctrl-C works via
  the client's SIG forwarding. If this ever matters, a later
  `--pty` mode can have the broker allocate a fresh pty pair and
  relay — the protocol has room; don't build it now.
- **fake-root vs real uid**: inside the rootfs you look like root;
  `ando id` truthfully reports the app uid (10xxx). That asymmetry is
  the feature, but worth one line in user-facing docs.
- **su descendants**: the Magisk daemon spawns the actual root
  process outside our process tree; disconnect-kill only reaches our
  direct child (the su client). Acceptable — same as any app using su.
- The child is **not** under `setsid` (only setpgid), deliberately:
  it must keep using the caller's tty fds, and the rootfs-session
  invariant (notes/rootfs-sessions.md) governs rootfs entries, not
  Android-side spawns. Confirm gpg-agent-style pgrp symptoms can't
  apply (child never shares the guest's pgrp).

## Rejected alternatives

- **tawcroot passthrough mode** — see "Why this must be a broker".
- **Reuse the debug ExecBroker socket** — debug-only by design, uid
  gate is `{0, 2000}` (adb), frame-relay stdio loses tty semantics.
  Share the header-encoding helpers, nothing else.
- **Filesystem socket in `<appData>/share`** — would work (tawcroot
  translates `sun_path` through the bind, and binding it from Rust
  is easy), but abstract + peercred avoids per-distro socket-path
  questions, stale-socket-file cleanup, and the
  `/usr/share/tawc`-writable exposure growing another tenant.
- **Broker in the Rust compositor event loop** — couples ando to
  compositor startup (sessions can exist for install/test flows
  without it). The broker is a standalone thread precisely so it's
  alive whenever the app process is.
- **Kotlin listener + JNI spawn helper** — the previous revision of
  this plan; see "Why a Rust broker thread" above.

## Implementation order

1. **Broker + client MVP**: `compositor/src/ando.rs` +
   `nativeStartAndoBroker`, client binary built and adb-pushed by
   hand; prove header/fd-pass/exit/signal path on the emulator
   (`.tawctarget`), including `ando /system/bin/id`, `ando getprop`,
   exit codes, Ctrl-C.
2. **Packaging**: build-script staging to jniLibs, AndoInstallProvider,
   `TAWC_ANDO_SOCKET` in RootfsEnv, notes/building.md update.
3. **Tests + docs**: integration tests (below), a short
   `notes/ando.md` describing protocol + security model, README index
   updates, user-facing mention wherever the terminal is documented.

Testing (integration suite, RUNINSIDE spawns):
- `ando /system/bin/getprop ro.product.cpu.abi` succeeds, output
  lands on guest stdout.
- exit-code propagation (`ando sh -c 'exit 7'` → 7).
- env hygiene: `ando sh -c 'echo $LD_PRELOAD$LD_LIBRARY_PATH'` empty.
- identity: `ando id -u` = app uid, not 0.
- signal: kill the in-rootfs ando mid-`ando sleep 100`; verify the
  Android-side sleep dies (no orphan).
- cwd fd: `ando sh -c 'pwd; touch x'` from a rootfs cwd prints the
  host path and creates the file there (visible in the guest cwd);
  repeat from a bind-mounted cwd (`/usr/share/tawc`).
- broker absent (debug-only test hook or socket name override):
  client errors cleanly, non-zero exit.

Verification items to clear early (cheap, before building much):
- tawcroot's exec path manual-loads a **static bionic** guest binary
  (the client) correctly — smoke it; if not, fix the loader or build
  the client dynamic against `/system/bin/linker64` (binds already
  expose /system).
- spawn primitives at our minSdk: `posix_spawn` +
  `POSIX_SPAWN_SETPGROUP` and `posix_spawn_file_actions_addfchdir_np`
  availability in bionic, else commit to the fork+exec shape.
- Magisk su grant flow from an ando child on the rooted phone
  (`ando su -c id` → uid 0) — needs the physical target and a manual
  grant tap the first time.
