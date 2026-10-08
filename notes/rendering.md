# Rendering, Window Management, and Coordinate System

## Emulator shader translator quirk (dead `samplerExternalOES` text)

The x86_64 emulator's guest GL driver ("Android Emulator OpenGL ES
Translator") scans shader source textually without honouring the
preprocessor: a `uniform samplerExternalOES tex;` declaration inside a
*disabled* `#if defined(EXTERNAL)` branch makes it treat `tex` as an
external sampler. The non-external texture program variants then sample
the unbound external target and draw opaque black — SHM surfaces
rendered black while AHB/external surfaces worked, with zero GL errors
(upload, FBO readback, and completeness all checked out; only in-shader
sampling failed). It reproduces in the app process but not in a minimal
standalone binary using identical GL calls, so probe results can mislead.

This is an emulator driver bug, and TAWC currently ships no workaround
— SHM surfaces are known-black on the emulator and the affected tests
fail there. A verified fix (resolving `#if defined(X)` blocks against
each variant's define list in the smithay fork's `texture_program`
before the source reaches the driver) is written up in
[issues/emulator-shm-black-shader-translator.md](../issues/emulator-shm-black-shader-translator.md)
if it ever needs applying. Until then, avoid dead `samplerExternalOES`
text in any new shader that must work on the emulator.

## Background Color

The compositor clears every frame to a flat color matching the rest of the app UI. Keep Rust `BACKGROUND_COLOR` in `render.rs` and Android resource `tawc_window_bg` (`#1B1B22` in dark mode) in sync.

## Window Management

Root toplevels are configured as maximized at their host's logical size
(`round(physical_size / output_scale)`), with `bounds` set to the same size.
Child toplevels (`xdg_toplevel.set_parent`) get no size, no `Maximized`, and
`bounds` = host size, so they pick their natural size; X11 transients keep
their requested size clamped to the host. The xdg-decoration and legacy KDE
server-decoration protocols are implemented to suppress desktop titlebars; TAWC
presents each Linux toplevel as an Android app surface.

smithay reports a new xdg toplevel before its `set_parent`, so
`XdgShellHandler::parent_changed` moves a toplevel onto its parent's host and
reconfigures it. The Activity for a freshly minted host is spawned after the
dispatch (`pending_activity_spawns`), so a dialog never flashes its own task.

**Placement** (`placement.rs`): every window is drawn through a per-window
transform `screen = offset + window_pt * scale`, recomputed from its geometry
(`Window::geometry()`, which leaves out CSD shadows) whenever it is needed:

```
s      = min(1, H.w / g.w, H.h / g.h)
offset = (H - g.size * s) / 2 - g.loc * s
```

So too-big windows are scaled down to fit and centered, small ones centered,
and a maximized window filling the host gets the identity. Child toplevels
are centered the same way, above a black 50% scrim drawn just below the
topmost one. Popups and subsurfaces inherit their root window's transform.
X11 override-redirect windows (menus, tooltips) are placed at their X position
relative to their `WM_TRANSIENT_FOR` window, else the topmost window below
them, through that window's placement. A dialog's parent, mapped again, keeps
its dialogs above it (`DesktopRegistry::map_window_to_host`). The visible
host's placements are in `query-state` (`windows=`, `scrim=`).

Downscaled windows are sampled with smithay's texture shader filtering; a
per-window fractional `preferred_scale` (so clients render at the physical
size) is not implemented.

## Popup and Subsurface Positioning

Popup surfaces (xdg_popup) are tracked via Smithay's `PopupManager`. On `new_popup`, we
compute constrained geometry using `PositionerState::get_unconstrained_geometry()` and send
a configure. The constraint target is the visible host rect mapped into the root window's
frame through its placement, minus the root's geometry origin and the parent popup chain
(`TawcState::popup_constraint_target`). A popup is not re-constrained if its window's
placement changes while it is open. The PopupManager handles the popup tree hierarchy and provides popup positions
relative to their toplevel root.

**Note:** Firefox uses wl_subsurface (not xdg_popup) for its dropdown menus. Both paths go
through Smithay's desktop window surface collection.

## Wayland subsurface z-order

Firefox with WebRender creates two wlegl surfaces per window: a toplevel (main
thread, holds the `xdg_toplevel` role plus a placeholder buffer) and a
subsurface covering the full window, into which WebRender renders the actual
chrome + page content from the Renderer thread. The subsurface is above the
toplevel by default (Wayland z-order), so the subsurface's pixels must overlap
the toplevel's placeholder.

Both wlegl and SHM surfaces now use Smithay's desktop render-element path.
`render_frame` walks the host's `Space` in z-order and asks each window for
`WaylandSurfaceRenderElement`s at its placement, so Smithay owns the
parent/subsurface/popup ordering within a window. Firefox must still render the WebRender subsurface above the
toplevel placeholder; the integration pixel tests cover this through the
Smithay element path rather than TAWC's old reversed draw list.

## Smithay Renderer State

