# External Wayland clients over Binder fd passing

> **Status:** app side implemented (AIDL + `ExternalWaylandService` + manifest +
> `nativeAdoptClientFd` + `event_loop` source + `aidl = true`), including the
> `onStartCommand` Binder-callback intake the Termux launcher uses. The Termux-side
> `wayland-run` helper is in [`termux-wayland/`](../termux-wayland/); on-device
> verification is still pending. See
> [../notes/android.md](../notes/android.md) ("Wayland Socket Sharing") for the
> as-built summary.

## Goal

Let a Wayland client running under Termux's uid (10549) render into
TAWC's Smithay compositor, on an unrooted device, without moving the
compositor out of the app and without a userspace byte relay.

This is the no-root half of `notes/android.md` "Wayland Socket Sharing":

> **Binder fd passing (preferred):** Compositor creates a `socketpair()`,
> passes one end to Termux via a ContentProvider or bound Service as a
> `ParcelFileDescriptor`. No `connect()` syscall occurs, so SELinux is
> never triggered.

## Why the existing socket cannot be reused

`activation.rs` binds `$TAWC_APP_SHARE_DIR/wayland-0`
(`app_paths.rs:29`) and chmods it `0777`. That is already as open as it
can be. Termux still cannot reach it:

1. **DAC:** `/data/data/me.phie.tawc` is `0700`; Termux cannot traverse
   into `share/`.
2. **SELinux:** a cross-app `connect()` between two `untrusted_app`
   processes is denied on Android 9+ (different MLS categories).

Both are checked on `connect()`. A file descriptor passed over Binder
never calls `connect()`, so neither applies.

## Design

One `SOCK_STREAM` socketpair per external client:

```
Termux client ──fd──▶ launcher ──fd over Binder──▶ ExternalWaylandService
                                                        │
                                              nativeAdoptClientFd
                                                        │
                                          calloop channel (OwnedFd)
                                                        │
                                     insert_client(UnixStream, ClientState)
```

Binder carries only the descriptor. Every Wayland byte stays on the
socketpair. The compositor keeps doing exactly what it does for the
in-container clients; the only new input is "here is a connected stream,
adopt it".

Scope: a launcher helper per client (e.g. `wayland-run foot`). A
long-lived `wayland-0` path inside Termux is a follow-up (see the end).

## App changes (the actual patch)

Six files, nothing in `activation.rs`.

### 1. AIDL

New `app/src/main/aidl/me/phie/tawc/compositor/IExternalWayland.aidl`:

```aidl
package me.phie.tawc.compositor;

import android.os.ParcelFileDescriptor;

interface IExternalWayland {
    /**
     * Returns one end of a fresh SOCK_STREAM socketpair. The compositor
     * keeps the other end and inserts it as a Wayland client.
     */
    ParcelFileDescriptor openClient();
}
```

### 2. Service

New `app/src/main/java/me/phie/tawc/compositor/ExternalWaylandService.kt`:

