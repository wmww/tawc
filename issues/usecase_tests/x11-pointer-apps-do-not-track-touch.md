# X11 pointer-tracking apps (xeyes) do not respond to touch

Found during the `gui-x11-apps` usecase test (physical, OnePlus 9,
Arch tawcroot), 2026-07-13.

## Summary

`xeyes` renders correctly through Xwayland but its pupils never track
injected touch. Tapping or holding a touch at the top-left vs the
bottom-right of the screen produces **byte-identical** screencaps —
the pupils hold their default downward gaze regardless of touch
location. So the plan's step 4 ("inject touch/pointer motion … the
pupils should track") cannot pass on the current build.

## Repro

1. `pacman -S --noconfirm xorg-xeyes` in the Arch tawcroot install.
2. `TAWC_OP_TITLE= scripts/rootfs-run.sh 'xeyes' &` (op-title empty so
   no LogScreenActivity steals foreground).
3. `adb shell input tap 150 450` then screencap; `adb shell input tap
   950 1750` then screencap. Also tried held touches via
   `adb shell input swipe X Y X Y 1600` with a mid-hold screencap.
4. Compare: all post-touch frames are identical (md5) to each other;
   pupils never move toward the touch. (Verified via screenshot
   sub-agent analysis; the only inter-frame delta was the status-bar
   clock.)

## Root cause (by design, per notes/input.md)

tawc is a touch-first compositor. The seat forwards touch as `wl_touch`
only; it deliberately does **not** synthesize pointer motion from touch
(notes/input.md lines ~26-32: "Do not expand that into a general
touch-to-pointer path"). The lone `wl_pointer` source is the optional
GTK3 broken-menus workaround, which emits only a *brief center
enter/leave per new toplevel* — no continuous motion. `xeyes` polls the
core X pointer (`XQueryPointer`), which Xwayland derives from
`wl_pointer`; with no ongoing pointer motion, the pointer never moves,
so the pupils never track.

Net: any X11 (or Wayland) app whose UX depends on pointer *motion* /
hover — xeyes, tooltips-on-hover, hover-highlight, mouse-look demos —
is non-interactive under touch on the current build. Rendering,
keyboard, tap-to-focus, and tap-as-click-ish behavior are unaffected.

## Assessment / layer

Compositor input model (documented, intentional). Not a regression —
the compositor behaves exactly as notes/input.md describes. This is a
known-limitation / test-plan-expectation mismatch, not a broken code
path. Two follow-ups worth a decision:

1. **Fix the plan.** `plans/usecase_tests/gui-x11-apps.md` step 4
   asserts pupil tracking, which contradicts the documented design.
   The plan has been kept (not marked passed) and annotated with this
   reference.
2. **Product decision (optional).** If pointer-motion X11 apps are ever
   a target usecase, tawc would need a deliberate touch/drag→`wl_pointer`
   motion path (the notes explicitly reserve that for *real* pointer
   hardware today). Low priority — classic pointer-hover apps are a
   poor fit for a touch phone regardless.

## What DID pass in the same run

- xterm renders (magenta SHM tint, root `#` prompt).
- Keyboard via broker `hardware-key`: typed `ls /`, real directory
  listing drew, fresh prompt returned.
- xterm + xeyes each got their own Android task (`query-state`
  `hosts=2`, `x11_surfaces=2`, one shared Xwayland pid).
- Clean shutdown: both clients closed → `x11_surfaces=0`,
  `toplevels=0`; Xwayland idled out (`xwayland_running=false`) after
  its `-terminate 5` grace.
</content>
</invoke>
