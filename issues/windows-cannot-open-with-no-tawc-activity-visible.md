# Windows can't open while no TAWC activity is in front

With no TAWC activity visible (home screen swiped away, only background
rootfs processes left), a Wayland client that connects never gets a
`CompositorActivity`: Android's background-activity-launch rules block
the start, and the client never renders. Repro:
`lazy_compositor::test_wayland_client_starts_and_idle_stops_compositor`
right after `test_swipe_hangs_up_terminals` without the latter's
MainActivity restart ("debug app never rendered").

Real case: a `nohup`'d job that outlives a swipe and later opens a
window. Likely needs a notification-driven launch (full-screen intent
or a "tap to show" action) or a BAL-exempt path.

Launcher taps are not affected: their window maps into the launch's
splash task, which is already in front, with no `spawnActivity`
(notes/launcher.md "Launch splash").
