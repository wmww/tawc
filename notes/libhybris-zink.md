# libhybris + Zink backend

**Status:** implemented end-to-end as the `LIBHYBRIS_ZINK` graphics
backend (`libhybris-zink` wire key). All the in-tree plumbing works:
the patched Mesa loads, the env routing reaches Zink, libhybris's
Vulkan is dlopened. **Currently blocked on hardware** — every Adreno
device we have access to ships a Vulkan 1.1 vendor driver, and Zink
requires `VK_KHR_dynamic_rendering` (a Vulkan 1.3 promotion). Will
"just work" on devices whose vendor driver exposes Vulkan 1.3+; until
then, Zink fails to init at the physical-device feature gate and Mesa
silently falls back to llvmpipe. The integration test
(`tests/integration/tests/libhybris_zink.rs::test_eglinfo_reports_zink_renderer`)
is the tripwire — it'll flip to passing the day a device with newer
Adreno lands.

Hands the chroot the distro's own libglvnd-fronted libGL/libGLES, with
**our patched Mesa** (`deps/mesa-patches/mesa/06-tawc-zink-nokms.patch`)
shadowed in front via `LD_LIBRARY_PATH=/usr/lib/mesa-zink/`. Mesa picks
**Zink** as the only Gallium driver, and Zink dlopens libhybris's
`libvulkan.so.1` (via a sibling shim at `/usr/lib/hybris-vulkan-only/`)
to drive GL → SPIR-V → Vulkan → libhybris-Vulkan-WSI → AHB present.
The existing `Hybris` backend stays untouched as the GLES fast path.

## Why

`Hybris` is GLES-only; desktop-GL apps (kitty, alacritty, supertuxkart,
anything with `#version 140` shaders) fail to render. The `gl-shims/`
trick papers over linkage but can't make a GLES driver run desktop-GL
shaders. A retired plan (`desktop-gl-dispatch.md`, deleted 2026-07-06;
see "Relationship to the retired dispatcher plan" below) proposed a
per-context dispatcher in libhybris's libEGL — ~1k LOC of new C with a
long bug tail.

Zink translates desktop GL *and* GLES to Vulkan inside Mesa. If we
route that Vulkan back through libhybris-vulkan (which already works
end-to-end for native Vulkan apps), we get desktop GL with no new C
code — only a tiny Mesa patch + config. The cost is GLES regression:
GLES apps go through Zink instead of straight libhybris GLES, which is
single-digit % overhead on most workloads. That's the whole tradeoff.

## Stack

```
GL/GLES app
  └─ libGL.so.1 / libEGL.so.1 / libGLESv2.so.2  →  distro libglvnd-fronted
       └─ libEGL_mesa.so.0                       →  /usr/lib/mesa-zink/  (our patched Mesa)
            └─ Gallium → Zink → SPIR-V
                 └─ libvulkan.so.1               →  /usr/lib/hybris-vulkan-only/  (symlink → libhybris)
                      └─ libhybris's libvulkan
                           └─ android_dlopen → Adreno Vulkan
                                └─ vkQueuePresentKHR → libhybris WSI
                                     → android_wlegl AHB → wl_surface.attach
```

**No DRM, no dmabuf anywhere.** Mesa's normal GL Wayland WSI (which
wants dmabuf) is sidestepped via Mesa's **Kopper** path: `eglSwapBuffers`
becomes `vkQueuePresentKHR` on a libhybris-vulkan swapchain, so
libhybris's existing AHB-based WSI does presentation. Same WSI native
Vulkan apps use today.

## The Mesa patch

`deps/mesa-patches/mesa/06-tawc-zink-nokms.patch` (~20 lines in
`src/egl/drivers/dri2/platform_wayland.c`). What it does:

1. After `dri2_initialize_wayland_drm()` fails (which it always does on
   us — no `/dev/dri/`), if `Options.Zink` is set (which
   `MESA_LOADER_DRIVER_OVERRIDE=zink` makes happen), call
   `dri2_initialize_wayland_swrast()` instead of returning false. The
   swrast init already supports `driver_name = "zink"` and Kopper at
   line ~3210 — it's just unreachable through stock Mesa's dispatcher
   because `eglapi.c`'s retry logic resets `Options.Zink = false`
   before the last-resort swrast call.
2. Force `fd_render_gpu = -1` before the swrast call so Kopper takes
   the no-DRM path. Stock surfaceless's `surfaceless_probe_device_sw`
   does the same thing — without it, `dri2_create_screen` passes the
   calloc default of 0 to `driCreateNewScreen3` and Kopper does DRM
   ioctls on stdin.

That's the whole patch. Everything else Mesa needs to make Zink work
without `/dev/dri/` already exists in upstream — the `kopper_init_screen`
→ `pipe_loader_vk_probe_dri` → `zink_create_screen` chain handles
`screen->fd == -1` cleanly.

## What ships

- Our patched Mesa stack, cross-built by `scripts/build-mesa-gfxstream.sh`:
  - `libEGL_mesa.so.0` (the patched EGL vendor lib)
  - `libgallium-<ver>.so` (with Zink baked in)
  - `libgbm.so.1` (libEGL_mesa NEEDs it; never actually exercised on
    the Kopper path)
  - soname symlinks for all three
