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

## Next Checks

- Compare against an older APK/build where SuperTuxKart last passed.
- Check whether the compositor advertises enough `wl_output` state for
  SDL's display enumeration.
- Capture `WAYLAND_DEBUG=1 supertuxkart` until SDL disconnects.
- Consider replacing SuperTuxKart as a routine integration probe if SDL
  keeps regressing independently of the graphics paths under test.
