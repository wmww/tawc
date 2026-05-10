# Xwayland support

Goal: run X11 clients (xeyes, xterm, GIMP, … anything still X-only)
inside the chroot, displayed via tawc's Wayland compositor through an
Xwayland server. Smithay supports the compositor side of XWayland out
of the box.

## Status at a glance

- **Phase 1 (bionic baseline) — done.** Bionic-cross-compiled
  Xwayland-24.1.11 shipped in the APK, spawned by the compositor; X
  clients connect to `:0` and render via `wl_shm`. No GLAMOR.
  Server-rendered pixmaps stay magenta-tinted on purpose.
- **Phase 2 step 1 — done (2026-04-29).** `-Dtawc=true` meson option
  + stub `hw/xwayland/xwayland-tawc.c` (returns NULL) + `TAWC-DRI`
  X11 extension scaffolding: protocol header
  (`Xext/tawcdriproto.h`), server-side stub
  (`Xext/tawc-dri.c` with `QueryVersion` reply + `PresentBuffer`
  no-op ack), registered via `mi/miinitext.c`. The Xwayland binary
  still builds and runs identically; the new extension is advertised
  on the wire and dispatches requests. Verification:
  `xwayland::test_tawc_dri_extension_round_trip` exercises
  `QueryExtension` + `QueryVersion` + `PresentBuffer` from a chroot
  xcb client (`tests/apps/tawc-dri-test/`).
- **Phase 2 step 2 — done (2026-04-29).** Server-side AHB
  allocation + shipping over `android_wlegl`. New files:
  `hw/xwayland/xwayland-tawc-libnativewindow.{c,h}` — dlopen wrapper
  around `AHardwareBuffer_allocate` / `_describe` / `_lock` /
  `_unlock` / `_release` / `_getNativeHandle` /
  `_createFromHandle`. `hw/xwayland/xwayland-tawc.c` implements
  `xwl_tawc_buffer_alloc` + `_get_wl_buffer`, which send a buffer
  through `android_wlegl.create_handle` + `add_fd` × N +
  `create_buffer` — the same shape libhybris's wayland-platform
  plugin uses. The compositor reuses its existing `wlegl_import.c`
  path on the receive side. Compositor side: dropped `libhybris/lib`
  from the Xwayland process's `LD_LIBRARY_PATH` — libnativewindow.so
  DT_NEEDS libui.so, and the libhybris stub libui ahead of
  `/system/lib64` collapses the load with
  `library libc.so.6 not found`. Verification:
  `xwayland::test_xwayland_test_pattern_ahb_round_trip` enables the
  `debug.tawc.xwl_test_pattern` prop, restarts the compositor, and
  asserts the standalone test pattern path round-trips a 512x512
  AHB through `android_wlegl`.
- **Phase 2 step 3 — done (2026-04-29).** TAWC-DRI now ships real
  client AHBs end-to-end. Protocol bumped to v0.2: `xTAWCDRIPresentBufferReq`
  carries `numFds`/`numInts`/width/height/stride/format/usage on
  the wire and the inline `numInts` u32s in the request body, with
  `numFds` fds passed via X11's SCM_RIGHTS-style FD passing
  (`ReadFdFromClient` on the server, `xcb_send_request_with_fds` on
  the client). Server side: `Xext/tawc-dri.c` calls `SetReqFds(num_fds)`
  on entry, reads the inline ints + fds, and calls a new helper
  `xwl_tawc_present_native_handle()` (in `hw/xwayland/xwayland-tawc.c`)
  which rebuilds the AHB via `AHardwareBuffer_createFromHandle`
  (METHOD_REGISTER, taking ownership of the handle struct + fds),
  wraps it through the step-2 shipping helpers, and attaches the
  resulting `wl_buffer` directly to the X11 window's `wl_surface`
  with `wl_surface_attach`/`damage`/`commit`. Direct attach
  bypasses Xwayland's normal pixmap router; it's correct for pure
  AHB-presenting clients (Phase 4 GL) but means server-side X11
  drawing on the same window is incompatible — out of scope for
  Phase 2. Test client (`tests/apps/tawc-dri-test/tawc-dri-test.c`)
  upgraded to allocate an AHB via `hybris_dlopen("libnativewindow.so")`
  + `AHardwareBuffer_*` (libhybris-common.so loads the bionic lib
  from the glibc chroot), CPU-fills with a green→yellow gradient
  distinct from step-2's red→blue test pattern, ships via
  `xcb_send_request_with_fds`, and re-presents at 5 Hz during
  `TAWC_DRI_HOLD_SECS` so the compositor's per-X11 Activity
  SurfaceView (which races the X11 toplevel map) catches a
  commit after its render output is bound. End-to-end verified:
  the gradient renders on the compositor's TURQUOISE background,
  going AHB → TAWC-DRI → X server → android_wlegl → compositor
  gralloc1 import → GL texture; no SHM, no GLAMOR, no magenta.
  Verification: `xwayland::test_tawc_dri_ahb_present_round_trip` asserts
  compositor logcat shows `wlegl: create_buffer 320x240 ... fmt=1`
  (AHB, not SHM) AND `wlegl: imported ANativeWindowBuffer as texture
  320x240` (full GL bind). Stress verified by
  `xwayland::test_tawc_dri_ahb_present_animated_loop`: a double-buffered
  60fps loop (alternating between two AHBs, repainting each frame
  with a swept gradient phase) runs 120 frames in 2.0s, the
  compositor imports all 120 AHBs cleanly, and the client sustains
  ≥50fps (no server-side back-pressure). All step-2 + step-3 changes
  consolidated into `deps/xwayland-patches/xwayland/02-tawc-step3-ahb-present.patch`.
  `xwl_tawc_pixmap_get_wl_buffer(PixmapPtr)` is still a NULL stub —
  step-3 doesn't go through the pixmap router. Phase-3 GL or the
  full GLAMOR rerouting (if we ever do it) will fill it in.
- **Phase 2 step 4 — done (2026-05-01).** libhybris client-side
  EGL-on-X11 platform plugin
  (`hybris/egl/platforms/x11/{eglplatform_x11.cpp,x11_window.{h,cpp},
  tawc_dri_protocol.h,Makefile.am}` in our libhybris fork). Mirrors the
  existing `eglplatform_wayland` plugin: `x11ws_GetDisplay` wraps an
  Xlib `Display *` and extracts the underlying `xcb_connection_t` via
  `XGetXCBConnection`; `x11ws_eglInitialized` probes the `TAWC-DRI`
  extension (`xcb_query_extension` + the major opcode); `x11ws_CreateWindow`
  treats `EGLNativeWindowType` as an X11 `Window` XID and builds an
  `X11NativeWindow : EGLBaseNativeWindow`. The window's
  `dequeueBuffer` allocates AHBs through libhybris's existing AHB
  gralloc backend (same `hybris_gralloc_allocate` path
  `ClientWaylandBuffer` already uses), and `queueBuffer` ships the
  AHB's `native_handle_t` (numFds + numInts inline + fds out-of-band
  via `xcb_send_request_with_fds`) over the existing X11 connection
  using a hand-rolled TAWCDRIPresentBuffer wire definition that
  matches the server-side `Xext/tawcdriproto.h`. Three-buffer
  round-robin pool with no server→client release feedback (TAWC-DRI
  doesn't carry one today; the round-robin gives the compositor
  enough breathing room before slot-N is reused).
  Wired through `--enable-x11` in `hybris/configure.ac`,
  `EGL_PLATFORM_X11_KHR` dispatch in `hybris/egl/egl.c` (under
  `#ifdef WANT_X11`, parallel to the existing `WANT_WAYLAND`
  branch), and a new `x11` SUBDIRS entry in
  `hybris/egl/platforms/Makefile.am`. `scripts/build-libhybris.sh`
  generates stub SONAMEs for `libX11.so.6` / `libxcb.so.1` /
  `libX11-xcb.so.1` (chroot supplies real impls at runtime, same
  pattern as the existing wayland-client/server stubs) and synthesises
  pkg-config files pointing at the host's plain-C X11/xcb headers.
  Verification: `xwayland::test_eglx11_renders_via_ahb` runs a
  chroot-side EGL-on-X11 client (`tests/apps/eglx11-test/`) for 30
  frames; the test asserts `eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR)`
  succeeds, GL_VENDOR=Qualcomm, and the compositor logs both
  `wlegl: create_buffer 320x240 ... fmt=1` (AHB import via
  TAWC-DRI, not SHM) AND
  `wlegl: imported ANativeWindowBuffer as texture 320x240`
  (full GL bind). On OnePlus 9 (Adreno 660) the client picks up the
  vendor GLES driver via libhybris's TLS thunks and ships frames
  end-to-end with no CPU readback.
