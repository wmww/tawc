# Xwayland support

Goal: run X11 clients (xeyes, xterm, GIMP, … anything still X-only)
inside the chroot, displayed via tawc's Wayland compositor through an
Xwayland server. Smithay supports the compositor side of XWayland out
of the box.

## Architecture: bionic-build Xwayland into the APK

We **cross-compile Xwayland and its X11/font/render dependencies for
aarch64 bionic** and ship them in the APK as assets, extracted to
`/data/data/me.phie.tawc/files/xwayland/` on first run. The
compositor process exec's `Xwayland` from that directory — so the
binary is a direct child of the Android app process, with no
su / chroot crossing required.

Rejected alternative: launching glibc-Xwayland from inside the chroot
via `su -c chroot rootfs Xwayland`. Rejected because smithay's
`XWayland::spawn` hardcodes `Command::new("Xwayland")` and FD
inheritance through `su` is fragile. Bionic-build keeps everything on
the Android side: standard libc, standard `Command::new("Xwayland")`,
and the bind into the chroot is a one-way symlink of `/tmp/.X11-unix`
so X clients can find the socket.

## Build status (2026-04-28)

**Xwayland-24.1.11 binary builds cleanly for aarch64-linux-android29.**
1.8 MB ELF, DT_NEEDED on `libpixman-1`, `libXfont2`,
`libwayland-client`, `libxcvt`, `libxshmfence`, `libmd`, `libXau`,
`libm`, `libc`. All staged into `build/xwayland-aarch64/install/`.

Build script: `client/build-xwayland-aarch64`. Pinned upstream tags
cloned into `./xwayland-src/<lib>/`, patches in
`./xwayland-patches/<lib>/` applied via `clone_pinned →
apply_patches`, cross-compiled with the NDK toolchain
(`aarch64-linux-android29-clang`), staged into
`build/xwayland-aarch64/install/{bin,lib,include,share}` which doubles
as the pkg-config sysroot for downstream stages.

### Patches (vendored in xwayland-patches/, derived from termux-packages)

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
| `libepoxy` | anholt/libepoxy | 1.5.10 |
| `libgbm-shim` | tawc-local | (`client/gbm-android-shim/`) |
| `dri-pc-stub` | tawc-local (data-only) | — |
| `xwayland` | xorg/xserver | xwayland-24.1.11 |

### Out-of-scope features (disabled at configure time)

- **GLAMOR + GLX.** OFF for now. X clients get pixman / software 2D
  rendering, but **GL-via-X is unsupported** — anything that calls
  `glXChooseVisual` / `glXCreateContext` (glxgears, supertuxkart's
  Irrlicht backend, plenty of legacy GL games) fails. See "GLAMOR /
  GLX next steps" below for the in-progress plan; build-side deps
  (libepoxy, AHB-backed libgbm shim, dri.pc stub) are already wired
  so a future change can flip `-Dglamor=true` / `-Dglx=true`.
- **DRI3 / present-pixmap.** OFF. DRI3 wants a real DRM render node
  and dmabuf-fd buffer passing; AHB doesn't expose its dmabuf fd
  publicly so we can't fake one even with a libgbm shim.
- **MIT-MAGIC-COOKIE auth.** Single-user on-device server; any client
  can use `:0` without auth.
- **XDMCP, secure-rpc, xselinux, xinerama, xres, xv, xshmfence-as-futex,
  xf86bigfont, listen_tcp, ipv6, systemd_notify.** Not needed.

`libdrm` is still pulled in because `xwayland-window.h` includes
`<xf86drm.h>` unconditionally even with `-Ddrm=false`. We ship the
small generic `libdrm.so` (no vendor drivers) for the few struct
definitions Xwayland needs at the type level.

### GLAMOR / GLX next steps (in progress, build-side scaffolding done)

Goal: turn `-Dglamor=true` / `-Dglx=true` on so X11 GL clients work
(glxgears, supertuxkart-via-X11, anything Irrlicht/SDL2-on-X11). The
bionic-side vendor EGL gives us real Adreno GL — no libhybris needed
for the server side.

