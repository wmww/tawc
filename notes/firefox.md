# Firefox

Firefox 149 renders via the libhybris Wayland EGL platform against the Adreno
660 vendor driver. Window chrome goes through AHB; fragments of content can
fall back to SHM (no `zwp_linux_dmabuf_v1` support yet in this compositor).

**Known issue:** on this chroot Firefox reliably lands on its "Firefox closed
unexpectedly while starting" crash-recovery dialog at startup, so the visible
content is usually that dialog rather than a real browser. Both the
pre-migration `tawc-egl.c` WSI and the current libhybris WSI produce identical
behaviour here — it's a Firefox-side startup problem (see
`issues/firefox-startup-crash-dialog.md`), not a rendering bug.

## Launching

```bash
adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run \
  'GDK_GL=gles:always MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 \
   MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 \
   MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
   DISPLAY= firefox --no-remote'"
```

Firefox-specific env vars (not in the chroot profile because they're app-specific):
- `GDK_GL=gles:always` -- forces GTK to use GLES (not desktop GL) for rendering
- `MOZ_ENABLE_WAYLAND=1` -- use Wayland backend (not X11)
- `MOZ_ACCELERATED=1` -- force hardware acceleration
- `MOZ_DISABLE_*_SANDBOX=1` -- chroot lacks namespace support for sandboxing
- `DISPLAY=` -- clear X11 display so Firefox doesn't try X11

### Why GDK_GL=gles:always (not disabled)

With `gles:always`, GTK uses GLES via libhybris's vendor EGL, producing AHB
buffers for the window chrome. With `GDK_GL=disabled`, GTK renders everything
via SHM (CPU) — the magenta-tinted SHM path in the compositor.

This requires the libGLESv2 shim (see "GL Library Shims" below) to provide
GLX stubs. Without the stubs, libepoxy (GTK's GL dispatch) aborts when
probing for GLX symbols in a GLES-only library.

## GL Library Shims

Android has GLES only -- no desktop GL, no GLX. But Firefox and GTK have conflicting
GL library requirements:

- **Firefox (WebRender):** `dlsym(libGL_handle, "glBindTexture")` -- resolves GLES
  symbols from `libGL.so`. Without it in `LD_LIBRARY_PATH`, Firefox loads the system
  Mesa `/usr/lib/libGL.so` which doesn't work with the Android GPU.

- **GTK/libepoxy:** `dlsym(libGLESv2_handle, "glXGetCurrentContext")` -- probes for
  GLX to detect context type. Aborts if the probe produces an error (vs returning NULL).

**Solution:** Shim libraries in `/tmp/gl-shims/` that re-export libhybris GLES symbols
AND provide GLX stubs returning NULL (indicating "no GLX context, use EGL"):

```
libGL.so         -- shim: GLX stubs + DT_NEEDED libGL.so.1
libGL.so.1       -> libGLESv2_real.so (symlink)
libGLESv2.so.2   -- shim: GLX stubs + DT_NEEDED libGLESv2_real.so
libGLESv2.so     -> libGLESv2.so.2 (symlink)
libGLESv2_real.so -- actual libhybris GLES (soname patched to break cycle)
```

The real libhybris library is renamed to `libGLESv2_real.so` (with `patchelf --set-soname`)
to prevent circular DT_NEEDED resolution.

Built as part of `bash client/build-libhybris`. Source: `client/libgl-shim.c`, `client/libglesv2-shim.c`.

## libhybris fixes for Firefox

These live as patches in our libhybris fork (see `libhybris/TAWC_FORK.md`):

1. **`eglQueryDeviceStringEXT` / `eglQueryDisplayAttribEXT` stubs** —
   Firefox's `glxtest -w` null-checks these via `eglGetProcAddress` and
   bails ("EGL test failed" → `Feature::HW_COMPOSITING` ForceDisabled →
   software WebRender) when they're NULL. The stubs honestly return
   "no info" (NULL / EGL_FALSE) without advertising
   `EGL_EXT_device_query`.
2. **Shared per-display `wl_event_queue`** — Firefox/WebRender creates
   two `wl_egl_window`s on different threads. With per-window queues
   (upstream behaviour) the main thread's `wl_buffer.release` events
   sit on a queue nobody dispatches, deadlocking the Renderer thread's
   dequeueBuffer.
3. **Attach + commit inside `queueBuffer`** — Adreno's WebRender path
   pushes frames through `queueBuffer` from a driver-internal thread
   that never calls `eglSwapBuffers`, so upstream libhybris's
   `finishSwap`-driven attach never fires. We drain the queue inline
   from `queueBuffer` so the submission path doesn't depend on
   `eglSwapBuffers`.

## Known Issues

- All Firefox sandboxing disabled. The chroot doesn't support clone/namespace operations.
- `setenforce 0` required (GDK's memfds bypass the LD_PRELOAD SELinux shim).
- If Firefox connects but shows a black screen, check SELinux: `adb shell su -c getenforce`
  (resets to Enforcing on every device reboot).

## Killing and Restarting

```bash
adb shell su -c "killall firefox"
```

The wayland flush shim (`libwayland-flush-shim.so`) is no longer needed.