- **Phase 2 step 5 — done (2026-05-01).** Real-app shakedown via
  `es2gears_x11` (Arch's `mesa-demos`) — a stock GLES-on-X11 client
  (the classic 3-spinning-gears demo) renders end-to-end via our
  TAWC-DRI pipe at native frame rate. ~1500 buffer round-trips per
  second over an 8-second smoke run, zero AHB import failures, gears
  visibly rotate correctly with no magenta tint, no distortion, no
  artefacts. Two fixes were needed to land it:
  - **EGL_NATIVE_VISUAL_ID translation.** Standard EGL-X11 toolchains
    (es2gears_x11, glmark2, every glad/glut-style EGLUT init) call
    `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` then
    `XGetVisualInfo(VisualIDMask)` and refuse to continue if the
    visual lookup fails. Android EGL hands back HAL pixel-format
    constants there (e.g. `0x2` for `RGBA_8888`), which match no X11
    visual ID. Fix: a new optional ws_module hook
    `eglGetConfigAttrib` in libhybris (`hybris/egl/ws.{h,c}`,
    `hybris/egl/egl.c`); the X11 platform plugin caches the screen's
    default visual ID at GetDisplay and returns it for
    EGL_NATIVE_VISUAL_ID. Other platforms leave the hook NULL and
    behaviour is unchanged.
  - **Server-side wl_buffer.release listener.** The step-3
    `xwl_tawc_present_native_handle` deliberately leaked the per-
    present `xwl_tawc_buffer + AHB + wl_buffer` trio, on the basis
    that the verification client only did one-shot presents. For a
    real GL app doing thousands of frames, that pattern hits the
    compositor's `RLIMIT_NOFILE` within seconds (each AHB carries
    ≥1 fd, and `tawc_wlegl_import` opened a fresh GraphicBuffer per
    create_buffer). Fix: add a `wl_buffer.release` listener in
    `xwayland-tawc.c` that destroys the trio on release, bounding
    the live working set to the compositor's queue depth (~3) regardless
    of total frame count.
  - **Present-flip verification.** Instrumented
    `present/present_scmd.c::present_check_flip()` with an `ErrorF`
    on each early-return + the success path; over the full
    es2gears run zero `xwl_present_flip` lines fired. That
    *confirms by absence* what the step-3 architecture was designed
    to do: the libhybris EGL-X11 pipe bypasses Present entirely,
    going TAWCDRIPresentBuffer → `xwl_tawc_present_native_handle` →
    direct wl_surface attach. There is no flip-vs-copy choice
    because there is no Present in the path; "direct attach" is
    strictly faster and zero-copy by construction. Verification:
    `xwayland::test_es2gears_x11_renders_via_ahb` runs es2gears for 4s
    and asserts ≥100 AHB imports, zero `createFromHandle` failures,
    no `Xwayland disconnected: Protocol error`. mesa-demos added to
    `scripts/install-test-deps.sh`.

  **Wine-game shakedown remains future work** — left for when a
  specific Wine workload becomes interesting.
- **Phase 3 — probably skip.** Server-side EGL acceleration
  (GLAMOR-equivalent). Only matters for legacy XRender-heavy apps
  that nobody runs on a phone.

## Why bionic

We briefly went glibc (the V4 experiment) under the assumption that
everything had to share libhybris's libc. Reverting was the right
call once we worked through Phase 2 carefully:

- The libhybris EGL-on-X11 plugin lives on the **client** side, in
  the chroot (which is glibc Arch). The X server is a separate
  process; libcs don't need to match across an X protocol connection.
- The X server's role in Phase 2 is allocating AHBs and shipping them
  to the compositor via `android_wlegl`. AHBs come from
  `libnativewindow.so`, a **bionic** library. From a bionic Xwayland
  we can `dlopen("libnativewindow.so")` directly. From a glibc
  Xwayland we'd have to load libhybris *into* the X server itself to
  bridge — which is the V3-style "bionic + libhybris-in-server" path
  the original notes flagged as having "larger unknowns."
- Bionic Xwayland needs ~5 small source patches (`FIONREAD` declared,
  `link()` → `symlink()`, `/tmp` prefix swap, `OPEN_MAX` fallback,
  Android `passwd` shape, `setgid`/`setuid` drop). Glibc Xwayland
  needs zero source patches but seven binary patches against
  `libc.so.6` and `ld-linux-aarch64.so.1` to silence syscalls the
  Android app-zygote seccomp filter SIGSYSes (`set_robust_list`,
  `rseq`, `clone3`, `accept`). The source-patch route is more honest
  and better-bounded.
- Bionic Xwayland is built and proven (the 2026-04-28 V1 baseline
  rendered xclock end-to-end through `wl_shm`). The glibc swap was
  reverted on 2026-04-29 once the Phase 2 design clarified what the
  server actually has to do.

The "Glibc alternative (not used)" section near the bottom preserves
the V4 toolchain-swap, seccomp binary patches, and packaging so we
don't have to re-derive any of it if a future workload ever needs a
glibc X server inside the APK.

## Architecture: bionic-build Xwayland into the APK

We **cross-compile Xwayland and its X11/font/render dependencies for
aarch64 bionic** (NDK `aarch64-linux-android29-clang`) and ship them
in the APK as `jniLibs/<abi>/lib*.so` (binaries + DT_NEEDED libs) plus
the XKB data tree as a tarball asset. On first run the compositor
PATH-resolves `Xwayland` against `<filesDir>/xwayland/bin/`, where
Kotlin lays down symlinks pointing at the real binaries in
`applicationInfo.nativeLibraryDir`. The binary is then a direct child
of the Android app process, with no su / chroot crossing required.

Rejected alternative: launching glibc-Xwayland from inside the chroot
via `su -c chroot rootfs Xwayland`. Rejected because smithay's
`XWayland::spawn` hardcodes `Command::new("Xwayland")` and FD
inheritance through `su` is fragile. Bionic-build keeps everything on
the Android side: standard libc, standard `Command::new("Xwayland")`,
and the bind into the chroot is a one-way symlink of `/tmp/.X11-unix`
so X clients can find the socket.

## Build status

**Xwayland-24.1.11 binary builds cleanly for aarch64-linux-android29.**
~1.8 MB ELF, DT_NEEDED on `libpixman-1`, `libXfont2`,
`libwayland-client`, `libxcvt`, `libxshmfence`, `libmd`, `libXau`,
`libm`, `libc`. Configured with `-Dglamor=false`. All staged into
`build/xwayland-aarch64/install/`.

**End-to-end verified on device:** xclock connects to `:0`, maps a
window, renders into a wl_shm buffer (magenta-tinted per the SHM
fallback policy) on top of the compositor's gradient background.
Covered by `xwayland::test_xwayland_xclock_renders_via_shm`.

Build script: `scripts/build-xwayland.sh`. Pinned upstream tags
cloned into `./deps/xwayland-src/<lib>/`, patches in
`./deps/xwayland-patches/<lib>/` applied via `clone_pinned →
apply_patches`, cross-compiled with the NDK toolchain
(`aarch64-linux-android29-clang`), staged into
`build/xwayland-aarch64/install/{bin,lib,include,share}` which doubles
as the pkg-config sysroot for downstream stages.

### Patches (vendored in deps/xwayland-patches/, derived from termux-packages)

All patches are tiny (≤30 lines each) and cleanly forward-port from
termux's pinned versions to our slightly-newer upstream tags. The
`@TAWC_TMP_PREFIX@` placeholder is substituted at apply time to
`/data/data/me.phie.tawc/xtmp` (where the compositor mkdirs `.X11-unix/`,
`.X11-pipe/`, etc.).

| Lib | Patch | Reason |
| --- | --- | --- |
| `libx11` | inline `FIONREAD` constant | bionic's `<sys/ioctl.h>` doesn't transitively include `<asm-generic/ioctls.h>` |
| `libxau` | `link()` → `symlink()` | bionic + Android FS rejects cross-dir hard links to data dir |
| `libxcb` | socket dir prefix swap | Android can't write `/tmp` |
| `xorgproto` | Android `struct passwd` shape | bionic lacks `pw_class` |
| `xtrans` | socket dir prefix swap (×2) | Android can't write `/tmp` |
| `libxfont2` | `OPEN_MAX` fallback | bionic doesn't define `NOFILES_MAX` or `NOFILE` |
| `wayland` | XDG_RUNTIME_DIR fallback to `/tmp` prefix | defense-in-depth; libwayland-client fails ENOENT otherwise |
| `xwayland` | drop `setgid()`/`setuid()` from `Popen()` on `__ANDROID__` | Android's seccomp filter SIGKILLs those calls; we run as the app uid anyway |

