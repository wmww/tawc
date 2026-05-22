# Initial configure size path needs direct coverage

The service-side display-size fallback is gone: initial xdg toplevel
configures now wait until the assigned `CompositorActivity` registers a
real `SurfaceView` size.

Current integration coverage exercises the behavior through real apps
(`vkcube`, GTK, Firefox, etc.), but it does not directly assert:

- no toplevel configure is sent before Activity surface registration;
- the first toplevel configure size is non-zero and matches the registered
  Activity logical size;
- pending fullscreen, decoration, and focus state accumulated before
  registration is included in that first configure.

Possible test shape:

- Teach `tests/apps/wayland-debug-app` to emit configure size/bounds.
- Add an integration test that starts a debug client before Activity
  registration, then asserts the first configure carries the real size.
- Add focused cases for pre-registration fullscreen, decoration, and
  focus/background pending state.

Residual risk: `wl_output` still advertises a 1x1 placeholder until the
first Activity surface registers. Normal xdg clients wait for toplevel
configure size, but a client that sizes itself from early `wl_output`
events could observe the placeholder.
