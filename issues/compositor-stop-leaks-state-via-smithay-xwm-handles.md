# Compositor stop leaks state when Xwayland's X11Wm ran

Calloop `LoopHandle` is a strong `Rc` into the loop's own source list, so
any source callback that captures a handle clone forms a cycle: dropping
the `EventLoop` then never drops the sources. tawc-side captures were
removed (callbacks now use `TawcState::loop_handle()`), and the Wayland
listening socket is explicitly removed after the dispatch loop exits, so
in-process compositor restarts always work.

Remaining leak: smithay's `X11Wm::start_wm` (deps/smithay,
`src/xwayland/xwm/mod.rs` ~lines 1035, 1958, 2166) captures `LoopHandle`
clones in its own source callbacks. Once an X11 client has connected in a
session, stopping the compositor leaks the loop's sources — including the
`Display`, client buffers/fds, and EGL-adjacent state — until the app
process dies. Restart still works thanks to the explicit listener removal.

Where to fix, by layer:
- calloop is the conceptual root: `LoopHandle` is a strong Rc and
  `EventLoop::drop` doesn't clear the source list, so any captured handle
  leaks the loop. Fixing there kills the whole class (including future
  tawc callbacks that capture a handle again — the `loop_handle()`
  convention is only enforced by comment). But calloop is a crates.io
  pin, not a fork; that path means vendoring or upstreaming.
- smithay fork is the practical fix and a real bug on its own terms:
  `start_wm` discards the RegistrationToken (`?;` at xwm/mod.rs:1046),
  so the source can never be removed by anyone, cycle or not. Patch:
  store token + handle in `X11Wm`, remove the source in its existing
  `Drop`. Upstreamable.
- tawc-side can't fix it: the token is unreachable, and draining the
  loop after killing Xwayland to trigger channel-close removal is
  timing-dependent.
- Complement (not a fix): have notification-exit kill the app process;
  the leak only matters because Android caches the process.

Low priority: bounded by one leaked compositor-instance per stop/start
cycle with X11 use, and Android kills the process eventually.
