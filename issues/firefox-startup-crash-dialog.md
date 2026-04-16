# Firefox shows crash-recovery / safe-mode dialog instead of browsing

Firefox 149 in the Arch chroot never reaches the actual browser UI. On every
launch (including immediately after `rm -rf /root/.mozilla /root/.cache/mozilla`
to force a fresh profile), one of two modal dialogs appears:

- **"Firefox closed unexpectedly while starting"** with *Refresh Firefox* /
  *Open…* buttons.
- **"Use this special mode of Firefox to diagnose issues"** when launched
  with `-safe-mode`.

Neither dialog is dismissed automatically, so the browser chrome never shows.

## What does work

Rendering through the libhybris → android_wlegl path draws these dialogs
correctly (right-way-up, at the 696×2400 AHB + 696×2326 SHM surfaces Firefox
requests), so this is purely a Firefox-side startup issue, not a compositor
bug. See `notes/rendering.md` for the AHB draw path (Y-flip + buffer_scale
handling fixed during the libhybris migration).

**This pre-dates the libhybris migration.** I verified by git-stashing all
post-migration changes back to the last pre-migration commit (01bfad9),
rebuilding libhybris + tawc-egl.c + compositor, and launching Firefox — same
crash dialog, same magenta-tinted left-half SHM rendering, same buffer sizes.
So this dialog is the state of Firefox on this chroot regardless of our WSI
architecture.

## Clues from stderr

Every launch logs these non-fatal errors that might contribute:

```
Crash Annotation GraphicsCriticalError: vaapitest: VA-API test failed:
    failed to open renderDeviceFD.
[Parent] WARNING: Failed to create DBus proxy for org.a11y.Bus: Cannot spawn
    a message bus without a machine-id: Invalid machine ID in
    /var/lib/dbus/machine-id or /etc/machine-id
Sandbox: Couldn't open video device /dev/video1
Sandbox: Couldn't open video device /dev/video0
ERROR: 0:2: '' : GLSL error: extension 'GL_EXT_shader_texture_lod' is not supported
ERROR: 0:2: '' : GLSL error: extension 'GL_EXT_draw_buffers' is not supported
```

`GL_EXT_shader_texture_lod` and `GL_EXT_draw_buffers` are Firefox's
shader-support probes — they aren't supposed to hard-fail WebRender, but
combined with the missing machine-id and VA-API, Firefox may be treating the
GPU process as unhealthy and killing it.

Also worth checking: the phantom-process-killer workaround in
`issues/phantom-process-killer-android-12.md` — if Firefox's content /
socket-process children get SIGKILL'd by the Android LMK before the parent
finishes initializing, Firefox's own restart-detector trips and it drops into
crash-recovery mode on the next launch.

## Reproduce

```
adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run \
  'rm -rf /root/.mozilla /root/.cache/mozilla; \
   GDK_GL=gles:always MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 \
   MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 \
   MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
   DISPLAY= firefox --no-remote'"
```

## Possible next steps

1. Generate `/etc/machine-id` in the chroot (`systemd-machine-id-setup` or
   `dbus-uuidgen > /etc/machine-id`).
2. Try forcing WebRender software: `MOZ_WEBRENDER=software` or
   `layers.acceleration.disabled=true` in prefs.
3. Check whether Firefox's GPU / content / RDD subprocesses are dying
   individually — correlate PIDs with logcat around the crash window.
4. Compare with a browser that's more tolerant (Chromium, Epiphany) to
   confirm whether the dialog behaviour is Firefox-specific.
