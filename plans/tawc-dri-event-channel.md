# TAWC-DRI Server→Client Event Channel

## Problem

TAWC-DRI is strictly one-way: the client sends `QueryVersion` /
`PresentBuffer`, the server never speaks. Two standing consequences:

1. **No resize notification.** The compositor resizes every X11
   toplevel to its host activity's logical size
   (`compositor/src/xwayland.rs::configure_x11_toplevel_for_host`).
   The app follows core ConfigureNotify and resets its viewport, but
   libhybris's `X11NativeWindow` keeps creation-time buffer
   dimensions, so clients render outside their buffers and windows
   composite solid black. Active bug:
   [issues/x11-presented-windows-black.md](../issues/x11-presented-windows-black.md).
2. **No buffer-release feedback.** `queueBuffer` marks each buffer
   free immediately after presenting; the 3-slot round-robin only
   heuristically avoids overwriting a buffer the compositor is still
   sampling ("enough breathing room" — see comment in
   `x11_window.cpp::queueBuffer`).

Both exist because a library sharing the app's X connection cannot
receive *core* events: there is one event queue per connection, the
app owns it, and selecting StructureNotify would push ConfigureNotify
into the app's queue where whoever reads first wins. The sanctioned
mechanism for library-private events is **X Generic Events (XGE)**
routed to a libxcb *special event queue* — exactly how Present
extension events reach Mesa's DRI3 loader without disturbing the
app's event loop, including on Xlib-owned connections (special-event
filtering happens inside libxcb before Xlib sees anything).

## Protocol: TAWC-DRI v0.3

Both header copies change together: `Xext/tawcdriproto.h` (Xwayland
patch stack in `deps/xwayland-patches/`) and the vendored
`hybris/egl/platforms/x11/tawc_dri_protocol.h` (libhybris fork).

New request:

- `TAWCDRISelectInput(eid, window, event_mask)` — `eid` is a
  client-allocated XID (`xcb_generate_id`), the routing key for the
  special event queue, same role as Present's event id. Mask bits:
  `TAWC_DRI_EVENT_MASK_CONFIGURE_NOTIFY`,
  `TAWC_DRI_EVENT_MASK_BUFFER_RELEASE`.

Changed request:

- `TAWCDRIPresentBuffer` gains a client-chosen `serial` (u32),
  echoed in BufferRelease.

New events, encoded as XGE generic events (`response_type` 35,
`extension` = TAWC-DRI major opcode). Lay the fields out exactly like
Present's events — `evtype` at bytes 8–9, `eid` at bytes 12–15 — so
libxcb's special-event matching finds the eid where it expects it
(verify the matching rule against libxcb `xcb_in.c` during
implementation; it is the reason Present events carry their event id
at that offset):

- `TAWCDRIConfigureNotify { eid, window, width, height }`
- `TAWCDRIBufferRelease { eid, window, serial }`

## Server side (Xwayland patch stack)

- `Xext/tawc-dri.c`: dispatch `SelectInput`; track
  `(client, window) → { eid, mask }`; clean up on client disconnect
  and window destroy (`AddResource` on the window, like Present's
  event contexts). Immediately emit one ConfigureNotify carrying the
  window's *current* size on SelectInput — closes the race where the
  WM resize lands between window creation and event registration
  (this exact race is why `eglx11-test` accidentally survived the
  black-window bug while `es2gears_x11` didn't).
- Emit ConfigureNotify when the window's size actually changes. Model
  on how Present learns of window config changes (ConfigNotify hook);
  a mismatch check at PresentBuffer time is *not* sufficient on its
  own — a 1 fps client would render a full wrong-sized frame before
  hearing about it.
- `hw/xwayland/xwayland-tawc.c`: the existing `wl_buffer.release`
  listener (which already frees the per-present trio) additionally
  emits `TAWCDRIBufferRelease(serial)` when the presenting client
  selected it. Requires the trio to remember `{ client, window,
  serial }`.

## Client side (libhybris x11 platform plugin)

- `X11NativeWindow` construction: `xcb_generate_id` an eid, send
  `SelectInput(CONFIGURE_NOTIFY | BUFFER_RELEASE)`, register via
  `xcb_register_for_special_xge(conn, &tawc_dri_id, eid, NULL)` (the
  same `xcb_extension_t` the plugin already uses for
  `xcb_send_request_with_fds`; libxcb resolves the major opcode by
  name at runtime).
- Drain `xcb_poll_for_special_event` at the top of `dequeueBuffer`:
  - ConfigureNotify → update `m_width`/`m_height`; the existing lazy
    reallocation in `dequeueBuffer` (dimension-mismatch check) does
    the rest, and `presentBuffer` already sends per-buffer
    dims/stride so the whole downstream pipe follows automatically.
  - BufferRelease → clear that serial's buffer `busy` flag.
- Replace the round-robin freshness heuristic with a real lifecycle:
  buffer `busy` from `queueBuffer` until its BufferRelease arrives.
  When all buffers are busy, block in `xcb_wait_for_special_event`
  (true backpressure) — with a sanity timeout/fallback so a
  wedged/killed server degrades to the old behaviour instead of
  deadlocking the client.
- Gate everything on `TAWCDRIQueryVersion` ≥ 0.3 and fall back to
  today's behaviour against older servers. Both ends ship in the same
  APK (Xwayland binary + rootfs libhybris copy), so this is
  belt-and-braces, not a real compatibility surface.

## Steps

1. Protocol headers + version bump (both vendored copies, one
   change).
2. Server: SelectInput dispatch + ConfigureNotify emission (hook +
   initial-state event).
3. Client: special event queue + resize handling. **This fixes the
   black-window bug** — verify before moving on.
4. PresentBuffer serial + BufferRelease emission + client buffer
   lifecycle/backpressure. Separable follow-up; land 1–3 first.
5. Tests, docs, fork bookkeeping.

## Verification

- The [issues/x11-presented-windows-black.md](../issues/x11-presented-windows-black.md)
  repro: `es2gears_x11` shows rotating gears filling the resized
  window (screencap), where today it is solid black. A prototype
  geometry-poll fix already proved the resize path renders correctly
  once buffers follow the window, so step 3's result is known-good
  territory.
- Integration: extend the es2gears test (or add a sibling) to assert
  broker counters report `last_wlegl_width/height` equal to the host
  logical size after the compositor's configure — pixel-blind but
  catches exactly this regression class. Complements the screencap
  assertion asked for in
  [issues/xwayland-gl-tests-pixel-blind.md](../issues/xwayland-gl-tests-pixel-blind.md).
- Client-side bisection tooling: `eglx11-test`'s
  `TAWC_EGLX11_{TRIANGLE,DEPTH,VBO,MVP,READBACK,W,H}` env modes.
- Release path: add a served-releases counter (server or compositor
  side) and assert it advances during
  `xwayland::test_tawc_dri_ahb_present_animated_loop`.
- Bookkeeping on completion: update notes/xwayland.md (round-robin
  and "no server→client release feedback" prose), TAWC_FORK.md,
  consolidate the Xwayland patch into the step-3 present patch or a
  new numbered patch, bump the libhybris pin in `deps/deps.list`, tag
  the fork.
