# X11 (TAWC-DRI) windows composite black on OnePlus 9

`es2gears_x11` (and any X11 GL client that outlives the compositor's
initial window resize) shows a mapped window that is solid black,
while `xwayland::test_es2gears_x11_renders_via_ahb` **passes** — the
AHB pipe tests are counter-only and pixel-blind
([xwayland-gl-tests-pixel-blind.md](xwayland-gl-tests-pixel-blind.md)).
Wayland-native `weston-simple-egl` renders correctly in the same
session.

Repro (2026-07-06, OnePlus 9 / Adreno 660):

```
scripts/rootfs-run.sh 'sh -c "HYBRIS_EGLPLATFORM=x11 timeout 8 es2gears_x11"'
# screencap → black window, no gears, no magenta tint
```

## Root cause (diagnosed 2026-07-06)

Not alpha (the original suspicion): the window composites *opaque*
black over the desktop, and CPU-filled TAWC-DRI buffers
(`tawc-dri-test`) plus glClear-only GL clients (`eglx11-test`) render
fine. The real chain:

1. The compositor resizes every non-override-redirect X11 toplevel to
   its host activity's logical size
   (`compositor/src/xwayland.rs::configure_x11_toplevel_for_host`,
   e.g. 300×300 → 540×1085). This landed after the 2026-05-01
   es2gears verification — that's the regression point.
2. The client handles ConfigureNotify and resets its GL viewport, as
   the EGL spec expects the surface to follow the window.
3. libhybris's `X11NativeWindow` never learns about the resize and
   keeps allocating creation-time-sized AHBs. The client renders a
   540×1085 viewport into 300×300 buffers: everything but the clear
   color lands outside the buffer → black window.

Evidence: an `LD_PRELOAD` shim hooking `eglSwapBuffers` +
`glReadPixels` showed gears present at frame 0 (300×300 viewport) and
gone forever once the viewport became 540×1085. `eglx11-test`
survives only because its window gets resized *before* its EGL
surface is created, so its buffer pool starts at the final size. A
prototype `GetGeometry` poll in `queueBuffer` restored visibly
rotating gears on device (confirming the diagnosis), but was reverted:
polling papers over TAWC-DRI having no server→client event channel at
all (same gap forces the release-less 3-buffer round-robin).

## Fix

[plans/tawc-dri-event-channel.md](../plans/tawc-dri-event-channel.md)
— give TAWC-DRI XGE generic events (ConfigureNotify + BufferRelease)
delivered to a libxcb special event queue in the plugin.

`tests/apps/eglx11-test` grew `TAWC_EGLX11_{TRIANGLE,DEPTH,VBO,MVP,
READBACK,W,H}` env modes during the bisection; they stay for the
plan's verification.
