# `STATE_QUERY_SENDER` global persists stale `calloop::Sender` across compositor restarts

## Summary

`server/compositor/src/lib.rs` keeps the calloop sender for the state
query channel in a global:

```rust
static STATE_QUERY_SENDER: Mutex<Option<...::Sender<()>>> = Mutex::new(None);
...
*STATE_QUERY_SENDER.lock().unwrap() = Some(state_query_sender);
```

Every `nativeOnSurfaceCreated` (i.e. every Activity recreation —
rotation, returning from background, etc.) starts a fresh
`run_compositor` with a new calloop loop and new channel. The old
`Sender` is overwritten, but `nativeQueryState` doesn't synchronize
with the swap: a query that arrives mid-restart can land on the stale
sender (which silently drops the message because the receiver is gone)
or on the fresh one.

## Impact

Low. The state query is debug-only (used by integration tests). Worst
case: a query during compositor restart returns nothing, the test
times out and retries. Has not been observed to cause flakes.

## Fix

Either:
- Clear the global on `nativeOnSurfaceDestroyed` so the race window is
  narrowed to "compositor not running" instead of "compositor running
  with stale sender".
- Replace the global with an atomic generation counter; the JNI side
  reads it before/after sending and ignores responses for older
  generations.
- Move the sender into a `OnceLock` with an internal `Mutex` and have
  the compositor own the receiver-end teardown.

Not a blocker for current functionality.