**Done in this branch:**

- `stage_libepoxy` — cross-compiles libepoxy 1.5.10. Meta-loader for
  GL/GLES/EGL via `dlsym`; resolves against bionic libEGL/libGLESv2
  at runtime.
- `stage_libgbm_shim` — tawc-local AHB-backed `libgbm.so` (sources in
  `client/gbm-android-shim/`). Each `gbm_bo` wraps an
  `AHardwareBuffer` so the bionic EGL driver can wrap it as an
  `EGLImage` via `EGL_ANDROID_get_native_client_buffer`. Limits:
  `gbm_bo_get_fd*` returns -1 (AHB doesn't expose its gralloc fd
  publicly), so the dmabuf-fd export path can't work as-is.
- `stage_dri_pc_stub` — stub `dri.pc` pointing at a non-existent
  `dridriverdir`; lets configure pass without us shipping any Mesa
  DRI drivers (modern Xwayland routes GLX through GLAMOR's
  `glamor_provider`, the legacy DRI loader is just a never-found
  fallback).

**Still missing before we can flip GLAMOR/GLX on:**

1. **`gl.pc` + Mesa libGL stub.** Xwayland's `glx/meson.build` needs
   `dependency('gl')`. The X server's GLX implementation calls a
   wide range of desktop-GL functions (`glClear`, `glDrawArrays`,
   `glAccum`, `glBegin`/`glEnd`, …) directly as ELF symbols, so a
   libepoxy meta-loader isn't enough — we need either a real Mesa
   libGL build for bionic or a hand-rolled libGL stub that forwards
   each entry point to GLES2/EGL via `dlsym`. The libGL stub is
   probably ~500 lines of mostly mechanical C.
2. **Mesa GL headers** — `<GL/gl.h>`, `<GL/glext.h>`,
   `<GL/internal/dri_interface.h>`. Public Khronos / Mesa headers,
   vendored as another stage.
3. **EGL platform substitution.** `xwayland-glamor-gbm.c:1732` calls
   `glamor_egl_get_display(EGL_PLATFORM_GBM_MESA, gbm)` — Android EGL
   doesn't have `EGL_PLATFORM_GBM_*`. Patch to fall back to
   `eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, …)`
   when the GBM platform isn't advertised. Also skip the
   `open(/dev/dri/renderDxxx)` step since we don't have one.
4. **Buffer transport to the compositor.** Xwayland sends GLAMOR
   output buffers via `zwp_linux_dmabuf_v1`. Android EGL doesn't
   advertise `EGL_EXT_image_dma_buf_import`, so the compositor side
   can't re-import. Two options:
     - (a) Patch Xwayland to use our `android_wlegl` protocol when
       the compositor advertises it (smaller protocol, AHB-native,
       matches what other tawc Wayland clients do).
     - (b) Extract dmabuf fd from `AHardwareBuffer` via private
       gralloc internals and ship via `zwp_linux_dmabuf_v1`. Hairy:
       AHB doesn't expose the fd, so this requires either AOSP
       internal-handle parsing or `AHardwareBuffer_sendHandleToUnixSocket`
       round-tripping through a socket pair.
   (a) is cleaner and reuses the AHB import path the compositor
   already has for native Wayland clients.

This is a multi-day sequel to the initial Xwayland landing. Most of
the work is items 3 + 4; item 1 (libGL stub) is mechanical but
tedious. **Until any of this lands, X11 GL clients have to use the
Wayland path** (`SDL_VIDEODRIVER=wayland,x11` for SDL apps, GTK/Qt
already prefer Wayland natively).

## Compositor-side wiring (done; on-disk in this branch)

