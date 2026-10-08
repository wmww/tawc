# gfxstream vkcube test fails on Pixel 9 Pro

`gfxstream::test_vkcube_renders_via_ahb` fails on a Pixel 9 Pro
(caiman, Android 16): vkcube exits before rendering. Reproduces on
`main` (2026-10-07) and with the vsync frame clock, so it is not a
frame-clock regression. gfxstream is experimental on physical devices;
needs a look at vkcube's stderr under gfxstream there.

vkcube's stderr (2026-10-08) — the bridge sees the GPU but the host
allocation for an imported color buffer fails with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`:

```
Selected GPU 0: Virtio-GPU GFXStream (Mali-G715), ... (1.4.0), driverVersion: ... (25.3.6)
MESA: error: Failed to allocate coherent memory: failed to allocate on the host: -2.
MESA: error: tawc_wsi: gfxstream_vk_AllocateMemory(import colorBuffer=7) failed: -2
```

Next: find which memory type the import requests and why the Mali host
side has no coherent type/heap for it.
