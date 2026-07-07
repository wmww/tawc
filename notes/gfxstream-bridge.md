# gfxstream bridge: a libhybris-free GPU path

**Status:** experimental and not production-ready. Phase 4 is done, so
end-to-end Vulkan-via-AHB works:
`gfxstream::test_vkcube_renders_via_ahb` passes against the Adreno
660 — chroot's gfxstream-vk WSI allocates an AHB-backed ColorBuffer
via kumquat blob create + `VkImportColorBufferGOOGLE`, presents the
ColorBuffer id over the `tawc_gfxstream` Wayland protocol, and the
compositor imports the resolved AHB through the existing
android_wlegl texture path. The production/default physical-device GPU
path is still libhybris, which works on all tested devices. Phase 5
(real-world AVD validation) and Phase 6 (Zink-on-gfxstream-vk for
GL/GLES) still TODO; see
[`gfxstream-bridge-remaining-work.md`](../plans/gfxstream-bridge-remaining-work.md).

## Orientation for future readers

This doc has been rewritten a few times as our understanding evolved.
Some context that trips up first-time readers:

- **`alwaysBlob` is already enabled.** `deps/gfxstream-patches/gfxstream/01-android-host-build.patch`
  passes `-DGFXSTREAM_UNSTABLE_VULKAN_BLOB_COLOR_BUFFER=1` to the Android
  meson branch. Chroot's gfxstream-vk sees `mCaps.vulkanCapset.alwaysBlob = 1`,
  which is why our AHB-export hook fires for VkBuffer / VkImage memory
  allocations (they go through `RESOURCE_CREATE_BLOB` rather than the
  iovec-allocating `RESOURCE_CREATE_3D`). Don't try to flip it again.