### Stages built (in dep order)

| Stage | Upstream | Tag |
| --- | --- | --- |
| `xorg-macros` | xorg/util/macros | util-macros-1.20.2 |
| `xorgproto` | xorg/proto/xorgproto | xorgproto-2024.1 |
| `xtrans` | xorg/lib/libxtrans | xtrans-1.6.0 |
| `wayland-protocols` | wayland/wayland-protocols | 1.45 |
| `libxcvt` | xorg/lib/libxcvt | libxcvt-0.1.3 |
| `pixman` | pixman/pixman | pixman-0.46.4 |
| `libffi` | libffi/libffi | v3.5.2 |
| `wayland` | wayland/wayland | 1.25.0 |
| `libxau` | xorg/lib/libxau | libXau-1.0.12 |
| `xcb-proto` | xorg/proto/xcbproto | xcb-proto-1.17.0 |
| `libxcb` | xorg/lib/libxcb | libxcb-1.17.0 |
| `libx11` | xorg/lib/libx11 | libX11-1.8.13 |
| `libxkbfile` | xorg/lib/libxkbfile | libxkbfile-1.1.3 |
| `freetype` | freetype/freetype | VER-2-13-3 |
| `font-util` | xorg/font/util | font-util-1.4.1 |
| `libfontenc` | xorg/lib/libfontenc | libfontenc-1.1.8 |
| `libxfont2` | xorg/lib/libxfont | libXfont2-2.0.7 |
| `libdrm` | mesa/drm | libdrm-2.4.125 |
| `libxshmfence` | xorg/lib/libxshmfence | libxshmfence-1.3.3 |
| `libmd` | hadrons.org tarball | 1.1.0 |
| `xkeyboard-config` | xkeyboard-config | xkeyboard-config-2.45 |
| `xkbcomp` | xorg/app/xkbcomp | xkbcomp-1.4.7 |
| `xwayland` | xorg/xserver | xwayland-24.1.11 (`-Dglamor=false`) |

### Out-of-scope features (disabled at configure time)

- **GLAMOR / GLX / DRI3.** Disabled in the bionic baseline. See
  Phase 2 below for the AHB-passthrough plan that replaces them.
- **MIT-MAGIC-COOKIE auth.** Single-user on-device server; any client
  can use `:0` without auth.
- **XDMCP, secure-rpc, xselinux, xinerama, xres, xv, xshmfence-as-futex,
  xf86bigfont, listen_tcp, ipv6, systemd_notify.** Not needed.

`libdrm` is still pulled in because `xwayland-window.h` includes
`<xf86drm.h>` unconditionally even with `-Ddrm=false`. We ship the
small generic `libdrm.so` (no vendor drivers) for the few struct
definitions Xwayland needs at the type level.

## Buffer transport plan: AHB-everywhere, GBM nowhere

The original plan's "Phase 1 = GLAMOR + Phase 2 = EGL-on-X11" framing
chased a desktop-Linux abstraction (GBM render-nodes, dmabuf-fds,
`zwp_linux_dmabuf_v1`) that doesn't fit the device. After several
days exploring it, the cleaner shape — informed by what tawc already
does on the Wayland side — is:

> Every X-server-managed buffer that lives on the GPU is an
> AHardwareBuffer. Every GPU-resident buffer is shipped to the
> compositor via `android_wlegl.create_buffer_v2` (the same
> protocol the compositor already imports for Wayland-app
> clients). Server-software-rendered pixmaps stay in `wl_shm`,
> exactly as today, and the compositor's magenta-tint debug
> rendering on those buffers stays as a load-bearing diagnostic.
> No GBM, no `/dev/dri/render*`, no `zwp_linux_dmabuf_v1`, no
> dmabuf fds appear anywhere.

This re-shapes the work into two phases that don't depend on each
other for correctness, and a third we may never do.

### Phase 1 (done — bionic baseline). Server-software Xwayland.

- NDK `aarch64-linux-android29-clang` toolchain.
- Bionic compat patches above.
- Asset packaging + Kotlin extractor: binaries + .so deps ride in
  the APK as `jniLibs/<abi>/lib*.so`, the XKB data tree extracts
  from `assets/xwayland/share.tar` into `<filesDir>/xwayland/share/`,
  and `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` are symlinks into
  `nativeLibraryDir` (see "In-app packaging" below).
- **No further Xwayland source patches required for SHM clients.**
  Server-rendered pixmaps flow through `xwayland-shm.c` → `wl_shm`
  → compositor → magenta tint. xclock, xeyes, xterm, xwd, GIMP-2D,
  anything that pixman-renders works, and the diagnostic that "this
  came from the CPU" is preserved.

End-to-end verified on device: `xwayland::test_xwayland_xclock_renders_via_shm`.

### Phase 2 (next big chunk). EGL-on-X11 client GL through libhybris.

Goal: `eglGetDisplay(EGL_PLATFORM_X11_KHR, …)` from a chroot client
returns a working EGL display, GL/Vulkan rendering happens via the
vendor blob through libhybris, the rendered frames travel client→X-
server→compositor as AHBs and present at the speed of every other
GPU app on the device.

**Three components, all roughly independent:**

1. **`xwayland-tawc.c`** — new file in the X server, modeled on
   `xwayland-shm.c` (376 LoC) and `xwayland-glamor-gbm.c`'s
   buffer-shipping helpers. Owns:
   - Binding the `android_wlegl` Wayland global on the X server's
     wl_registry.
   - Allocating server-side AHBs by `dlopen("libnativewindow.so")`
     directly — bionic-native, no libhybris bridge needed in the
     server. (`AHardwareBuffer_allocate` / `_lock` / `_describe`,
     same calls `compositor/src/wlegl.rs` already uses on the
     compositor side.)
   - `xwl_tawc_pixmap_get_wl_buffer(PixmapPtr)`: builds a
     `wl_buffer` for an AHB-backed pixmap by sending it to the
     compositor over `android_wlegl.create_buffer_v2`. Buffer-
     release and buffer-destroy listeners.
2. **`Xext/tawc-dri.c`** + protocol XML — new file in the X server,
   modeled on `dri3/dri3_request.c`. Implements a `TAWC-DRI` X11
   protocol extension that's the AHB equivalent of DRI3:
   - `TAWCDRIPresentBuffer(window, ahb_handle, width, height,
     format)` — clients send an AHB handle (or our own opaque
     buffer-id) to the X server.
   - Server side wraps the AHB as a pixmap, hands off to
     `xwayland-tawc.c` for compositor delivery.
   - Why a tawc-specific extension and not stock DRI3: DRI3's wire
     format passes a dmabuf fd plus stride/format/modifier. AHB
     handles aren't dmabuf fds (the gralloc fd isn't publicly
     exposed); modifier semantics are made-up if we tried to fake
     one. A bespoke protocol speaks our actual buffer ABI honestly.
3. **libhybris `eglplatform_x11.cpp`** — new file in libhybris (the
   client-side glibc-built libhybris that already ships in the
   chroot), parallel to the existing `eglplatform_wayland.cpp`.
   Implements the X11-side EGL platform plugin: client calls
   `eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, Display *, …)`,
   plugin returns an EGL display rooted in the X connection, and on
   `eglSwapBuffers` allocates an AHB via
   `AHardwareBuffer_allocate`, renders into it via vendor GLES,
   sends to the X server via `TAWC-DRI`. Big-shape work but
   bounded.

**Plus small modifications to existing Xwayland files** (all
conditional on `-Dtawc=true`):

- `xwayland-pixmap.c:xwl_pixmap_get_wl_buffer` — extra branch
  that routes AHB-backed pixmaps through the tawc path before the
  existing GLAMOR/SHM split.
- `xwayland-screen.c:registry_global` — bind `android_wlegl` and
  stash on `xwl_screen->tawc_wlegl`.
- `xwayland-screen.h` / `xwayland-types.h` — new fields.
- `xwayland-glamor.c` — third configuration axis: `XWL_HAS_TAWC
  && !XWL_HAS_GLAMOR`. Most of the pixmap-flow code today is
  `#ifdef XWL_HAS_GLAMOR`-gated; we want a clean "no GLAMOR but
  yes Wayland-EGL-via-tawc" mode.
- `meson.build` — `-Dtawc=true` option, conditional sources.

### Phase 2 implementation order

The three components are *technically* independent (each compiles
without the others), but there's a clear order that de-risks the
plan fastest and gives a working end-to-end pipe at the earliest
possible step. Build in this order:

