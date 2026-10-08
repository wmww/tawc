# Window fit, centering, and dialog scrim

Not started. Design approved; the recommendations below are decided.

## Goal

- **Too big:** a window whose geometry is larger than its host's logical
  size is scaled down to fit, keeping its aspect ratio, and centered.
- **Too small:** a window smaller than the host is centered instead of
  pinned top-left.
- **Child windows** (dialogs: `xdg_toplevel.set_parent`, X11
  `WM_TRANSIENT_FOR`) draw centered over their parent, with the parent
  darkened by a scrim.
- **Unrelated small toplevels** are centered on the plain background. They
  already get their own host, so nothing else draws under them.
- Touch and pointer input land where the user sees the window.
- Maximized windows that fill the host exactly look and behave the same as
  today.

"Popups" in the request most likely means dialogs. `xdg_popup` menus are
different: they already sit relative to their parent and must not get a
scrim. They only need to follow their root window's placement (see below).

## What happens today (verified in source)

- Every xdg toplevel, children included, is configured to the full host
  size with `Maximized` set, and `bounds` set to the same size
  (`compositor.rs` `configure_toplevel_for_host`). X11 toplevels and X11
  transients are configured to `(0,0)` at host size
  (`xwayland.rs` `configure_x11_toplevel_for_host`). Windows that come out
  a different size are clients that ignore this: fixed-size dialogs, apps
  whose min size is bigger than a phone, or X11 apps that resize
  themselves.
- Child toplevels go to their parent's host
  (`DesktopRegistry::choose_host`), so a dialog shares a `Space` with its
  parent. Both are mapped with their surface origin at the output origin
  (`desktop_window_map_location`). Stacking is just `Space` order. Nothing
  raises a window or dims anything.
- Rendering is one call, `Space::render_elements_for_region`, at output
  scale (`render.rs` `render_frame`). Smithay's `Space` has no per-window
  scale.
- Input converts Android physical coords to logical with `OutputScale`, then
  `surface_at` (`event_loop.rs`) runs `Space::element_under` and
  `Window::surface_under`. Smithay derives surface-local coords as
  `location - focus_origin`.
- Popup constraining passes `get_unconstrained_geometry` a target of
  `(0,0, output_logical_size)` (`compositor.rs` `new_popup` /
  `reposition_request`). That is only correct because every root surface
  sits at the origin.
- X11 override-redirect windows (menus, tooltips) ignore their X position
  and render at the host origin. Smithay's X11 `bbox` has `loc = 0`, and
  `configure_notify` is a no-op. This bug exists today and sits right next
  to this work.
- When a toplevel is destroyed, keyboard focus does not move back to the
  parent (`toplevel_destroyed`). Today that is hidden because the parent
  sits under a full-screen dialog. Once the scrim exists it becomes visible.

## Design

### Placement

Add a per-window screen transform, computed in TAWC rather than stored in
Smithay:

```rust
struct Placement { offset: Point<f64, Logical>, scale: f64 } // screen = offset + window_pt * scale
```

`window_pt` is in the window's own frame: the frame the `Space` uses today,
with the root `wl_surface` origin at `(0,0)`. For geometry `g` (from
`Window::geometry()`, which leaves out CSD shadows) and host logical size
`H`:

```
s      = min(1, H.w / g.w, H.h / g.h)
offset = (H - g.size * s) / 2 - g.loc * s
```

A maximized window that fills the host gets `s = 1` and `offset = 0`, so the
common path does not change. Placement is recomputed every frame from
current geometry, which is cheap. There is no stored state to go stale.

Placement per role:

| Window | Placement |
|---|---|
| Toplevel with no parent | fit + center |
| Child toplevel (Wayland parent / X11 transient) | fit + center, drawn above a scrim covering everything below it |
| `xdg_popup`, subsurfaces | inherit the root window's transform (they come out of `Window::render_elements` / `surface_under`) |
| X11 override-redirect | parent placement applied to its X position (phase 4) |

In single-activity mode, unrelated toplevels share one host. They keep
today's stacking and get fit + center, with no scrim.