- **The kumquat-over-Unix-socket transport is unusual for gfxstream.**
  Upstream gfxstream's two production paths are (a) virtio-gpu inside
  an AVF/Crostini VM with guest-kernel DRM mediation, and (b) the
  Android Emulator (host = developer's desktop). Neither is what we
  do. We are running gfxstream-vk over kumquat from a plain Linux
  chroot on Android. We compose tested primitives (kumquat blob
  create, `VkImportColorBufferGOOGLE`, AHB-backed ColorBuffers) but
  the *combination* is novel; nobody has built end-to-end "Linux
  chroot ships swapchain images out as opaque ids to a host
  compositor" before. The integration glue is on us; the primitives
  themselves work.
- **`VulkanNativeSwapchain` flag does not help.** It enables host-side
  `DisplayVk`/`CompositorVk`; the chroot-side `gfxstream_vk_wsi.cpp`
  still delegates to Mesa's `wsi_common`. There's no companion
  guest-side path that bypasses Mesa wsi. Don't try this again.
- **`GfxStreamVulkanMapper` is broken in our setup.** It assumes a
  second non-gfxstream Vulkan ICD in the chroot to import dmabuf fds.
  We only have gfxstream-vk → infinite-recursion. `04-kumquat-zero-vulkan-info.patch`
  routes around it. Don't undo that patch.
- **`compositor/src/dmabuf.rs` is gone.** The Mesa-wsi-shaped
  dmabuf-v1 stepping-stone path it implemented has been retired in
  favour of `compositor/src/gfxstream_present.rs` (the custom Vulkan
  WSI's compositor half — see "Compositor side — what landed May 2026"
  below). The AHB→dmabuf-fd export callback that backs kumquat's
  `RESOURCE_CREATE_BLOB` reply lives in `compositor/src/ahb_export.rs`
  now. `WleglBufferData::from_ahb` is also a keeper — both backends
  (libhybris/android_wlegl and gfxstream-bridge/tawc_gfxstream) attach
  it to the wl_buffer's user-data, so the renderer's lookup is one
  branch instead of two.
- **`VulkanAllocateHostVisibleAsUdmabuf` is force-disabled.**
  Gfxstream auto-enables this feature when the host kernel is ≥ 6.6
  *and* `/dev/udmabuf` exists, then `VkEmulation::create`
  unconditionally opens the device O_RDWR and calls
  `GFXSTREAM_FATAL("udmabuf failed to initialize")` if that fails.
  On Android the device exists on 6.6+ kernels (Pixel 10 Pro Fold and
  newer) but SELinux denies `untrusted_app` access to
  `udmabuf_device`, so the open fails and the FATAL would crash the
  whole tawc process when gfxstream initializes. We don't use the
  udmabuf path (our AHB-export hook handles host-visible memory), so
  `bridge.rs` pins `VulkanAllocateHostVisibleAsUdmabuf:disabled` in
  the renderer-features string passed through
  `STREAM_RENDERER_PARAM_RENDERER_FEATURES`. Watch for similar
  pattern in future gfxstream bumps — any new feature that
  auto-enables on a kernel/device-presence check and FATALs on
  init-fail can reproduce the same class of bug.
- **Kumquat binds early, gfxstream initializes on first connect.**
  `bridge::spawn()` still starts a thread and binds
  `kumquat-gpu-0` unconditionally when gfxstream is compiled in, so
  clients can connect without a broker handshake. The patched
  `KumquatBuilder::build()` does not construct `KumquatGpu`; the
  renderer is built only after the first accepted client. Non-gfxstream
  backends therefore pay no gfxstream init cost and avoid renderer
  startup crashes.

## The idea in one paragraph

Instead of loading Android vendor `.so`s into the chroot via libhybris,
forward GL/Vulkan command streams **out** of the chroot to a small
Android-side service that uses the system's native EGL/Vulkan in a
normal Android process. The chroot side is plain Mesa with a
GL/Vulkan-over-IPC backend; the Android side is a normal Android
process where the closed-source vendor blob lives in its native
habitat. Buffers are shared as dmabufs (or AHBs where gralloc-backed
allocation is required). No vendor code ever enters the chroot, so the
TLS / linker-namespace / CFI / inlined-`mrs` problem class is gone
wholesale.

This is the same architecture Chrome OS uses for Crostini: Linux apps
in an LXC, Mesa virtio-gpu driver, the host (Chrome OS) renders via
its real GPU stack. We want it in reverse: chroot is the "guest",
Android is the "host".

## Building blocks

**gfxstream** ([google/gfxstream](https://github.com/google/gfxstream))
is the obvious reuse target. It is the wire protocol the Android
emulator already uses to forward Vulkan/GL between the AVD guest and
the host machine. Both halves are open source:

- **Guest side** is upstream in Mesa as `gfxstream-vk` (and a GL
  driver). Builds for x86_64 and aarch64. Talks Vulkan/GL to a
  transport, normally virtio-gpu but pluggable.
- **Host side** is a renderer that decodes the protocol and runs the
  calls against a local GL/Vulkan implementation. An Android host
  backend already exists upstream — ships in AOSP's `com.android.virt`
  APEX for AVF (crosvm-on-Android). Uses EGL-on-EGL and
  `VK_USE_PLATFORM_ANDROID_KHR`. See "Host backend research" below.

**rutabaga / cross-domain context** (crosvm) is the other shape — same
goal, more general protocol, designed for the Sommelier model. Worth
re-evaluating at decision time; at time of writing gfxstream looked
like the shorter path because the protocol and the Mesa-side guest
already exist and Android-side EGL/Vulkan is the renderer's normal
target.

## The transport sharp edge

`virtio-gpu` is a kernel device — you can't fabricate one in
userspace. So we don't get to use Mesa's stock virtio-gpu Gallium
driver against a `/dev/dri/cardN` of our own making. The transport has
to be the gfxstream protocol over a Unix socket (or a memfd ring
buffer), not virtio-gpu kernel queues.

**Resolved:** Mesa's kumquat backend does exactly this — pure
userspace, Unix socket, no kernel driver. See "Transport research"
below for full details.

## Android-side bridge daemon

A normal Android service process linked against `libEGL.so` and
`libvulkan.so` (so the vendor blob loads against its proper bionic,
exactly how the Android system itself uses it). It accepts gfxstream
protocol on a Unix socket exposed into the chroot via the existing
broker mechanism (next to the Wayland socket), holds one EGL/Vulkan
context per chroot client, and translates command streams.

**Runs in-process** (compositor app, separate thread holding the GL
context). The vendor driver is the same one Android uses for every app
— if it's unstable enough to crash, the compositor experience is
degraded regardless. In-process avoids an IPC hop on buffer handoff
(exported AHB stays in the same address space) and simplifies
lifecycle. Can split to a sibling Service later if stability proves
to be a real problem.

SELinux: should be fine as long as the bridge runs as the compositor
app's uid in our app's domain (where EGL/Vulkan already work). No new
permissions, unlike libhybris-in-chroot which fights `runas_app`.

## Buffer sharing

Same question as today: how does a buffer the chroot rendered into
become something the compositor can sample as a GL texture?

Two paths, both feasible:

1. **Bridge allocates AHB on the chroot's behalf.** Chroot client asks
   for a swapchain image; bridge calls `AHardwareBuffer_allocate`,
   sends the dmabuf fd back to the chroot for the GPU to render into,
   and the AHB itself flows through the existing compositor import
   path (`AHardwareBuffer_createFromHandle` → `EGL_ANDROID_native_buffer`
   → GL texture). Reuses everything. Cross-process AHB import already
   works on stock firmware ≥ 12 (verified — see `notes/gpu-strategy.md`).

2. **Plain dmabufs + `linux-dmabuf-v1`.** Bridge uses gralloc/AHB
   internally to allocate, exports the dmabuf fd, sends it via Wayland
   `linux-dmabuf-v1`. Compositor imports via either AHB
   reconstruction or `EGL_EXT_image_dma_buf_import` (if the device's
   EGL exposes it — true on the AVD's gfxstream EGL, sometimes true
   on vendor stacks).

(1) is the safe default because it never leaves the AHB path that
already works. (2) is incrementally interesting if we want a future
where the compositor doesn't depend on Android EGL extensions, but
that's a separate fight.

**Update (May 2026):** the framing of both paths above was misleading.
(2)'s `EGL_EXT_image_dma_buf_import` doesn't work on Android vendor
EGL (confirmed in `notes/gpu-strategy.md` and `notes/xwayland.md`),
ruling out honest dmabuf-v1 client support. (1)'s "send the dmabuf
fd back to the chroot" is right for *host-visible* memory: the AHB's
`native_handle_t.data[0]` is the backing gralloc dmabuf and the
chroot mmaps it for CPU access. **Swapchain images don't need a
fd at all** — they're DEVICE_LOCAL, the chroot never reads or
writes them — so the WSI path is split off from the dmabuf-fd path
entirely. The chroot ships only a u32 `colorbuffer_id` over a
custom `tawc_gfxstream` Wayland protocol; the compositor (same
process as the gfxstream renderer) resolves the id to an AHB via
`tawc_gfxstream_lookup_ahb` (a one-liner C entry point in our
gfxstream patch). See "Custom Vulkan WSI" below.

## Cost vs libhybris

Lose: every GL/Vulkan call crosses an IPC boundary. Vulkan amortizes
this fine — command buffers are designed for exactly this batching,
which is why gfxstream went Vulkan-first. GL is more painful but
liveable; modern Mesa GL drivers already build command buffers
internally and flush in batches.

Gain:

- **No closed-source code in the chroot.** No TLS, no thunk patcher,
  no CFI bypass, no linker-namespace gymnastics, no per-Android-version
  bionic-slot whack-a-mole. Vendor blob runs where it expects to run.
- **x86 and ARM identical.** The one-architecture-only thunk patcher
  is the load-bearing constraint that keeps us aarch64-only today.
  This makes the AVD a first-class GPU target.
- **AVD parity unblocks dev loop.** Most graphics work currently has
  to happen on physical hardware; a working emulator path lets a lot
  more iteration happen on a laptop.
- **No vendor patching.** Today every device is tested against our
  libhybris fork's behaviour with that vendor's quirks. The bridge
  daemon uses the device's own EGL/Vulkan — whatever that vendor
  ships for Android apps already works, by definition, so we
  inherit their stack's quality rather than re-deriving it.

## Open questions to resolve before committing

1. **Does Mesa's `gfxstream-vk` still support a non-virtio transport
   in 2026?** **Yes — answered, see "Transport research" below.**
2. **Does an Android-targeting gfxstream host backend exist?**
   **Yes — answered, see "Host backend research" below.**
3. **GL performance under IPC.** Vulkan is fine, GL might not be.
   **Largely answered: route GL via Zink-on-gfxstream-vk rather than
   native gfxstream-GL — see "GL/GLES path: Zink, not native
   gfxstream-GL" below.** Zink rebuilds the per-call GL pattern into
   batched Vulkan command-buffer submits *before* the IPC boundary,
   which directly mitigates the per-call cost. A microbenchmark is
   still worthwhile before committing if any priority client is
   GL-heavy on legacy compat-profile features Zink doesn't cover
   gracefully.
4. **Does the `linux-dmabuf-v1` / AHB import path stay zero-copy
   when both sides go through gfxstream?** **Yes — answered, see
   "Zero-copy buffer sharing" below.**

## Transport research (May 2026)

**Answer to open question #1: yes, Mesa's gfxstream-vk has a
fully-supported, actively-maintained non-virtio transport called
"kumquat". It communicates over a Unix domain socket (SEQPACKET) and
is exactly the shape we need.**

### Architecture (two layers)

Mesa's gfxstream guest has a clean **platform abstraction**
(`src/gfxstream/guest/platform/`) with four backends:

| Backend | Path | Transport |
|---------|------|-----------|
| `drm` | `platform/drm/` | DRM ioctls on `/dev/dri/renderD*` (real virtio-gpu) |
| `kumquat` | `platform/kumquat/` | Unix socket (`AF_UNIX SEQPACKET`) |
| `fuchsia` | `platform/fuchsia/` | Fuchsia syscalls |
| `windows` | `platform/windows/` | Windows named pipes |

Selection: `createPlatformVirtGpuDevice()` in `platform/VirtGpu.cpp`
checks the env var **`VIRTGPU_KUMQUAT`**. If set (any value), it
calls `kumquatCreateVirtGpuDevice()`. Otherwise it calls
`osCreateVirtGpuDevice()` (the DRM path on Linux).

Build-time: `-Dvirtgpu_kumquat=true` in Mesa's meson options. This
pulls in a Rust library (`src/virtio/virtgpu_kumquat/`) compiled via
meson's Rust support, plus a C FFI wrapper
(`src/virtio/virtgpu_kumquat_ffi/`).

### Kumquat transport details

1. **Socket path:** `VirtGpuKumquatDevice` constructor (C++) builds
   the path `/tmp/kumquat-gpu-<descriptor>` (default
   `/tmp/kumquat-gpu-0`), then calls `virtgpu_kumquat_init()`.

2. **Rust core:** `VirtGpuKumquat::new(gpu_socket)` (in
   `virtgpu_kumquat.rs`) creates a `Tube` connected to that path.
   `Tube` (`src/util/rust/sys/linux/tube.rs`) is a thin wrapper
   around `AF_UNIX SEQPACKET` with `SCM_RIGHTS` fd-passing.

3. **Wire protocol:** `kumquat_gpu_protocol` — a custom
   request/response protocol (defined in
   `src/virtio/protocols/protocols/kumquat_gpu_protocol.rs`). Not
   virtio wire format, not vtest — purpose-built for kumquat. Messages
   are fixed-size structs; fds (for buffer handles, fences) travel via
   `SCM_RIGHTS` ancillary data. Commands include context create/destroy,
   resource create (3d + blob), transfer to/from host, command submit,
   capset queries, and snapshot save/restore.

4. **Buffer sharing:** resources created via kumquat come back with an
   fd (the `MesaHandle`) that can be mapped via `mmap`. For blob
   resources, the host allocates and sends the fd back; the guest maps
   it directly (shared memory). Fences are `eventfd`-based.

5. **No kernel driver needed.** The whole point of kumquat is to work
   without `/dev/dri/cardN`. The guest Mesa library talks directly to
   the kumquat server over the socket — pure userspace.

### What we'd need

1. **A kumquat server (host side).** The server must speak the
   `kumquat_gpu_protocol`, create a gfxstream rendering context (or
   rutabaga context), and dispatch commands to the real GPU. Google has
   a server implementation — it's used for testing and for
   non-virtio-gpu deployments — but it's not clearly upstreamed as a
   standalone binary. We'd need to either find/extract it, or write
   our own using `rutabaga_gfx` as the backend. The server is
   conceptually simple: accept socket, read kumquat commands, forward
   to rutabaga/gfxstream host renderer, send responses + fds back.

2. **Socket path customization.** The default `/tmp/kumquat-gpu-0`
   works fine for our use case — we just need to bind-mount or
   symlink the socket into the chroot. Or: trivially patch to read
   from an env var (one line in `VirtGpuKumquatDevice`).

3. **Build Mesa with `-Dvulkan-drivers=gfxstream
   -Dvirtgpu_kumquat=true`** for the chroot's aarch64 rootfs. The
   Rust toolchain is needed at Mesa build time.

4. **Set `VIRTGPU_KUMQUAT=1`** in the chroot environment. That's the
   only runtime config needed on the guest side.

### Connection to the AOSP gfxstream guest (what changed)

The AOSP gfxstream repo (`platform/hardware/google/gfxstream`) has
an older transport model with four `HostConnectionType` values
(`QEMU_PIPE`, `ADDRESS_SPACE`, `VIRTIO_GPU_PIPE`,
`VIRTIO_GPU_ADDRESS_SPACE`), selected via the Android property
`ro.boot.hardwares.gltransport` or `GFXSTREAM_TRANSPORT` env var.
None of these are Unix sockets — they all go through kernel devices
(goldfish pipe at `/dev/goldfish_pipe`, or virtio-gpu DRM).

Mesa's gfxstream-vk is a **separate, refactored codebase** that
shares protocol encoders with AOSP but has its own platform layer.
The kumquat backend exists only in Mesa, not in the AOSP guest tree.
This is fine — we'd build Mesa for the chroot anyway.

### Viability assessment

**This is viable with moderate effort.** The guest side is
off-the-shelf: build Mesa with the kumquat option, set one env var.
The server side is the real work, but the protocol is well-defined
and the rendering backend (rutabaga → gfxstream host renderer) is
proven. The Unix socket transport with fd-passing gives us zero-copy
buffer sharing for free (the host allocates an fd-backed buffer,
sends the fd to the guest, guest maps it — same physical pages).

## Host backend research (May 2026)

**Answer to open question #2: an Android host backend already exists
upstream, is production quality, and ships in AOSP as part of the
Android Virtualization Framework (AVF).** The "retargeting" work
originally described as "the bulk of the new Android-side work" is
already done.

### Where it lives

The gfxstream host renderer builds for Android via `host/Android.bp`.
The root `Android.bp` defines `gfxstream_host_cc_defaults` with
`-DVK_USE_PLATFORM_ANDROID_KHR` for Android targets. The comment in
that file is explicit:

> "host" in the name means the environment where VMM runs. For the
> Android Virtualization Framework case, this is Android.

`rutabaga_gfx` (in crosvm) links `libgfxstream_backend` and ships
inside the `com.android.virt` APEX. crosvm on Android uses this to
give VMs GPU acceleration — the exact architecture we want, minus the VM.

### GL backend: EGL-on-EGL

`host/gl/glestranslator/egl/egl_global_info.cpp` unconditionally
sets `sEgl2Egl = true` on Android. The EGL-on-EGL backend
(`egl_os_api_egl.cpp`) `dlopen`s the system's `libEGL.so` and
`libGLESv2.so`. No GLX, no WGL — it calls Android EGL natively.

### Vulkan backend: plain dlopen

`host/vulkan/vulkan_dispatch.cpp` loads `libvulkan.so` via
`SharedLibrary::open()` (which calls `dlopen`). On Android this
finds `/system/lib64/libvulkan.so`. The Android.bp enables
`VK_USE_PLATFORM_ANDROID_KHR` for proper surface handling.

### Native window

`host/native_sub_window_android.cpp` implements the display
abstraction using `ANativeWindow_acquire/setBuffersGeometry/release`.
We'd either pass a real Surface from our app or run headless and
extract frames via AHB export (Option A below).

### Init flags

`stream_renderer_init()` accepts a flags bitmask:
`STREAM_RENDERER_FLAGS_USE_EGL_BIT | USE_GLES_BIT | USE_VK_BIT`
gives a fully functional GL+Vulkan renderer calling Android's native
drivers.

### What's left

The host renderer already works on Android. The remaining work is:

1. **Transport**: wire the kumquat Unix-socket protocol into the
   `stream_renderer_*` API (which is transport-agnostic — rutabaga
   provides the transport layer in AVF).
2. **Build outside AOSP**: the host builds with Soong (`Android.bp`).
   We'd need to either port to CMake/NDK or use a Soong-based build
   step. The repo also has CMake support, but Android-target config
   may need work.
3. **Surface plumbing**: decide between passing a real `ANativeWindow*`
   or running headless with AHB export (see zero-copy section below).

## Zero-copy buffer sharing: what the code says (May 2026 research)

**Short answer: zero-copy is feasible, but gfxstream's host renderer
owns its buffers. Our daemon would need to either (a) export
gfxstream's internally-allocated AHBs to the compositor, or (b) add an
`AndroidAHB` external-memory import path so gfxstream renders into
compositor-allocated AHBs. Option (a) works today with no gfxstream
modifications; (b) would require moderate patching but has a template
in the QNX code path.**

### Buffer ownership model

gfxstream's host-side `VkEmulation` always allocates its own buffers.
`createVkColorBuffer()` creates a `VkImage`, calls
`allocExternalMemory()` to get a `VkDeviceMemory`, and binds them.
There is **no API to create a ColorBuffer backed by an
externally-provided VkImage or AHB**. The guest tells the host "I need
a color buffer with these dimensions and format", the host allocates
it, and hands back a handle.

### External memory export (the "gfxstream allocates, we import" path)

On Android, `allocExternalMemory()` sets `ExternalMemory::Mode::AndroidAHB`:
- `VkExportMemoryAllocateInfo` with
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID`
  goes into the `VkMemoryAllocateInfo` pNext chain.
- After allocation, `vkGetMemoryAndroidHardwareBufferANDROID()` exports
  the `VkDeviceMemory` as an `AHardwareBuffer*`.
- `exportColorBufferMemory()` dups and returns that AHB handle.
- `VkExternalMemoryImageCreateInfo` with the AHB handle type is always
  appended to `VkImageCreateInfo::pNext` when external memory is
  supported (either import or export).

**This means gfxstream-allocated ColorBuffers are already backed by
real AHBs on Android.** Our daemon could call `exportColorBufferMemory()`
after a guest `eglSwapBuffers` and hand the resulting AHB to the
compositor for zero-copy sampling.

### How the emulator displays rendered frames

The emulator's `PostWorker` borrows a ColorBuffer via
`borrowForComposition()`/`borrowForDisplay()`, which returns a
`BorrowedImageInfoVk` containing the raw `VkImage`, `VkImageView`,
layout state, and queue family ownership. `DisplayVk::post()` blits
this image into its own swapchain. The host owns the buffer
throughout; the guest never provides one.

### External buffer import (the "we allocate, gfxstream renders into
it" path)

There is **no built-in Android path** for importing an externally-
allocated AHB as the backing store of a gfxstream ColorBuffer. However,
the QNX port (`ExternalMemory::Mode::QnxScreenBuffer`) demonstrates
exactly this pattern:

1. `allocExternalMemory()` allocates a QNX screen buffer externally
2. Queries its Vulkan memory properties via
   `vkGetScreenBufferPropertiesQNX()`
3. Validates size and memory-type compatibility with the VkImage
4. Appends `VkImportScreenBufferInfoQNX` to the allocate chain
5. `vkAllocateMemory()` imports (not allocates) the external buffer

An `AndroidAHB` equivalent would:
1. Call `AHardwareBuffer_allocate()` with the right format/usage
2. Query via `vkGetAndroidHardwareBufferPropertiesANDROID()`
3. Validate size/memory-type compat
4. Append `VkImportAndroidHardwareBufferInfoANDROID` to the chain
5. `vkAllocateMemory()` imports the AHB

This is ~50 lines of new code in `allocExternalMemory()`, following the
QNX template line by line.

### Vulkan path: VK_ANDROID_external_memory_android_hardware_buffer

Fully supported. The `external_memory.cpp` `calculateMode()` function
selects `AndroidAHB` mode on `__ANDROID__` builds. The handle-type
transform functions in `vk_common_operations.cpp` map guest
`VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID`
to the host's AHB handle type (identity transform on Android). All the
Vulkan extension plumbing is already wired.

### Post/present interception

`PostWorker::post()` takes a `ColorBuffer*` and a completion callback.
`PostWorker::composeImpl()` borrows source and target ColorBuffers,
runs composition, and signals a future. The completion callback is a
natural interception point: instead of posting to a swapchain, our
daemon would signal the compositor that the AHB-backed ColorBuffer is
ready to sample.

### Recommended approach

**Option A (no gfxstream modifications):** Let gfxstream allocate
ColorBuffers normally (which produces AHBs on Android). After the
guest swaps, our daemon calls `exportColorBufferMemory()` to get the
AHB, then passes it to the compositor via the existing AHB import
path. The compositor imports it as a GL texture via
`EGL_ANDROID_image_native_buffer`. This is zero-copy: the guest
rendered directly into the AHB that the compositor samples.

**Option B (moderate gfxstream patch):** Our daemon pre-allocates AHBs
via `AHardwareBuffer_allocate()` and tells gfxstream to use them as
ColorBuffer backing. Requires adding an `AndroidAHB` case in
`allocExternalMemory()` following the QNX template. Advantage: the
compositor owns the buffer lifecycle and can triple-buffer without
coordinating with gfxstream's internal reference counting. Disadvantage:
~50-100 lines of gfxstream patching to maintain.

Option A is the right starting point. Option B is worth pursuing only
if buffer lifecycle coordination proves painful.

**Update (May 2026):** Option A is what we're doing. The initial
attempt routed AHBs through `zwp_linux_dmabuf_v1` (using Mesa wsi
unchanged) which got us through `vulkaninfo` but hit a wall on
swapchain image creation. The final design is a custom Vulkan WSI
that ships ColorBuffer ids over a custom Wayland protocol — see
§"Custom Vulkan WSI" below.

## Distro Mesa is half the story (May 2026)

We checked what Arch and Debian actually ship: **the gfxstream-vk
driver is there, but built without the kumquat backend.**

- **Arch / Arch ARM:** `vulkan-gfxstream` is a Mesa subpackage that
  ships `libvulkan_gfxstream.so` + the ICD JSON. `vulkan-drivers=...,
  gfxstream,...` is set. `virtgpu_kumquat` does not appear in the
  PKGBUILD ([Arch ARM
  PKGBUILD](https://github.com/archlinuxarm/PKGBUILDs/blob/master/extra/mesa/PKGBUILD),
  [Arch
  PKGBUILD](https://gitlab.archlinux.org/archlinux/packaging/packages/mesa/-/blob/main/PKGBUILD)).
- **Debian unstable:** `-Dgfxstream` enabled on 64-bit-with-LLVM
  archs. Same omission — no `-Dvirtgpu_kumquat` ([Debian
  rules](https://salsa.debian.org/xorg-team/lib/mesa/-/raw/debian-unstable/debian/rules)).

Without `-Dvirtgpu_kumquat=true` at Mesa build time the kumquat
backend isn't compiled in — `VIRTGPU_KUMQUAT=1` at runtime is a
no-op and gfxstream-vk falls through to the DRM/`/dev/dri/cardN`
path, which we don't have.

**Implication for Phase 1:** we don't rebuild Mesa wholesale. The
minimal thing is **ship our own `libvulkan_gfxstream.so`** (built
once, host-side, with `-Dvulkan-drivers=gfxstream
-Dvirtgpu_kumquat=true`) plus an ICD JSON in
`/usr/lib/gfxstream/` pointing at it. Vulkan's loader
picks it up via `VK_ICD_FILENAMES`; the rest of Mesa stays the
distro's. Same shape as the existing `LibhybrisInstallProvider`
asset overlay (APK asset → real-file copy at install time, host
kept clean).

Rust toolchain still required at build time (for
`virtgpu_kumquat_ffi`) — host-side, not in the rootfs. Add the
Mesa source and build options to `deps/deps.list` like every other
pinned dep.

### Faking `/dev/dri/cardN` in tawcroot was considered and rejected

Tempting, since tawcroot already does syscall interception. Doesn't
pencil out:

- DRM's UAPI is large (full virtio-gpu ioctl surface plus generic
  DRM bits — `RESOURCE_CREATE`, `EXECBUFFER`, `TRANSFER_*`, `MAP`,
  `WAIT`, `GET_CAPS`, `CONTEXT_INIT`, `PRIME_HANDLE_TO_FD`, …) and
  version-sensitive.
- `mmap(drm_fd, offset)` semantics are the killer — virtgpu blob
  resources are accessed by the kernel mapping host-shared pages.
  Faking that in userspace needs userfaultfd or hand-rolled
  AHB-dmabuf remapping with full lifecycle tracking. Massive
  scope-creep for tawcroot.
- Both wire formats (virtio-gpu DRM, kumquat) terminate at the same
  `stream_renderer_*` API on the Android side — host-side server
  work is identical either way. The only thing tawcroot-fake-DRM
  saves is a one-time cross-build of `libvulkan_gfxstream.so`.
- It only covers tawcroot installs (1/3 of methods); chroot/proot
  installs still need the .so.
- It fights gfxstream-vk's design — kumquat exists for exactly
  this "no kernel virtio-gpu available" case.

## GL/GLES path: Zink, not native gfxstream-GL

GL/GLES is covered by **Zink** (Mesa's GL→Vulkan translator), not by
shipping a separate gfxstream guest GL driver. The path becomes:

```
GL app → Zink → Vulkan calls → gfxstream-vk encoder → kumquat
       → Android-side bridge → Android Vulkan → vendor blob
```

Why Zink over native gfxstream-GL:

- **One driver, one transport, one host integration.** No second
  guest driver to build and pin, no second protocol surface in the
  bridge.
- **Mitigates the GL-over-IPC problem** (open question #3). GL is
  designed for per-call dispatch; Zink rebuilds that into Vulkan
  command-buffer submits, which is exactly the access pattern
  gfxstream's protocol amortizes well. So Zink isn't just GL
  fallback — it's the right answer to the IPC-cost question.
- **Already implemented on the libhybris side.** The `libhybris-zink`
  backend ([libhybris-zink.md](libhybris-zink.md)) is the same idea
  (Zink-on-libhybris-vulkan). Bridge inherits it for free.
- **Distros already ship Zink** as part of Mesa, no extra packaging.

Caveats: Zink's GL compat profile is good in 2026 but not 100% —
ancient fixed-function paths occasionally surprise. GLES2/3.x is
clean. GTK4, Qt6, Firefox, Chromium, modern games: fine. Truly old
X clients via Xwayland: usually fine, occasionally ugly. If a
priority workload exposes a Zink gap, gfxstream-GL stays available
as a fallback to revisit then — not as a default to maintain now.

Toggle in the rootfs is just `MESA_LOADER_DRIVER_OVERRIDE=zink` +
`__GLX_VENDOR_LIBRARY_NAME=mesa` in `RootfsEnv.kt`'s bridge
branch.

## Coexistence with libhybris

We are NOT dropping libhybris. The bridge ships alongside it and
the user picks per-launch which backend the chroot uses. Selection
is runtime, not install-time — the only thing that actually differs
between the two backends is which `.so` gets loaded, controlled by
env vars; reinstalling a multi-GB rootfs to flip it would be
overkill.

### Granularity

1. **Both backends ship in every install.** Build-time gating
   mirroring `-PtawcMethods=...` lets a release APK drop one for
   size if needed, but the default ships both.
2. **Global default via app setting** → drives env vars at every
   chroot entry. v0 UI: a radio selector on the home page (Hybris
   / Bridge), persisted via Android `SharedPreferences` (no
   datastore in the app today — first user-facing pref). Read by
   `RootfsEnv.build` on each entry.
3. **Per-app override (future, optional).** Power-user feature:
   `.desktop` `Exec=env TAWC_GPU=bridge firefox`. Cheap to add
   later once (2) exists; don't block v1 on it.

### Defaults

- **aarch64 physical:** `libhybris` (proven, lower latency, our
  investment).
- **x86_64 emulator:** `bridge` (libhybris doesn't work on AVD —
  see notes/emulator.md "libhybris on x86_64". This is the
  whole reason we're building the bridge.)
- **Eventually flip the physical default to `bridge`** once it's
  shipped on by default for a release on AVD and proven stable.

### Where the env toggle lives

`app/src/main/java/me/phie/tawc/install/RootfsEnv.kt`. Every method
(tawcroot/proot/chroot) funnels through `RootfsEnv.envArgv`, which
is applied via `/usr/bin/env -i KEY=VAL …` on the in-rootfs `bash
-lc`. Single source of truth, no on-disk profile.d state,
consistently applied across all entry paths.

(Old plan referenced `/etc/profile.d/01-tawc.sh`; that file no
longer exists — see `notes/installation.md` "Nothing under
/etc/profile.d/". Don't reintroduce it.)

Branch in `RootfsEnv.build` on a new enum read from the
SharedPreferences-backed setting:

```kotlin
enum class GpuBackend { HYBRIS, BRIDGE }

// inside build():
when (gpu) {
    GpuBackend.HYBRIS -> {
        put("LD_LIBRARY_PATH",
            "${LibhybrisInstallProvider.GUEST_GL_SHIMS_DIR}:${LibhybrisInstallProvider.GUEST_LIB_DIR}")
        put("HYBRIS_EGLPLATFORM", "wayland")
        put("HYBRIS_VULKANPLATFORM", "wayland")
        put("GDK_GL", "gles:always")
    }
    GpuBackend.BRIDGE -> {
        put("VK_ICD_FILENAMES", BridgeInstallProvider.GUEST_ICD_PATH)
        put("VIRTGPU_KUMQUAT", "1")
        put("MESA_LOADER_DRIVER_OVERRIDE", "zink")
        put("__GLX_VENDOR_LIBRARY_NAME", "mesa")
        // no LD_LIBRARY_PATH override — distro Mesa is the GL/EGL stack
    }
}
```

Both backends' assets live alongside each other in the rootfs:

- libhybris: `/usr/lib/hybris/` (laid down by `TawcInstaller` /
  `LibhybrisInstallProvider`, untouched by this work).
- bridge: `/usr/lib/gfxstream/libvulkan_gfxstream.so` +
  `/usr/lib/gfxstream/gfxstream_vk_icd.<arch>.json` (new
  `BridgeInstallProvider`, same shape as `LibhybrisInstallProvider`).

The compositor's kumquat server thread is cheap to leave always-on
(or start lazily on first connection); it doesn't conflict with
`android_wlegl` since each Wayland client picks at most one of "use
android_wlegl" (libhybris path) or "use kumquat for buffers"
(bridge path) by virtue of which Vulkan/EGL library it loaded.

### Current packaging split

The bridge ships only the kumquat-enabled `libvulkan_gfxstream.so`
and ICD JSON into the rootfs. The rest of Mesa stays the distro's.
Libhybris stays installed too; backend selection is per launch.

## Build status (May 2026)

**End-to-end Vulkan from chroot to vendor blob via gfxstream is
working on a physical device, and the build pipeline is in place for
x86_64 emulator parity.** `vulkaninfo --summary` from inside the
chroot enumerates a `Virtio-GPU GFXStream (Adreno (TM) 660)` device
whose `driverID = DRIVER_ID_QUALCOMM_PROPRIETARY` confirms the host-
side renderer reaches the real Adreno blob.

Phases done: 0.1 (host renderer cross-build, both aarch64 and x86_64
via `scripts/build-gfxstream-backend.sh --abi=both`), 0.2 (kumquat
server), 1.1 (guest driver kumquat-enabled, both ABIs via
`scripts/build-mesa-gfxstream.sh --abi=both`), **1.2 (`BridgeInstallProvider`
ships the chroot-side .so + ICD as per-ABI APK assets and lays them
into each rootfs at install time)**, 2 (UI radio + persisted pref +
RootfsEnv branch), 3 (kumquat embedded as a thread of the compositor
process — `target_os="android"`-gated, so both ABIs compile in),
**Phase 4 partial: host-visible Vulkan memory works end-to-end** —
chroot allocates VkBuffers / staging textures through gfxstream's
`alwaysBlob` path (enabled via gfxstream patch
`01-android-host-build.patch`'s `-DGFXSTREAM_UNSTABLE_VULKAN_BLOB_COLOR_BUFFER=1`),
our hook extracts the real dmabuf fd from the AHB's `native_handle_t`
via dlsym'd `AHardwareBuffer_getNativeHandle`, and the chroot mmaps
the fd to get a CPU view aliasing the host AHB's pages. `vulkaninfo
--summary` enumerates surface caps and present modes correctly
(closed the earlier `SURFACE_LOST_KHR` gap).

**Phase 4 partial done: compositor side of the custom WSI** — the
new `tawc_gfxstream` Wayland protocol, the `gfxstream_present.rs`
handler, the `tawc_gfxstream_lookup_ahb` C entry point in
`libgfxstream_backend.so`, and the dmabuf-v1 stepping-stone cleanup
all landed. See "Compositor side — what landed May 2026" below.

**Phase 4 done end-to-end** — the chroot-side WSI dispatch shell
landed alongside the per-image alloc primitive
`allocate_swapchain_image()`, all in
`deps/mesa-patches/mesa/05-tawc-vulkan-wsi.patch`. vkcube reaches our
`vkCreateSwapchainKHR`, binds `tawc_gfxstream`, allocates AHB-backed
ColorBuffers via kumquat blob create + `VkImportColorBufferGOOGLE`,
and presents them through to the compositor's existing AHB import
path. `gfxstream::test_vkcube_renders_via_ahb` passes on the
physical Adreno 660 device.

Still TODO: Phase 6 (Zink-on-gfxstream-vk for GL/GLES; see "GL/GLES
under custom WSI" below — the rest of the `gfxstream::` integration
suite — gtk3, gtk4, firefox, supertuxkart, weston-simple-egl,
eglinfo — is all gated on this), Phase 7 (real-world AVD
validation beyond `vulkaninfo --summary`).

The x86_64 build path is symmetric with aarch64 — same scripts, same
gradle tasks, same `BridgeInstallProvider`, just `--abi=x86_64`
instead. The `GraphicsBackend` default flips to `GFXSTREAM` on
x86_64 since libhybris is a non-starter there
(`notes/emulator.md` "libhybris on x86_64").

Tests pin their backend per spawn via the broker `GRAPHICS` header on
RUNINSIDE (`tawc-exec --in-rootfs … --graphics gfxstream`, or
`RootfsProcess::spawn_with` from the Rust harness), so a single
`scripts/run-integration-tests.sh` exercises libhybris, gfxstream
and CPU side by side without flipping the user's persisted
`Settings.graphicsBackend` pick. The kumquat server is part of
`libcompositor.so` and starts when the compositor thread starts (see
`compositor/src/bridge.rs`); the chroot-side bits come from the same
`TawcInstaller` pass that lays down libhybris.

### Test coverage layout

The integration suite splits the gfxstream-aware tests across three
modules so a regression in one backend can't accidentally hide behind
another:

- **`libhybris::`** — libhybris-only assertions. The TLS / dlclose
  regression smoke (`test_libhybris_tls_dlclose_does_not_abort`),
  vulkaninfo via libhybris's `vulkanplatform_wayland.so`, eglinfo
  with the `Android META-EGL` signature, and every "X renders via
  hardware buffers through libhybris" smoke (weston-simple-egl,
  vkcube, GTK3/4, Firefox, supertuxkart).
- **`gfxstream::`** — the same hardware-buffer smokes under the
  bridge backend, plus an `eglinfo` software-fallback guard.
  Vulkan-native `vkcube` is green through the custom WSI. The Mesa-EGL
  cases (gtk3/gtk4/firefox/supertuxkart/weston-simple-egl/eglinfo)
  remain gated on Phase 6: route GL/GLES through Zink-on-gfxstream-vk
  so `eglSwapBuffers` ultimately presents through the Vulkan WSI.
- **`cpu_graphics::`** — backend-agnostic SHM paths plus an
  `eglinfo` llvmpipe/swrast sanity. These don't care about the
  GPU backend at all — they exist to keep the SHM plumbing covered
  while the GL/Vulkan paths are still being wired up.

`xwayland::` always pins libhybris (TAWC-DRI / `xwl_tawc` are
libhybris-native — no analogue under gfxstream until a bridge-side
AHB-shipping pipe gets built). `apps::`
and `text_input::` / `touch_input::` pin CPU: their launch / input-dispatch logic is
buffer-path-independent, and CPU is the most portable spawn (no GPU
required), so they can run unattended on the emulator too.

### What works

- **gfxstream host renderer cross-builds for Android NDK** (`scripts/build-gfxstream-backend.sh`) — 8.7MB libgfxstream_backend.so, ELF aarch64, dynamically linked against `libdl/libnativewindow/libandroid/liblog/libc++_shared`. Patches in `deps/gfxstream-patches/gfxstream/01-android-host-build.patch` add `host_machine.system() == 'android'` cases to the ~6 meson.build files that switch on platform, fix two case-insensitive include typos (`GlesCompat.h` → `gles_compat.h`), swap one VNDK header for its NDK equivalent (`<vndk/hardware_buffer.h>` → `<android/hardware_buffer.h>`), and add `-DANDROID=1` (NDK clang defines `__ANDROID__` but not bare `ANDROID`, which several files check). The full GLES+Vulkan+Composer surface is built — it was less work than patching for vulkan-only because frame_buffer.cpp / color_buffer.cpp use a lot of unguarded GL constants.
- **kumquat runs as a thread of the compositor process** (`compositor/src/bridge.rs`), bound at `/data/data/me.phie.tawc/share/kumquat-gpu-0` which the existing share bind exposes to every rootfs at `/usr/share/tawc/kumquat-gpu-0`. The chroot client picks up that path through our Mesa patch's `VIRTGPU_KUMQUAT_GPU_SOCKET` env var (`RootfsEnv` sets it on the gfxstream branch). The compositor crate pulls in `kumquat_virtio` from the rutabaga_gfx workspace with the `gfxstream` feature, so cargo cross-compiles the server into `libcompositor.so` and emits a `DT_NEEDED` for `libgfxstream_backend.so`. Four rutabaga patches support this: AHB fd export hook, keep-server-alive fixes, server-as-library packaging, and the `GfxStreamVulkanMapper` bypass.
- **`libgfxstream_backend.so` + `libc++_shared.so` ride along as jniLibs.** `scripts/build-gfxstream-backend.sh` cross-builds the gfxstream host renderer with the NDK and stages both into `app/src/main/jniLibs/<abi>/` for `arm64-v8a` and `x86_64`. PackageManager extracts them to `nativeLibraryDir` (with the `apk_data_file` SELinux label that's also where `libcompositor.so` lands), so the dynamic linker resolves the `DT_NEEDED` chain at app startup with no extra plumbing.
- **End-to-end Vulkan works on the device.** `libvulkan_gfxstream.so` + ICD JSON ride in the APK as `assets/mesa-gfxstream/`, and `BridgeInstallProvider` (sibling of `LibhybrisInstallProvider` in `TawcInstaller.providers`) lays them into every rootfs at `/usr/lib/gfxstream/` (a tawc-owned namespace, matching `/usr/lib/hybris/`) at install time. With the gfxstream backend selected, `vulkaninfo --summary` from the chroot reports `Virtio-GPU GFXStream (Adreno (TM) 660), driverID=DRIVER_ID_QUALCOMM_PROPRIETARY`. No daemon spawn, no pidfile, no broker action lifecycle.
- **gfxstream host renderer also builds on x86_64 Linux** (`meson setup -Dgfxstream-build=host` on `deps/gfxstream`) — the original validation milestone, now superseded by the NDK cross.
- **`libvulkan_gfxstream.so` + `libvirtgpu_kumquat_ffi.a` cross-build for glibc**, advertise `VK_KHR_wayland_surface`, and load under the chroot's stock vulkan-icd-loader for both supported ABIs.
- **Build script: `scripts/build-mesa-gfxstream.sh`.** Mirrors the `build-libhybris.sh` "stub .so + synthetic .pc + cross gcc" pattern, with a host-built sysroot for libraries whose real symbols are referenced at link time. It reads `build/sysroots/<distro>-<arch>/` (and the compatibility `build/<arch>-sysroot` link), populated by `scripts/build-host-sysroot.sh`, because empty stubs lose the `wl_*_interface` / `drmIoctl` symbols that wayland-scanner-generated protocol files and gfxstream's DRM platform code reference. Pure stubs are fine for libudev / libffi (only DT_NEEDED matters).
- **Mesa patches at `deps/mesa-patches/mesa/`** (xwayland-patches style — sentinel-based idempotent re-apply on patch hash change):
  - `01-add-cargo-toml.patch`: drops `Cargo.toml` files into the four Mesa-internal Rust crates (`mesa3d_util`, `mesa3d_protocols`, `virtgpu_kumquat`, `virtgpu_kumquat_ffi`) plus a workspace `Cargo.toml`, so cargo can build them directly. Templates copied from the matching crates in magma-gpu/rutabaga_gfx (which Cargo-builds the same source). Differences: thiserror 2.0 (Mesa floor; rutabaga ships 1.0), zerocopy 0.8.13.
  - `02-meson-external-kumquat-ffi.patch`: adds a meson option `virtgpu_kumquat_external_ffi=true` that, when set, skips the four `subdir(...)` calls that build the Rust pieces via meson and instead resolves `dep_virtgpu_kumquat_ffi` via plain pkg-config. Also gates the `add_languages('rust')` block on the same condition, so meson never spins up the Rust subproject machinery at all.
  - `03-kumquat-socket-env-override.patch`: lets the chroot-side gfxstream-vk read `VIRTGPU_KUMQUAT_GPU_SOCKET` for the kumquat socket path. Compositor binds at `<appdata>/share/kumquat-gpu-0` (so the existing share bind exposes it inside the rootfs at `/usr/share/tawc/kumquat-gpu-0`), and `RootfsEnv` sets the env var to that path on the gfxstream branch. The upstream `/tmp/kumquat-gpu-0` default still works when the env var is unset.
  - `05-tawc-vulkan-wsi.patch`: custom Wayland WSI for the gfxstream bridge — surface entrypoints, swapchain shell + per-image alloc (kumquat blob create + `VkImportColorBufferGOOGLE`), present loop with `wl_surface.frame()` throttling for FIFO mode. End-to-end with `gfxstream::test_vkcube_renders_via_ahb`.
- **The cargo + meson-external split** is what unblocked kumquat. Mesa's `subprojects/packagefiles/*/meson.build` hard-code `native: true` on every Rust crate's `static_library()`. The proc-macro chain (cfg-if/syn/quote/proc-macro2/unicode-ident) and the regular host-machine crates (cfg-if as `mesa3d_util` dep) can't both satisfy meson in a cross-build context — see git history for the dead-end attempts. Cargo handles cross-builds + proc-macros transparently and produces a static lib that's just linked in like any other dep.
- **`deps/deps.list` pins:** `mesa` (mesa-25.3.6), `gfxstream` (current main), `rutabaga_gfx` (magma-gpu fork with the kumquat server source).
- **Cross-build sysroot:** use `scripts/build-host-sysroot.sh`. It downloads distro packages on the host and extracts the target headers/libs into `build/sysroots/<distro>-<arch>/`; no live device rootfs is required.

### Why kumquat must run as untrusted_app (the SELinux trap)

The SCM_RIGHTS half of the kumquat protocol is what hands a memfd-backed
ring blob from server to guest on every `ResourceCreateBlob` (the very
first one being the gfxstream AddressSpaceStream). On Android, the
kernel silently drops the FD half of the cmsg whenever the sender and
receiver live in different SELinux domains — the rule that would
allow it (`fd { use }` between source and target) is `dontaudit`'d, so
not even an AVC denial shows up in `dmesg` or `logcat`. The visible
symptom is benign-looking on both ends: `sendmsg` succeeds with the
correct fd count on the server, `recvmsg` returns the same byte count
but `descriptor_vec.is_empty()` on the client, and the guest's
`virtgpu_kumquat::resource_create_blob` falls into its
`_ => Err(MesaError::Unsupported)` arm because the response carries
no SCM_RIGHTS payload to construct a `MesaHandle` from. That bubbles
back through the FFI as `-EINVAL`, and Mesa logs:

```
MESA: error: DRM_VIRTGPU_KUMQUAT_RESOURCE_CREATE_BLOB failed with Invalid argument
MESA: error: Failed to create virtgpu AddressSpaceStream
```

…before the chroot's vulkan-icd-loader gives up with
`vkEnumeratePhysicalDevices failed with ERROR_INITIALIZATION_FAILED`.

The earlier "run kumquat under `su`" approach put the server in
`u:r:magisk:s0` — which is why every Vulkan probe out of the chroot
dead-ended on the AddressSpaceStream blob even though every other
piece of the pipeline (socket bind, message framing, gfxstream init
"Adreno (TM) 660" in logcat) was working. Confirmed by toggling
`setenforce 0` end-to-end: `vulkaninfo --summary` immediately starts
returning the right device.

The current fix collapses the two domains trivially: kumquat runs on
a thread of the compositor app, so server and client are
`u:r:untrusted_app:s0:c<…>` by construction — `fd use` is
intra-domain and the kernel delivers the cmsg unchanged. (Earlier
iterations spawned kumquat as a broker-managed child process. Same
SELinux outcome — both children of the broker inherit
`untrusted_app` — but with a separate jniLib, a pidfile, a
start/stop action pair, and a "remember to start the daemon"
footgun. Folding it into the compositor process drops all of that.)

### Confirmed not-blockers

- **gfxstream's host-side Android backend is real and present in the tree.** Confirmed `host/gl/glestranslator/egl/egl_global_info.cpp` unconditionally sets `sEgl2Egl = true` on Android (EGL-on-EGL via dlopen of system libEGL); `host/vulkan/vulkan_dispatch.cpp` dlopens `libvulkan.so`; `host/native_sub_window_android.cpp` wraps `ANativeWindow_*`. The earlier "host backend research (May 2026)" section claimed this from code reading — the host build above confirms it compiles and links.
- **Real-aarch64-`.so`-from-device sysroot pattern works.** No surprises with ABI mismatches; the aarch64 Arch ARM `libwayland-client.so.0` we pulled satisfies link without complaint.
- **`VK_ICD_FILENAMES` cleanly overrides the loader's search path** when libhybris is removed from `LD_LIBRARY_PATH`. We confirmed the chroot's vulkan-icd-loader picks up our ICD JSON exclusively when env is sanitised — no surprise interactions with the libhybris-installed `libvulkan.so.1` (which intercepts at a different layer).

### Remaining work to a fully-integrated bridge backend

End-to-end Vulkan-via-AHB works:
`gfxstream::test_vkcube_renders_via_ahb` passes against the physical
Adreno 660 device. Phase 4 is done; the rest of the `gfxstream::`
integration suite (gtk*/firefox/supertuxkart/weston-simple-egl/
eglinfo) is gated on the GL/GLES work tracked in
[`gfxstream-bridge-remaining-work.md`](../plans/gfxstream-bridge-remaining-work.md).

**Done:**

1. **~~`BridgeInstallProvider` Kotlin.~~** Done — `app/src/main/java/me/phie/tawc/install/BridgeInstallProvider.kt` is a sibling of `LibhybrisInstallProvider` in `TawcInstaller.providers`. Gradle's `buildMesaGfxstream` + `packMesaGfxstream` ship the .so + ICD JSON as raw assets under `assets/mesa-gfxstream/`; `CompositorService.ensureMesaGfxstreamExtracted` stages them into `<filesDir>/mesa-gfxstream/` on the same versioned-stamp pattern as libhybris; the provider returns two `TawcInstall.COPY` entries that land at `/usr/lib/gfxstream/libvulkan_gfxstream.so` + `/usr/lib/gfxstream/gfxstream_vk_icd.aarch64.json`.
2. **~~`GpuBackend` enum + `RootfsEnv` branch.~~** Done — `GraphicsBackend` enum lives in `app/src/main/java/me/phie/tawc/Settings.kt`, exposed in the in-app Settings screen, read by `RootfsEnv.build(method)` on each rootfs spawn. Tests pin via the broker `GRAPHICS` header.
3. **~~Kumquat in the compositor process.~~** Done — `compositor/src/bridge.rs` spawns the kumquat server on a sibling thread. Same SELinux domain → SCM_RIGHTS works.
4. **~~Host-visible memory CPU access.~~** Done — `01-drop-nativewindow-dep.patch` reinstates upstream's AHB-export logic via an embedder hook; `compositor/src/ahb_export.rs::ahb_export_callback` dlsym's `AHardwareBuffer_getNativeHandle` from `libnativewindow.so` and ships the AHB's underlying dmabuf fd to the chroot. The chroot mmaps it to get a CPU view aliasing the host pages. `vulkaninfo --summary` and the first ~3 BLOB allocations vkcube does all work via this path.
5. **~~Kumquat-mapper fallback.~~** Done — `04-kumquat-zero-vulkan-info.patch` zeros `vulkan_info.device_id` in the server's `RESOURCE_CREATE_BLOB` response so the chroot's `createMapping` takes its plain-mmap branch instead of `GfxStreamVulkanMapper` (which recursively uses gfxstream-vk and corrupts colorBuffer ids — see §"Why `GfxStreamVulkanMapper` is unusable").

**Done (compositor side — May 2026):**

6. **~~Compositor `tawc_gfxstream` global + handler.~~** Done.
   `compositor/src/gfxstream_present.rs` impls `GlobalDispatch` +
   `Dispatch` for the new protocol. On `create_buffer` it calls into
   `tawc_gfxstream_lookup_ahb` (gfxstream patch 02-) to resolve the
   colorbuffer_id, wraps the AHB in `WleglBufferData::from_ahb`, and
   parks that as the wl_buffer's user-data so the existing
   AHB-to-GLES-texture path is reused unchanged. Bound unconditionally
   on every Display so libhybris clients can ignore it while
   gfxstream-backend clients use it.
7. **~~Custom Wayland protocol `tawc_gfxstream`.~~** Done.
   `compositor/protocols/tawc_gfxstream.xml`, one request
   `create_buffer(id, colorbuffer_id, width, height, format)`. No fd
   passing; the chroot ships only the u32.
8. **~~C entry point for FrameBuffer → AHB lookup.~~** Done as a
   gfxstream patch (`deps/gfxstream-patches/gfxstream/02-tawc-lookup-ahb.patch`)
   rather than a separate compositor C++ shim — the entry point lives
   inside `libgfxstream_backend.so` next to the private types it uses
   (`FrameBuffer`, `ColorBuffer`, `BlobDescriptorInfo`), and the
   compositor's Rust side just declares one `extern "C"` symbol. The
   compositor crate doesn't ingest gfxstream's full include tree —
   cleaner separation than a shim TU would have given us.
9. **~~Stepping-stone cleanup.~~** Done:
   * `compositor/src/dmabuf.rs` is gone.
   * `DmabufHandler` / `DmabufState` / `DmabufGlobal` /
     `DmabufFeedbackBuilder` / `(dev, ino)` registry / `DMABUF_AHB_BUFFERS`
     side table — all removed.
   * `zwp_linux_dmabuf_v1` global removed from `TawcState::new`.
   * The old local `dmabuf` and WLEGL split render paths are gone.
     Both libhybris and gfxstream buffers attach the same
     `WleglBufferData` user-data and import through Smithay's
     renderer surface state.
   * AHB export hook moved to `compositor/src/ahb_export.rs` (the
     keepers from the old `dmabuf.rs`).

**Done (chroot-side WSI dispatch + per-image alloc — May 2026):**

10. **~~Custom WSI dispatch shell.~~** Done.
    `deps/mesa-patches/mesa/05-tawc-vulkan-wsi.patch` lands strong
    `gfxstream_vk_*` implementations of
    `vkCreateWaylandSurfaceKHR` / `vkDestroySurfaceKHR`, every
    surface caps query, `vkCreateSwapchainKHR` /
    `vkDestroySwapchainKHR` / `vkGetSwapchainImagesKHR`,
    `vkAcquireNextImage(2)KHR`, and `vkQueuePresentKHR`. Adds the
    `tawc_gfxstream` protocol XML to Mesa's tree, runs
    `mod_wl.scan_xml` on it, links libvulkan_gfxstream against
    libwayland-client.

11. **~~Per-image alloc primitive — `allocate_swapchain_image()`.~~**
    Done. The implementation:
      1. Creates a plain VkImage (no external-memory hints) and
         queries memReqs for its size.
      2. Drives a kumquat `RESOURCE_CREATE_BLOB` with
         `VIRGL_BIND_RENDER_TARGET` and the image's
         width/height/format/size. The host gfxstream renderer's
         blob handler decodes the create3d cmd (forwarded via
         kumquat's SUBMIT_3D before the BLOB request) and allocates
         an AHB-backed ColorBuffer via its existing
         `createColorBufferWithResourceHandle` path.
      3. `vkAllocateMemory` with `VkImportColorBufferGOOGLE`
         chained on `pNext`, naming the colorbuffer from (2).
         `VkImportColorBufferGOOGLE` is gfxstream's internal "bind
         this VkDeviceMemory to host ColorBuffer N" extension; not
         part of Vulkan's external-memory framework.
      4. `vkBindImageMemory`.

    Sharp edges that bit during the build-up:

    - **`VkExternalMemoryImageCreateInfo{AHB}` on the swapchain
      VkImage.** Without this the chroot's plain VkImage gets bound
      to AHB-backed memory but the Adreno driver sets the image up
      for normal binding — GPU writes land in a tiled scratch
      that's never synced to the AHB. Surface looks like the
      swapchain works (no crashes, proper present cadence,
      compositor sees and imports the AHB) but the screen stays
      black because the AHB never receives any pixels.
    - **Forward user-provided `VkImportColorBufferGOOGLE` through
      the encoder.** `vk_make_orphan_copy(*pAllocateInfo)` strips
      the user's pNext chain. The encoder normally synthesises its
      own import struct only inside the `exportAhb` path (which
      needs gralloc and isn't built in our LINUX_GUEST_BUILD) or
      the `exportDmabuf` path (in-VM Wayland sharing, hangs in our
      deployment). Patch in `ResourceTracker.cpp` adds a small
      forwarder for caller-provided import. `VkMemoryDedicatedAllocateInfo`
      is already forwarded by the existing path — needed because
      AHB external-memory bindings must be dedicated.
    - **Retain the `VirtGpuResourcePtr` for the image's lifetime.**
      Dropping the shared_ptr at function return sends
      `RESOURCE_UNREF` to the kumquat server, which destroys the
      host ColorBuffer the compositor is about to look up. Stored
      on `gfxstream_vk_swapchain_image` and freed in
      `free_swapchain_image()`.
    - **Advertise R8G8B8A8 only.** Android `AHardwareBuffer` has no
      BGRA8888 format; advertising `VK_FORMAT_B8G8R8A8_UNORM`
      caused vkcube to pick it and the host's `vkAllocateMemory`
      to NULL-deref inside the Adreno blob during AHB allocation.
    - **`AHardwareBuffer_acquire` in `tawc_gfxstream_lookup_ahb`.**
      `exportColorBuffer` / `dupExternalMemory` on Android does not
      bump the refcount on its own (`AHardwareBuffer*` is not
      "duped"); without an explicit acquire, `WleglBufferData::Drop`
      would `AHardwareBuffer_release` an unowned ref → SIGBUS on
      next operation.

    Plus a Wayland-throughput fix:
    `vkQueuePresentKHR` requests a `wl_surface.frame()` callback in
    FIFO mode and dispatches our private event queue until it fires
    (bounded). Without this the chroot has no compositor-driven
    pacing — apps with a frame-count exit (`vkcube --c 3000`) burn
    through their budget in milliseconds and the test asserts on
    a stale `clients=0` snapshot.

12. **(Optional, deferred)** Sentinel-fd optimisation on the kumquat
    server for DEVICE_LOCAL allocations. See §"Sentinel-fd
    optimisation" in the deferred work plan. Tiny memory savings;
    not on the critical path.

**Other patches we keep:**

- All four rutabaga patches stay.
  `01-drop-nativewindow-dep.patch` is what gives us the AHB export
  hook (used by the host-visible-memory path, via
  `ahb_export.rs`). `02-keep-server-alive-on-client-error.patch` /
  `03-kumquat-server-as-lib.patch` are unrelated server resilience
  / packaging fixes. `04-kumquat-zero-vulkan-info.patch` routes
  around the broken `GfxStreamVulkanMapper` recursion (still
  happens even under the custom WSI, because the chroot still has
  only one Vulkan ICD).

### Host sysroot

`scripts/build-host-sysroot.sh` downloads distro packages on the host,
extracts them into `build/sysroots/<distro>-<arch>/`, and maintains
`build/<arch>-sysroot` as a compatibility symlink. Production Mesa
builds request `--profile=prod`; test-app builds request
`--profile=full` for GTK/Cairo/X11 headers.

There is no device-rootfs sysroot pull path anymore.

## Custom Vulkan WSI (May 2026)

**The dmabuf-v1 path is a dead end. The right architecture is to
replace Mesa wsi with a custom Vulkan WSI layer that bypasses Mesa's
swapchain machinery entirely.** Mesa wsi was designed around Linux-guest
kernel DRM mediation, which our kumquat-userspace transport doesn't
have, and trying to make it work has burned a lot of cycles on bugs
that are essentially "Mesa wsi assumed something only true in a VM
deployment."

### How we got here

Earlier iterations tried:
1. Cookie envelope (memfd-as-cookie shipped through dmabuf-v1) — broken
   because PLATFORM_AHB exports aren't exclusively WSI; host-visible
   Vulkan memory needs real CPU access too.
2. Real dmabuf fds via `AHardwareBuffer_getNativeHandle` (the
   approach now living in `compositor/src/ahb_export.rs`, used solely
   for host-visible memory) — works for the chroot's CPU-access needs
   on host-visible memory; doesn't help swapchain images because
   Mesa wsi creates them with OPTIMAL+DMA_BUF which the host vendor
   driver can't size.
3. Various Mesa-side patches forcing LINEAR tiling, adjusting aspect
   masks, etc. — each one moves the goalposts a step but the fundamental
   problem is Mesa wsi has too many assumptions about kernel DRM that
   don't hold here.

Each attempt confirmed the same conclusion: **Mesa wsi is the wrong
abstraction layer for our deployment.** Replacing it is the structural
fix, and the rest of the WSI surface is small enough that this is
actually less ongoing maintenance than fighting Mesa wsi every time it
gets updated.

### The architecture (target)

**Two clearly separate paths**, each appropriate for what it carries:

| Path | Wire | Handle | When CPU access? |
|---|---|---|---|
| Host-visible memory | kumquat (Unix socket + SCM_RIGHTS) | Real dmabuf fd from AHB | **Yes** — chroot mmaps |
| WSI / swapchain image | Custom Wayland protocol | u32 ColorBuffer resource_id | **No** — compositor looks up AHB by id |

The first path is what we already have today. The second path replaces
Mesa wsi.

### Why the WSI path needs no fd

The chroot never reads or writes swapchain image bytes. Swapchain images
are `DEVICE_LOCAL` (not `HOST_VISIBLE`) so apps can't `vkMapMemory`
them; rendering happens via `vkCmdDraw`/etc. on the host GPU. Readback
(if any) uses a separate `HOST_VISIBLE` staging buffer — that's the
canonical Vulkan readback pattern, and it routes through the
*host-visible memory* path, not the WSI path. So the WSI path's only
job is to name *which* AHB the chroot is rendering into and tell the
compositor to display it.

The compositor and the host gfxstream renderer run in the same process
(`libcompositor.so` + kumquat thread + `libgfxstream_backend.so`),
which means the AHardwareBuffer* the chroot's GPU is rendering into is
already in the compositor's address space. The compositor just needs
the right u32 to find it:

```
ColorBuffer handle (u32)
  -> FrameBuffer::getFB()->findColorBuffer(handle)   // ColorBufferPtr
    -> cb->exportBlob()                              // BlobDescriptorInfo
      -> blob->handle                                // AHardwareBuffer*
```

That's three in-process C++ calls. A small `extern "C"` shim in
`compositor/native/` wraps them; the compositor's Rust side calls
through FFI.

The same u32 (`resource_id` on the chroot side, `handle` in gfxstream's
`m_colorbuffers`) names the object in all three places by virtue of the
kumquat protocol's normal flow — see "How the chroot gets the ColorBuffer
resource_id" below.

### Wire format

New Wayland protocol `tawc_gfxstream` —
`compositor/protocols/tawc_gfxstream.xml`. Single global with one
request:

```
create_buffer(new_id wl_buffer, colorbuffer_id, width, height, format)
```

Both sides agree on `colorbuffer_id` from the kumquat protocol's
`RESOURCE_CREATE_BLOB` response. Compositor looks up the AHB via
`tawc_gfxstream_lookup_ahb` and adopts the ref into the wl_buffer's
user-data. The chroot's custom WSI emits one `create_buffer` per
`vkQueuePresentKHR`. No SCM_RIGHTS, no plane / stride / modifier
negotiation, no `zwp_linux_dmabuf_v1`.

### Implementation surface

Three pieces:

- **Compositor side — handler for the new protocol** (done, May 2026).
  `compositor/src/gfxstream_present.rs`: Smithay GlobalDispatch + Dispatch
  impls. On `create_buffer`, the handler calls the
  `tawc_gfxstream_lookup_ahb` extern "C" entry point in the gfxstream
  patch (see below) to get the AHB by colorbuffer_id, wraps it in
  `WleglBufferData::from_ahb`, and stores it as the wl_buffer's
  user-data. The renderer's existing AHB-import path doesn't change.

- **gfxstream patch — C entry point** (done, May 2026).
  `deps/gfxstream-patches/gfxstream/02-tawc-lookup-ahb.patch`. Adds
  `extern "C" AHardwareBuffer* tawc_gfxstream_lookup_ahb(uint32_t handle)`
  inside `frame_buffer.cpp`. ~30 LOC. Living inside libgfxstream_backend
  means direct access to private types (FrameBuffer, ColorBuffer,
  BlobDescriptorInfo) — keeps the compositor crate's build off the
  gfxstream include tree.

- **Chroot side — custom Vulkan WSI layer** (done).
  `deps/mesa-patches/mesa/05-tawc-vulkan-wsi.patch` (file:
  `src/gfxstream/guest/vulkan/gfxstream_vk_tawc_wsi.cpp`) ships
  strong `gfxstream_vk_*` overrides for the surface entrypoints,
  every surface caps query, the swapchain shell, the Wayland
  registry binding, the present loop, and the per-image alloc
  primitive `allocate_swapchain_image()`:

    1. Create a plain VkImage (no `VkExternalMemoryImageCreateInfo`)
       and query memReqs.
    2. Drive a kumquat `RESOURCE_CREATE_BLOB` from the WSI with
       `VIRGL_BIND_RENDER_TARGET` and the image's width / height /
       format / size. Host gfxstream allocates an AHB-backed
       ColorBuffer and returns a u32 resource handle. The
       `VirtGpuResourcePtr` is **retained on the swapchain image**
       — dropping it sends `RESOURCE_UNREF`, which destroys the
       host ColorBuffer the compositor will look up at present.
    3. `vkAllocateMemory` with `VkImportColorBufferGOOGLE` chained
       on `pNext`, naming the colorbuffer from (2). This is
       gfxstream's internal "bind this VkDeviceMemory to host
       ColorBuffer N" extension; *not* part of Vulkan's external
       memory framework.
    4. `vkBindImageMemory`.

  `vkQueuePresentKHR` emits
  `tawc_gfxstream.create_buffer(colorbuffer_id, ...)` over the
  Wayland client connection, then in FIFO mode requests a
  `wl_surface.frame()` callback and dispatches our private event
  queue until it fires (bounded). The surface formats list is
  R8G8B8A8 only — Android's `AHardwareBuffer` has no BGRA8888
  format, so advertising `VK_FORMAT_B8G8R8A8` causes the host's
  AHB-backed `vkAllocateMemory` to NULL-deref inside the Adreno
  blob.

### How the chroot gets the ColorBuffer resource_id

Same way it gets resource_ids today, no protocol change:

1. Chroot sends `RESOURCE_CREATE_BLOB` (no id).
2. Server calls `kumquat_gpu.allocate_id()` to mint a u32.
3. Server forwards to gfxstream's `stream_renderer_create_blob`; gfxstream
   uses that u32 as the `FrameBuffer::m_colorbuffers` key when it
   allocates the AHB-backed ColorBuffer.
4. Server replies with `resp.resource_id = <same u32>`.
5. Chroot stores it in `VirtGpuKumquatResource::mResourceHandle` and
   reads it back via `getResourceHandle()`.

The u32 names the same object in three places (chroot resource, kumquat
server resources map, host `FrameBuffer` color buffer table). When the
chroot ships it over our custom Wayland protocol, the compositor uses
it to look up the matching AHB in the third place — same process, same
namespace.

### Sentinel-fd optimisation (deferred)

The kumquat `RESOURCE_CREATE_BLOB` response is *required* to ship an fd
via SCM_RIGHTS (the client's `RespResourceCreate(resp, handle)` pattern
match expects it). For host-visible allocations the fd is the real
dmabuf and the chroot mmaps it. **For DEVICE_LOCAL allocations
(swapchain images, depth buffers, etc.) the chroot never touches the fd
— it's pure protocol fluff.**

Optimization: have the kumquat server detect "this is a DEVICE_LOCAL
allocation" (via the AHB's memory-type flags) and ship a single
long-lived `/dev/null` fd via SCM_RIGHTS instead of doing the
`getNativeHandle` dance. Saves a FFI call per swapchain image plus an
fd per image. Tiny absolute savings, but a clean signal of intent.

Deferred — not on the critical path. Worth doing once the custom WSI
is in and we want to clean up. Could also be folded into a kumquat
protocol patch that adds an explicit "no fd needed" variant of the
response, but `/dev/null` works without any protocol change.

### Compositor-side AHB lifetime

Resolved (May 2026) — option (2) below: live lookup via
`FrameBuffer::exportColorBuffer` on each `create_buffer` request. No
registry to maintain, lifetime story matches the libhybris path.

`tawc_gfxstream_lookup_ahb` (gfxstream patch 02-) is the in-process C
entry point. It calls
`FrameBuffer::getFB()->exportColorBuffer(handle)`, pulls the AHB out
of the returned `BlobDescriptorInfo::descriptorInfo.handle`, and
returns it with refcount += 1 (gfxstream's
`vkGetMemoryAndroidHardwareBufferANDROID` does the bump internally).
The compositor's handler hands that ref to a `WleglBufferData`, whose
`Drop` releases on wl_buffer destroy — same lifetime envelope as the
libhybris path.

If we ever need a registry (e.g. to detect colorbuffer-id reuse), the
hook would attach in `Gfxstream::stream_renderer_resource_create`.
Don't add it speculatively.

### GL/GLES under custom WSI

Deferred GL/GLES routing work lives in [gfxstream-bridge-remaining-work.md](../plans/gfxstream-bridge-remaining-work.md).

### Why `GfxStreamVulkanMapper` is unusable in our setup

Worth recording because this is easy to rediscover. The canonical
gfxstream deployment has a *second* Vulkan ICD in the chroot
(e.g. Mesa radv inside an AVF VM) that
`GfxStreamVulkanMapper` uses to import dmabuf fds via
`VK_KHR_external_memory_fd` and `vkMapMemory` them. Our chroot only
has `libvulkan_gfxstream.so` as the ICD, so the mapper recursively
picks gfxstream-vk for the import. Its `importDmabuf` path goes
through `virtgpu_kumquat_resource_import`, which treats the imported
fd as a SHM-encoded "emulated handle" (reading the first 4 bytes as a
resource_id). A real dmabuf fd has 0 there, so the returned resource
handle is 0, which becomes `VkImportColorBufferGOOGLE{colorBuffer = 0}`
and the host aborts with `Failed to get allocation info for
ColorBuffer:0`.

`04-kumquat-zero-vulkan-info.patch` papers this over by zeroing
`vulkan_info.device_id` in the kumquat server's `RESOURCE_CREATE_BLOB`
response so the chroot's `createMapping` takes its plain-mmap branch
instead of the mapper. The plain-mmap path works for our use case. The
patch is correct regardless of which WSI we land on — the mapper is
fundamentally broken for our "no second Vulkan ICD" deployment.

### `VulkanNativeSwapchain` doesn't help

Flipping kumquat's `set_wsi(RutabagaWsi::Surfaceless)` to
`VulkanSwapchain` enables `STREAM_RENDERER_FLAGS_VULKAN_NATIVE_SWAPCHAIN_BIT`
and creates host-side `DisplayVk`/`CompositorVk` — but the chroot's
`gfxstream_vk_wsi.cpp` still delegates to Mesa's `wsi_common`. The flag
is purely a host-side capability for letting the host present to its
own window; there's no companion guest-side path in gfxstream-vk that
bypasses Mesa wsi. Of the gfxstream architectures upstream supports,
only the AOSP/AVF VM case has a guest-kernel virtio-gpu DRM driver
that Mesa wsi can rely on. Our kumquat-userspace transport doesn't
have that, and there's no kumquat-aware shortcut in Mesa wsi. The
only structural fix is to replace Mesa wsi (see "Custom Vulkan WSI"
above).

## Deferred work

Remaining GL/GLES and x86_64 AVD work lives in [gfxstream-bridge-remaining-work.md](../plans/gfxstream-bridge-remaining-work.md).

## Relation to existing notes

- `notes/gpu-strategy.md` — overall graphics strategy. The bridge is
  now an implemented backend: it puts both halves on the *same* driver
  instance via IPC instead of via libhybris shared address space.
- `notes/emulator.md` — "libhybris on x86_64" section enumerates
  three options (A/B/C) for porting the thunk patcher, all expensive.
  The bridge sidesteps that whole tree: the chroot doesn't need
  bionic compat at all on x86, because nothing bionic-linked runs
  there.
- `notes/wsi-layer.md` — the chroot's GL/Vulkan WSI today. Under the
  bridge, the chroot's WSI becomes "Mesa with `gfxstream-vk`",
  i.e., upstream off-the-shelf. Libhybris-fork-side WSI patches
  stay needed for users on the `libhybris` backend (which we ship
  indefinitely alongside the bridge — see "Coexistence with
  libhybris" above).
- `notes/libhybris-zink.md` — Zink-on-libhybris-vulkan, the
  implemented libhybris-side version of routing GL through
  Zink-on-Vulkan. Under the bridge backend the same routing maps
  onto Zink-on-gfxstream-vk and is in fact the *primary* GL path
  for that backend (not just a desktop-GL fallback). May be
  obsoletable on the bridge side by `MESA_LOADER_DRIVER_OVERRIDE=zink`
  alone; revisit during Phase 6.
- `notes/installation.md` — "Nothing under `/etc/profile.d/`" policy
  is what makes the runtime GPU-backend toggle clean: env lives in
  `RootfsEnv.kt`, applied via `env -i` on every entry, no on-disk
  drift between method-specific entry paths.