**1. `TAWC-DRI` protocol XML + minimal X server stub.** Define the
wire (request: `TAWCDRIPresentBuffer(window, buffer-id, w, h,
format)`; events: buffer-release). Implement the server side as a
no-op that just accepts and acks the request, plus a synthetic
client (test harness in `tests/`) that opens an X connection,
sends `TAWCDRIPresentBuffer` with a fabricated buffer-id, and
verifies the round-trip. **Verification:** integration test that
exercises the protocol with no real buffers and no GL anywhere.
This proves the extension wiring before any AHB code exists.

**2. `xwayland-tawc.c` against a server-allocated test AHB.**
**Done (2026-04-29).** Bound `android_wlegl` on the X server's
`wl_registry`, added a libnativewindow dlopen wrapper, implemented
`xwl_tawc_buffer_alloc` + `xwl_tawc_buffer_get_wl_buffer` (the
standalone helpers — the `xwl_tawc_pixmap_get_wl_buffer(PixmapPtr)`
overload is still a stub for step 3 to wire into the pixmap router).
Added `-tawc-test-pattern` argv (and `TAWC_XWL_TEST_PATTERN=1` env
fallback, since smithay's `XWayland::spawn` doesn't take argv
extras), exposed to compositor via `debug.tawc.xwl_test_pattern`
prop. **Verification (passing):** `xwayland::test_xwayland_test_pattern_ahb_round_trip`
sets the prop, restarts the compositor, and checks for `wlegl:
create_buffer 512x512 stride=... fmt=1 usage=0x...` in compositor
logcat. The on-screen attach (and the `present_check_flip()`
instrumentation hook) lives with step 3 — for step 2 we only
verified the protocol round-trip.

**3. Glue step 1 to step 2. Done (2026-04-29).** Took a more direct
shape than originally sketched: rather than route through
`xwl_pixmap_get_wl_buffer` (which would require turning the AHB
into a real X PixmapPtr and integrating with the X server's
window-pixmap flow), `xwl_tawc_present_native_handle()` attaches
the wl_buffer directly to the X11 window's wl_surface. That keeps
PRESENTBUFFER as a "shove this AHB onto this window" primitive —
no PixmapPtr involvement. The price is that server-side X11 drawing
on the same window won't see the AHB (no XGetImage / XCopyArea
integration), which is fine for pure-GL clients (Phase 4); mixed-
rendering clients would need the pixmap-router path
(`xwl_tawc_pixmap_get_wl_buffer`) revived if/when a real workload
needs it. **Verification:** `xwayland::test_tawc_dri_ahb_present_round_trip`
shows a chroot-side gradient AHB on the compositor; logs prove the
AHB went through android_wlegl and was bound as a GL texture. **At
this point everything except the libhybris EGL plugin is done and proven.**

**4. libhybris `eglplatform_x11.cpp`. Done (2026-05-01).** The biggest
single piece and the riskiest unknown went smoother than expected
because step 3 had de-risked everything downstream. New plugin under
`hybris/egl/platforms/x11/` modeled on `eglplatform_wayland.cpp` /
`wayland_window.{h,cpp}`. `x11ws_GetDisplay` accepts an Xlib
`Display *` and pulls the `xcb_connection_t` via `XGetXCBConnection()`;
`eglInitialized` probes TAWC-DRI; `CreateWindow` builds an
`X11NativeWindow : EGLBaseNativeWindow` whose `dequeueBuffer`
allocates AHBs via `hybris_gralloc_allocate` (the AHB gralloc
backend, same path `ClientWaylandBuffer` uses) and `queueBuffer`
sends a `TAWCDRIPresentBuffer` over the existing X11 connection via
`xcb_send_request_with_fds`. Wire definitions vendored as a
hand-rolled `tawc_dri_protocol.h` mirroring `Xext/tawcdriproto.h`
on the server side — small enough not to be worth pulling in xcbproto.
Build wired via `--enable-x11`, `EGL_PLATFORM_X11_KHR` dispatch in
`hybris/egl/egl.c`, X11/xcb stub `.so`s + synth pkg-config in
`scripts/build-libhybris.sh`. **Verification:**
`xwayland::test_eglx11_renders_via_ahb` runs a chroot-side EGL-on-X11
test program (`tests/apps/eglx11-test/`) for 30 frames and asserts the
compositor logged `wlegl: create_buffer ... fmt=1` AND
`wlegl: imported ANativeWindowBuffer as texture` for that surface
(AHB, not SHM, not magenta). On OnePlus 9 Adreno 660 the client
picks up the vendor GLES driver through libhybris's TLS thunks.
The `present_check_flip` instrumentation hook is still pending;
since the test only asserts the compositor sees AHBs (which is the
sufficient outcome for zero-CPU-readback), the explicit "did Present
pick flip mode" check is folded into step 5's real-app shakedown.

**5. Real-app shakedown.** `glmark2-x11`, then a Wine GL game
(picks up X subwindows / `XGetImage` edge cases). Land integration
tests for the GL-X11 path: an analog of
`xwayland::test_xwayland_xclock_renders_via_shm` that asserts an EGL-X11
client renders via AHB (not SHM, not magenta-tinted).

Why this order:
- Step 1 is cheap and proves the protocol-extension scaffolding
  before either of the substantive pieces commits to a buffer
  ABI.
- Step 2 proves the buffer-shipping plumbing without requiring
  client GL to exist. If any of the AHB-via-`android_wlegl`
  assumptions are wrong (sizing, sync, format negotiation), we
  find out without having written a single line of libhybris
  plugin.
- Step 3 closes the X server side completely. After this step the
  X server can carry an AHB end-to-end on demand; only the client
  side is missing.
- Step 4 is then the long pole, but with the entire downstream
  pipe instrumented and known-good. When the libhybris plugin
  doesn't render correctly, we know the bug is in the plugin —
  not in the protocol, not in the pixmap glue, not in the
  compositor.

What can land in parallel without blocking:
- The `TAWC-DRI` XML + protocol headers can be generated and
  vendored into both `deps/xwayland-src/xwayland/` and `libhybris/` early.
- The `meson.build` `-Dtawc=true` option and stub `xwayland-tawc.c`
  (returns NULL until step 2 fills it in) lands in step 1 so the
  Xwayland binary keeps building through the whole sequence.

### Phase 3 (probably never). Server-side GL acceleration.

i.e. the original "GLAMOR" idea — the X server holds an EGL context
and renders RENDER glyphs / cursor / Cairo ops onto AHB-backed
pixmaps server-side. The set of apps that benefit is small and
shrinking (xterm, GIMP-2.10's pre-GTK3 paths, KDE-3-era stuff). The
buffer-transport piece is already solved by `xwayland-tawc.c`; what
Phase 3 adds is "give the X server an EGL context with no rooted
window/device". libhybris doesn't currently expose that without a
`wl_display` or `ANativeWindow`, and the upstream-Linux idiom for
it (`EGL_PLATFORM_GBM`) doesn't apply.

If we ever need this, the cleanest path is to give Xwayland its
own bonus `wl_display` — Xwayland is already a Wayland client and
already has one — and use `EGL_PLATFORM_WAYLAND_KHR` against
libhybris-loaded-into-the-server to get an EGL display, then render
to FBOs on AHB-backed EGLImages. No GBM needed; just an extra chunk
in `xwayland-tawc.c` and a vtable rerouting in `xwayland-glamor.c`.
But: if Phase 2 lands and the modern-app set works at native
speed, Phase 3 has no addressable bottleneck.

### What this lets us delete from the plan

- All references to `EGL_PLATFORM_GBM_MESA`, `gbm_create_device`,
  `gbm_bo`, `EGL_LINUX_DMA_BUF_EXT`, `zwp_linux_dmabuf_v1`,
  `wl_drm`, `/dev/dri/render*`. None of those concepts apply.
- The "AHB-backed libgbm shim" idea — the GBM API was the wrong
  shape; we don't need to satisfy it.
- The "patch libhybris to alias EGL_PLATFORM_GBM_MESA" idea — same.
- The Mesa-Zink experiment. Already deleted.
- Indirect GLX, libGL stub, all GLX server-side discussion — never
  needed in either phase, and the no-desktop-GL policy makes it
  not worth shipping anyway.

## Why GLAMOR-off doesn't block GL-client zero-copy

The instinct is that "Xwayland with no GL of its own" can't carry GL
buffers without touching them. It can. The asymmetry to keep in mind:
GLAMOR is for the *server's own* drawing (XRender, server-side
`XCopyArea`, software-X-client acceleration, Cairo-over-X). It is not
on a GL client's swap path. A pure-GL X client renders into its own
buffer (in our case, an AHB allocated through the libhybris EGL-on-X11
plugin) and asks the server to *carry* that buffer to the screen. The
server doesn't need a GL context to carry.

