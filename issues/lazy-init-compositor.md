# Lazy-init compositor so crashes happen on client connect, not app launch

Today `CompositorService.onCreate` calls `nativeStartCompositor`, which builds
the smithay EGL context, GlesRenderer, AHB importer, Wayland Display, socket
listener, and event loop up-front. Any abort in that setup kills the whole tawc
process before MainActivity is on screen, which means the user can't reach
in-app Settings to change away from the backend that's crashing.

The Pixel 10 Pro Fold udmabuf FATAL fixed in `d2c9683` is the motivating case;
deferring init would have meant that crash only fired when a chroot client
connected, leaving the rest of the UI usable.

## Design

Bind `share/wayland-0` eagerly from a thin accept thread spawned by
`nativeStartCompositor`. That thread should do nothing but block in `accept(2)`
until the first client connects.

On first accept:

- Send the accepted fd over a channel to a freshly-spawned compositor thread.
- Let the compositor thread do its own EGL/GlesRenderer/calloop setup.
- Plug the accepted fd straight into the compositor's `WaylandListener` source.

EGL contexts are bound to the creating thread, so the accept thread cannot
pre-build GPU state. The current `run_compositor` flow already does EGL init on
the compositor thread; this refactor is "split the socket bind out, deliver the
fd via channel", not an EGL ownership rewrite.

The Wayland socket bind sits at the very end of `run_compositor` today
intentionally; see the comment in `compositor/src/lib.rs:661-668` about avoiding
the bind-to-dispatch gap. The lazy accept thread must preserve that user-visible
property by holding the first accepted client until the compositor event loop is
ready to dispatch it.

## Cost and trade-offs

- **First connect latency.** A client that connects before init completes blocks
  on its first Wayland roundtrip until EGL, GlesRenderer, and Display are ready.
  Expect roughly 150-300 ms one-time.
- **Out-of-band clients.** A chroot daemon that connects to the Wayland socket
  without an Activity running still triggers init. The socket existence and
  connection acceptance are decoupled from whether the app is in the foreground,
  matching today's behavior.
- **Foreground service lifecycle.** `CompositorService` is a `START_STICKY`
  foreground service. With lazy init, the heavy work moves later, but the service
  still has to be created before any binding so it owns the accept thread.

## What this is not

Process isolation. The compositor still runs in the main app process. If init
crashes inside that process, the app dies, but now only when a client connects,
by which point Settings is reachable. Full process separation
(`:compositor` process via `android:process`) remains a separate, bigger
refactor.

