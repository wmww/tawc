# X11 GL pipe tests are pixel-blind

`xwayland::test_es2gears_x11_renders_via_ahb` and
`xwayland::test_eglx11_renders_via_ahb` assert only broker counters
(AHB create-buffer, texture imports, zero `createFromHandle`
failures). They kept passing for ~2 months while every X11 GL window
composited solid black (libhybris `X11NativeWindow` missing resize
handling — see [x11-presented-windows-black.md](x11-presented-windows-black.md),
fix planned in [plans/tawc-dri-event-channel.md](../plans/tawc-dri-event-channel.md)).

Grow at least one of them a screencap assertion: capture during the
run and assert the window region contains non-black client pixels
(e.g. es2gears' red/green gear colors, or run
`eglx11-test TAWC_EGLX11_TRIANGLE=1` and look for its pure-blue
triangle). The screenshot machinery exists in the integration crate
(`/data/local/tmp/tawc-dev/` scratch); the compositor's lime AHB edge
tint must not be counted as client content.
