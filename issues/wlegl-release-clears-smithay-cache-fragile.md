# wlegl release path clears Smithay's cached `BufferAssignment` — fragile

## Summary

`server/compositor/src/render.rs::release_consumed_wlegl_buffers`
sends `wl_buffer.release` itself, then reaches into Smithay's
`SurfaceAttributes` cache and clears `attrs.buffer = None` to suppress
Smithay's own auto-release on the next commit:

```rust
with_states(&surface, |surf_states| {
    let mut guard = surf_states.cached_state.get::<SurfaceAttributes>();
    let attrs = guard.current();
    if let Some(BufferAssignment::NewBuffer(cached)) = &attrs.buffer {
        if cached == &buf {
            attrs.buffer = None;
        }
    }
});
```

This is necessary because libhybris's wayland-egl needs the release
before the next dequeueBuffer (Smithay's auto-release fires too late,
on the *next* commit), but releasing twice trips libhybris's `fronted`
assertion.

The fragility: we're poking the private cache that
`SurfaceAttributes::merge_into` reads to decide whether to send an
auto-release. This is implementation-specific behaviour of our patched
Smithay branch (`wmww/smithay#tawc-patches`) — a Smithay update that
restructures the cache, or simply drops the public `cached_state.get`
mutability on `SurfaceAttributes`, breaks this silently. The failure
mode is hard to spot: rendering still works, but every commit
double-releases the buffer and libhybris aborts.

## Fix options

1. Upstream a "skip auto-release" hook into our Smithay fork: Smithay
   exposes a callback or per-surface flag that disables its auto-release
   for surfaces we manage.
2. Drive release from Smithay's commit handler instead — release in the
   commit *before* calling Smithay's `merge_into`, which means the
   cached attribute is the *previous* buffer and Smithay's auto-release
   is then a no-op. Requires reworking the commit ordering.
3. Status quo + a Smithay-version pinning comment in `Cargo.toml`
   that calls out the dependency on this specific cache layout.

The current code is correct, but worth a short test: a debug-only
sanity check after `with_states` that asserts `attrs.buffer.is_none()`
afterward, to fail loudly if Smithay's cache layout changes.

## Where

`server/compositor/src/render.rs::release_consumed_wlegl_buffers`
(approx the function that sends `buf.release()`).
