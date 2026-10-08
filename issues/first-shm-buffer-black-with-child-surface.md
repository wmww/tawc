# First SHM buffer renders black when a child surface follows it

A toplevel whose first buffer is followed right away by a child surface
(xdg_popup or child xdg_toplevel) shows that first buffer as solid black.
The next commit of the same toplevel renders correctly. Reproduced on
`main` before the window-placement work, so it is not placement-related.

Repro (emulator, CPU backend, SHM):

```sh
scripts/rootfs-run.sh '/usr/local/bin/wayland-debug-app popup'
# screencap: parent area is (0,0,0) inside the magenta edge tint; the
# popup itself is orange. Tapping the parent (client recommits) fixes it.
```

`wayland-debug-app dialog` shows the same thing under the dialog scrim.
Single-surface scenes (`small`, `oversize`, `render-pattern`) render their
first buffer fine. The client paints before attach (`request_redraw`), and
the edge tint proves the parent texture is drawn, so the import of that
first buffer looks wrong. Not yet investigated: whether smithay's SHM
import or TAWC's commit path sees the buffer before the child commit.

`window_placement::test_dialog_centered_over_scrim` taps first so the
parent recommits before it samples pixels; drop that once this is fixed.

Possibly the same root cause as
[rendering-orientation-pixel-test-flaky.md](rendering-orientation-pixel-test-flaky.md),
whose failures also sample pure black (0,0,0) from a single-surface
first buffer.

Not limited to child surfaces: `gtk4-demo --run=dialog` (one
non-resizable 401x139 toplevel, emulator CPU/SHM) also shows a black
interior inside the edge tint, and `render-pattern` (single surface) fails
its pixel test on the emulator with (1,1,1) on `main` too.

Does not reproduce on the physical phone (OnePlus 9, libhybris): `popup`
and `dialog` each rendered their first parent buffer correctly in every
run (popup 3/3, dialog 2/2), so this looks emulator-specific.
