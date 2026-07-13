# Usecase test: play a simple game (Chocolate Doom + Freedoom)

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** physical only — SDL/GL rendering needs libhybris.
**Usecase:** a user installs a small game and plays it. Games are the sharpest test of the SDL → Wayland → libhybris path, and there is already a suspicious SDL issue on file.

## Prerequisites

- Cache proxy up (README step 6).
- `pacman -S --noconfirm chocolate-doom freedoom` (if either is missing
  from the repos, `prboom-plus` + `freedoom` is an acceptable
  substitute; adjust flags accordingly).

## Steps

1. Find the WAD path (`pacman -Ql freedoom | grep wad`).
2. Launch without audio (there is no audio bridge — plans/audio.md):
   `scripts/rootfs-run.sh 'chocolate-doom -iwad /usr/share/doom/freedoom1.wad -nosound' &`
   (rootfs env already sets `SDL_VIDEODRIVER=wayland,x11`).
3. Screenshot: the game menu/title screen should render fullscreen-ish,
   un-tinted (GL path).
4. Drive it a little: Enter through the menu into a level via broker
   `hardware-key` actions, send a few movement keys, screenshot twice
   and confirm the view changed (i.e. it's actually playing, not a
   frozen frame).
5. Watch stability ~2 minutes of input; then quit via the in-game menu
   (Esc → Quit) rather than killing it, to test clean SDL teardown.
6. If launch fails with SDL "no displays available" or "video subsystem
   not initialized": that is `issues/supertuxkart-sdl-no-displays.md`
   territory — retry a couple of times (it's intermittent there),
   gather logs (`adb logcat -s tawc-native` around the failure), and
   add your reproduction details to that issue rather than filing a new
   one.

## Expected results

- Game starts, renders, responds to keys, and exits cleanly with
  `-nosound`. Audio is expected to be absent, and its *absence must not
  crash the game* — if the game refuses to run without the `-nosound`
  flag (i.e. default audio init is fatal), record that as a finding:
  users won't know the flag.

## Known issues / caveats

- `issues/supertuxkart-sdl-no-displays.md` — intermittent SDL display
  init failure, directly adjacent to this test.
- No audio bridge exists (plans/audio.md); silence is expected.
- `issues/hardware-backspace-stuck-down.md` if you use Backspace in
  menus.

## Cleanup

Quit the game, `pacman -Rns chocolate-doom freedoom`, delete
screenshots on device and host.

## Run result (2026-07-13, physical 50f4ca18, Arch tawcroot) — FAIL

Blocked by the adjacent SDL issue; details appended to
`issues/supertuxkart-sdl-no-displays.md`.

Package availability: chocolate-doom, freedoom, prboom-plus, gzdoom and
crispy-doom are **all absent from the Arch Linux ARM repos** (`pacman -Ssq`
finds only `doomretro`). The plan's chocolate/prboom substitutes do not
exist here. Substituted `doomretro` (SDL2 engine) + a freedoom1.wad v0.13.0
fetched via the cache proxy to still exercise the SDL → Wayland → libhybris
path. (A real user following this usecase on Arch ARM would hit the missing-
package wall immediately — a doom-specific packaging gap, not a tawc bug.)

Outcome: doomretro fails SDL video init 100% of the time (`SDL_GetNumVideoDisplays
... "Video subsystem has not been initialized"`), under both
`SDL_VIDEODRIVER=wayland` and `x11`. Only SDL's standalone error message box
renders (Xwayland/SHM, magenta as expected); the game never launches, so no
frame ever rendered, input/stability/clean-exit steps were not reached, and
the `-nosound`/audio question could not be evaluated. WAYLAND_DEBUG shows the
compositor advertises a complete wl_output that SDL binds and then discards.
Full repro + diagnostics in the issue above. Not added to Completed.