The mechanism is `Present.PresentPixmap` in flip mode. Xwayland
implements it (`hw/xwayland/xwayland-present.c`) by calling
`xwl_pixmap_get_wl_buffer(pixmap)` and doing
`wl_surface.attach(wl_buffer); commit` on the toplevel's surface. With
`xwayland-tawc.c` returning an AHB-backed `wl_buffer` (shipped via
`android_wlegl`), that's the entire fast path: client → AHB → X server
holds it as opaque pixmap storage → wl_buffer attached to wl_surface →
compositor imports as GL texture. No `glReadPixels`, no pixman touch,
no GL context anywhere in the server.

### Where CPU readback could sneak in (and whether real workloads hit it)

| Path | Triggered by | Without GLAMOR | Real-world impact |
| --- | --- | --- | --- |
| Present **copy mode** (instead of flip) | partial damage with `PresentOptionCopy`, size/format mismatch, pixmap busy in another presentation | pixman copy → CPU | `eglSwapBuffers` defaults to full-window flip; almost never trips for GL apps |
| `XGetImage` / `XShmGetImage` / `XCopyArea` reading the buffer | client explicitly asks for pixels | server CPU-reads the AHB | Pure GL apps never do this. Wine does in narrow paths (screenshot, drag thumbnails) — eats one slow op, fast path unaffected |
| X subwindows composited over the GL drawable | child X windows in front of the GL surface | pixman composite → CPU | Pure GL apps don't have subwindows. Wine simulating Win32 child controls *can* — manifests as "slow when window has child widgets", not "GL broken" |
| Software cursor over the buffer | — | — | Non-issue: Xwayland delegates to compositor via `wl_pointer.set_cursor`. No buffer touch. |
| DMA fence / sync | every swap | `xshmfence` (futex-on-shm); works under bionic | None |

### The actual thing to verify on Phase 2

The real concern isn't *whether* the X server has a slow path (it
does, for the workloads above), it's whether **Present picks flip
mode reliably** for the libhybris-EGL-on-X11 buffers. Flip vs copy is
decided by `present_check_flip()` in
`Xext/present/present_scmd.c` (in `xserver`). That predicate checks:
window region == buffer region (full-window), depth/format match,
no `PresentOptionCopy`, no async-flip weirdness, pixmap not currently
in use elsewhere.

Instrument `present_check_flip` (one printf at each early-return) and
confirm the GL client's path goes flip. If it doesn't, fix the
buffer-allocation predicate in the libhybris plugin or
`xwayland-tawc.c` until it does. If it does flip, the swap path is
end-to-end zero-copy by construction — no further verification needed
beyond "compositor sees the AHB and binds it as a texture", which we
already do for Wayland clients. This is the explicit verification
hook for steps 2 and 4 of the implementation order above.

Recording it here so we don't re-derive the answer.

## X11 surface ↔ Android Activity assignment

X11 toplevels go through the same host-and-Activity model as Wayland
toplevels (see [multi-activity.md](multi-activity.md)) — every X11
toplevel gets its own Android task, except for child-style windows
that ride on a parent.

Policy in `pick_host_for_x11`, mirroring Wayland's
`assign_toplevel_to_host`:

- **Override-redirect popups** (menus, tooltips, dropdowns, splash
  screens) ride on the parent toplevel's host. These are
  client-controlled, briefly-mapped windows; spawning a recents card
  per dropdown would be ridiculous.
- **`transient_for` windows** (modal dialogs, file pickers, "About"
  boxes) likewise ride on the parent's host — same logic as
  Wayland's `toplevel.parent()` short-circuit.
- **Plain toplevels** (e.g. `xterm`, `xeyes`, `glxgears`, the main
  window of a Wine app) each mint a fresh `ActivityId` and trigger
  `spawn_activity_from_native(&host)` — the same reverse-JNI to
  `NativeBridge.spawnActivity` that Wayland's `assign_toplevel_to_host`
  uses. Kotlin opens `CompositorActivity` with
  `tawc://activity/<id>`, which registers as that host on creation.
- **`single_activity_mode`** (config flag, off by default) collapses
  everything onto the first existing host, matching the same flag's
  effect on Wayland toplevels.

Net effect: spawning 5 xterms from a chroot session gives 5 recents
cards, same as 5 Wayland-native terminals would. A right-click menu
inside one of those xterms doesn't spawn anything — it lives on the
xterm's task. xclock-style single-window apps still appear as one
recents card.

### The X11Surface ↔ wl_surface association race

xclock's first commit on its wl_surface can land *before* smithay's
`WL_SURFACE_SERIAL` handler binds the X11Surface to that wl_surface.
If we tried to look up "the X11Surface for this committing
wl_surface" inside the commit hook, the lookup would miss and we'd
record no host association. xclock then attaches no further buffer
for ~1s (xclock-update-1 cadence), and the renderer skips the
surface for that window — visible as "test passes (SHM import was
logged) but xclock never appears on screen" or "test sometimes
fails because the second commit doesn't fire before the test gives
up".

Fix: `associate_pending_x11_surfaces(state)` scans *all*
x11_surfaces every commit, picking up any whose wl_surface is now
set but whose host entry is missing. The same helper also runs
once per render frame from `import_shm_buffers`, so the gap is
closed even if no further commits fire after smithay's belated
binding.

## SELinux: app exec of bundled binaries

Modern Android (10+) denies `execute_no_trans` from `untrusted_app`
onto `app_data_file` — i.e. an app can extract a binary into its own
files dir and chmod it 0755, but `Command::new("Xwayland")` returns
EACCES when fork+exec actually fires:

```
avc: denied { execute_no_trans }
  for path="/data/data/me.phie.tawc/files/xwayland/bin/Xwayland"
  scontext=u:r:untrusted_app:...  tcontext=u:object_r:app_data_file:...
  tclass=file permissive=0
```

Fix: ship the binaries (and their `.so` deps) as `jniLibs/<abi>/lib*.so`
so the OS extracts them into `applicationInfo.nativeLibraryDir`. That
dir gets the **`apk_data_file`** SELinux type, which untrusted_app *is*
allowed to exec. Same trick `libproot.so` already uses. `Xwayland` →
`libxwayland.so`, `xkbcomp` → `libxkbcomp.so`, the `lib/lib*.so` deps
copy across as-is (the build emits flat versionless `.so` filenames,
no symlink chains to flatten).

`CompositorService.ensureXwaylandExtracted` lays down symlinks at
`<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` pointing at the real
binaries in `nativeLibraryDir`; the compositor's PATH lookup hits
the symlink and `execve(2)` follows it to the apk_data_file target,
where SELinux is happy. `LD_LIBRARY_PATH` is set to
`nativeLibraryDir` directly via the `TAWC_NATIVE_LIB_DIR` env var
that Kotlin exports before `nativeStartCompositor`. No `su`, no
`magiskpolicy --live`, works on rootless devices.

## Compositor-side wiring (done; on-disk in this branch)

- `compositor/src/xwayland.rs` — spawns Xwayland on event-loop start,
  wires `X11Wm` on `XWaylandEvent::Ready`, implements `XwmHandler` +
  `XWaylandShellHandler` directly on `TawcState` (which is the calloop
  data type — no wrapper struct).
- `xwayland` feature added to the smithay dep in
  `compositor/Cargo.toml`.
- `[patch.crates-io] smithay = { path = "../deps/smithay" }` pulls
  in the fork; the commit is pinned in `deps/deps.list`.
- `TawcState` gained `xwayland_shell_state`, `xwm`,
  `x11_surfaces: Vec<X11Surface>`, `x11_to_host: HashMap<…>`, and
  `xdisplay`. `CompositorHandler::client_compositor_state` now
  handles `XWaylandClientData` clients (anvil pattern).
- `render::collect_surface_draws`, `import_shm_buffers`, and
  `send_frame_callbacks` all walk `state.x11_surfaces` alongside
  `state.toplevels` so X11 windows render and tick alongside Wayland
  ones. AHB path doesn't touch them — Xwayland is software-only here.
