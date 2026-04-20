# libhybris wayland: frame_callback is one commit behind after queueBuffer refactor

## Summary

Commit `7d1ba3a` ("wayland-egl: attach+commit inside queueBuffer…") moved the
`wl_surface_attach` + damage + `wl_surface_commit` sequence out of
`finishSwap` and into a `drainOneQueuedBufferLocked()` helper that is now
called both from `finishSwap` and from the end of `queueBuffer`.

In the normal `eglSwapBuffers` flow the vendor EGL driver calls
`dequeueBuffer` + `queueBuffer` **inside** `_eglSwapBuffers`, so by the time
`finishSwap` runs the queue is already drained. `finishSwap` therefore:

1. Waits for the previous `frame_callback` (good).
2. If `swap_interval > 0`, issues `wl_surface_frame` and stores the new
   callback in `this->frame_callback`. The callback is only in the surface's
   **pending** state at this point.
3. Calls `drainOneQueuedBufferLocked()` — no-op, queue is empty.
4. Returns without calling `wl_surface_commit`.

Upstream always called `wl_surface_commit` at the end of `finishSwap`, which
flushed the pending frame_callback onto a real (possibly empty) commit. Now
that commit never happens; the pending frame_callback only moves to current
state on the **next** `queueBuffer` drain (the next frame).

## Impact

- Throttling is one frame behind upstream. The compositor signals the
  frame_callback tied to frame N+1 instead of frame N. Usually invisible,
  but it does increase end-to-end latency by one frame at `swap_interval=1`.
- Latent deadlock if `eglSwapBuffers` is called twice without an intervening
  `dequeueBuffer`/`queueBuffer` (rare — would require a vendor EGL that
  no-ops on repeat swaps). The second `finishSwap` would wait forever for a
  frame_callback that is still in pending state.

Neither failure mode has been observed in practice on either Pixel 4a or
OnePlus 9 Adreno stacks, but the behaviour is a real divergence from upstream.

## Fix

Easiest option: at the end of `finishSwap`, after `drainOneQueuedBufferLocked`,
if the queue was empty but a frame_callback is pending, emit a bare
`wl_surface_commit(wl_surface_wrapper)` to flush it. That matches upstream's
semantics exactly and removes both concerns above.

## Location

`libhybris/hybris/egl/platforms/wayland/wayland_window.cpp` — `finishSwap()`
and `drainOneQueuedBufferLocked()`.
