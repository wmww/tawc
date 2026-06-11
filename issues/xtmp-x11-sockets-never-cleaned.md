# share/xtmp/.X11-unix sockets are never cleaned

With `<rootfs>/tmp` now age-swept (`RootfsTmpSweeper`), the one
runtime-socket location nothing ever cleans is the app-side
`<appData>/share/xtmp/.X11-unix/` backing dir (bound at
`/tmp/.X11-unix` in every method, deliberately skipped by the sweep
because it's shared across spawn surfaces and is a real bind mount on
chroot). Stale `X<n>` sockets from dead Xwayland instances accumulate
there indefinitely — tiny in bytes, but unbounded.

Possible fix: clean it where the compositor owns the Xwayland
lifecycle (it already mkdirs the dir before launching Xwayland), not
in the sweep — the compositor knows which display numbers are live.

Noticed during the rootfs-tmp-sweep review; not user-visible today.