- All three install methods surface `<tawc-data>/xtmp/.X11-unix` at
  `/tmp/.X11-unix` inside the rootfs so X clients find `:0` at the
  standard path: `ChrootMounter` does a real bind-mount; tawcroot and
  proot use an asymmetric fake-bindmount via their respective `-b
  src:dst` flag. `RootfsEnv` exports `DISPLAY=:0` and
  `SDL_VIDEODRIVER=wayland,x11` — without the SDL hint, SDL2 apps
  (supertuxkart, anything Irrlicht-based) silently pick X11 whenever
  `DISPLAY` is set, hit the GLAMOR-disabled X server, and die in
  `createWindow`. Wayland-first with X11 fallback keeps SDL apps on
  the libhybris/EGL path while leaving X11 reachable for the X-only
  clients that genuinely need it.
- `start_xwayland` sets `LD_LIBRARY_PATH=$TAWC_NATIVE_LIB_DIR` for the
  Xwayland process — **not** `$TAWC_NATIVE_LIB_DIR:<libhybris>/lib`.
  libhybris's `lib/libui.so.1.0.0` is a glibc-built stub for
  chroot-side use, and putting it ahead of `/system/lib64` makes the
  bionic linker pick it up when resolving `libnativewindow.so`'s
  `DT_NEEDED libui.so` (Phase 2 step 2 dlopens libnativewindow for
  AHB allocation). The libhybris stub then fails to find glibc's
  `libc.so.6` and the load collapses. Server-side AHB allocation
  needs only the bionic libnativewindow already in `/system/lib64`,
  so libhybris stays out of the X server's link path.

## Smithay fork patches

Committed to `tawc-patches` as `5842fc8d Add XWayland support for the
TAWC compositor` (separate commit from the older Android support one):

1. `src/xwayland/x11_sockets.rs` — opt-in
   `TAWC_XWL_RUNTIME_DIR` env var lets the compositor move
   `/tmp/.X{n}-lock` and `/tmp/.X11-unix/X{n}` off `/tmp` (which
   doesn't exist on Android). Defaults to `/tmp` so upstream
   behaviour is unchanged.
2. `src/xwayland/xserver.rs` — append `-ac` to the `Xwayland` argv.
   The compositor and Xwayland both run as the Android app uid, but
   X clients launched from the chroot run as root, and SO_PEERCRED
   inside Xwayland would otherwise reject those connections. There's
   no real privilege boundary either way — only the app's own
   clients can reach the abstract socket and the filesystem socket
   lives in the app's private data dir.

## In-app packaging

Gradle splits Xwayland's tree across two output paths:

1. `stageXwaylandJniLibs` copies
   `build/xwayland-aarch64/install/bin/{Xwayland,xkbcomp}` into
   `app/src/main/jniLibs/arm64-v8a/lib{xwayland,xkbcomp}.so`, plus
   each `lib/*.so` next to them. The OS extracts these to
   `applicationInfo.nativeLibraryDir` at install time, where they
   get the `apk_data_file` SELinux type that untrusted_app may
   exec — see "SELinux: app exec of bundled binaries" above.
2. `packXwaylandShare` tars the XKB data tree
   (`share/X11`, `share/xkeyboard-config-2`) into
   `app/src/main/assets/xwayland/share.tar`. We can't flatten this
   into jniLibs because Xwayland reads it via fopen at the baked-in
   `-Dxkb_dir` path and the files cross-reference each other by
   relative path inside the tree.

On first `CompositorService.onCreate` after install / app upgrade,
`ensureXwaylandExtracted` extracts `share.tar` into
`<filesDir>/xwayland/share/` and creates the
`<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` symlinks pointing at
the real binaries in `nativeLibraryDir`. The compositor's
`xwayland::start_xwayland` then sets `PATH` and `LD_LIBRARY_PATH` so
the smithay `Command::new("Xwayland")` lookup picks up our copy.

Aarch64-only — matching libhybris, since there's no point shipping
software-only Xwayland on the emulator without the GPU stack.

## Build script usage

```sh
# Build everything from a fresh clone (idempotent re-runs)
bash scripts/build-xwayland.sh

# Rebuild a single stage (after editing a stage_<name>() function)
bash scripts/build-xwayland.sh --only=libx11

# Wipe install + builddirs (forces fresh)
bash scripts/build-xwayland.sh --clean
```

Add new stages by appending a `stage_<name>()` function and listing
it at the bottom of the script. Each stage clones into
`./deps/xwayland-src/<name>/`, optionally applies patches from
`./deps/xwayland-patches/<name>/`, and installs into the shared `$PREFIX`.

## Project policies that constrain the plan

**No desktop GL.** Our devices don't support it, so X clients don't
get it. GLES via EGL is the only client GL. This matches
`gl-shims/libGL.so`'s existing stance (returns NULL for `glX*` so
probes fall through to EGL). It removes GLX, libGL-stub-extensions,
indirect-rendering, and Mesa from the plan entirely.

**No GBM, no DRI3-style dmabuf passing.** Stock Android phones don't
expose `/dev/dri/renderD*` or dmabuf fds for AHB. Every protocol
seam in the X-server stack that wants a dmabuf is replaced with
`android_wlegl` (server↔compositor) or our `TAWC-DRI` extension
(client↔server). Both are AHB-shaped end-to-end.

**Magenta tint stays.** The compositor's magenta tint on `wl_shm`
buffers is a debug diagnostic, not a styling bug. Server-software
pixmaps stay in `wl_shm` so the tint is visible exactly when the X
server CPU-rendered. xclock-with-software-Render appearing magenta
is *correct and informative* — it's the signal that a code path
hasn't been GPU-accelerated yet.

**Phase 1 = transport + plumbing. Phase 2 = client GL. Phase 3 =
optional server-side GL acceleration that we may never bother
with.** See "Buffer transport plan" above for the file-by-file
shape.

## Glibc alternative (not used, documented for the future)

Recording the V4 toolchain-swap so a future workload that genuinely
needs a glibc binary in the APK doesn't have to re-derive any of
this. **Not the path we're on.** The bionic substrate above is
simpler for Phase 2 (direct AHB API access in the server, no
seccomp binary patches) and that's what's currently shipping.

### Toolchain swap

Cross-compile against `aarch64-linux-gnu-gcc` (the same toolchain as
`scripts/build-libhybris.sh`), not the NDK. Set
`CFLAGS=-D_GNU_SOURCE` not `-DANDROID`. Drop the bionic-only patches
(`libxfont2/01-bionic-open-max`, `xorgproto/01-xos_r-android-passwd`,
the FIONREAD-inline form of the libx11 patch) and replace them with
a smaller libx11 patch that pulls `<sys/ioctl.h>` out from under
`#ifdef XTHREADS` so the include fires regardless of libc.

### Glibc sysroot in the APK

Bundle the cross toolchain's loader + libc alongside the binaries:

- `lib/glibc/ld-linux-aarch64.so.1`
- `lib/glibc/libc.so.6`
- `lib/glibc/libstdc++.so.6`
- `lib/glibc/libgcc_s.so.1`
- `lib/glibc/libpthread.so.0`, `libdl.so.2`, `libm.so.6`,
  `libresolv.so.2`, `librt.so.1`

Strip aggressively — `libstdc++.so.6` is ~20 MB unstripped. Total
glibc sysroot weight after `--strip-unneeded` was ~11 MB on top of
the otherwise-bionic-sized X11 dep tree.

### patchelf the binaries

The compositor exec's `Xwayland` directly — no chroot crossing, no
ld.so.conf tweaking. So set the ELF interpreter and rpath at build
time:

- `PT_INTERP` → `<install>/lib/glibc/ld-linux-aarch64.so.1`
- `DT_RUNPATH` → `<install>/lib:<install>/lib/glibc:<libhybris>/lib`

Apply the same to `bin/xkbcomp` (which Xwayland exec's at startup to
build its keymap) and to every `lib/*.so` that has a baked-in
RUNPATH from libtool/meson.

### Seccomp binary patches: the actual blocker

When the V4 toolchain swap first landed, `Xwayland` spawned by the
compositor (i.e. inheriting the Android app seccomp filter) died
within ~1 ms with `signal: 31 (SIGSYS)`.

How chroot and proot avoid this:

- The **chroot** path runs everything via Magisk `su`. Magisk's su
  daemon forks fresh from its own non-app context, so su's children
  never inherit the app filter. Pure escape hatch.
- **proot** runs as the app uid with the app filter intact, but
  attaches as a ptrace tracer to every tracee. Android's
  `untrusted_app` filter uses `SECCOMP_RET_TRAP` for the deprecated
  syscalls glibc still emits — `RET_TRAP` raises a *catchable*
  SIGSYS. Termux's proot fork ships a SIGSYS handler that, on each
  trap, reads the tracee's registers, rewrites the syscall number
  to its `*at`-equivalent, and resumes. See `notes/proot.md`.