TAWC feeds committed renderable buffers into Smithay's `RendererSurfaceState`
with `on_commit_buffer_handler`. The Smithay fork has a compositor-provided
external buffer hook under
`smithay::backend::renderer`: TAWC wraps `android_wlegl` and
`tawc_gfxstream` AHB buffers in `ExternalBufferData`, reports their size,
alpha, and y-inversion metadata, and imports them through
`GlesRenderer::import_buffer`.

AHB import still lives in tawc. `WleglBufferData` owns the
`AHardwareBuffer`, carries an `AhbTextureImporter`, and creates the GL texture
from `ExternalGlesBuffer::import_gles`. This keeps Smithay generic: it sees a
renderable external `wl_buffer`, not Android allocation details.

SHM buffers also use Smithay's renderer import helper. TAWC no longer keeps
parallel per-surface SHM or WLEGL texture maps; diagnostics that need attached
buffer counts read Smithay renderer surface state for mapped desktop windows.

Rendering asks each window in a host-local Smithay `Space<Window>` projection
for its render elements at physical location `round(offset * output_scale)` and
scale `output_scale * s`, then wraps each `WaylandSurfaceRenderElement` in TAWC's custom
`RenderElement<GlesRenderer>` to preserve SHM/AHB tinting and forced-opaque
shader policy. Smithay sizes surface elements from the scale passed at *draw*
time, so the wrapper multiplies that by the window's `s`; locations were
already computed at the combined scale. The scrim is a
`SolidColorRenderElement` in the same list. Smithay owns window ordering, popup/subsurface collection,
surface geometry, viewport handling, and damage inside those elements; TAWC
still owns Android host selection and the final shader uniforms. The
frame/output transform owns the framebuffer Y flip; the wrapper leaves Smithay
element geometry and buffer transforms intact.

TAWC maintains one `Space<Window>` projection per Android host for xdg and X11
windows. Android foreground/background events map the advertised Smithay
`Output` only into the foreground host's space and keep that output sized to
the foreground host. The render loop draws only that foreground host; frame
callbacks are sent only through `Window::send_frame` for windows in that visible
space. Smithay owns popup/subsurface traversal and output visibility
bookkeeping while TAWC keeps Android Activity policy.

Smithay `Space` locations are window-geometry locations. TAWC maps each window
at `window.geometry().loc` so the underlying `wl_surface` origin sits at the
`Space` origin: `Space` holds every window in its own *window frame*, and the
placement transform is applied only at render and input time.

For input, touch and pointer picking map the screen point into each window's
frame through its placement, top-down, and ask
`Window::surface_under(WindowSurfaceType::ALL)` (see [input.md](input.md)).
Keyboard and text-input focus policy remains TAWC-owned after the surface has
been picked.

`tests/integration/tests/rendering.rs` contains a raw-screenshot pixel test
for a deterministic SHM pattern. It catches output-scale, y-orientation, and
basic placement regressions in the Smithay element path.

## Coordinate System

**This is subtle. Do not "fix" without understanding.**

1. **Logical vs physical:** Subsurface positions (from `wl_subsurface.set_position`) and
   popup positions (from xdg_positioner) are in logical (surface-local) coordinates.
   The renderer works in physical pixels. Convert logical edges through
   `OutputScale` and round the physical edges.

2. **Y-axis flip:** TAWC creates the `GlesFrame` with
   `Transform::Flipped180`, so Smithay render-element geometry stays in normal
   top-left desktop coordinates. Do not flip each element's destination rect.
   Per-surface buffer transforms stay owned by `WaylandSurfaceRenderElement`
   and are passed through when the wrapper draws with TAWC's tint shader.

3. **Window frame vs screen:** smithay (`Space`, popups, subsurfaces, seat
   focus origins and event locations) works in each window's frame, root
   surface origin at `(0,0)`. Host logical "screen" coordinates are
   `placement.offset + window_pt * placement.scale`. Convert only at the
   edges: rendering, incoming input, popup constraint targets.

4. **Surface size follows the wl_surface spec:**
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

   Smithay renderer surface state applies this size model for rendering and
   hit testing.

The canonical output scale factor lives in `TawcState::output_scale` as an
`OutputScale`, not an integer. Do not hardcode a scale elsewhere.

## Frame clock

Rendering follows Android's display vsync, on demand (`vsync.rs`,
`event_loop.rs`):

- A process-lifetime `tawc-vsync` thread owns an `ALooper` and its
  `AChoreographer`; each compositor run attaches a fresh channel. It never
  exits because the NDK keeps the choreographer in a destructor-less
  `thread_local`: a thread per run leaked 4 fds per cycle
  (`test_compositor_cycles_do_not_leak`). The
  compositor arms one `AChoreographer_postFrameCallback64` at a time
  (`Vsync::request` → `ALooper_wake`); the callback sends the vsync
  timestamp (`CLOCK_MONOTONIC` ns) over a calloop channel.
- `event_loop::after_dispatch` runs after every dispatch: Xwayland
  service, X11 association catch-up, dead window/popup/text-input cleanup,
  focus update on `toplevels_changed`, flush. It then arms vsync if
  `needs_render` or `frame_callbacks_pending` is set and the visible host
  has an EGL surface. Code that dirties state never arms anything itself.