- Tarred into `assets/mesa-zink/<abi>/mesa-zink.tar` by Gradle's
  `packMesaZink<Abi>`, extracted to `<filesDir>/mesa-zink/` by
  `CompositorService.ensureMesaZinkExtracted`, laid into each rootfs at
  `/usr/lib/mesa-zink/` by `MesaZinkInstallProvider`.
- Mesa-with-Zink at runtime: distro Mesa is what's already in `/usr/lib/`,
  ours layers on top via `LD_LIBRARY_PATH`.

Always installed alongside libhybris and gfxstream-vk (~20 MB per
rootfs): same logic as the gfxstream provider — toggling
`GraphicsBackend` is an env change, not a manifest change.

## Build-time flag

`-PtawcGraphics=libhybris,gfxstream,cpu` (i.e. omit `libhybris-zink`)
disables this backend at build time. Effect:

- `scripts/build-mesa-gfxstream.sh` skips the Mesa-Zink configure +
  ninja step (still builds the gfxstream-vk Vulkan ICD). Saves the
  bulk of the Mesa cross-build (~3-5 min on a cold `scripts/build-app.sh`).
- Gradle's `packMesaZink<Abi>` becomes a `Delete` task that wipes the
  `app/src/main/assets/mesa-zink/<abi>/` dir — so flipping the flag
  back and forth doesn't leave a stale tarball in the source tree.
- `BuildConfig.GRAPHICS_LIBHYBRIS_ZINK_ENABLED = false`. Read at
  runtime by `me.phie.tawc.install.EnabledGraphicsBackends`. The
  Settings UI iterates `EnabledGraphicsBackends.enabled` (filtered
  view of `GraphicsBackend.entries`), so `LIBHYBRIS_ZINK` simply
  doesn't appear. `MesaZinkInstallProvider.entries()` short-circuits
  to an empty list (no install attempt, no misleading "asset missing"
  log line on every startup). `Settings.fromKeyOrDefault` falls back
  to `GraphicsBackend.DEFAULT` for any persisted pref that resolves
  to a disabled backend, so an APK downgrade from
  zink-enabled → zink-disabled doesn't strand the user on a backend
  that no longer exists.

Default (`-PtawcGraphics` unset) ships all four backends. The other
three (`libhybris`, `gfxstream`, `cpu`) always compile in regardless
of the flag — their footprint is negligible and the gate just hides
them from the Settings picker when they're omitted from the list.

## Env (`RootfsEnv.LIBHYBRIS_ZINK`)

- `LD_LIBRARY_PATH=/usr/lib/mesa-zink:/usr/lib/hybris-vulkan-only` —
  ours wins over distro Mesa AND libhybris's libEGL/libGLES.
- `MESA_LOADER_DRIVER_OVERRIDE=zink` — flips `Options.Zink=true` in
  Mesa's EGL init.
- `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json`
  — libhybris's `00_libhybris.json` vendor JSON would otherwise win
  libglvnd's lex-order scan and route libEGL to libhybris's GLES-only
  libEGL, bypassing Zink. Pin Mesa as the only vendor.
- No `HYBRIS_EGLPLATFORM` / `GDK_GL` — libhybris's libEGL never gets
  loaded on this path, and Zink supports desktop GL so GDK doesn't
  need pinning to GLES.

## Device findings (12 May 2026)

Tested on the two physical Adreno devices we have access to. Both fail
at the same gate.

| Device | OS | GPU | Vulkan apiVersion | driverVersion |
|---|---|---|---|---|
| OnePlus 9 | Android 14 | Adreno 660v2 | **1.1.128** | 0.530.0 |
| Pixel 4a | Android 16 | Adreno 618 | **1.1.128** | 0.490.0 |

Trace from the OnePlus (via temporary `fprintf(stderr, …)` prints):

```
init_wayland: try DRM (Zink=1)
dri2_create_screen: type=KOPPER fd=-1 driver=zink kopper=1 ...
zink: dlopen libvulkan.so.1 OK
zink: vk_GetInstanceProcAddr=0x… (libhybris)
zink: zink_create_instance returned 0x…              ← instance OK
zink: pdev=0x… type=1 (IntegratedGpu = Adreno 660)   ← physical device OK
zink: getting physical device info
ZINK: VK_KHR_dynamic_rendering required!             ← BLOCKED
zink: failed to detect features
dri2_create_screen: driCreateNewScreen3 returned NULL
init_wayland: swrast returned 0
```

Vulkan instance creates fine, Adreno is picked as the physical device,
and Zink rejects it for missing `VK_KHR_dynamic_rendering` — a
Vulkan 1.3 promotion. Android updates don't refresh GPU userspace
drivers (those are baked into the vendor partition at launch); Adreno
Vulkan 1.3 didn't really land until Pixel 8 / Snapdragon 8 Gen 2 era.

So:

- On Vulkan ≥ 1.3 Adreno (newer Pixel 8+, recent Snapdragon flagships,
  presumably newer Tensor Pixels): **this path should work as designed.**
  Untested for lack of hardware.
- On Vulkan 1.1/1.2 Adreno (every device we have): Zink declines to
  init. Mesa's silent llvmpipe fallback is gated by our env (we don't
  build llvmpipe into the staged Mesa-Zink tree, so the swrast
  fallback also fails). `eglInitialize` returns FALSE — GL apps fail
  to init at all, which is louder than llvmpipe but still a fail.

## Verification (when it works)

1. `vulkaninfo --summary` from a `LibhybrisZink` spawn — reports Adreno,
   apiVersion ≥ 1.3.
2. `eglinfo -B` — `OpenGL ES profile renderer:` mentions "Zink" and
   "Adreno". **If it says "llvmpipe", Zink failed to init.** Caught by
   `test_eglinfo_reports_zink_renderer`.
3. GTK4 demo renders correctly — GL via Zink, AHB on present.
4. Firefox launches with WebGL — realistic browser path.
5. kitty / alacritty / glxgears — desktop-GL apps the dispatcher plan
   was trying to enable.

## Selection in tests

Per-spawn via the broker `GRAPHICS` header:
`tawc-exec --in-rootfs <id> --graphics libhybris-zink …` or
`RootfsProcess::spawn_with(..., GraphicsBackend::LibhybrisZink)`.
`tests/integration/tests/libhybris_zink.rs` is a representative subset
of `libhybris::` rather than a full mirror — vulkaninfo (libhybris still
the Vulkan path), eglinfo (Zink and **not** llvmpipe), GTK4 (real GL
app landing as AHB via libhybris's Vulkan WSI).

## Risks

- **Zink Vulkan feature gap.** Already realised on every device we
  have. Mitigated only by hardware progress. Zink also wants sync2,
  descriptor indexing, etc. — on a 1.3-capable Adreno those likely all
  come along.
- **Mesa Kopper presence.** Kopper is upstream Mesa ≥23 with Zink.
  Our patch lives in 25.3.6 (pinned via `deps/deps.list`); bumping
  Mesa requires rebasing the patch against the new
  `platform_wayland.c` (which has been refactored every few releases).
- **GLES regression on a working device.** When Zink does come up,
  every GLES call goes through Zink → SPIR-V → Vulkan instead of
  direct libhybris GLES. Single-digit % typical, more on draw-call-
  bound workloads. The `LIBHYBRIS` backend stays available for
  fast-path GLES.
- **`gl-shims/` still ships** as part of the `LIBHYBRIS` backend's
  runtime. Only the `LIBHYBRIS_ZINK` branch in `RootfsEnv` excludes
  it from `LD_LIBRARY_PATH`. We delete `gl-shims/` outright only if
  `LIBHYBRIS_ZINK` replaces `LIBHYBRIS` as the default.
- **Possible tawcroot interaction with mesa init.** An observation
  from ~May 2026 (retired dispatcher plan): mesa-rendered kitty
  crashed inside `libtawcroot.so+0x3988` regardless of Zink/llvmpipe,
  suspected tawcroot syscall-handler issue in the mesa init path.
  Unverified since and tawcroot has changed a lot; re-check if kitty
  misbehaves once a Zink-capable device lands.

## Relationship to the retired dispatcher plan

`plans/desktop-gl-dispatch.md` (deleted 2026-07-06) proposed adding
desktop-GL **without** regressing GLES: a per-context dispatcher inside
libhybris's libEGL routing GLES contexts to today's libhybris code and
desktop-GL contexts to mesa+Zink-on-libhybris-vulkan. Retired because:

- This backend delivers the same capability via config alone, and the
  dispatcher's mesa backend is the same Zink path — it fails the same
  Vulkan 1.3 feature gate on every device we have.
- The desktop-GL gap on Vulkan 1.1 devices is now
  [plans/gl-on-gles-translator.md](../plans/gl-on-gles-translator.md).
- If Zink's GLES overhead ever proves unacceptable on 1.3+ hardware
  (still unmeasured — no capable device), the cheap mitigation is
  per-spawn backend selection (`LIBHYBRIS` for GLES apps, this backend
  for desktop-GL apps), not ~1k LOC of handle-wrapping C.

Durable design facts from the plan, should per-context routing ever
come back:

- EGL's API choice lives on the **context**, not the display — one
  `EGLDisplay` can host GLES and desktop-GL contexts simultaneously.
  Any GLES/GL split must therefore dispatch inside whatever library
  answers `libEGL.so.1`, tagging configs at `eglChooseConfig` and
  propagating the tag to contexts/surfaces. Wrapped handles for
  config/context/surface/image/sync; display shared unwrapped.
- libhybris has an unused glvnd vendor dir (`hybris/egl/glvnd/`).
  Activating it would register libhybris as a proper glvnd vendor and
  replace the `gl-shims/libGL.so.1 → libGLESv2.so.2` symlink and the
  `libgl-shim.c` GLX-NULL stubs.
- When a config request matches both backends, tie-break toward hybris
  — apps often blindly take `configs[0]`.
