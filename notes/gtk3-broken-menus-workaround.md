# GTK3 broken menus workaround

## Summary

tawc has a compatibility workaround for GTK3 native Wayland menubars on
touch-only Android devices. The Settings screen exposes it as **GTK3 broken
menus workaround**, with the description:

> Spoof a pointer briefly entering each window, allows GTK3 menus to work correctly

It is enabled by default. When enabled, the Wayland seat advertises
`wl_pointer` and tawc briefly sends pointer enter/leave at the center of each
new xdg toplevel. When disabled, tawc removes the pointer capability and stops
sending these synthetic pointer events.

This is intentionally a contained workaround, not the start of a general
touch-to-pointer input path. Android touchscreen input still goes through
`wl_touch`; tawc does not translate finger taps into pointer buttons.

## Symptom

With native GTK3 Wayland apps such as `lxterminal` and
`gtk3-demo --run=menus`, the first fresh touchscreen tap on a non-leftmost
menubar item can open the correct popup, immediately destroy it, and then open
the leftmost menu instead.

Physical target observations on 2026-05-19:

- `lxterminal` Help tap delivered correct `wl_touch.down` coordinates
  (`160.0,13.5`).
- GTK first requested the Help popup with
  `xdg_positioner.set_anchor_rect(134, 0, 49, 27)`.
- GTK then destroyed it and requested the File popup with
  `set_anchor_rect(0, 0, 42, 27)`.
- `gtk3-demo --run=menus` showed the same pattern: a fresh `bar` tap delivered
  `wl_touch.down(..., 110.0, 23.5)`, requested
  `set_anchor_rect(90, 0, 39, 46)`, then replaced it with
  `set_anchor_rect(0, 0, 51, 46)`.
- `lxterminal` and `gtk3-demo` `WAYLAND_DEBUG=1` logs contained no
  `wl_pointer` / `get_pointer` events in the old touch-only configuration.

The installed packages on the physical device were:

- GTK3: `gtk3 1:3.24.52-1`
- GTK4: `gtk4 1:4.22.4-1`

This matters because GTK MR !8240 was included in GTK 3.24.49, so the tawc
failure is not the already-fixed KDE/GTK crossing bug by itself.

## What Actually Triggers It

The bug is not universal GTK3 touch-only Wayland breakage. A host fake
compositor under `build/gtk-touch-host-repro/` ran GTK 3.24.52 with a
keyboard+touch seat and no pointer capability. With no legacy KDE
server-decoration protocol, `gtk3-demo --run=menus` opened the touched menu on
the first tap:

- `POPUP 1 anchor_rect=51,37 39x46`
- no second leftmost popup

The same harness reproduced tawc's failure after adding the legacy KDE
server-decoration protocol and answering `SERVER` mode. In that mode GTK
removes client-side chrome, the menubar moves to window-local y=0, and the
same touch-only `bar` tap creates:

- `POPUP 1 anchor_rect=51,0 39x46`
- `POPUP 2 anchor_rect=0,0 51x46`

Keeping the KDE protocol present but answering `CLIENT` mode leaves GTK in
client-side decoration mode, keeps the menubar below the titlebar at y=37, and
the same touch-only tap opens only the tapped menu.

The trigger is therefore GTK3's cold pointer-emulating touch/crossing path plus
forced legacy KDE server-side decoration, which puts the leftmost menubar item
at window-local `(0,0)`.

## Ruled Out

These did not explain or fix the specific tawc failure:

- Output scaling or coordinate translation. The touch coordinates and first
  popup request were correct.
- xdg-decoration. GTK3 uses the legacy KDE server-decoration protocol for this
  path.
- Touch-only Wayland in general. Host GTK 3.24.52 works when GTK remains in
  client-side decoration mode.
- Exposing `wl_pointer` without sending pointer enter/leave. GTK binds the
  pointer and may send `wl_pointer.set_cursor`, but the fresh non-leftmost
  touch still falls back to the leftmost menu.
- `gtk-touchscreen-mode=true`.
- `GDK_CORE_DEVICE_EVENTS=1`.
- Preloading a constructor that calls `gdk_disable_multidevice()`.
- A redundant same-position `wl_touch.motion` after touch down.

The earlier popup-grab-stack bug found during investigation was real but
separate. It is covered by
`input::test_touch_grabbed_popup_switches_to_next_popup`.

## Workaround Behavior

The tested workaround is to make GTK see a pointer crossing away from the
leftmost `(0,0)` menubar item before the first touch:

- Advertise `wl_pointer`.
- Send `wl_pointer.enter` at the center of the toplevel.
- Send `wl_pointer.leave`.
- Then deliver touches normally as `wl_touch`.

Host tests with the failing KDE server-decoration setup showed that center
enter/leave fixes both the local GTK test app and host `gtk3-demo --run=menus`:

- center enter/leave, then `bar` tap: only
  `POPUP 1 anchor_rect=51,0 39x46`
- no second `anchor_rect=0,0 51x46` popup

The pointer does not need to hover over the tapped menu item, and no pointer
motion event is required after entering. However, entering at `(0,0)` and
leaving does not work, and entering at `(0,0)`, moving to `(1,1)`, then leaving
also does not work. The useful priming point needs to be away from the
leftmost menu item.

## Implementation

The code is deliberately labeled and grouped as "GTK3 broken menus workaround"
so it is easy to remove later:

- `Settings.gtk3BrokenMenusWorkaround`: persisted setting, default `true`.
- `SettingsActivity.buildGtk3BrokenMenusWorkaroundCheckbox()`: checkbox and
  explanatory text.
- `NativeBridge.nativeSetGtk3BrokenMenusWorkaround()`: live toggle.
- `SurfaceEvent::Gtk3BrokenMenusWorkaroundChanged`: compositor-thread event.
- `TawcState::gtk3_broken_menus_workaround_enabled`: compositor state flag.
- `compositor/src/gtk3_menus_workaround.rs`: isolated Rust helper.
- `XdgShellHandler::new_toplevel`: calls `prime_toplevel()` for each new
  toplevel.

When enabled at compositor startup, `TawcState::new()` calls
`seat.add_pointer()`. When the setting changes live, the helper calls
`seat.add_pointer()` or `seat.remove_pointer()` and updates the state flag.

The pointer capability is a seat-level Wayland capability, so it is visible to
all clients while the workaround is enabled. There is no Wayland mechanism in
use here to advertise a pointer to only one GTK client. The compatibility trade
off is acceptable because the synthetic pointer stream is minimal and the
feature can be disabled live.

Existing clients may not bind a newly-added pointer immediately enough to be
helped for already-created windows. The supported behavior is that after the
setting is enabled, newly-created toplevels are primed.

## Removal Map

If GTK3 or tawc's decoration policy changes enough that this workaround is no
longer needed, remove:

- the Settings key/property and Settings screen checkbox;
- the Kotlin `NativeBridge` declaration and JNI function;
- the `SurfaceEvent` variant and event-loop handler branch;
- `TawcState::gtk3_broken_menus_workaround_enabled` and initial
  `seat.add_pointer()` branch;
- `compositor/src/gtk3_menus_workaround.rs`;
- the `prime_toplevel()` call in `XdgShellHandler::new_toplevel`;
- the broker get/set actions and their docs;
- this note.

