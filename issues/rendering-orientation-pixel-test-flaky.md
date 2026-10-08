# `rendering::test_shm_render_pattern_orientation_pixels` is flaky on physical

Fails roughly 2 runs in 3 on physical 50f4ca18 / Arch tawcroot, in
`scripts/run-integration-tests.sh --no-build rendering`. Failure is in
the block-color sampling, and *which* block fails varies:

    panicked at tests/rendering.rs:51: bottom-left block: Rgb { r: 21, g: 28, b: 36 }
    panicked at tests/rendering.rs:43: top-left block: Rgb { r: 0, g: 0, b: 0 }

Pre-existing, not caused by the wl_output-at-startup change: measured
2026-08-10 on both sides of that change with a rebuilt/reinstalled APK
each time — baseline 1 pass / 2 fails, changed build 1 pass / 2 fails,
same panic lines. `rendering::test_shm_xdg_popup_position_pixels` in the
same file passed every run.

The all-black sample suggests the screencap is taken before the
CompositorActivity is actually on screen (or while another window is
front), rather than a geometry error: the test gates on
`wait_for_rendered_toplevels(1)` + a flat 250ms sleep, and
`rendered_toplevels` is a compositor-side counter — it says the
compositor drew a frame, not that Android has composited that Activity
to the display. Likely fix is a real on-screen gate before
`adb::screencap_raw()` (e.g. poll the screencap until the expected block
colors stabilize, or wait on the Activity being resumed+focused) instead
of the sleep.

On the emulator (2026-10-08) it fails every run, on `main` too
(`top-left block: Rgb { r: 1, g: 1, b: 1 }`, 3/3 each side). See also
[first-shm-buffer-black-with-child-surface.md](first-shm-buffer-black-with-child-surface.md):
a first SHM buffer reproducibly rendering black there suggests a
compositor-side cause rather than screencap timing.