```kotlin
package me.phie.tawc.compositor

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.Process
import me.phie.tawc.BuildConfig

/**
 * Exported fd source for Wayland clients outside TAWC's uid.
 *
 * A Termux process cannot connect to `share/wayland-0` (DAC traversal +
 * SELinux cross-app connect), so it asks here for one end of a socketpair.
 * The other end goes into the compositor's display through
 * [NativeBridge.nativeAdoptClientFd]; no connect() ever happens.
 */
class ExternalWaylandService : Service() {

    private val binder = ExternalBinder()

    inner class ExternalBinder : IExternalWayland.Stub() {
        override fun openClient(): ParcelFileDescriptor {
            if (!callerAllowed()) {
                throw SecurityException(
                    "external Wayland: uid ${Binder.getCallingUid()} is not allowed"
                )
            }

            val pair = ParcelFileDescriptor.createSocketPair()

            // Rust owns the near end from here. If no compositor thread
            // exists it queues the fd and asks Kotlin to start one.
            if (!NativeBridge.nativeAdoptClientFd(pair[1].detachFd())) {
                pair[0].close()
                throw IllegalStateException("compositor refused the client fd")
            }

            // Binder dups this into the caller's process.
            return pair[0]
        }
    }

    override fun onCreate() {
        super.onCreate()
        // Sets TAWC_* env, caches the app context for the reverse-JNI
        // onActivationRequested callback, and starts the socket holder.
        // Idempotent; must precede the first openClient().
        CompositorService.ensureActivation(this)

        // Optional: start the compositor now instead of on the first
        // nativeAdoptClientFd queue miss.
        CompositorService.ensureRunning(this)
    }

    /**
     * Termux cannot bind, so it starts this service with a Binder token
     * in the intent and then calls openClient() on it. That synchronous
     * callback is what carries the fd (an Intent cannot). The one-way
     * adopted signal lets the launcher execve() the client safely.
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val host = intent?.getBundleExtra(EXTRA_HOST)?.getBinder(KEY_HOST)
        if (host != null) {
            try {
                val pfd = IExternalWayland.Stub.asInterface(host).openClient()
                if (pfd != null) {
                    NativeBridge.nativeAdoptClientFd(pfd.detachFd())
                }
            } catch (e: Exception) {
                // The launcher died or the handshake was already used.
            } finally {
                val data = Parcel.obtain()
                try {
                    host.transact(TRANSACTION_CLIENT_ADOPTED, data, null, IBinder.FLAG_ONEWAY)
                } finally {
                    data.recycle()
                }
            }
        }
        stopSelf(startId)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    companion object {
        const val EXTRA_HOST = "me.phie.tawc.extra.HOST"
        const val KEY_HOST = "host"
        // Matches WaylandRun; openClient() is FIRST_CALL_TRANSACTION (1).
        const val TRANSACTION_CLIENT_ADOPTED = 2
    }

    /**
     * No custom permission: the Termux-side helper is an `app_process`
     * process, not an installed package, so it cannot hold a manifest
     * permission. Check the uid's packages instead.
     */
    private fun callerAllowed(): Boolean {
        val uid = Binder.getCallingUid()
        if (uid == Process.myUid()) return true
        val packages = packageManager.getPackagesForUid(uid).orEmpty()
        if (packages.any { it == "com.termux" }) return true
        // Lets the PC-side prototype run from `adb shell`.
        return BuildConfig.DEBUG && packages.any { it == "com.android.shell" }
    }
}
```

`CompositorService.ensureActivation` and `ensureRunning` are already
public in its companion object. Nothing else in that file changes.

### 3. Manifest

In `app/src/main/AndroidManifest.xml`, next to `CompositorService`:

```xml
<service
    android:name=".compositor.ExternalWaylandService"
    android:exported="true" />
```

`exported` is required for Termux to start it. Both entry points are
gated by [ExternalWaylandAccess](../app/src/main/java/me/phie/tawc/compositor/ExternalWaylandAccess.kt):
the bind path uses `Binder.getCallingUid()`, and the start path recovers
the launcher's uid from `SO_PEERCRED` on the socketpair. Do not add a
custom permission: an `app_process` caller has no manifest and cannot be
granted one.

### 4. Native declaration

In `app/src/main/java/me/phie/tawc/compositor/NativeBridge.kt`, beside
`nativeStartActivation`:

```kotlin
/** Adopt one end of a client socketpair (Binder fd passing). The fd is
 *  owned by native after this call. False only if it could not be
 *  queued (too many pending, or the compositor is gone). */
external fun nativeAdoptClientFd(fd: Int): Boolean
```

### 5. Rust

#### `compositor/src/lib.rs`

Add the import:

```rust
use std::os::fd::{FromRawFd, OwnedFd};
```

Add the static next to `STATE_QUERY_SENDER`:

```rust
/// Fds handed over by `ExternalWaylandService`, plus the running
/// compositor's calloop sender. One mutex so a start cannot miss an fd
/// queued concurrently.
static EXTERNAL_CLIENTS: Mutex<(
    Option<smithay::reexports::calloop::channel::Sender<OwnedFd>>,
    Vec<OwnedFd>,
)> = Mutex::new((None, Vec::new()));
```

In `nativeStartCompositor`, immediately after
`*STATE_QUERY_SENDER.lock().unwrap() = Some(state_query_sender);`:

```rust
    let (external_sender, external_channel) =
        smithay::reexports::calloop::channel::channel::<OwnedFd>();
    {
        let mut slot = EXTERNAL_CLIENTS.lock().unwrap();
        slot.0 = Some(external_sender);
        let pending = std::mem::take(&mut slot.1);
        let sender = slot.0.as_ref().unwrap();
        for fd in pending {
            // A dead channel means this compositor run is already
            // exiting; the client sees EOF and reconnects.
            let _ = sender.send(fd);
        }
    }
```