- A vsync tick (`frame_tick`) first sends `wl_surface.frame` done to the
  visible host's windows and flushes, then renders the visible host if
  `needs_render`. Callbacks first give clients the whole period: sending
  them after render+swap (3–4 ms at 120 Hz) cost Firefox ~3 fps. Replies
  are dispatched only after the tick, so the render can't see half a
  frame. Every successful render answers `wp_presentation` feedback,
  including direct ones (host register/resize): GTK4 lost a Ctrl+V paste
  (`test_gtk4_widget_factory_copy_paste_and_text_input`) when a resize
  render left its feedback pending. Callback and presentation times are
  the vsync timestamp; presentation uses flag `VSYNC`, the output period
  as `refresh`, and an estimated MSC.
- Every commit sets both `needs_render` and `frame_callbacks_pending`.
  They are separate because Register/SurfaceChanged render directly and
  clear `needs_render` without sending callbacks.
- Idle (nothing committed, nothing dirty, or no visible bound host) means
  no ticks and no wakeups. A 250 ms timer covers the time-based work:
  `check_idle` and Xwayland start retries. `nativeStopCompositor` wakes
  the loop through a `LoopSignal`, since dispatch blocks without timeout.
- `query-state` reports `vsync_ticks`, `last_vsync_ns`, `vsync_period_ns`
  (shortest tick gap over the last 32 ticks), `tick_latency_max_ns`
  (worst vsync-to-tick-done over the same window; near the period means
  the compositor itself drops frames) and `output_refresh_mhz`.
  `helpers::assert_client_at_refresh_rate` divides frames by vsync-clock
  time, so adb latency doesn't skew it.

Measured (fps over vsync-clock time, one tick per frame):

| Device | Rate | weston-simple-egl (window / `-f` / `-b`) | vkcube | es2gears (Wayland / X11) | Firefox rAF page |
|---|---|---|---|---|---|
| 60 Hz phone | 60 | 60 | 60 | | |
| Pixel 9 Pro | 60 (Smooth display off) | 60 | | | |
| Pixel 9 Pro | 120 | 120 / 120 / 120 | 120 | 120 / 120 | 117.5–118 (Firefox rAF 120) |

Worst tick latency at 120 Hz: 3–5 ms of 8.3. The old 16 ms re-armed
timer gave ~55 fps at 60 Hz. Firefox's shortfall is its commits
occasionally missing a vsync; it was 115 with callbacks sent after
render.

Not done: render-late pacing with `AChoreographer_postVsyncCallback`
(API 33) frame deadlines, and real present times from
`EGL_ANDROID_get_frame_timestamps` (`HW_COMPLETION`).

## Refresh rate

`TawcState::output_refresh_mhz` is the single output mode's refresh
(default 60 Hz). `CompositorActivity`:

- requests the display's fastest mode in `surfaceCreated`:
  `preferredRefreshRate` + `Surface.setFrameRate(…, DEFAULT)` on API 30+,
  `preferredDisplayModeId` on 29. Without a request Android keeps the app
  at 60 Hz on a faster panel.
- reports `Display.getRefreshRate()` (`nativeSetOutputRefreshRate`) then
  and on every `DisplayListener.onDisplayChanged`. That is the rate the
  app actually gets, after user caps (Smooth display off sets
  `peak_refresh_rate=60`) and per-uid frame-rate overrides. The test phone
  is 120 Hz-capable but capped to 60 this way.
- persists it in `Settings.outputRefreshMhz`; `CompositorService` pushes
  that on start so a client's first `wl_output.mode` is already right. A
  service can't read display modes itself (`DisplayManager.getDisplay()`
  is null for non-visual contexts on Android 12+).

`AChoreographer_registerRefreshRateCallback` is deliberately not used: it
reports the display mode, not the per-app override, so it would fight
the Activity's value.

## SHM Buffer Support

SHM buffers (`wl_shm`) are supported alongside the AHB path. SHM matters even for
GPU-accelerated clients because cursor themes, toolkit subsurfaces/popups (GTK3/4), and
EGL fallback paths all use `wl_shm`.

**Magenta tint:** SHM surfaces are rendered with a distinct magenta tint via a custom
`GlesTexProgram` shader. This is intentional -- it makes it visually obvious when a client
falls back to SHM instead of using hardware-accelerated AHB buffers.

The render wrapper detects SHM buffers from Smithay's `buffer_type` metadata
and applies TAWC's magenta shader policy. AHB buffers are detected from
`WleglBufferData` attached to the `wl_buffer`.

## Alpha and Opaque Regions

`wl_surface.set_opaque_region` is a protocol optimization hint. TAWC's custom
AHB draw path currently ignores it for correctness: RGBA buffers render using
their sampled alpha. Android no-alpha formats (`RGBX_8888`, `RGB_888`,
`RGB_565`) are still treated as implicit full-surface opaque buffers because the
format itself has no alpha channel.

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
toplevels with the new logical sizes. The Android Settings slider and
`set-output-scale` broker action both use the same 0.5x..4.0x range,
snapped to 0.25x.

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