The compositor's `Command::new("Xwayland")` is neither — it inherits
the app filter and runs glibc, no su, no ptrace tracer. So the trap
fires in glibc's early init and there's no handler to catch it,
and the child dies.

**Diagnosis.** `strace -f` couldn't catch the offender (the child
dies before any syscall can be intercepted; status 0x1f = WIFSIGNALED
SIGSYS). Bisection via raw-asm single-syscall test binaries
identified four offenders that glibc 2.43 emits during early init /
first thread setup / first X client accept, and that aren't in the
app-zygote BPF allowlist:

| syscall | nr | when |
| --- | --- | --- |
| `set_robust_list` | 99 | every `__libc_start_main` and `_Fork` |
| `rseq` | 293 | every `__libc_start_main` (and the dynamic loader) |
| `clone3` | 435 | first `pthread_create` |
| `accept` | 202 | every accept of an X11 client connection |

bionic doesn't issue any of those on the main thread, so the filter
never had to whitelist them.

**Fix.** Binary-patch the cross-toolchain's `libc.so.6` and
`ld-linux-aarch64.so.1` at build time, in `stage_glibc_sysroot`.
Driver was `client/build-xwayland-patch-glibc-seccomp.py` (deleted
along with V4 — re-derive from this section if needed).

| Patch | Replacement | Why |
| --- | --- | --- |
| `set_robust_list` `svc #0` (×2 in libc, ×1 in ld) | `mov x0, #0` | glibc ignores the return value; robust futexes work fine without registration as long as nothing tries to recover from a thread crashing while holding one (we don't). |
| `rseq` `svc #0` (×1 in libc, ×1 in ld) | `mov x0, #-ENOSYS` | engages glibc's "kernel doesn't support rseq" fallback path; restartable sequences are an optimisation, not a requirement. |
| `clone3` `svc #0` (×1 in libc) | `mov x0, #-ENOSYS` | glibc falls back to legacy `clone` for thread creation when clone3 returns ENOSYS. |
| `accept` entry: `mov x6, #0xca` | `mov x6, #0xf2` | retargets glibc's `accept()` libc wrapper at syscall 242 (`accept4`) instead of 202. The wrappers' arg layouts only differ by a `flags=0` that the wrapper already loads into x3. |

That's 7 four-byte patches across two ELFs. The patcher disassembles
both files via `aarch64-linux-gnu-objdump`, locates the three svc
sites by their preceding `mov x8, #imm`, locates the accept-entry
patch via the `--disassemble=accept` symbol view, and rewrites in
place. Re-running on a fresh extract is safe — assert expected bytes
before writing.

### Approaches considered and rejected for the seccomp problem

- **LD_PRELOAD shim.** Tried `accept` → `accept4` as an LD_PRELOAD
  shim first; it broke because Xwayland Popens the system shell
  (bionic) which inherited LD_LIBRARY_PATH/LD_PRELOAD and tried
  to load our glibc lib and the shim into the bionic linker, which
  errored on the version-script symbols. Binary-patching libc keeps
  the env clean for child processes.
- **Rebuild glibc.** Considered a glibc fork that strips the
  offending calls at the source level. The binary patch is two
  orders of magnitude smaller.
- **proot's approach (SIGSYS-handler in a tracer).** Fully general,
  handles every future deprecated syscall automatically. But
  requires Xwayland to be a ptrace tracee of a tawc-owned tracer
  process — extra moving part, extra performance cost (every syscall
  stops the tracee at signal-stop), and we'd need to bring the
  tracer up before exec. The four-instruction patch handled the
  actual offenders Xwayland 24.1 issued.
- **Pre-init SIGSYS handler in DT_NEEDED library.** Attempted (early
  on) — DT_NEEDED'd a tiny shim that runs in DT_INIT, installs a
  SIGSYS handler with raw `rt_sigaction`, and modifies
  `ucontext->uc_mcontext.regs[0] / .pc` to fake a return. Two
  reasons it lost out:

  * ld-linux runs *before* any DT_INIT processes, so its own startup
    syscalls (`set_robust_list`, `rseq`) still have to be statically
    patched.
  * Once ld-linux's two syscalls are patched into "lie to glibc
    about kernel support", glibc and the kernel disagree on whether
    those features are registered. For a `printf("hello"); exit;`
    binary the disagreement is invisible. For xkbcomp's keymap
    compile pass, glibc segfaults somewhere downstream of that
    state mismatch (and not via SIGSYS, so the handler doesn't
    even run).

  Patching `libc.so.6` directly with the same NOPs avoids the
  ld.so-vs-libc state divergence: libc never thinks the syscall
  happened.

The list of svcs to patch is empirical — if a future Xwayland code
path SIGSYSes on some other syscall, the fix is to add it to the
patcher script alongside the existing four.

## Alternatives considered and rejected

- **V2 (bionic + Mesa-Zink).** Built, blocked on dmabuf import:
  Xwayland's GLAMOR requires `wl_drm` or `zwp_linux_dmabuf_v1` v4
  from the compositor; bionic Android EGL has no
  `EGL_EXT_image_dma_buf_import` to import what Mesa-Zink would emit.
  Mesa is also ~17 MB unstripped. Build wiring deleted.
- **V3 (bionic + libhybris-loaded-into-server).** Loading libhybris
  into the X server itself for a server-side EGL context (the path
  Phase 3 would take if we ever do server-side GL). Larger unknowns
  than just dlopening `libnativewindow.so` for AHB allocation, which
  is all Phase 2 actually needs.
- **V4 (glibc Xwayland).** See the "Glibc alternative" section
  above. Built and verified end-to-end, then reverted in favour of
  bionic for Phase 2.
- **V5 (chroot Xwayland).** Rejected at the original Xwayland landing.
  Xwayland is logically compositor-side, not chroot-side; would break
  multi-/zero-chroot scenarios.
- **chroot-Mesa-as-X-client-libEGL.** Vanilla Arch Mesa expects
  `/dev/dri/renderDxxx` + an upstream DRM kernel driver matching the
  chip. Stock Android phones have neither (Adreno via `/dev/kgsl-3d0`,
  Mali via `/dev/mali0`, both closed). Mesa probe falls through to
  llvmpipe (software-only) without `gl-shims/` masking. Phase 2's
  X11-platform-in-libhybris is the actual answer to the same question.
- **libhybris-as-Mesa-DRI-driver.** A `libhybris_dri.so` satisfying
  Mesa's DRI loader interface but routing to libhybris GLES. Lindroid
  has explored. Substantial new code on a dense interface; nothing in
  it for our plan. Parked indefinitely.
- **Indirect GLX with libGL-stub.** Buys nothing the GLES-overlapping
  subset of EGL-on-X11 doesn't get faster, and the apps that genuinely
  need desktop-GL-only features can't be served by stubs anyway.
  Dropped as part of the no-desktop-GL policy above.
- **AHB-backed libgbm shim.** The original idea for plugging
  Xwayland's GLAMOR-via-GBM expectation into AHB-land. Reverted: GBM
  is an abstraction we don't need *anywhere* in the new plan, since
  `xwayland-tawc.c` ships AHBs to the compositor directly via
  `android_wlegl` and `TAWC-DRI` ships AHBs from clients to the
  X server directly. There is no protocol seam left that needs to
  speak GBM, so there's nothing to shim.
- **Patching libhybris to alias `EGL_PLATFORM_GBM_MESA` to
  `EGL_PLATFORM_ANDROID_KHR`.** Considered for getting Xwayland's
  GLAMOR-via-GBM init to work against libhybris. Same fate as the
  libgbm shim — there's no GBM seam left in the plan that needs
  this alias. If Phase 3 ever does want a server-side EGL context,
  the route is `EGL_PLATFORM_WAYLAND_KHR` against a wl_display the
  X server already has, not a GBM-platform alias.
- **Mesa-with-no-driver-just-for-libgbm.** Considered as a way
  to satisfy GBM API surface without a DRI render-node, using
  Mesa's softpipe allocator. Same fate: the new plan has no
  GBM seam to satisfy.
- **"GLAMOR with the buffer transport replaced under it"** —
  i.e. flip `-Dglamor=true`, ship a fake libgbm that satisfies
  link, replace `xwl_glamor_pixmap_get_wl_buffer` with our AHB
  variant. Plausible but the value calculus argues against it:
  Phase 2 (libhybris-EGL-on-X11) gives modern apps the path that
  actually matters; the apps GLAMOR would help are ones nobody
  runs on a phone. Reconsider only if a real bottleneck appears
  on a real workload.

## Refactor history

- **2026-04-28** — initial bionic scaffolding through libxcb (clean),
  hit libX11's bionic-`FIONREAD` issue. Adopted termux-packages'
  patch series and forward-ported. All stages incl.
  Xwayland-24.1.11 binary cross-compiled cleanly. Shipped to phone,
  hooked into compositor via `xwayland.rs` + smithay tawc-patches
  additions; xclock rendered end-to-end via SHM (verified with
  `xwayland::test_xwayland_xclock_renders_via_shm`).
- **2026-04-28** — V2 (bionic + Mesa-Zink) built but never shipped
  (dmabuf import gap). Source stayed in tree behind unflipped flags.
- **2026-04-28** — V4 baseline: switched the cross toolchain from NDK
  bionic to `aarch64-linux-gnu` glibc, dropped the V2 Mesa stages,
  added a `glibc-sysroot` stage and a `patchelf` stage. End-to-end
  re-verified on device with xclock through the V4 binary.
- **2026-04-28/29** — Seccomp diagnosis + binary patches:
  bisected glibc 2.43's startup syscalls against the app-zygote
  filter, found `set_robust_list`, `rseq`, `clone3`, and `accept`
  trapped, landed binary patches in
  `client/build-xwayland-patch-glibc-seccomp.py`. xclock works fully
  end-to-end through the compositor's `Command::new("Xwayland")`
  spawn.
- **2026-04-29** — Pre-init SIGSYS handler experiment:
  attempted to replace the libc-side static patches with a
  DT_NEEDED'd shim that installs a SIGSYS handler in DT_INIT.
  Mechanics work but ld-linux runs before any DT_INIT and the
  necessary ld.so patches leave glibc in a "thinks robust-list is
  registered, kernel disagrees" state that crashes xkbcomp. Reverted
  to all-binary-patch.
- **2026-04-29** — Plan re-shape from "GLAMOR-first" to
  "AHB-everywhere". After scoping the actual Xwayland source
  (xwayland-pixmap.c is a 2-way `glamor || shm` hardcoded branch,
  `xwayland-glamor-gbm.c` bakes in DRI3+`zwp_linux_dmabuf_v1` end
  to end), realised every protocol seam GLAMOR wants is the wrong
  shape for this device. Rebuilt the plan around AHB+`android_wlegl`
  end-to-end.
- **2026-04-29** — Reverted from V4 (glibc) back to bionic
  Xwayland. The libc-on-the-server doesn't actually have to match
  libhybris's libc (libhybris lives on the chroot/client side, not
  in the X server), and bionic gives us direct
  `dlopen("libnativewindow.so")` access for Phase 2's server-side
  AHB allocation — no libhybris-in-server bridge needed. Dropped the
  glibc sysroot, patchelf stage, and seccomp binary patcher; restored
  the bionic source patches. The V4 work is preserved in this doc
  ("Glibc alternative") in case we ever need a glibc binary in the
  APK for some other reason.
