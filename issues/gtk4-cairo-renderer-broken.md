- GTK4 with `GSK_RENDERER=cairo` (software / SHM path) prints `READY`,
  then immediately spams `Gsk-WARNING: drawing failure for render node
  Gsk*Node: the target surface has been finished` and the client exits.
- Reproduces on the device with `gtk4-debug-app text-input` and presumably
  any other GTK4 client. Default GSK renderer (Vulkan/GL via libhybris)
  works fine.
- Blocks the `tests/input.rs` group from running on the emulator. The
  group is currently structured to forward an `INPUT_ENV` shell prefix to
  the gtk4 debug app — once cairo works, switching `INPUT_ENV` to
  `"GSK_RENDERER=cairo"` is enough to unblock emulator support.
- Repro:
  ```
  adb shell '/system/bin/sh /data/local/tmp/arch-chroot-run \
      "GSK_RENDERER=cairo /tmp/gtk4-debug-app/gtk4-debug-app text-input"'
  ```
- Likely a compositor SHM/buffer-release bug (releasing the wl_buffer
  while cairo still holds the underlying mmap), but could also be a GTK4
  cairo-renderer bug — needs investigation. SHM works fine for the GTK3
  cairo path (`gtk3-demo-application` with `GDK_GL=disabled`), so something
  is GTK4-specific.
