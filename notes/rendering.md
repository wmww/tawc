# Rendering, Window Management, and Coordinate System

## Background Color

The compositor clears every frame to a flat color matching the rest of the app UI. Keep Rust `BACKGROUND_COLOR` in `render.rs` and Android resource `tawc_window_bg` (`#1F1B22` in dark mode) in sync.

## Window Management

All toplevels are configured as maximized at the full logical output size:
`round(physical_size / output_scale)`. The xdg-decoration protocol is
implemented, always requesting server-side decorations (which we don't draw,
so clients get no decorations). Surfaces are rendered at (0,0) instead of
centered.

## Popup and Subsurface Positioning

Popup surfaces (xdg_popup) are tracked via Smithay's `PopupManager`. On `new_popup`, we
compute constrained geometry using `PositionerState::get_unconstrained_geometry()` and send
a configure. The PopupManager handles the popup tree hierarchy and provides popup positions
relative to their toplevel root.

**Note:** Firefox uses wl_subsurface (not xdg_popup) for its dropdown menus. Both paths go
through the same surface tree drawing code.

## Wayland subsurface z-order

Firefox with WebRender creates two wlegl surfaces per window: a toplevel (main
thread, holds the `xdg_toplevel` role plus a placeholder buffer) and a
subsurface covering the full window, into which WebRender renders the actual
chrome + page content from the Renderer thread. The subsurface is above the
toplevel by default (Wayland z-order), so the subsurface's pixels must overlap
the toplevel's placeholder.

Both wlegl and SHM surfaces use a unified drawing path: `collect_surface_draws`
walks each toplevel tree plus its popups with `with_surface_tree_downward`,
which invokes its processor post-order (children first, then parent). Each tree
is independently **reversed** before drawing so parents are drawn first (behind)
and subsurfaces last (on top). Without the reverse, Firefox renders as a black
rectangle because the toplevel's placeholder buffer occludes WebRender's output.
Popup trees are appended after their parent toplevel so they draw on top.

## Coordinate System

**This is subtle. Do not "fix" without understanding.**

1. **Logical vs physical:** Subsurface positions (from `wl_subsurface.set_position`) and
   popup positions (from xdg_positioner) are in logical (surface-local) coordinates.
   The renderer works in physical pixels. Convert logical edges through
   `OutputScale` and round the physical edges.

2. **Y-axis flip:** Smithay's GlesRenderer uses a GL projection where Y=0 is at the
   **bottom** of the screen, not the top, so we compute
   `physical_y = screen_h - round((logical_y + logical_h) * output_scale)`. The buffer
   itself is also Y-down (Wayland convention) vs. Y-up (GL), so we pass
   `Transform::Flipped180` to `Frame::render_texture_from_to`. Both are
   needed — Firefox and the `weston-simple-egl` triangle expose the bug if
   either is missing.

3. **Surface size follows the wl_surface spec:**
   - `surface_logical_size = wp_viewport.dst` if set, otherwise
     `buffer_size / buffer_scale`.
   - `surface_physical_size = surface_logical_size * output_scale`.

   Both `wp_viewporter` and `wl_surface.set_buffer_scale` matter here:
   - **vkcube / weston-simple-egl** allocate buffers at the configured logical
     size with `buffer_scale=1` and no viewport. The compositor scales that
     logical-size buffer to the physical output.
   - **Fractional-scale-aware GTK / Firefox / GTK4** learn the scale from
     `wp_fractional_scale_v1.preferred_scale`, allocate a physical-resolution
     buffer, and use `wp_viewport.set_destination(logical_width, logical_height)`
     to describe the logical surface size. Integer-only clients see the
     rounded-up fallback in `wl_surface.preferred_buffer_scale` and
     `wl_output.scale`.

   **Firefox specifically requires `wp_viewporter`** — Firefox's WebRender
   renders into a HiDPI subsurface but commits `buffer_scale=1`, relying on
   `wp_viewport.set_destination` to set the on-screen size. Without
   viewporter Firefox triggers `FEATURE_FAILURE_REQUIRES_WPVIEWPORTER` and
   ends up with a 2×-oversized surface.

   `buffer_scale` and `viewport_dst` live on `SurfaceWleglState` /
   `SurfaceShmState`; the `logical_size` helper in `render.rs` applies the
   precedence rule.

