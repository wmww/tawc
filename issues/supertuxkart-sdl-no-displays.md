# SuperTuxKart SDL intermittently reports no displays on physical target

On the physical target, focused SuperTuxKart integration-test runs have
failed against both the freshly installed Debian sid rootfs and the
pre-existing Arch rootfs, while a later full Debian sid integration run
passed both SuperTuxKart tests.

Observed filters:

    TAWC_INSTALL_ID=debian-sid scripts/run-integration-tests.sh test_supertuxkart
    TAWC_INSTALL_ID=arch scripts/run-integration-tests.sh test_supertuxkart

Debian sid result:

    apps::test_supertuxkart_launches
      supertuxkart crashed/exited before first paint
    libhybris::test_supertuxkart_renders_via_ahb
      supertuxkart crashed/exited before rendering

Arch result:

    apps::test_supertuxkart_launches
      supertuxkart crashed/exited before first paint
    libhybris::test_supertuxkart_renders_via_ahb
      Failed to read PID/PGID from pidfile ... within 10s

Manual Debian stderr with the compositor running:

    Unable to initialize SDL!: The video driver did not add any displays
    No display created: : Video subsystem has not been initialized
    [fatal  ] irr_driver: Couldn't initialise irrlicht device. Quitting.

Tried without improvement:

    SDL_VIDEODRIVER=wayland
    SDL_VIDEODRIVER=x11
    SDL_VIDEODRIVER=wayland,x11
    SDL_VIDEO_WAYLAND_MODE_EMULATION=0/1
    SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0/1
    SDL_VIDEO_WAYLAND_PREFER_LIBDECOR=0
    supertuxkart --screensize=800x600 --windowed

This appears separate from Debian sid enablement:

- Debian originally could not find `supertuxkart` because Debian installs
  it under `/usr/games`; the rootfs PATH fix resolves that.
- The remaining SDL/no-display failure reproduces on Arch too.
- A later full Debian sid suite passed:

      test result: ok. 66 passed; 0 failed; 8 ignored

## Second SDL2 app reproduces it: DOOM Retro (2026-07-13, physical, Arch)

Ran the `gui-doom-game` usecase test on physical 50f4ca18. chocolate-doom,
freedoom, prboom-plus, gzdoom and crispy-doom are all absent from the Arch
Linux ARM repos; the only doom engine available is `doomretro` (SDL2 —
depends on sdl2/sdl2_image/sdl2_mixer). Used it with a freedoom1.wad
(v0.13.0, fetched via the cache proxy) as an SDL2 substitute.

Result: **doomretro fails SDL video init on every attempt** (not
intermittent here — 100% over ~12 runs). doomretro prints and then pops a
modal SDL error message box:

    The call to SDL_GetNumVideoDisplays() failed on line 1,318 of i_video.c:
    "Video subsystem has not been initialized"

Key observations that narrow the bug:

- Both `SDL_VIDEODRIVER=wayland` and `SDL_VIDEODRIVER=x11` fail identically.
  So it is not a wayland-vs-Xwayland fallback problem — SDL's whole video
  init is failing.
- The failure box itself *does* render (via SDL's standalone X11 messagebox
  path, Xwayland + SHM, so it is magenta-tinted as expected). So Xwayland
  X11 SHM windows work; it is specifically `SDL_VideoInit` that fails.
- The compositor advertises a **complete** `wl_output`. `WAYLAND_DEBUG=1`
  under the wayland driver shows SDL bind `wl_compositor` v6, `xdg_wm_base`
  v7, `wl_shm`, `wl_seat` v9, and `wl_output` v4, then receive full output
  state — `geometry(0,0,68,150,...,"tawc","Android")`, `mode(1080x2169@60)`,
  `name("tawc-0")`, `scale(2)`, `done()` — plus `zxdg_output_v1`
  `logical_position(0,0)` / `logical_size(540,1085)` / `done`. SDL then
  **tears everything back down** (destroys zxdg_output, releases wl_output,
  keyboard, pointer, wl_seat, destroys all managers) and reports video-not-
  initialized. So this directly answers the "does the compositor advertise
  enough wl_output state" next-check: it does, and SDL rejects it anyway.
- One protocol anomaly worth checking: the compositor emits `wl_output#16.done()`
  **twice** — once as part of the wl_output burst and again right after the
  zxdg_output events. Unclear if SDL cares, but it is a real double-done.
- `tawc-native` logcat during the failure is silent except the normal
  `output_bind name="tawc-0"`; no EGL/GLES/wayland error compositor-side.
  The failure is entirely inside SDL's client-side video init.
- Audio (`-nosound` question in the plan) is untested: video init fails
  before doomretro reaches audio setup, so the no-audio-bridge behavior was
  never exercised. (doomretro has no `-nosound` flag anyway.)

This confirms the failure is an SDL2-video ↔ tawc problem, not SuperTuxKart-
specific: a second, unrelated SDL2 app hits the same wall, and here 100%
reproducibly. Consider driving diagnosis from doomretro rather than STK —
it is smaller, in-repo (`doomretro` on Arch), and fails every time.

## Next Checks

- Compare against an older APK/build where SuperTuxKart last passed.
- Check whether the compositor advertises enough `wl_output` state for
  SDL's display enumeration.
- Capture `WAYLAND_DEBUG=1 supertuxkart` until SDL disconnects.
- Consider replacing SuperTuxKart as a routine integration probe if SDL
  keeps regressing independently of the graphics paths under test.
