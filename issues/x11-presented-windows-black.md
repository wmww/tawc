# X11 (TAWC-DRI) windows composite black on OnePlus 9

`es2gears_x11` (and any other X11 GL client) shows a mapped,
correctly-sized window that is solid black on screen, while
`xwayland::test_es2gears_x11_renders_via_ahb` **passes** — AHB
create-buffer (`fmt=1` = RGBA_8888) and texture-import counters advance,
zero `createFromHandle` failures. Wayland-native `weston-simple-egl`
renders correctly in the same session, so compositing and the wlegl AHB
path are fine; only X11/TAWC-DRI-presented content is invisible.

Repro (2026-07-06, OnePlus 9 / Adreno 660):

```
scripts/rootfs-run.sh 'sh -c "HYBRIS_EGLPLATFORM=x11 timeout 8 es2gears_x11"'
# screencap → black ~600x600 window, no gears, no magenta tint
```

Notes/xwayland.md records gears "visibly rotate correctly" on
2026-05-01, so this regressed since. Prime suspect: that verification
predates removing the force-opaque alpha workaround
([plans/verify-libhybris-ahb-alpha.md](../plans/verify-libhybris-ahb-alpha.md)).
X11 clients get RGBA_8888 AHBs but GLX/X11 apps typically never write
meaningful alpha; if the buffer's alpha channel is 0, sampled-alpha
compositing produces exactly this: counters advance, screen black,
no tint.

Next step: sample the imported texture's alpha in the compositor (or
temporarily force alpha=1 for TAWC-DRI buffers) to confirm, then decide
per the alpha plan. The pixel-blind test should also grow a screencap
assertion so this can't silently pass again.

Found while evaluating gl4es (verdict in notes/gpu-strategy.md); it
blocks any visible X11 GL demo but is unrelated to gl4es itself. Also
blocks visual verification for
[plans/gl-on-gles-translator.md](../plans/gl-on-gles-translator.md).
