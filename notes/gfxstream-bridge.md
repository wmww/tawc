# gfxstream bridge: a libhybris-free GPU path

**Status:** design sketch, not implemented. Captured here so we can pick
it up if/when libhybris becomes untenable (new Android version breaks
the thunk patcher, x86 demand grows, vendor blob ABIs drift, …).

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
`/usr/local/share/vulkan/icd.d/` pointing at it. Vulkan's loader
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
- **Already in our notes.** `notes/desktop-gl-dispatch.md` is the
  libhybris-side version of the same idea (Zink-on-libhybris-vulkan
  for desktop GL). Bridge inherits it for free.
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

- **aarch64 physical:** `hybris` (proven, lower latency, our
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

- libhybris: `/usr/lib/hybris/` + `/usr/local/lib/gl-shims/` (laid
  down by `TawcInstaller` / `LibhybrisInstallProvider`, untouched
  by this work).
- bridge: `/usr/local/lib/libvulkan_gfxstream.so` +
  `/usr/local/share/vulkan/icd.d/gfxstream_vk_kumquat.json` (new
  `BridgeInstallProvider`, same shape as `LibhybrisInstallProvider`).

The compositor's kumquat server thread is cheap to leave always-on
(or start lazily on first connection); it doesn't conflict with
`android_wlegl` since each Wayland client picks at most one of "use
android_wlegl" (libhybris path) or "use kumquat for buffers"
(bridge path) by virtue of which Vulkan/EGL library it loaded.

### What this changes in the implementation order

The "Phase 1 — guest side off-the-shelf" step in the plan below
is now: **build only `libvulkan_gfxstream.so` (kumquat-enabled)
host-side, ship as APK asset, lay down via a new
`BridgeInstallProvider`, plus a one-line ICD JSON.** Not "rebuild
Mesa for the chroot." The rest of Mesa is the distro's.

The "Phase 7 — retire libhybris" step is dropped. We ship both
indefinitely; the radio selector is the user's choice.

## Implementation plan

### Phase 0 — de-risk the unknowns
0.1 **Build gfxstream host renderer with the NDK (no Soong).** The
single biggest unknown. Try the in-tree CMake first; expect to fix
Android-target gaps. Output: `libgfxstream_backend.so` in
`build/gfxstream-host/`, calling `stream_renderer_init(USE_EGL|
USE_GLES|USE_VK)` against system `libEGL/libvulkan`. Smoke test
inside a tiny APK.

0.2 **Audit the kumquat server side.** Either lift Google's server
out of AVF/crosvm or write a small one over `rutabaga_gfx` that
proxies kumquat → `stream_renderer_*`. Pick before committing —
that picks build pipeline (Rust + crosvm vs C++/Rust hybrid). Add
to `deps/deps.list` if vendored.

0.3 **GL perf microbench (Q3) using Zink.** Once 0.1+0.2 stand up,
run a real Zink-on-gfxstream-vk test (`glmark2`, gtk4-debug-app)
through a stub kumquat link. Validate that Zink's command-buffer
batching does in fact tame the per-call cost. If not, scope down
to "Vulkan via bridge, GL stays libhybris" and replan.

### Phase 1 — guest side, ICD overlay
1.1 **Cross-build only `libvulkan_gfxstream.so` from Mesa** with
`-Dvulkan-drivers=gfxstream -Dvirtgpu_kumquat=true`. New
`scripts/build-mesa-gfxstream.sh`, vendored Mesa pinned in
`deps/deps.list`. Adds Rust toolchain to `notes/building.md`.
Output: a single .so + a one-line `gfxstream_vk_kumquat.json` ICD
manifest pointing at our install path.

1.2 **`BridgeInstallProvider`** mirroring `LibhybrisInstallProvider`:
APK asset → real-file copy under
`/usr/local/lib/libvulkan_gfxstream.so` + ICD JSON at install time.
Symlinks under `/usr/local/lib` per existing convention.

1.3 **Wire env into `RootfsEnv`.** New `GpuBackend` enum, branch
in `RootfsEnv.build`. Default reads from SharedPreferences via the
home-page radio (see Phase 2 below). Default value: `HYBRIS` on
aarch64 physical, `BRIDGE` on x86_64 emulator.

1.4 **Validate guest stack alone** with `vulkaninfo` / `vkcube`
against a stub host — confirm Mesa loads our ICD, gfxstream-vk
picks kumquat, socket connects.

### Phase 2 — UI: GPU backend radio on the home page
Two-option radio (Hybris / Bridge) above or alongside the
"Install new distro" CTA on `MainActivity`. Persisted via
`SharedPreferences` (first user-facing pref in the app).
`RootfsEnv.build` reads on each chroot entry; selection takes
effect on the next process the user launches in the rootfs (not
mid-process).

### Phase 3 — Android-side bridge daemon, in-process
3.1 **Spawn a kumquat-listening thread from
`CompositorService.onCreate`.** Owns one EGL/Vulkan context per
kumquat client. SELinux-wise it inherits the app domain — should
just work, unlike libhybris-in-chroot.

3.2 **Pump kumquat → `stream_renderer_*`.** Resource create/destroy,
transfer, command submit, fences (eventfd). Capability negotiation.
Snapshot/restore stubbed.

3.3 **Headless first.** Run gfxstream host renderer headless (no
`ANativeWindow*`); we never use its DisplayVk. End-to-end smoke:
chroot `vkcube` → bridge → Adreno → no frames yet, but command
stream completes.

### Phase 4 — zero-copy buffer handoff (Option A in §"Zero-copy buffer sharing")
4.1 **Intercept post.** Hook `PostWorker::post()` completion (or
`stream_renderer` post callback) so we don't drive `DisplayVk`. On
swap, call `exportColorBufferMemory(colorBufferHandle)` →
`AHardwareBuffer*`.

4.2 **Map kumquat client → wl_surface.** Bridge needs to know which
compositor surface this AHB is for. Easiest: bridge runs alongside
a tiny per-client wl_display connection that owns a `wl_surface`
and feeds AHBs via the existing `android_wlegl` import path the
compositor already speaks. Reuses everything from the libhybris
path on the compositor side.

4.3 **Triple-buffering / lifecycle.** gfxstream owns the buffer
pool; release callbacks drive its internal refcount. If this gets
painful, fall through to Option B (~50 LOC patch in
`allocExternalMemory()` mirroring `QnxScreenBuffer`).

4.4 **First real frame:** chroot `vkcube` → AHB → compositor
texture → on screen.

### Phase 5 — Vulkan WSI for real apps
5.1 **`VK_KHR_wayland_surface` in the guest.** gfxstream-vk's WSI
is Android-shaped; remap to Wayland either guest-side (mirroring
libhybris's trick) or via a tiny implicit layer. Bridge resolves
the `wl_surface` to a kumquat-side ColorBuffer.

5.2 **Format negotiation** matching what the compositor's gralloc
importer accepts (same constraints as today's libhybris path;
mostly inherits).

5.3 **Suite:** `vulkaninfo`, `vkcube`, Firefox WebGPU, glmark2-vulkan.

### Phase 6 — Zink wired up for GL/GLES
Mostly env-only at this point — the rootfs already has Mesa with
Zink from the distro. Validate:

6.1 `glxgears`, `glmark2` (desktop GL via Zink → bridge).

6.2 `weston-simple-egl`, gtk4-debug-app (GLES2/3 → Zink → bridge).

6.3 Firefox accelerated rendering with `MOZ_X11_EGL=1` /
`gfx.canvas.accelerated=true`.

6.4 Decide whether the `desktop-gl-dispatch.md` design (API-aware
libEGL routing) is still needed at all under the bridge path —
plain `MESA_LOADER_DRIVER_OVERRIDE=zink` may be enough.

### Phase 7 — AVD / x86_64 parity
The whole point. Repeat 1.1–6.3 with `--abi=x86_64`. No thunk
patcher, no bionic-slot whack-a-mole. Default `GpuBackend` flips
to `BRIDGE` on x86_64 emulator targets (already wired in 1.3).

### Cross-cutting
- **Integration tests:** extend `tests/integration/` with bridge
  variants of existing GPU tests; gate on `TAWC_GPU=bridge|hybris`
  (env-var override, separate from the persisted SharedPreferences
  default — same shape as `TAWC_TARGET`) so we run both backends in
  CI.
- **`deps/deps.list`** entries for Mesa (gfxstream-only build),
  gfxstream host renderer, kumquat server (or rutabaga), pinned by
  commit. Don't skip — silent drift here would be brutal.
- **`notes/building.md`** updated per change (Rust toolchain for
  Mesa, NDK/CMake quirks for gfxstream host, any new host pkgs).
- **`notes/gpu-strategy.md`** flip the "Alternative we haven't
  taken" wording to "Both backends ship; user-selectable" once
  Phase 2 lands.
- **`notes/wsi-layer.md`** add a "Bridge backend" subsection once
  Phase 5 is real; libhybris-WSI section stays.

Riskiest milestones: 0.1 (NDK build of gfxstream host), 0.2
(kumquat server source), 0.3 (Zink GL perf). Land those before
committing to phases 1–6.

## Relation to existing notes

- `notes/gpu-strategy.md` — current libhybris-based strategy. The
  bridge is the alternative described there as "out of scope" for
  cross-driver sharing. It supersedes that out-of-scope-ness by
  putting both halves on the *same* driver instance via IPC instead
  of via shared address space.
- `notes/emulator.md` — "libhybris on x86_64" section enumerates
  three options (A/B/C) for porting the thunk patcher, all expensive.
  The bridge sidesteps that whole tree: the chroot doesn't need
  bionic compat at all on x86, because nothing bionic-linked runs
  there.
- `notes/wsi-layer.md` — the chroot's GL/Vulkan WSI today. Under the
  bridge, the chroot's WSI becomes "Mesa with `gfxstream-vk`",
  i.e., upstream off-the-shelf. Libhybris-fork-side WSI patches
  stay needed for users on the `hybris` backend (which we ship
  indefinitely alongside the bridge — see "Coexistence with
  libhybris" above).
- `notes/desktop-gl-dispatch.md` — design for routing desktop-GL
  apps through Zink-on-Vulkan. Originally framed against
  libhybris-vulkan; under the bridge backend the same routing maps
  onto Zink-on-gfxstream-vk and is in fact the *primary* GL path
  for that backend (not just a desktop-GL fallback). May be
  obsoletable on the bridge side by `MESA_LOADER_DRIVER_OVERRIDE=zink`
  alone; revisit during Phase 6.
- `notes/installation.md` — "Nothing under `/etc/profile.d/`" policy
  is what makes the runtime GPU-backend toggle clean: env lives in
  `RootfsEnv.kt`, applied via `env -i` on every entry, no on-disk
  drift between method-specific entry paths.
