# Firefox on tawc - Progress Report

## Current Status (2026-03-31)

Firefox 149 (Arch Linux ARM aarch64) is installed in the chroot and **renders via SHM buffers** visible in the compositor (with magenta SHM tint). Requires `setenforce 0` (SELinux permissive) because Firefox/GDK's memfds bypass the LD_PRELOAD SELinux shim (see notes.md).

**GTK3 GPU rendering is fully working** - confirmed with gtk3-widget-factory rendering via AHB buffers through our tawc-egl wrapper.

## What Works

1. **Firefox installs and launches** - `pacman -S firefox` in the chroot, runs with `MOZ_ENABLE_WAYLAND=1`
2. **Wayland connection** - Firefox connects, binds all required globals (wl_compositor, xdg_wm_base, wl_shm, wl_seat, wl_output, wl_data_device_manager, tawc_buffer_manager_v1)
3. **Window creation** - Creates xdg_toplevel with title "Mozilla Firefox" (requires pointer capability on wl_seat -- see fixes below)
4. **SHM buffer rendering** - Firefox renders via software (SHM buffers), visible with magenta tint in compositor. First frame confirmed 972x1040.
5. **Multi-process** - Firefox content processes connect as separate Wayland clients
6. **Stability** - Firefox runs without crashes (with all sandbox vars disabled)
7. **GTK3 GPU rendering** - gtk3-widget-factory renders correctly via AHB (confirmed working screenshot)

## Compositor Fixes Made

### 1. wl_seat pointer capability (CRITICAL for Firefox)

**Problem:** Firefox refuses to create an xdg_toplevel window if the compositor's wl_seat advertises 0 capabilities (no pointer, keyboard, or touch). The Wayland debug showed Firefox binding globals and creating surfaces but never calling `xdg_wm_base.get_xdg_surface()`.

**Fix:** Added `seat.add_pointer()` in `compositor.rs` TawcState::new(). Now the seat advertises `capabilities(1)` (WL_SEAT_CAPABILITY_POINTER), and Firefox proceeds to create its window.

### 2. Physical output dimensions

**Problem:** Output physical size was (0, 0) mm, which could cause Firefox to compute 0 DPI.