Add it to `run_compositor`'s parameter list after `state_query_channel`
(note that `run_compositor` puts `surface_event_channel` before
`state_query_channel`):

```rust
    surface_event_channel: smithay::reexports::calloop::channel::Channel<SurfaceEvent>,
    state_query_channel: smithay::reexports::calloop::channel::Channel<StateQueryResponse>,
    external_channel: smithay::reexports::calloop::channel::Channel<OwnedFd>,
    initial_scale: f64,
```

Pass it in the same position at the `std::thread::spawn` call site:

```rust
            surface_event_channel,
            state_query_channel,
            external_channel,
            initial_scale,
```

Then pass it to `event_loop::run` after `state_query_channel` (here
`surface_event_channel` comes *after* `state_query_channel`):

```rust
        touch_channel, pointer_channel, text_input_channel, clipboard_channel, state_query_channel,
        external_channel,
        surface_event_channel,
        &RUNNING,
```

and add the matching parameter to `event_loop::run`:

```rust
    state_query_channel: Channel<mpsc::Sender<String>>,
    external_channel: Channel<OwnedFd>,
    surface_event_channel: Channel<SurfaceEvent>,
```

In `clear_senders()`:

```rust
    EXTERNAL_CLIENTS.lock().unwrap().0 = None;
```

New JNI entry point (next to `nativeStartCompositor`):

```rust
/// Take one end of a client socketpair from `ExternalWaylandService`.
/// The Java caller has already `detachFd()`d it, so this call owns it.
/// With no compositor running the fd is queued and Kotlin is asked to
/// start one; `nativeStartCompositor` drains the queue.
#[unsafe(no_mangle)]
pub extern "system" fn Java_me_phie_tawc_compositor_NativeBridge_nativeAdoptClientFd(
    mut env: JNIEnv,
    _class: JClass,
    fd: jint,
) -> jboolean {
    init_native_logging();
    cache_jni_globals(&mut env);

    // SAFETY: ParcelFileDescriptor.detachFd() transferred ownership.
    let fd = unsafe { OwnedFd::from_raw_fd(fd) };

    {
        let mut slot = EXTERNAL_CLIENTS.lock().unwrap();
        if let Some(sender) = slot.0.as_ref() {
            return sender.send(fd).is_ok() as jboolean;
        }
        if slot.1.len() < 32 {
            slot.1.push(fd);
        } else {
            return 0;
        }
    }

    crate::call_native_bridge_void("onActivationRequested", "()V", &[]);
    1
}
```

No `app_paths` or display access happens on this JNI thread; the fd
literally waits for the compositor thread.

#### `compositor/src/event_loop.rs`

Imports:

```rust
use std::os::fd::OwnedFd;
use std::os::unix::net::UnixStream;
```

`event_loop::run` gains the parameter:

```rust
    external_channel: Channel<OwnedFd>,
```

Add this source just before the listener source:

```rust
    // --- Source 8: externally supplied client fds ---
    // One end of a socketpair per external client. These are ordinary
    // Wayland streams; only the transport differs.
    loop_handle.insert_source(external_channel, |event, _, data: &mut TawcState| {
        let ChannelEvent::Msg(fd) = event else { return };
        let stream = UnixStream::from(fd);
        if let Err(e) = stream.set_nonblocking(true) {
            error!("external client: set_nonblocking failed: {}", e);
            return;
        }
        let client_state = ClientState::new(data.client_count.clone(), data.client_ids.clone());
        if let Err(e) = data
            .display_handle
            .insert_client(stream, Arc::new(client_state))
        {
            error!("external client: insert_client failed: {}", e);
        }
    })?;
```

That is the same `insert_client` call as the listener path
(`event_loop.rs:1143-1148`), so globals, seats, outputs, input and
rendering are unchanged.

### 6. Gradle

If AIDL is not already on, add it in `app/build.gradle.kts`:

```kotlin
    buildFeatures {
        buildConfig = true
        aidl = true
    }
```

## Termux side (implemented in `termux-wayland/`)

