# SHM Buffer Support

## How It Works

Wayland's `wl_shm` protocol works by having the **client** create a shared memory region
(via `memfd_create()` or a tmpfile), then pass the file descriptor to the compositor via
`wl_shm.create_pool(fd, size)`. The compositor mmaps the fd to access pixel data.

### The SELinux Problem (Solved)

On Android, the compositor runs as `untrusted_app`, and chroot clients' memfds get the
SELinux label `u:object_r:tmpfs:s0`. Android's SELinux policy denies `untrusted_app` from
mmapping `tmpfs` files from other contexts. The failure is silent and catastrophic -- the
fd is consumed from SCM_RIGHTS ancillary data, the protocol parser gets out of sync, and
all subsequent messages from that client are silently dropped.

### The Solution: ashmem LD_PRELOAD Shim

The `client/ashmem-shim/` directory contains a small LD_PRELOAD library that transparently
redirects `memfd_create()` to use Android's `/dev/ashmem` driver instead. Ashmem fds are
in SELinux's `mlstrustedobject` set (because Binder uses ashmem for IPC), so the compositor
can mmap them without any SELinux modifications or root access.

The shim intercepts:
- `memfd_create()` -> `open("/dev/ashmem")` + `ASHMEM_SET_NAME`
- `ftruncate()` -> `ASHMEM_SET_SIZE` ioctl (on ashmem fds only)
- `posix_fallocate()` -> `ASHMEM_SET_SIZE` ioctl (weston uses this instead of ftruncate)
- `fallocate()` -> `ASHMEM_SET_SIZE` ioctl
- `close()` -> cleanup fd tracking + real close

#### Building

```bash
cd client/ashmem-shim && ./build
```

#### Usage

```bash
LD_PRELOAD=/tmp/ashmem-shim/libashmem-shim.so \
WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0 \
weston-simple-shm
```

The shim is completely transparent to Wayland clients -- no source code modifications or
recompilation needed.

### Future Considerations

ashmem is deprecated upstream in favor of `memfd_create`, but it remains present on all
shipping Android devices for backwards compatibility. If ashmem is eventually removed:
- If Android loosens SELinux policy for memfd cross-process sharing, the shim can simply
  be dropped
- If not, the shim's interception pattern stays the same -- just swap the backend from
  ashmem to whatever mechanism works

## Architecture Notes

### How Smithay Handles wl_shm

1. Client sends `wl_shm.create_pool(fd, size)` -- fd is passed via SCM_RIGHTS
2. Smithay handler calls `mmap(fd, size, PROT_READ|PROT_WRITE, MAP_SHARED)`
3. A `Pool` object wraps the mapping
4. Client creates `wl_buffer` from pool (offset, width, height, stride, format)
5. On commit, compositor calls `import_shm_buffer()` which reads pixel data from the
   mmap'd region and uploads to a GL texture

### Compositor SHM Rendering Path

SHM textures are rendered with a magenta tint shader to visually distinguish them from
AHB (zero-copy GPU) surfaces. This tint is intentional for debugging and should be removed
or made optional once the AHB path is mature.

### Why Standard Wayland Clients Need SHM

Even clients that primarily use EGL for rendering often need `wl_shm` for:
- **Cursor themes:** `wl_cursor_theme_load()` creates SHM pools for cursor images
- **Subsurfaces:** Some toolkits use SHM for popups, tooltips, and other non-GL surfaces
- **Fallback rendering:** When EGL is unavailable, clients fall back to SHM software rendering
- **GTK3/4:** Uses SHM for cursor images and sometimes for small auxiliary surfaces
