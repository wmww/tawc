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
  calls against a local GL/Vulkan implementation. Today the targets
  are desktop GL/Vulkan; we'd need to retarget it at Android `libEGL`
  / `libvulkan`. This is the bulk of the new Android-side work.

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
buffer), not virtio-gpu kernel queues. gfxstream supports non-virtio
transports already — the AVD's "ranchu pipe" is one — but verifying
this still works in 2026 Mesa is task #1 before committing.

If it doesn't, the fallback is heavier: ship a tiny VM (Firecracker,
crosvm) inside Android purely to host a real virtio-gpu device for
the chroot. Probably untenable for a phone target, but worth knowing
the option exists.

## Android-side bridge daemon

A normal Android service process linked against `libEGL.so` and
`libvulkan.so` (so the vendor blob loads against its proper bionic,
exactly how the Android system itself uses it). It accepts gfxstream
protocol on a Unix socket exposed into the chroot via the existing
broker mechanism (next to the Wayland socket), holds one EGL/Vulkan
context per chroot client, and translates command streams.

Process lifecycle ties to compositor lifecycle. It can run inside the
compositor app process (separate thread holding the GL context) or as
a sibling Service — Service is cleaner for crash isolation, in-process
saves an IPC hop on the buffer-handoff path. Pick at implementation
time.

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
   in 2026?** Read the Mesa source. If yes, scope is small. If no,
   we're either modifying Mesa (tractable, gfxstream guest is
   contained) or falling back to a real VM (probably out of scope).
2. **Does an Android-targeting gfxstream host backend exist?** Check
   gfxstream's tree; it's plausible an Android-as-host renderer
   exists internally at Google for nested-AVD scenarios but isn't
   upstream. If it doesn't exist, retargeting is the bulk of the
   work — not crazy (the host renderer is fundamentally a
   GL/Vulkan call dispatcher), but real engineering.
3. **GL performance under IPC.** Vulkan is fine, GL might not be.
   Worth a microbenchmark before committing if any priority client
   is GL-heavy. (Most modern toolkits — GTK4, Qt6, browsers — are
   moving toward Vulkan anyway, which softens this.)
4. **Does the `linux-dmabuf-v1` / AHB import path stay zero-copy
   when both sides go through gfxstream?** Specifically, does the
   bridge's renderer write directly into the AHB (zero-copy) or does
   it copy out of an internal target into the AHB (one-copy)? The
   answer depends on how the host renderer handles framebuffer
   targets — a surface backed by an externally-allocated AHB needs
   to be wired through to the underlying EGL/Vulkan as a foreign
   image, not as gfxstream's own backing store.

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
  i.e., upstream off-the-shelf, and our libhybris-fork-side WSI
  patches stop being needed.
