## Intermittent SIGSEGV in glGenTextures when GTK3 is loaded via libhybris

### Summary

Hardware-rendered GTK3 Wayland apps intermittently (~15-20%) segfault at `glGenTextures` inside the WSI layer's buffer allocation. The crash requires all three conditions simultaneously:
1. GTK3 loaded (dozens of transitive library deps consuming glibc TLS slots)
2. EGL init through the tawc-egl WSI wrapper
3. A Wayland connection to the **real** compositor (dummy server doesn't trigger it)

Without any one of these three conditions: 0% crash rate.

### Crash site

Inside `alloc_buffer()` in `client/tawc-wsi/tawc-egl.c` (line 627):
```
ahb_allocate(...)             ← succeeds
pfn_getClientBuffer(...)      ← succeeds
pfn_createImage(...)          ← succeeds
glGenTextures(1, &buf->tex)   ← SIGSEGV
```

`eglMakeCurrent` returns success, `eglGetCurrentContext` returns a valid handle. The GL context is nominally current but the first GL dispatch crashes inside the Adreno driver.

### Root cause hypothesis

The WSI wrapper's `eglGetDisplay(wayland_display)` does `wl_display_get_registry` + `wl_display_dispatch_pending` **before** calling `do_init()` (which loads libhybris and runs the TLS patcher). With the real compositor, `dispatch_pending` processes actual protocol events (global announcements), which may trigger code paths in libwayland-client that affect the glibc TLS block layout. Combined with GTK's ~40 library deps that also consume TLS slots, this pushes the TLS layout into a state where the libhybris TLS patcher's computed offset is wrong.

### What's been ruled out

| Condition | Crashes? |
|-----------|----------|
| Direct libhybris EGL (no WSI wrapper) + GTK | No |
| WSI wrapper + EGL_DEFAULT_DISPLAY (no Wayland) + GTK | No |
| WSI wrapper + Wayland + no GTK | No |
| WSI wrapper + Wayland + GTK | **Yes ~15-20%** |
| WSI wrapper + dummy Wayland server + GTK | No |
| WSI wrapper + real compositor + GTK | **Yes ~15-20%** |
| Just linking `-lwayland-client -lwayland-egl` (unused) + GTK | No |
| Buffer size (64×64 vs 1080×2400) | No effect |
| GL resolution method (eglGetProcAddress vs dlsym) | No effect |
| Delay between process restarts | No effect |

### Reproducer

`gtk-crash/` contains a standalone reproducer (`repro.c`) and build script.

The reproducer uses `tawc_create_test_surface()` (exported from the WSI wrapper) to create a test surface that triggers `alloc_buffer` without needing compositor protocol exchange. The compositor must be running for the Wayland socket, but no protocol interaction occurs.

```bash
bash gtk-crash/build-and-test.sh [iterations]   # default 30
```

Or manually in chroot:
```bash
gcc -o repro repro.c -L/tmp/tawc-wsi -lEGL -lGLESv2 -lwayland-client -ldl -Wall -g
./repro          # 0% crash
./repro gtk      # ~15-20% crash
```

### Workaround

The integration test harness (`testing/integration/src/debug_app.rs`) retries app startup up to 3 times, detecting the crash via process exit after the READY signal.

### Fix direction

Likely in our libhybris fork's TLS patching code (`libhybris/hybris/common/hooks.c`). The bionic TLS bridge doesn't handle the TLS slot pressure from GTK3's library dependencies correctly when combined with Wayland protocol dispatch.
