# `android_wlegl::create_buffer` leaks the new wl_buffer resource on import failure

## Summary

In `server/compositor/src/wlegl.rs`,
`Dispatch<AndroidWlegl, ()>::request` for the
`android_wlegl::Request::CreateBuffer` arm:

```rust
let ahb = unsafe { tawc_wlegl_import(...) };
if ahb.is_null() {
    resource.post_error(android_wlegl::Error::BadHandle, "...");
    return;
}
```

When `tawc_wlegl_import` fails we `post_error` and return without
calling `data_init.init(id, ...)` on the new `wl_buffer` `id`. The
client-side `wl_buffer` object exists; the server-side resource is
never bound to a dispatch handler. Subsequent requests on that id
(notably `wl_buffer.destroy`) get routed to wayland-server's "no
dispatch" path.

In practice `post_error` kills the client immediately, so the leak is
academic — but if the error class ever changes (e.g. emit a softer
`wl_display.error` or a non-fatal protocol failure), the half-bound
resource becomes a real bug.

## Fix

Either:
- Initialize the resource with a "dead" placeholder
  (`data_init.init(id, DeadBuffer)`) so destroy is handled cleanly, then
  post the error.
- Keep `post_error` (which is fatal), and add a comment noting the
  fatal-error invariant so it doesn't get accidentally weakened.

## Where

`server/compositor/src/wlegl.rs` around the `if ahb.is_null()` branch
inside the `CreateBuffer` arm.