A small `app_process` program, the same shape as `am`. Source, build
script, and `wayland-run` wrapper live in `termux-wayland/`.

**`bindService` cannot be used from here.** `app_process` is not an app
process, so AMS has no record for the caller and
`ActiveServices.bindServiceLocked` throws
`SecurityException: Unable to find app for caller`. Borrowing
`com.termux`'s package name or a hidden `ActivityThread` does not help:
the check is on the calling Binder thread, not the package.

**`startService` cannot carry the fd either.** It tolerates the null
caller (`startServiceLocked` treats it as foreground, which is how `am`
works), but `ActivityManagerService.startService` rejects any intent
whose `hasFileDescriptors()` is true with
`IllegalArgumentException: File descriptors passed in Intent`.

**The working handshake: a Binder token in the Intent, fd in the
callback.**

1. **Hand the app a Binder token.**

   ```java
   HostBinder host = new HostBinder();   // Binder subclass
   Bundle extras = new Bundle();
   extras.putBinder("host", host);
   Intent intent = new Intent().setComponent(new ComponentName(
       System.getenv().getOrDefault("TAWC_PACKAGE", "me.phie.tawc.dev"),
       "me.phie.tawc.compositor.ExternalWaylandService"));
   intent.putExtra("me.phie.tawc.extra.HOST", extras);
   startService(intent);   // IActivityManager.startService by reflection, caller = null
   ParcelFileDescriptor clientEnd = host.awaitAdopted(30_000);
   ```

   A Binder is not a file descriptor, so AMS accepts the Intent.
   `app_process` starts its Binder thread pool in
   `AppRuntime.onStarted()`, before `main()`, so the callback is handled
   while `main()` waits.

2. **Answer `openClient()`, wait for adoption, then `execve` the client.**

   ```java
   // HostBinder.onTransact: the app calls the existing
   // IExternalWayland.openClient(), transaction 1.
   ParcelFileDescriptor[] pair = ParcelFileDescriptor.createSocketPair();
   clientEnd = pair[0];                  // stays here for the child
   reply.writeNoException();
   reply.writeInt(1);                    // AIDL non-null marker
   pair[1].writeToParcel(reply, 0);      // Binder dups it
   pair[1].close();
   ```

   The app's `onStartCommand` reads the token, calls `openClient()`, and
   passes the returned fd to `nativeAdoptClientFd`. Reusing the app's own
   AIDL means neither side needs a new interface. It then sends a
   **one-way adopted signal** (transaction 2, not in the AIDL):

   ```java
   // HostBinder.onTransact, transaction 2.
   adopted.countDown();
   ```

   **The client cannot be spawned with `ProcessBuilder`.** Android's
   `UNIXProcess` child calls `closeDescriptors()` before exec, which
   closes every fd ≥ 4 (`FAIL_FILENO + 1`); the socketpair never reaches
   the client and `wl_display_connect` fails. Instead the launcher
   `execve`s itself, which keeps every descriptor without `FD_CLOEXEC`:

   ```java
   // Wait for the app's adopted signal first: execve destroys our Binder
   // threads, and the signal is only sent after the fd reply is delivered.
   ParcelFileDescriptor clientEnd = host.awaitAdopted(30_000);

   // createSocketPair sets CLOEXEC on Q+; clear it so execve inherits the fd.
   Os.fcntlInt(clientEnd.getFileDescriptor(), F_SETFD, 0);
   Map<String, String> env = new HashMap<>(System.getenv());
   env.put("WAYLAND_SOCKET", Integer.toString(clientEnd.getFd()));
   // execve does not search PATH, so resolve it ourselves.
   Os.execve(resolve(args[0]), args, toEnvp(env));
   ```

   `libwayland-client` checks `WAYLAND_SOCKET` **before**
   `WAYLAND_DISPLAY`, takes ownership of the fd, and uses it as the
   display transport. The client must call `wl_display_connect(NULL)`;
   all normal toolkits do.

`WAYLAND_SOCKET` only works for one client, so `wayland-run <cmd>` is
the interface. Build with `termux-wayland/build.sh` (`javac` against
`android.jar` or the bundled stubs, plus `d8`), `chmod 0400` the dex
container (Android 14 rejects executing writable dex), and run through
`app_process`.