### Coordinate model: transform only at the edges

Keep `Space` in window frames, exactly as it is mapped now. Apply
`Placement` only when rendering and when input comes in. Reasons:

- Smithay's `Space` can't scale an element. Doing the layout inside `Space`
  would mean scaling everything by hand anyway.
- `Window::surface_under`, popup offsets, subsurface positions, and the GTK3
  workaround's synthetic enter at `(w/2, h/2)` with origin `(0,0)` all stay
  correct unchanged.
- Smithay's seat code only uses `location - focus_origin`. If TAWC hands it
  both in the focused window's frame, scaling is invisible to Smithay.

### Rendering

Replace `render_elements_for_region` with a per-window loop in `Space`
z-order:

- For each window, call
  `AsRenderElements::render_elements(window, renderer, round(offset * out_scale), Scale(out_scale * s), 1.0)`.
  A combined scale is enough. Smithay sizes surfaces, subsurfaces, and
  popups from it, so `RescaleRenderElement` is not needed. Wrap each
  element in the existing `TawcWaylandRenderElement` so the tint shader
  still applies.
- Insert a `SolidColorRenderElement` scrim covering the whole host just
  below the topmost child toplevel. Black at about 50% alpha, like Android's
  dialog dim. That needs a small enum element (Wayland | Solid) so
  `draw_render_elements` gets one type.
- Elements that hang off the screen edge are fine. The frame is fully
  redrawn every time.
- Scaled-down windows are sampled with whatever filtering smithay's
  texture shader uses. Check that text stays readable. If it doesn't, see
  "Fractional scale" below.

### Input

- **Hit test** (`surface_at`): walk the visible host's windows top-down.
  For each, `local = (screen - offset) / s`, then
  `window.surface_under(local - map_loc, ALL)`. Return the surface, an
  origin in that window's frame, and the window, or its `Placement`. The
  scrim is opaque to input: a point under the scrim and outside the child
  hits nothing. For true modal dialogs that matches Android, and the client
  would ignore the parent anyway. It is also why hits that miss every
  window must stop rather than fall through.
- **Delivery**: pass Smithay the window-frame location (`local`), not the
  screen location. That covers down, motion, and button events.
- **Gestures in progress**: a touch slot or a held button keeps its
  original focus. Its motion has to be transformed with **that** window's
  placement, not the one under the finger. Store a `Placement` source (the
  window) per touch slot at down, and for the pointer while Smithay reports
  a grab. A plain hover with no grab just re-hit-tests.
- `pointer_location` becomes window-frame coords. Keep a separate
  screen-logical copy for `query-state` `pointer_x`/`pointer_y`. Today's
  tests only check `pointer_focus`, but the screen value is the meaningful
  one to report.
- Scroll stays unscaled: 15 px per detent in the client's units. That
  matches what a desktop client expects whatever its on-screen size.

### Popup constraint target

Change `new_popup` and `reposition_request` to constrain against the
**visible host rect in the popup parent's frame**:

- Start from the host rect `(0,0,H)` in screen space.
- Map it into the root window's frame: `(-offset / s, H / s)`.
- Subtract the parent popup chain's offset within the root, as anvil's
  `get_popup_toplevel_coords` does.

Use the popup's host size, not `output_logical_size`. If a window's
placement changes while a popup is open, the popup is not re-constrained.
That is acceptable for menus.

### Stacking and focus

- When a child toplevel maps, raise it (`Space::raise_element`). When the
  parent is raised later, its children stay above it.
- When a child is destroyed or unmapped, give keyboard and text-input focus
  back to the next window down in the same host (normally the parent),
  through `set_input_focus`.
- Back already sends Escape to the focused surface, so it closes most
  dialogs without changes.

### Configure policy for child toplevels

This is a separate decision from rendering (phase 3). Today dialogs are
maximized, so the scrim only shows for dialogs that refuse.

