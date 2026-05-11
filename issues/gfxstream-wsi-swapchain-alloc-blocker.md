# gfxstream WSI: swapchain image alloc primitive not implemented yet

The custom Vulkan WSI for the gfxstream bridge is fully wired in
dispatch on both sides (compositor + chroot). Strong `gfxstream_vk_*`
overrides for the surface entrypoints, swapchain shell, Wayland
registry binding for `tawc_gfxstream`, present loop -- all in
`deps/mesa-patches/mesa/05-tawc-vulkan-wsi.patch`. What's missing is
the per-image alloc primitive `allocate_swapchain_image()`. It
currently returns `VK_ERROR_INITIALIZATION_FAILED` so apps fail
fast rather than hanging.

## What it needs to do

The chroot needs to render into a host-allocated AHB-backed
ColorBuffer and ship that ColorBuffer's u32 to the compositor on
present. Concretely:

1. Drive a kumquat `RESOURCE_CREATE_BLOB` directly from the WSI,
   with `VIRGL_BIND_RENDER_TARGET` and image-shaped `width / height /
   format` fields. The host gfxstream renderer's blob handler sees
   the bind flag, recognizes this as a ColorBuffer request, and
   allocates an AHB-backed ColorBuffer through gfxstream's Android
   external-memory path. Returns a u32 resource handle (== gfxstream
   `m_colorbuffers` key == `tawc_gfxstream` wire `colorbuffer_id`).
2. Create a plain VkImage matching the requested swapchain extent /
   format / usage. **No `VkExternalMemoryImageCreateInfo`** -- the
   image isn't external from the chroot's perspective; it's just a
   normal image whose memory will be imported.
3. Allocate VkDeviceMemory with a `VkImportColorBufferGOOGLE`
   structure chained onto `VkMemoryAllocateInfo::pNext`, naming the
   colorbuffer from step (1). `VkImportColorBufferGOOGLE` is
   gfxstream's internal extension for "this memory is backed by host
   ColorBuffer N" -- it is NOT part of Vulkan's external memory
   framework. The host-side gfxstream interceptor handles it by
   binding the chroot's VkDeviceMemory to the AHB underlying that
   ColorBuffer.
4. `vkBindImageMemory(image, memory, 0)`.

The `tawc_gfxstream` Wayland protocol on present already ships only
the u32; no fds, no plane / stride / modifier negotiation. The
compositor calls `tawc_gfxstream_lookup_ahb(u32)` and wraps the
returned `AHardwareBuffer*` in a wl_buffer using the existing
`WleglBufferData::from_ahb` path (same as the libhybris/android_wlegl
backend uses).

## Why this shape, and why earlier attempts failed

Earlier attempts (May 2026) tried a different shape: pass
`VkExportMemoryAllocateInfo{handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT}`
with a dedicated image to standard `vkAllocateMemory`. That triggers
gfxstream-vk's `LINUX_GUEST_BUILD` `exportDmabuf` code path, which
*does* drive a kumquat `RESOURCE_CREATE_BLOB` as a side effect. So
it looked like a shortcut to get the host AHB allocation we wanted.

But that path was written for an entirely different use case:
**guest-app-to-guest-compositor cross-process buffer sharing inside a
guest VM (AVF)**. A Wayland client in a Linux VM that wants to share
a buffer with the VM's compositor via `linux-dmabuf-v1` allocates
this way; the VkDeviceMemory ends up exportable as a Linux dmabuf fd
that gets passed through Wayland to another process inside the VM.

That has nothing to do with what we want. Specifically:

  * We don't ship dmabuf fds on the wire. Our protocol carries
    only a u32.
  * The Adreno host driver doesn't accept
    `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` as an image
    external-memory handle type (Android Vulkan's native external
    type is AHB; DMA_BUF is a Linux-kernel concept). Returns
    `memReqs.size = 0` for any image carrying that hint.
  * Without the hint, the host's vkAllocateMemory hangs trying to
    bind an AHB-backed ColorBuffer to a VkImage that wasn't
    declared as bindable to external memory.
  * The whole sequence imposes "size the image first, then allocate"
    ordering, but for AHB-backed images the AHB has to exist first
    (the AHB's actual layout determines the size).

So the bug isn't an upstream bug to file or a vendor-driver quirk to
work around. It's that we were using an existing path designed for a
different purpose, then tripping over the consequences.
**The fix is to stop using that path and explicitly drive the
allocation in the right order, using the gfxstream-internal
`VkImportColorBufferGOOGLE` machinery that already exists for exactly
this kind of "guest VkDeviceMemory backed by host ColorBuffer" case.**

## Implementation pointers

  * `deps/mesa/src/gfxstream/guest/vulkan/gfxstream_vk_tawc_wsi.cpp`:
    `allocate_swapchain_image()` is the function to fill in.
  * `deps/mesa/src/gfxstream/guest/vulkan_enc/ResourceTracker.cpp`
    around `on_vkAllocateMemory`'s `LINUX_GUEST_BUILD` `exportDmabuf`
    path (line ~3899) shows the existing pattern for driving
    `instance->createBlob()` with `VIRGL_BIND_RENDER_TARGET` and
    using the returned resource handle as the colorbuffer_id. Reuse
    the same primitives (`gfxstreamResourceCreate3d`,
    `VirtGpuCreateBlob`, `instance->createBlob()`,
    `instance->execBuffer()`, `bufferBlob->wait()`,
    `bufferBlob->getResourceHandle()`) but call them directly from
    the WSI, not via `vkAllocateMemory`.
  * `VirtGpuDevice::getInstance()` is how the singleton is reached;
    it's currently used by `ResourceTracker`, which means we may
    need a small accessor exposing it (or the encoder) to the
    `vulkan/` directory. Check the symbols rather than assuming the
    `vulkan/` and `vulkan_enc/` objects can see each other.
  * `VkImportColorBufferGOOGLE` lives in
    `deps/mesa/src/gfxstream/codegen/vulkan/vulkan-headers/include/vulkan/vulkan_gfxstream.h`
    (or wherever the gfxstream Vulkan extension headers live in the
    Mesa tree).

## Reproducer (after the fix lands)

1. `bash scripts/build-mesa-gfxstream.sh --abi=aarch64`
2. `JAVA_HOME=… ANDROID_HOME=… ./gradlew assembleDebug`
3. `bash scripts/app-build-install.sh --no-build`
4. `tawc-exec --action set-graphics-backend --arg value=gfxstream`
5. `bash scripts/run-integration-tests.sh gfxstream::test_vkcube_renders_via_ahb`

Currently: vkCreateSwapchainKHR returns `VK_ERROR_INITIALIZATION_FAILED`
with `tawc_wsi: allocate_swapchain_image() is not implemented yet ...`
in stderr. After the fix: vkcube renders a spinning cube via AHB
through the bridge.