- **2026-04-29** — Phase 2 step 2 landed: server-side
  libnativewindow dlopen, AHB allocation, native-handle ship via
  `android_wlegl`, end-to-end round-trip verified by
  `xwayland::test_xwayland_test_pattern_ahb_round_trip`. Caught one
  load-order trap on first device run: `libnativewindow.so`
  DT_NEEDS `libui.so`, and libhybris's chroot-side `libui.so.1.0.0`
  stub was on the X server's `LD_LIBRARY_PATH`, so the bionic linker
  took it first and failed on glibc `libc.so.6`. Fix: drop
  `libhybris/lib` from the X server's library path entirely. Step-1
  patch `02-tawc-extension-step1.patch` was rolled into a single
  consolidated `02-tawc-step2-buffer-shipping.patch` since
  `xwayland-tawc.{c,h}` evolved between the steps.
- **2026-04-29** — Phase 2 step 3 landed: TAWC-DRI v0.2 ships a
  real native_handle (numFds + numInts + inline ints + FD-passed
  fds) over the X11 wire. Server reconstructs the AHB via
  `AHardwareBuffer_createFromHandle` (METHOD_REGISTER) and attaches
  the resulting wl_buffer directly to the X11 window's wl_surface.
  Hit three landmines on the way:
  (1) X server SIGABRTed (Scudo abort) in the dispatch — caused by
      `free(handle)` after AHardwareBuffer_createFromHandle in
      METHOD_REGISTER, which transfers ownership of the handle struct
      to the AHB (the compositor's wlegl_import.c flagged this with a
      comment; replicated the convention).
  (2) libxtrans dropped the SCM_RIGHTS-attached fds because the
      dispatch didn't `SetReqFds(num_fds)` up front, same shape as
      DRI3's pixmap_from_buffers.
  (3) Test client's chroot dlopen couldn't find libnativewindow.so
      (it's a bionic-only lib); used `hybris_dlopen` from
      libhybris-common.so to load it through libhybris's bionic
      loader bridge. `xcb_send_request_with_fds` mutates the iovec,
      so re-presenting in a loop required a fresh iovec per call.
  After that, the green→yellow gradient renders on screen end-to-end:
  client AHB → TAWC-DRI → X server → android_wlegl → compositor
  gralloc1 import → GL texture, no SHM, no magenta tint.
  Patch consolidated into `02-tawc-step3-ahb-present.patch`.
- **2026-05-01** — Phase 2 step 4 landed: libhybris client-side
  EGL-on-X11 platform plugin
  (`hybris/egl/platforms/x11/{eglplatform_x11.cpp,x11_window.{h,cpp},
  tawc_dri_protocol.h}` + `Makefile.am`). Mirrors the wayland
  plugin's shape; `dequeueBuffer` allocates AHBs through the
  existing AHB gralloc backend, `queueBuffer` ships them via
  `xcb_send_request_with_fds(TAWCDRIPresentBuffer, …)` over the
  client's existing X11 connection. Wired through `--enable-x11`
  in `configure.ac`, `EGL_PLATFORM_X11_KHR` dispatch in
  `hybris/egl/egl.c`, X11/xcb stub libs and synth pkg-config in
  `scripts/build-libhybris.sh`. Verification:
  `xwayland::test_eglx11_renders_via_ahb` runs the new chroot client
  (`tests/apps/eglx11-test/`) for 30 frames, asserts EGL_VENDOR=Android
  + GL_RENDERER=Adreno, and confirms the compositor logged the
  AHB import + GL bind for the surface. No source-code regressions
  needed in the existing TAWC-DRI / Xwayland tests; the X server
  side works unchanged.
- **2026-05-01** — Phase 2 step 5 landed: real-app shakedown via
  `es2gears_x11`. Two follow-on fixes shipped along the way.
  (1) The libhybris EGL plugin now translates `EGL_NATIVE_VISUAL_ID`:
  Android EGL returns HAL pixel-format constants there, but every
  EGL-X11 toolchain (es2gears, glmark2, EGLUT-style init code)
  refuses to start if `XGetVisualInfo(VisualIDMask)` doesn't find
  that ID. Added a new optional `eglGetConfigAttrib` ws_module hook
  in `hybris/egl/ws.{h,c}` + `hybris/egl/egl.c`; the X11 plugin
  caches the screen's default visual at GetDisplay and returns it
  for that attribute. Other platforms leave the hook NULL.
  (2) The Xwayland `xwl_tawc_present_native_handle` deliberately
  leaked the per-present `xwl_tawc_buffer + AHB + wl_buffer` trio
  ("step-3 verification client is one-shot, this is fine"). For a
  real GL app doing thousands of frames, that exhausts the
  compositor's `RLIMIT_NOFILE` within seconds — each AHB carries
  fds, and `tawc_wlegl_import` opens a fresh GraphicBuffer per
  create_buffer. Added a `wl_buffer.release` listener
  (`buffer_release_listener` in `xwayland-tawc.c`) that destroys
  the trio when the compositor releases its reference, bounding
  the live working set to the compositor queue depth regardless of
  total frame count. With both fixes: es2gears renders the
  classic 3-spinning-gears demo (red/green/blue, correctly shaded,
  no magenta) on the compositor's gradient background;
  ~1500 buffer round-trips per second sustained, zero
  createFromHandle failures.
  (3) Instrumented `present_check_flip()` in
  `present/present_scmd.c` with `ErrorF` printfs on every
  early-return + the success path. Captured 0 `xwl_present_flip`
  lines over the full es2gears run, confirming the libhybris
  EGL-X11 pipe never enters the Present extension by design — it
  shortcuts directly via TAWCDRIPresentBuffer → wl_surface attach.
  Verification: `xwayland::test_es2gears_x11_renders_via_ahb`
  asserts ≥100 AHB imports + 0 createFromHandle failures + 0
  Xwayland protocol-error disconnects over a 4s run.