- `compositor/src/xwayland.rs` — spawns Xwayland on event-loop start,
  wires `X11Wm` on `XWaylandEvent::Ready`, implements `XwmHandler` +
  `XWaylandShellHandler` on `TawcState` plus a forwarding impl on
  `LoopData` (calloop's data-type bound on `X11Wm::start_wm`).
- `xwayland` feature added to the smithay dep in
  `server/compositor/Cargo.toml`.
- `[patch.crates-io] smithay = { path = "../../smithay" }` so we
  pick up the local fork's pending patches without a github push.
  Switch back to `{ git = ..., branch = "tawc-patches" }` once the
  fork is pushed.
- `TawcState` gained `xwayland_shell_state`, `xwm`,
  `x11_surfaces: Vec<X11Surface>`, `x11_to_host: HashMap<…>`, and
  `xdisplay`. `CompositorHandler::client_compositor_state` now
  handles `XWaylandClientData` clients (anvil pattern).
- `render::collect_surface_draws`, `import_shm_buffers`, and
  `send_frame_callbacks` all walk `state.x11_surfaces` alongside
  `state.toplevels` so X11 windows render and tick alongside Wayland
  ones. AHB path doesn't touch them — Xwayland is software-only here.
- `ChrootMounter` bind-mounts `<tawc-data>/xtmp/.X11-unix` into the
  chroot at `/tmp/.X11-unix` so X clients inside the chroot find `:0`
  at the standard path. `01-tawc.sh` now also exports `DISPLAY=:0`
  and `SDL_VIDEODRIVER=wayland,x11` — without the SDL hint, SDL2
  apps (supertuxkart, anything Irrlicht-based) silently pick X11
  whenever `DISPLAY` is set, hit the GLAMOR-disabled X server, and
  die in `createWindow`. Wayland-first with X11 fallback keeps SDL
  apps on the libhybris/EGL path while leaving X11 reachable for
  the X-only clients that genuinely need it.

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

## In-app extraction

Gradle's `packXwayland` task tars the cross-compiled
`build/xwayland-aarch64/install/{bin/Xwayland,bin/xkbcomp,lib,share/X11,share/xkeyboard-config-2}`
into `assets/xwayland/arm64-v8a.tar` (~12 MB). On first
`CompositorService.onCreate` after install / app upgrade,
`ensureXwaylandExtracted` extracts that tarball into
`<filesDir>/xwayland/` preserving symlinks (matching the existing
`ensureLibhybrisExtracted` pattern: stage to `xwayland.new`, rename,
write `.version` stamp keyed on `longVersionCode`). The compositor's
`xwayland::start_xwayland` then sets `PATH` and `LD_LIBRARY_PATH` so
the smithay `Command::new("Xwayland")` lookup picks up our copy.

Aarch64-only — matching libhybris, since there's no point shipping
software-only Xwayland on the emulator without the GPU stack.

## Build script usage

```sh
# Build everything from a fresh clone (idempotent re-runs)
bash client/build-xwayland-aarch64

# Rebuild a single stage (after editing a stage_<name>() function)
bash client/build-xwayland-aarch64 --only=libx11

# Wipe install + builddirs (forces fresh)
bash client/build-xwayland-aarch64 --clean
```

Add new stages by appending a `stage_<name>()` function and listing
it at the bottom of the script. Each stage clones into
`./xwayland-src/<name>/`, optionally applies patches from
`./xwayland-patches/<name>/`, and installs into the shared `$PREFIX`.

## Refactor history

- **2026-04-28** — initial scaffolding through libxcb (clean), then
  hit libX11's bionic-`FIONREAD` issue. Adopted termux-packages' patch
  series (vendored to `xwayland-patches/`) and forward-ported to our
  pinned versions. All 21 stages incl. Xwayland-24.1.11 binary
  cross-compile cleanly. Shipped to phone, hooked into compositor
  via `xwayland.rs` + smithay tawc-patches additions; xclock renders
  end-to-end (verified with `apps::test_xwayland_xclock_renders_via_shm`).
  Setuid-in-Popen seccomp issue worked around with a tawc patch
  (`xwayland-patches/xwayland/01-bionic-no-setuid.patch`).
