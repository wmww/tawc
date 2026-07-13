# kumquat listener thread leaks per compositor start

`bridge::spawn()` starts a `kumquat-server` thread on every compositor
start, but nothing stops it on compositor stop: the thread blocks forever
in `Kumquat::run()`'s `wait_ctx.wait()`. Each in-process stop/start cycle
therefore accumulates one idle thread plus its `kumquat-gpu-0` listener
fd (visible as extra unix sockets on `share/kumquat-gpu-0` in
`/proc/<pid>/fd`; observed +1 per cycle on the phone).

Functionally benign so far: `KumquatBuilder::build()` unlinks the stale
socket path before rebinding, so new clients always reach the newest
thread; old threads hold unlinked listeners and never accept anything.

Fix sketch: keep a shutdown signal (or the thread handle + a pipe added
to the wait_ctx) so nativeStopCompositor can stop the thread, or reuse a
single process-lifetime kumquat thread across compositor restarts
instead of spawning a new one per start.

Low priority: bounded by one thread+fd per stop/start cycle, and Android
kills the cached process eventually.
