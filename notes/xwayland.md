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
| `expat` | libexpat | R_2_6_4 |
| `libxext` | xorg/lib/libxext | libXext-1.3.6 |
| `libxfixes` | xorg/lib/libxfixes | libXfixes-6.0.1 |
| `libxxf86vm` | xorg/lib/libxxf86vm | libXxf86vm-1.1.6 |
| `libxrender` | xorg/lib/libxrender | libXrender-0.9.12 |
| `libxrandr` | xorg/lib/libxrandr | libXrandr-1.5.4 |
| `libxdamage` | xorg/lib/libxdamage | libXdamage-1.1.6 |
| `cutils-stub` | tawc-local | (header-only no-op for `<cutils/trace.h>`) |
| `mesa` | mesa/mesa | mesa-25.1.6 (Zink-only, Vulkan-backed) |
| `xwayland` | xorg/xserver | xwayland-24.1.11 |

### Out-of-scope features (disabled at configure time)

- **DRI3 / present-pixmap.** OFF. DRI3 wants buffer passing via real
  dmabuf fds; we route X11 GL through GLAMOR + zwp_linux_dmabuf
  instead.
- **MIT-MAGIC-COOKIE auth.** Single-user on-device server; any client
  can use `:0` without auth.
- **XDMCP, secure-rpc, xselinux, xinerama, xres, xv, xshmfence-as-futex,
  xf86bigfont, listen_tcp, ipv6, systemd_notify.** Not needed.

`libdrm` is still pulled in because `xwayland-window.h` includes
`<xf86drm.h>` unconditionally even with `-Ddrm=false`. We ship the
small generic `libdrm.so` (no vendor drivers) for the few struct
definitions Xwayland needs at the type level.

### GLAMOR + GLX via Mesa-Zink

X11 GL clients (glxgears, anything Irrlicht/SDL2-on-X11) work
through GLAMOR backed by Mesa-Zink. The path:

```
X11 client ── glXCreateContext ──> Xwayland (GLAMOR)
                                       │
                                       ▼ libGL.so (Mesa)
                                       ▼ libgallium-*.so (Zink)
                                       ▼ libvulkan.so   (NDK loader)
                                       ▼ vendor Vulkan ICD (Adreno)
                                       ▼ GPU
```

Zink translates GL → SPIR-V → Vulkan, so the device's stock Vulkan
driver (Adreno on tested hardware) does the actual rendering — no
libhybris needed on the server side, no DRM render-node access, no
kernel changes. Mesa runs against bionic via the NDK toolchain, same
as Xwayland itself.

**Mesa build configuration** (see `stage_mesa` in
`client/build-xwayland-aarch64`):

| Option | Setting | Why |
| --- | --- | --- |
| `gallium-drivers` | `zink` only | Vulkan-translation driver. No llvmpipe / softpipe. |
| `vulkan-drivers` | (empty) | We use the system Vulkan loader, no Mesa ICD. |
| `platforms` | `x11,wayland` | Xwayland needs X11; wayland EGL is symmetry/future. |
| `glx` | `dri` | Modern Xwayland GLX; libGL ↔ DRI loader ↔ zink_dri.so. |
| `llvm` | `disabled` | Zink doesn't need it (only llvmpipe / RADV do). |
| `glvnd` | `disabled` | Self-contained `libGL.so`, no separate dispatch. |
| `osmesa` / `xmlconfig` | `false` / `disabled` | No software offscreen, no driconf. |
| `zstd` | `disabled` | Disk-cache compression we don't need. |
| `egl-native-platform` | `x11` | Matches Xwayland's X server expectation. |

**Mesa bionic-compat patches** (vendored under
`xwayland-patches/mesa/`, derived from termux-packages):

| Patch | What it does |
| --- | --- |
| `0000-disable-android-detection.patch` | Forces `DETECT_OS_ANDROID 0` so Mesa stays on its plain Linux/POSIX paths and doesn't pull AOSP `<cutils/trace.h>` / `<log/log.h>`. |
| `0002-fix-for-getprogname.patch` | Bionic's `getprogname()` shape vs glibc's `program_invocation_short_name`. |
| `0015-define-reallocarray.patch` | Bionic's `reallocarray` is in `<malloc.h>` not `<stdlib.h>`. |

**Resulting size** (unstripped): `libgallium-25.1.6.so` is ~17 MB —
that's where the bulk of Zink + the GL state tracker lives. The
rest (`libGL.so` 800 KB, `libEGL.so` 500 KB, `libGLESv2.so` 90 KB,
`libgbm.so` 24 KB, `zink_dri.so` 120 KB) sums to under 2 MB. Strip +
LTO would bring this down further; not done yet.

**Process isolation.** The compositor links bionic's vendor
`libEGL.so` / `libGLESv2.so` directly for its own Smithay rendering
path — that's the real Adreno GL via the system loader. Mesa's libs
ship under `<filesDir>/xwayland/lib/` and only enter the picture via
`LD_LIBRARY_PATH` when the compositor execs Xwayland; no chance of
them getting injected into the compositor's address space.

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
