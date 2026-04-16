# Rendering, Window Management, and Coordinate System

## Window Management

All toplevels are configured as maximized at the full logical output size (physical pixels /
scale factor). The output scale is 2x, so on a 1080x2400 display, apps see a 540x1200
logical surface. The xdg-decoration protocol is implemented, always requesting server-side
decorations (which we don't draw, so clients get no decorations). Surfaces are rendered at
(0,0) instead of centered.

## Popup and Subsurface Positioning

Popup surfaces (xdg_popup) are tracked via Smithay's `PopupManager`. On `new_popup`, we
compute constrained geometry using `PositionerState::get_unconstrained_geometry()` and send
a configure. The PopupManager handles the popup tree hierarchy and provides popup positions
relative to their toplevel root.

**Note:** Firefox uses wl_subsurface (not xdg_popup) for its dropdown menus. Both paths go
through the same rendering code in `draw_shm_surfaces`.

## Wayland subsurface z-order for wlegl (AHB-backed) surfaces

Firefox with WebRender creates two wlegl surfaces per window: a toplevel (main
thread, holds the `xdg_toplevel` role plus a placeholder buffer) and a
subsurface covering the full window, into which WebRender renders the actual
chrome + page content from the Renderer thread. The subsurface is above the
toplevel by default (Wayland z-order), so the subsurface's pixels must overlap
the toplevel's placeholder.

`draw_wlegl_surfaces` walks each toplevel tree with `with_surface_tree_downward`
and then **reverses** the collected list before drawing. `with_surface_tree_downward`
invokes its processor post-order (children first, then parent), which is the
*reverse* of Wayland z-order — drawing in visit order would paint the toplevel
last and cover the subsurface. Without the reverse, Firefox renders as a black
rectangle because the toplevel's placeholder buffer occludes WebRender's output.
The same reversal is not needed for `draw_shm_surfaces` today only because
nothing produces overlapping SHM subsurfaces with this ordering pathology; if
an app starts doing so, apply the same `reverse()` there.

## Coordinate System

**This is subtle. Do not "fix" without understanding.**

1. **Logical vs physical:** Subsurface positions (from `wl_subsurface.set_position`) and
   popup positions (from xdg_positioner) are in logical (surface-local) coordinates.
   The renderer works in physical pixels. Multiply positions by `output_scale`.

2. **Y-axis flip:** Smithay's GlesRenderer uses a GL projection where Y=0 is at the
   **bottom** of the screen, not the top. For SHM surfaces at non-origin positions:
   `physical_y = screen_h - logical_y * scale - texture_h`. For AHB (android_wlegl)
   surfaces drawn fullscreen at the origin the per-row offset cancels out, but the
   buffer itself is still Y-down (Wayland convention) vs. Y-up (GL), so we pass
   `Transform::Flipped180` to `Frame::render_texture_from_to` — same reason as SHM.
   Getting this wrong flips the client's content upside down; Firefox and the
   `weston-simple-egl` triangle both expose the bug.

3. **AHB buffers are drawn 1:1 at their pixel dimensions.** In theory,
   `wl_surface.set_buffer_scale(n)` means the compositor should divide by `n`
   and re-scale. In practice, Firefox/WebRender renders its main surface at
   the output's physical resolution (1080×2400) but commits `buffer_scale=1`
   — applying the spec formula `dst = buffer / buffer_scale × output_scale`
   would draw it at 2× size and blow it off-screen. Since every libhybris
   client we have allocates buffers that already match the intended
   on-screen dimensions, we skip the buffer_scale math and draw at buffer
   size. This matches the SHM draw path, which also uses buffer dimensions.

The canonical scale factor lives in `TawcState::output_scale`. Do not hardcode `2` elsewhere.

## SHM Buffer Support

SHM buffers (`wl_shm`) are supported alongside the AHB path. SHM matters even for
GPU-accelerated clients because cursor themes, toolkit subsurfaces/popups (GTK3/4), and
EGL fallback paths all use `wl_shm`.

**Magenta tint:** SHM surfaces are rendered with a distinct magenta tint via a custom
`GlesTexProgram` shader. This is intentional -- it makes it visually obvious when a client
falls back to SHM instead of using hardware-accelerated AHB buffers.

The SHM path is tracked separately from AHB: `surface_shm` HashMap holds `SurfaceShmState`
per surface. Surfaces using the AHB channel protocol are never checked for SHM buffers.

### SELinux and Memfd Sharing

When root processes in the chroot create memfds, they get label `u:object_r:tmpfs:s0`.
The `untrusted_app` domain lacks permission on `tmpfs`, so the compositor can't mmap them.
The failure causes the Wayland protocol parser to get out of sync.

**Solution (with root):** An LD_PRELOAD shim (`client/memfd-selinux-shim/`) intercepts
`memfd_create()` and calls `fsetxattr` to relabel the memfd as `appdomain_tmpfs`.

**Shim limitation:** GDK/GLib calls `syscall(SYS_memfd_create, ...)` directly, bypassing
the LD_PRELOAD. Workaround: `setenforce 0`. Proper fix: intercept `syscall()` itself, or
run clients as the compositor's UID.

**Without root:** Run client processes as the same app/UID. Their memfds natively get
`appdomain_tmpfs` label.
