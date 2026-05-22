# Lazy-init kumquat so gfxstream crashes happen on client connect, not app launch

Today `CompositorService.onCreate` calls `bridge::spawn()`, which builds the
kumquat server. That path loads `libgfxstream_backend.so` and runs
`stream_renderer_init`, the gfxstream host Vulkan/EGL emulation. Any abort in
that setup kills the whole tawc process before MainActivity is on screen, which
means the user can't reach in-app Settings to change away from the backend that's
crashing.

The Pixel 10 Pro Fold udmabuf FATAL fixed in `d2c9683` is the motivating case;
deferring init would have meant that crash only fired when a client connected
with the gfxstream backend selected, leaving the rest of the UI usable.

## Design

`KumquatBuilder::build` bundles `KumquatGpu::new()` with `Listener::bind`, in
that order (`deps/rutabaga_gfx/kumquat/server/src/kumquat.rs:145-177`). Invert
that sequence with a rutabaga patch, likely
`rutabaga-patches/05-lazy-gpu-init.patch`:

- Bind the kumquat listener immediately.
- Leave `kumquat_gpu_opt: Option<KumquatGpu>` as `None` after build.
- In `Kumquat::run`, lazily build `KumquatGpu` the first time a client is
  accepted, before dispatching its first command.

The data model already has `Option<KumquatGpu>`, so only the build sequence
should need to change. After the change, non-gfxstream users (`LIBHYBRIS`,
`LIBHYBRIS_ZINK`, and `CPU`) pay zero gfxstream init cost; the first client that
opens a gfxstream-vk Vulkan instance pays it once.

The kumquat thread does not need EGL ownership, so its handoff is simpler than
the compositor lazy-init work. It is still the same general shape: bind early,
block on accept, and perform heavy initialization only after the first client
arrives.

## Cost and trade-offs

- **First connect latency.** The first gfxstream client pays renderer-init cost
  before its first command dispatches. Subsequent clients see no extra cost.
- **Backend selection.** Gating `bridge::spawn` on
  `Settings.graphicsBackend == GFXSTREAM` is subsumed by lazy init. If no
  gfxstream-vk client ever connects, the renderer never builds.
- **Crash behavior.** Kumquat still runs in the main app process. If gfxstream
  init aborts, the app dies, but now only when a gfxstream client connects, by
  which point Settings is reachable.

