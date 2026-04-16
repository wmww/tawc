# `WleglBufferData: Send + Sync` is misleading and unsound off the GL thread

## Summary

`server/compositor/src/wlegl.rs` declares
`unsafe impl Send for WleglBufferData {}` and
`unsafe impl Sync for WleglBufferData {}` so the struct can live as
user-data on a `wl_buffer`. The struct holds:

- `ahb: *mut ndk_sys::AHardwareBuffer` — refcounted, AHB ops are thread-safe.
- `texture: Mutex<Option<GlesTexture>>` — a Smithay GL texture handle.

`Drop for WleglBufferData` runs when the `wl_buffer` is destroyed. That
drops the inner `Option<GlesTexture>`, which in turn calls
`glDeleteTextures` from `GlesTexture::drop`. **That call requires the
EGL context to be current on the calling thread.** If `WleglBufferData`
is ever dropped off the render thread, the texture is leaked at best
and trashes another thread's GL state at worst.

Today the compositor is single-threaded (one calloop loop owns both the
Wayland dispatch and the renderer), so the drop always lands on the
right thread by accident. The `Send + Sync` impls don't reflect that
constraint — they advertise that any thread can freely drop the value.

## Why it has to be Send + Sync at all

`wl_buffer.data::<T>()` requires `T: Send + Sync` because Smithay can be
configured with multi-threaded display dispatch. We don't use that, but
the bound is still on the API.

## Fix options

1. Move the `GlesTexture` out of `WleglBufferData` into a separate
   render-thread-owned cache keyed by `wl_buffer.id()`. Then
   `WleglBufferData` only holds the AHB pointer (genuinely Send + Sync)
   and texture cleanup happens on the render thread when the entry is
   evicted.
2. Wrap the texture in a "deferred-drop" that sends it back to the
   render thread via a channel on Drop. Heavyweight.
3. Keep current code, narrow the `Send`/`Sync` impls behind a comment
   that documents the single-threaded assumption explicitly. Cheapest;
   trips up anyone who later tries to multi-thread the dispatch.

Today (1) is the cleanest if/when this matters. Until then, this
issue is a tripwire for the next contributor — file kept so the
`unsafe impl` doesn't read as endorsed.