The canonical output scale factor lives in `TawcState::output_scale` as an
`OutputScale`, not an integer. Do not hardcode a scale elsewhere.

## SHM Buffer Support

SHM buffers (`wl_shm`) are supported alongside the AHB path. SHM matters even for
GPU-accelerated clients because cursor themes, toolkit subsurfaces/popups (GTK3/4), and
EGL fallback paths all use `wl_shm`.

**Magenta tint:** SHM surfaces are rendered with a distinct magenta tint via a custom
`GlesTexProgram` shader. This is intentional -- it makes it visually obvious when a client
falls back to SHM instead of using hardware-accelerated AHB buffers.

The SHM path is tracked separately from AHB: `surface_shm` HashMap holds `SurfaceShmState`
per surface. Surfaces using the AHB channel protocol are never checked for SHM buffers.

## Verified clients

- `weston-simple-egl` (AHB)
- `gtk3-widget-factory` (AHB + SHM popups)
- Firefox / WebRender (AHB)
- `gtk4-demo` / `gtk4-widget-factory` 4.22.2 on Void (AHB, no magenta tint;
  manually tested 2026-04-20 and re-verified 2026-05-04)

### Output scale advertisement

We advertise all scale paths clients commonly need:

- `wp_fractional_scale_manager_v1`: the authoritative scale for modern
  clients. Smithay sends `preferred_scale` as `round(scale * 120)`.
- `wp_viewporter`: required with fractional scale because clients usually
  render at physical resolution and set a logical `set_destination`.
- `wl_compositor` v6 `wl_surface.preferred_buffer_scale`: integer fallback,
  rounded up from the fractional scale.
- `wl_output.scale`: integer fallback, also rounded up by Smithay's
  `Scale::Fractional`.
- `xdg-output`: reports the logical output size derived from the fractional
  output scale.

Runtime scale changes should update the single `OutputScale`, call
`Output::change_current_state` with `Scale::Fractional`, resend
`TawcState::send_surface_scale` for every live surface, then reconfigure
toplevels with the new logical sizes.

### GTK4 minimum version

**GTK4 must be ≥ 4.22 on libhybris/Adreno.** Even with the proper scale
signal, GTK4 4.18.x's GpuRenderer has an unrelated regression that
produces blank windows (background fills, no widget content or text) when
committing AHB buffers via `android_wlegl`. The same path works on GTK4 ≥
4.22.2. Symptom on 4.18: every GTK4 app renders an off-white window with
at most a faint headerbar strip; cairo fallback (`GSK_RENDERER=cairo`)
renders correctly via SHM. GTK3 and non-GTK GLES/Vulkan clients are
unaffected.

The bug exists across all `GSK_RENDERER` flavours (`gl|ngl|vulkan`) and
isn't fixable via `GSK_GPU_DISABLE` flags or any compositor-side change —
it's in GTK4 4.18's GpuRenderer interaction with libhybris-wrapped Adreno
EGL, fixed upstream by GTK4 4.22. (Confirmed via `WAYLAND_DEBUG=client`:
post-fix, GTK 4.18 sends the same physical-sized buffer as 4.22 and wraps
it identically, so the visible breakage lives entirely inside the client's
own GpuRenderer pixel writes.)

In practice: **Manjaro ARM ships gtk4 1:4.18.6-1 in arm-testing as of
2026-05-04 — too old.** Void aarch64 ships gtk4-4.22.2_1 — works. The
fix is to update the Manjaro arm-testing channel; until then GTK4 apps on
Manjaro fall back to cairo with the magenta tint (or just won't render).

### SELinux and Memfd Sharing

Chroot processes run in the `magisk` SELinux context. By default, their memfds
get label `u:object_r:tmpfs:s0`, which the compositor (`untrusted_app`) can't
access. `ChrootMounter`'s mount script applies a `magiskpolicy` type_transition
rule so that magisk-created memfds automatically get `appdomain_tmpfs:s0`
instead — the same label that normal Android app memfds receive.

**Without root:** Run client processes as the same app/UID. Their memfds natively get
`appdomain_tmpfs` label.