**Decided:** configure child toplevels with no size (`0x0`), no
`Maximized`, and `bounds` = host size. They pick their natural size and get
centered over the dimmed parent, like an Android dialog. Dialogs that pick
something too big are fit-scaled. GTK4 honours `bounds`. GTK3 and X11
dialogs that pick oversized defaults (file choosers) get scaled. If that
reads badly in practice, the fallback is to keep maximizing dialogs whose
natural size exceeds about 80% of the host.

X11 transients get the same treatment in `configure_x11_toplevel_for_host`:
leave the client's requested size and clamp it to the host.

### Fractional scale (optional)

A window drawn at `s < 1` wastes buffer pixels and gets downsampled. We
could send that window's surfaces `preferred_scale = out_scale * s`. Then
the client renders at exactly the physical size, its logical size stays the
same, and text stays sharp. The risk is a feedback loop if a client changes
geometry in response. Only do this after the basic path works, and only if
blurriness is a real problem.

### Diagnostics

Add per-window placement to `query-state`: root surface id, role,
geometry, offset, scale, and whether the scrim is drawn. Tests can then
assert layout without pixel-guessing. Add no per-frame or per-event logging.

## Phases

1. **Placement + render.** `Placement` computation, per-window render loop,
   centering and fit-scaling for all toplevels, and the scrim under child
   toplevels. `query-state` placement fields.
2. **Input.** Transform-aware `surface_at`, window-frame delivery, gesture
   and grab transform pinning, screen-space `pointer_x`/`pointer_y`, scrim
   swallowing input, and the popup constraint target. Phases 1 and 2 have
   to land together: with phase 1 alone, input is wrong on any window that
   isn't full-size.
3. **Dialog configure policy.** Natural-size child toplevels, raise on map,
   focus back to the parent on close.
4. **X11 override-redirect positions.** Track OR geometry
   (`configure_notify` / `X11Surface::geometry().loc`). Place OR windows at
   `parent_placement(x11_pos - parent_x11_pos)`, using `transient_for` or
   else the topmost window in the host. This fixes the existing bug where
   menus render at the origin.
5. **Optional:** per-window fractional scale.

## Tests

The compositor crate does not build on the host (`ndk-sys`), so coverage
comes from integration tests. Extend `tests/apps/wayland-debug-app` with
modes:

- `oversize`: ignores configure and commits a fixed buffer about 2× the
  host in one dimension, with the existing four-color corner pattern.
  Assert that the corners land where the fit-scaled, centered rect says,
  and that background shows in the letterbox bars.
- `small`: a fixed 200×150 window. Assert it is centered.
- `dialog`: a parent toplevel plus a fixed-size child with `set_parent`.
  Assert the child is centered, a parent pixel outside the child is
  darkened, and a tap on the scrim reaches neither surface.
- Touch on `oversize`, `small`, and `dialog`: a tap at a known screen point
  reports the expected surface-local coords. The app already emits
  `SURFACE_TOUCH_DOWN` with local coords. Add a drag that starts on the
  child and leaves it, to cover gesture pinning.
- Popup on a centered window: extend `test_shm_xdg_popup_position_pixels`
  so the popup lands at parent placement + positioner offset and is
  constrained to the visible screen.
- Pointer: the same as touch for one case, through `pointer_input.rs`.

Manual checks on the standing target: a GTK3 dialog (e.g. gedit
Save As), a GTK4 dialog, an app with a large min size, and an X11 dialog,
plus menus on each.

## Docs to update when done

- `notes/rendering.md`: "Window Management" ("rendered at (0,0) instead of
  centered"), the Smithay renderer paragraph about
  `render_elements_for_region`, and the coordinate-system section (add the
  window frame vs screen transform).
- `notes/input.md`: the hit-test and focus paragraphs, gesture pinning, and
  the scrim.
- `notes/xwayland.md`: OR window placement (phase 4).

## Decided

- The scrim swallows taps. Back (Escape to the focused dialog) is the
  way out.
- Fit-scaling applies to fullscreen hosts too; it is the same math.

## Open questions

- Does an X11 toplevel that ignores the configure (picks its own size)
  happen often enough to pull phase 4 earlier?