The start arrives via AMS, so `onStartCommand` cannot see the original
caller uid and any app may start the exported service. Fine for a dev
feature; see `termux-wayland/README.md`.

## Build and test on the PC

1. Build the debug APK (`scripts/build-app.sh`, or
   `./gradlew :app:assembleDebug`) and install it.
2. Start a client through the helper. The fd is queued until the
   compositor's event loop runs; the client blocks in `wl_display_connect`
   until the display exists.
3. Verify:
   - `wayland-info` prints `wl_compositor`, `wl_shm`, `xdg_wm_base`,
     `wl_seat`, `wl_output`.
   - `weston-simple-shm` or `foot` opens a TAWC window.
   - `adb logcat -s tawc-native` has no `insert_client failed`.
   - `nativeQueryState` reports one client.
4. For a no-Termux smoke test, run the same helper from `adb shell`
   (uid 2000); debug builds pre-allow `com.android.shell`, so the
   `startService` path accepts it.

## Risks and notes

- **Identity: `SO_PEERCRED` names the socketpair's creator.** For the
  `startService` path that is the Termux launcher, so `nativePeerUid`
  recovers the real caller even though AMS is the Binder caller. The
  credential is fixed at `socketpair()` time, so a later Binder call
  cannot spoof it. A relay design would report the relay's uid for every
  client; per-client policy would need the uid as an `openClient` argument.
- **One fd per client.** This is deliberate; no listener or relay.
- **Queue cap 32.** More pending fds are refused with a closed pair.
  The client sees EOF.
- **Compositor restarts.** `clear_senders()` drops the sender; a
  restart recreates the channel and drains the queue. A send that loses
  the race with teardown just closes that client.
- **No permission on the service.** Access is decided in
  [ExternalWaylandAccess](../app/src/main/java/me/phie/tawc/compositor/ExternalWaylandAccess.kt)
  from `Settings.externalAccessMode` (Disabled / Allowed apps only / Any
  app) and the package allowlist. The default mode is Allowed apps only,
  seeded with `com.termux` (`com.android.shell` too in debug) only while
  the preference key is absent, so upgrading users keep a working
  launcher while an explicit empty allowlist stays empty. Because Android
  11+ filters `getPackagesForUid` by package visibility, the manifest
  declares `<queries>` for `com.termux`, `com.android.shell` (debug), and
  `MAIN`/`LAUNCHER` intents; without the first two the allowlist silently
  denies everyone, and without the last the picker lists nothing.
- **Both entry points are gated.** `onStartCommand` authenticates via
  `SO_PEERCRED` on the fd before adopting it, so the start path is no
  longer an open door. A denial closes the fd and sends the reason on
  transaction 3; `onCreate` also skips activation entirely while the mode
  is Disabled. An older launcher has no handler for transaction 3 and
  still execs the client, which then fails to connect — the adopted
  signal is still sent after the refusal for that case.
- **`onCreate` does the one-time asset extraction on the main thread.**
  `ensureActivation` (xkb, libhybris, Xwayland) can take seconds on the
  first bind in a fresh process. Kept synchronous so the first
  `openClient()` cannot race the `TAWC_*` env; move to an async warm-up if
  this becomes a shipping path.
- **`WAYLAND_SOCKET` is invisible to `WAYLAND_DISPLAY`.** A client that
  passes an explicit socket name will ignore it.
- **`ProcessBuilder` cannot be used.** Android's `UNIXProcess` child calls
  `closeDescriptors()` before exec and closes every fd ≥ 4, so an
  inherited socketpair is dropped. Only `Os.execve` (process replacement)
  preserves it, and that requires the adopted signal so the fd reply is
  not cut off mid-transaction.
- **Root shortcut.** With root, tawc's existing chroot path already
  connects directly (notes/android.md). This patch is for no-root.

## Follow-ups (not in this patch)

- **Listener adoption:** Termux creates `$XDG_RUNTIME_DIR/wayland-0`,
  passes the *listening* fd; TAWC adds a second `Generic<UnixListener>`
  source. Gives the normal `WAYLAND_DISPLAY` UX and real peer
  credentials.
- **Relay:** a long-lived Termux helper exposes a path and calls
  `openClient()` per accept. Costs a userspace copy; avoids changing
  every launch command.
- **Protocol-level access control:** pass the client's uid/pid as an
  `openClient` argument and enforce it in the service.