**Fix:** Changed to (68, 150) mm in `lib.rs` (approximate for a 6.7" 1080x2400 phone).

### 3. SHM buffer import via surface tree scan

**Problem:** Reading `cached_state.get::<SurfaceAttributes>().current().buffer` in `CompositorHandler::commit()` always returned `None` due to Smithay's transaction system not having applied state yet at that point. The buffer IS visible in `current()` after `dispatch_clients()` returns.

**Fix:** Moved SHM buffer detection to `import_shm_from_surface_trees()`, a tree scan that runs after `dispatch_clients()` in the render loop. Tracks buffer identity (`buf.id()`) to avoid double-import, releases old buffers only when replaced by new ones.

### 4. SHM texture vertical flip

**Problem:** SHM buffers rendered upside-down (toolbar at bottom instead of top).

**Fix:** Changed SHM rendering transform from `Transform::Normal` to `Transform::Flipped180`. This is correct because SHM buffers have top-left origin while GL textures have bottom-left origin. AHB textures (GL_TEXTURE_EXTERNAL_OES) handle Y-flip internally in the driver, so they remain `Transform::Normal`.

### 5. Frame callbacks for SHM surfaces

**Problem:** Firefox needs frame callbacks on its rendering surfaces to know when to submit new frames.

**Fix:** Frame callbacks are sent to all toplevel surface trees (which includes subsurfaces) via `send_frames_surface_tree()`.

### 6. Calloop event loop (2026-04-01 refactor)

**Problem:** The original raw `poll()` event loop didn't properly integrate with
smithay's Wayland dispatch. This led to missed client messages and a cascade of
hacks: double `dispatch_clients()` calls, output mode toggling to "wake up" clients,
and an LD_PRELOAD flush shim for Firefox.

**Fix:** Replaced with a proper calloop event loop (the standard smithay pattern).
Three event sources: display fd (dispatch client messages + flush responses), listener
socket (accept connections), frame timer (~60fps render loop). All the hacks were
deleted. Firefox and GTK3 work without any shims — the real bug was SELinux blocking
memfd sharing, not the event loop.

## Key Findings

### Firefox falls back to software rendering

LD_DEBUG tracing confirmed: **Firefox never dlopen's libEGL.so at all** in our environment. It only loads `libwayland-egl.so.1`. Firefox normally uses GL/Vulkan via WebRender on most systems, but something in our environment causes its GPU detection to fail, so it falls back to SHM (software) rendering.

Possible reasons Firefox doesn't attempt EGL:
- Our tawc-egl wrapper may not be detected during Firefox's GPU capability probing
- Firefox may probe EGL before `LD_LIBRARY_PATH` takes effect (e.g., during early startup)
- The libhybris EGL may report capabilities that Firefox considers unsupported
- Firefox's `about:support` Graphics section would reveal the exact reason, but we can't access it without a working interactive session

Getting Firefox to use GPU rendering via our tawc-egl wrapper remains a goal -- SHM is a stepping stone.

### Firefox Wayland Proxy

Firefox 149 uses a built-in Wayland proxy (`libmozwayland.so`). During early testing, the proxy reported:
```
Wayland Proxy Error: Failed to connect to Wayland display '/data/data/me.phie.tawc/wayland-0' error: No such file or directory
```

This was likely caused by the compositor data dir bind mount not being set up yet, not by the absolute path itself. The error resolved when the chroot mounts were properly established. We currently use a symlink at `/tmp/wayland-0` for convenience, but absolute paths should work too as long as the socket is accessible inside the chroot.

### Mount table explosion (FIXED)

The `arch-chroot-run` script caused exponential mount accumulation (24K+ mounts observed) due to two issues:
1. `mountpoint -q` doesn't detect file bind mounts, causing re-mounting on every invocation
2. `mount --rbind /apex` with shared propagation creates feedback loops where new mounts on the host cascade through all bind copies

**Fix:**
- Replaced `mountpoint -q` with `grep -q ' $path ' /proc/mounts` for reliable detection
- Added `rslave` propagation to all bind mounts (`mount -o bind,rslave` / `mount -o rbind,rslave`) to prevent feedback loops. Uses toybox's `-o` syntax since Android's mount doesn't support `--make-rslave`.

## Launch Commands

### GTK3 apps (working, GPU-accelerated):
```bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/tmp
export LD_LIBRARY_PATH=/tmp/tawc-wsi:/usr/local/lib
export LD_PRELOAD=/tmp/memfd-selinux-shim/libmemfd-selinux-shim.so
export HYBRIS_PATCH_TLS=1
export HOME=/root
export GDK_GL=gles:always
gtk3-widget-factory
```

### Firefox (SHM rendering, working):
```bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/tmp
export LD_LIBRARY_PATH=/tmp/tawc-wsi:/usr/local/lib
export LD_PRELOAD=/tmp/memfd-selinux-shim/libmemfd-selinux-shim.so:/tmp/libwayland-flush-shim.so
export HYBRIS_PATCH_TLS=1
export MOZ_ENABLE_WAYLAND=1
export HOME=/root
export GDK_GL=gles
export MOZ_DISABLE_CONTENT_SANDBOX=1
export MOZ_DISABLE_GMP_SANDBOX=1
export MOZ_DISABLE_RDD_SANDBOX=1
export MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1
export DISPLAY=
# Symlink must exist: ln -sf /data/data/me.phie.tawc/wayland-0 /tmp/wayland-0
# Flush shim must be built: see client/wayland-flush-shim/
firefox --no-remote
```

## Known Issues

### Keyboard support
`seat.add_keyboard()` crashes the compositor because Smithay uses xkbcommon which needs keymap data files. Possible fixes:
- Bundle minimal xkb data in the APK assets
- Create a custom keymap in code without relying on system files
- Use `XKB_DEFAULT_RULES=""` or similar env vars
- Cross-compile xkbcommon with embedded data

### Firefox sandbox
All Firefox sandbox must be disabled (`MOZ_DISABLE_CONTENT_SANDBOX=1` etc.). The chroot environment doesn't support the clone/namespace operations Firefox's sandbox requires.

## Client Output Buffer Flush Deadlock (SOLVED)

**Root cause identified (2026-03-31):** Firefox and GTK3 encode `wl_surface.attach()` + `wl_surface.commit()` into their Wayland output buffer, but **never flush the buffer to the server socket**. This creates a deadlock:

1. Client renders and encodes attach+commit into libwayland's output buffer
2. Client's event loop calls `poll()` waiting for server events
3. Server's `dispatch_clients()` calls `epoll_wait()` — returns 0 (no client data)
4. Server has no events to send (no frame callback requests pending, configure already sent)
5. Both sides wait forever

**Evidence:**
- `strace` on compositor: `epoll_pwait(128, [], 32, 0, NULL, 8) = 0` — no client data
- `strace` on Firefox: `ppoll([{fd=11, events=POLLIN}], ...)` — blocked waiting for server events
- WAYLAND_DEBUG shows `-> wl_surface#16.attach(wl_buffer#35, 0, 0)` — client ENCODES attach
- But the attach never reaches the server (Smithay's `wl_surface.attach` handler never called)
- A test client with explicit `wl_display_flush()` works perfectly

**Test client proof:** `client/test-shm-client.c` creates an SHM buffer, attaches, commits, and explicitly calls `wl_display_flush()`. The compositor successfully imports the buffer (visible in logs: `commit[6]: current=NewBuffer`).

**Server-side mitigations (necessary but not sufficient alone):**
- Sending `output.change_current_state()` periodically — generates wl_output events that keep client event loops active (prevents full deadlock on client poll)
- Polling the Display fd with timeout — ensures `dispatch_clients()` properly picks up client data when it arrives
- `flush_clients()` after all event sends — ensures server events reach the socket

**Detailed analysis (2026-03-31):**

The pure Rust wayland-backend correctly:
- Write events to `BufferedSocket.out_data` (confirmed via `__android_log_write` instrumentation in `client.rs`)
- Flush events to sockets via `sendto()` (confirmed via strace: 32-byte `sendto` calls happen every 0.5s for output mode events)
- Deliver events to Firefox's Wayland proxy (confirmed via strace: Firefox reads 64-byte messages via `recvmsg(fd=12)`)

Firefox's Wayland proxy then:
- Reads compositor events (output mode changes) from fd 12
- Forwards them to internal Firefox processes via `sendmsg(fd=72/121/142/145/147)`
- But **NEVER sends data back to the compositor on fd 12**

The proxy's event loop polls `{fd=8(eventfd), fd=12(compositor), fd=34(pipe)}`. Only fd 12 triggers (POLLIN from compositor events). The eventfd (fd 8), which should signal when internal clients have outbound data, never fires. This means internal Firefox processes encode attach+commit but the proxy never forwards them to the compositor.

With `MOZ_DISABLE_WAYLAND_PROXY=1`, Firefox's main process connects directly. The compositor sends output mode events which arrive. But Firefox STILL doesn't flush its outbound buffer — it uses a non-standard Wayland dispatch mechanism that doesn't call `wl_display_flush()` after processing events.

An `LD_PRELOAD` shim intercepting `wl_display_dispatch_pending()` was tried but Firefox doesn't call these standard APIs.

**Solution:** `client/wayland-flush-shim/flush-shim.c` — LD_PRELOAD library that hooks `wl_display_connect()` to capture the display pointer, then starts a background thread calling `wl_display_flush()` (resolved via `dlsym(RTLD_DEFAULT)` to get `libmozwayland.so`'s own version) every 100ms. This forces Firefox's output buffer to be written to the socket. **Confirmed working.**

Standard LD_PRELOAD interception of `wl_display_read_events()` / `wl_display_dispatch_pending()` does NOT work because Firefox uses `libmozwayland.so`'s internal copies of these functions, not the system `libwayland-client.so`. However, `wl_display_connect` IS interceptable (Firefox's loader resolves it from the LD_PRELOAD before libmozwayland).

**Alternative approaches not pursued:**
- Cross-compile libwayland-server for Android/bionic to use `wayland-server = { features = ["system"] }` (the chroot's glibc version can't be used in the Android app)
- Patch wayland-backend to bypass epoll (tried, doesn't help — the root cause is client-side)

## SHM Buffer Import (FIXED)

The SHM buffer import pipeline now works correctly (verified with test client):
- `import_shm_from_surface_trees()` scans toplevel surface trees via `with_surface_tree_downward`
- Compares buffer identity (`buf.id()`) to avoid double-import
- Releases old buffers when replaced by new ones
- Properly handles subsurface positioning during rendering

## Next Steps (Priority Order)

1. **Add keyboard support** — Needed for any interactive use of Firefox. `seat.add_keyboard()` currently crashes (xkbcommon needs keymap files).

2. **Continuous rendering** — Firefox currently renders initial frames but may stop. Need to verify frame callback flow keeps Firefox's rendering pipeline active.

3. **GPU rendering** — Getting Firefox WebRender to use our tawc-egl wrapper. GTK3 GPU rendering already works via AHB. Firefox currently falls back to software/SHM because it never opens libEGL.so.
