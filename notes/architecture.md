# Compositor Architecture

## Module Layout

The compositor (`compositor/src/`) is split into:

- **lib.rs** -- JNI entry points + `run_compositor()` which sets up EGL, Wayland display,
  output, and listening socket, then hands off to the event loop.
- **event_loop.rs** -- Calloop-based event loop. Sources: Wayland display fd
  (dispatch client messages), listener socket (accept connections), touch input channel
  (Android touch -> wl_touch), text-input channel, state-query channel, surface-event
  channel (per-Activity `SurfaceView` lifecycle), frame timer (~60 fps render loop).
  Also owns `set_host_foreground` and the per-host render iteration.
- **host.rs** -- `OutputHost` (per-Activity render target — owns ANativeWindow + EGLSurface
  + foreground bool), `SurfaceEvent` (Register / SurfaceChanged / SurfaceDestroyed /
  ActivityDestroyed / FocusChanged), and the JNI -> compositor surface-event channel.
  See [multi-activity.md](multi-activity.md).
- **input.rs** -- Touch input delivery from Android JNI to the compositor via calloop
  channel. Events carry the `ActivityId` of the SurfaceView that produced them.
  Global `OnceLock<Sender>` allows JNI callbacks to send events cross-thread.
- **launcher.rs** -- Walks XDG `applications/` dirs inside a chroot rootfs and
  parses `.desktop` files via the `freedesktop-desktop-entry` crate
  (`default-features = false` to drop gettext-rs's libintl C build, which has no
  pre-built NDK toolchain). Resolves `Icon=` to an absolute on-device PNG path
  by walking the installed icon themes and their `Inherits=` chains; SVG
  sources are rasterized into a per-install cache by **icon_cache.rs**, XPM is
  skipped (see notes/launcher.md "Icon resolution").
  Returns a JSON array string to Kotlin (`LauncherActivity`) via the
  `nativeLauncherScan` JNI entry. No compositor-state interaction — just pure
  file I/O, safe to call from any thread.
- **text_input.rs** -- `zwp_text_input_v3` server impl bridging Android InputConnection.
- **compositor.rs** -- `TawcState` (the single Smithay/calloop state, including `hosts`,
  `single_activity_mode`, `RenderState`, and all Smithay handler trait impls including
  `assign_toplevel_to_host` (the multi-window policy)).
- **desktop.rs** -- `DesktopRegistry`: owns Smithay desktop windows, surface -> host
  assignment, foreground-host selection, and persistent per-host `Space<Window>`
  projections.
- **render.rs** -- `RenderState` (GPU/EGL state), buffer import (AHB + SHM -> GL textures),
  foreground-host frame rendering, Smithay `Window::send_frame` callback dispatch, the SHM
  magenta tint / wlegl-opaque shaders, and `create_egl_surface_for_window` for binding new hosts.
  Each frame is cleared to `BACKGROUND_COLOR` (Material3 dark surface, matches the
  rest of the app's UI) before any surfaces are drawn. GL setup (EGL init + ~14 shader
  programs: ~45 ms phone, ~75 ms emulator) runs on a `gl-init` helper thread via
  `LazyRenderState` and is joined by its first user (buffer creation, host surface bind,
  frame), so `nativeStartCompositor` → dispatch is ~7 ms instead of ~80–100 ms. `init`
  must leave the EGL context unbound for the handoff.
- **egl_android.rs** -- Raw EGL context creation (with `EGL_KHR_surfaceless_context`
  default-current) and `AndroidNativeSurface` for Smithay.
- **wlegl.rs** -- `android_wlegl` server: reconstruct client-allocated gralloc buffers
  into AHardwareBuffers via the C helper, expose them as wl_buffers.
- **gl_import.rs** -- GL/EGL extension loading and texture import. Imports AHardwareBuffers
  as GlesTextures and provides shared GL utilities (dummy textures) to other modules.
- **protocol.rs** -- wayland-scanner generated code for `android_wlegl`.
- **native/wlegl_import.c** -- ~50-line C helper calling
  `AHardwareBuffer_createFromHandle(REGISTER)` (dlsym'd from libnativewindow.so).

Kotlin side (`app/src/main/java/me/phie/tawc/`):

- **MainActivity.kt** -- Home screen (only Activity in `category.LAUNCHER`). Shows
  the one open distro with a Terminal FAB, a drawer to switch distros or
  install one, and ⋮ for Distro info / Run command / Task manager / Settings (notes/android.md
  "Home screen"). Nothing starts the compositor explicitly (see
  "Compositor lifecycle" below); user-launched rootfs commands go through
  `UserRootfsSession`, which holds a session reason for the process's lifetime.
- **launcher/LauncherActivity.kt** -- Per-distro app picker. Reads the rootfs's
  `.desktop` files via [`NativeBridge.nativeLauncherScan`][launcher.rs] (Rust does the
  scan + parsing), shows a type-to-filter list with each entry's icon, fires
  `UserRootfsSession.runInside` on a process-wide `LAUNCH_SCOPE` so the
  launcher Activity can finish without killing the launched program. `Enter`
  launches the top filtered match.
- **launcher/IconLoader.kt** -- Async PNG icon decoder for launcher rows.
  Caches `path → Bitmap` in a byte-bounded `LruCache` (an eighth of the heap,
  floored at ~32 icons) so re-renders on filter keystrokes don't re-decode
  without letting a big distro's icon set grow unbounded;
  `BitmapFactory.inSampleSize` keeps memory bounded for big source PNGs.
  ImageView.tag carries the requested path so a stale completion (rapid filter
  typing) doesn't slam the wrong bitmap into a recycled view.
- **ui/Scaffold.kt** -- Helpers shared by the non-compositor activities — builds the
  `MaterialToolbar` (with back/up arrow on child screens) plus the content column, and
  exposes the button factories. `buildDrawerScreen` is the home variant
  (drawer + FAB overlay, `tawcFab`). Buttons with text keep a visible fill
  (`primaryButton` accent / `destructiveButton` red / `tonalButton` muted;
  `tonalIconButton` is the icon-only filled base). Narrow icon-only buttons use
  `plainIconButton`: no fill, circular ripple, and a 24dp `?attr/colorControlNormal`
  glyph — the same mark a toolbar's own up arrow draws, so back arrows match
  wherever they appear. One deliberate exception: the
  launcher's ⋮ at 21dp (solid dots read heavier than the line icons). Colour
  carries meaning where it did before: the `+` on a bind suggestion is an
  accent-tinted glyph.
- **compositor/CompositorService.kt** -- Bound (never started, not foreground) service
  that owns the Rust compositor thread. Activities bind to it; it tracks them by
  `activityId` so reverse-JNI calls can find the right Activity. A process-level binding
  keeps it alive while the compositor runs — binding, unlike starting, is allowed from
  the background. What keeps the *process* alive is its `Compositor` hold in
  [session-service.md](session-service.md), whose notification also owns "Exit".
- **session/** -- `SessionHolds` + `SessionService`: the one foreground service, up
  exactly while something is alive in a rootfs. See [session-service.md](session-service.md).
- **compositor/CompositorActivity.kt** -- One per Wayland window. Reads `activityId` from
  `intent.data?.lastPathSegment` (UUID under `tawc://activity/<id>`); falls back to
  `"primary"` for the launcher path. Forwards `SurfaceHolder` / touch / focus events
  to native tagged with the id.
- **compositor/NativeBridge.kt** -- JNI surface, `attachService` to capture app context
  for the `spawnActivity` / `finishActivity` reverse-JNI callbacks.

## Key Design Decisions

- `TawcState` is the single calloop data type — protocol state, `RenderState`, the
  `wl_output`, frame counters, etc. all live on it. Upstream smithay's
  `add_pre_commit_hook<D, _>` asserts at runtime that the type passed to handler
  callbacks matches the type `CompositorState` was constructed for, so a wrapper
  struct is not an option. `Display<TawcState>` lives inside a calloop `Generic`
  source rather than on the state struct (avoids a self-referential cycle).
- `dispatch_clients()` runs only from the Wayland fd `Generic` source; the frame
  timer flushes pending writes but does not dispatch. Smithay's calloop integration
  wakes the fd source whenever a client message arrives.
- calloop `LoopHandle` is a strong `Rc` into the loop's own source list, and
  `EventLoop::drop` does not clear that list — so any source callback that
  captures a handle clone leaks every source (Display, client fds, the X11
  lock) past compositor stop, until Android kills the cached process. Two
  guards: TAWC callbacks must reach the loop via `TawcState::loop_handle()`
  instead of capturing clones (convention only — nothing enforces it), and
  the smithay fork's `X11Wm` stores its `RegistrationToken`s plus a
  type-erased remover and deregisters its sources (event channel, focus
  ping, pending selection transfers) in `Drop`, since its callbacks do
  capture handles. Symptom when regressed: after a notification-exit with
  X11 used, the next in-process start logs "Failed to acquire lock
  display=0" and X11 clients get "Can't open display :0"; a clean stop
  logs smithay's "Cleaning up X11 lock.".
- Renderable xdg and X11 windows live in Smithay `Window`s mapped into
  `Space<Window>`. Smithay renderer surface state owns committed buffer
  metadata and texture import; TAWC wraps the resulting render elements only
  to preserve Android buffer tinting and forced-opaque shader policy.

## Compositor lifecycle

Socket-activated, like Xwayland inside it: nothing starts the compositor
explicitly, and an idle one stops.

- **`activation.rs`** binds `share/wayland-0` once per app process and owns
  the listener for the life of the process; it also holds Xwayland's prepared
  `:0` socket whenever no compositor has borrowed it. Started by
  `CompositorService.ensureActivation` (asset extraction + `TAWC_*` env +
  `nativeStartActivation`), which every spawn path calls first
  (`TawcApplication` startup thread, `UserRootfsSession.startInside`,
  `TerminalActivity.spawnSession`) so the sockets are listening before any
  guest exists and extraction never sits in the connect→accept gap. No
  `.lock` file: one process, one holder.
- While no compositor thread exists the holder thread `poll()`s both sockets
  without accepting. Readable → reverse-JNI `onActivationRequested` →
  `CompositorService.ensureRunning`; the connection waits in the listen
  backlog (~7 ms start→dispatch plus the service bind). It sleeps without a
  timeout; a self-pipe wakes it when the Xwayland setting changes, and it
  retries once a second only while `:0` is wanted but could not be prepared.
  It polls a *dup* of the X11 fd, since the original may be borrowed and
  consumed by Xwayland meanwhile.
- A running compositor accepts on its own `try_clone()` of the listener
  (calloop `Generic` → `insert_client`) and `take_x11()`s the X socket;
  on exit it removes its listener source explicitly (the `LoopHandle`
  Rc-cycle below would otherwise let a dead loop steal connections) and
  `return_x11()`s an unused socket. After an Xwayland run consumed it, the
  compositor (or the idle holder) prepares a fresh one.
- **Auto-stop** (`event_loop::check_idle`, frame timer): no Wayland client
  and no Xwayland (it exits by itself 5 s after its last X client) for 1 s.
  Not zero: app startup has short-lived helper connections, and each cycle
  costs a GL context. A connection pending when the thread exits restarts it
  at once — none is refused. The debug `compositor-hold` pins it; the stop
  decision and `nativeStartCompositor`'s "already running" answer share the
  lifecycle lock so they cannot cross.
- **Native state leads.** `nativeStartCompositor` returns whether it spawned
  a thread (Kotlin then re-seeds per-run state: session hold, settings,
  clipboard announce, a fresh `MouseWatcher`) and first waits out a previous
  run's teardown; `nativeStopCompositor` blocks until the thread is fully
  gone (~10–20 ms on the phone); the thread's last act is
  `onCompositorStopped`, ignored if a restart already won. Teardown order in
  `event_loop::run`: clear the JNI channel senders, drop state, then the loop
  (a calloop sender pings its loop on drop and warns if it is gone).
  `LazyRenderState` joins its `gl-init` thread on drop so a run that never
  touched GL still destroys its context before reporting itself gone.
- **Selection-only clients** (`wl-copy`'s daemon) never leave by themselves.
  When no client has a `wl_surface`, the live selection is a Wayland
  client's, and its mirror into Android completed
  (`TawcState::selection_mirrored`), the compositor installs the payloadless
  Android selection: the owner gets `cancelled` and exits, pastes are served
  from Android. An unmirrored selection (non-text, over cap) keeps its owner
  and pins the compositor — it is the only copy. Not exercised end to end
  yet: `wl-copy` itself fails earlier, see `plans/wl-clipboard-support.md`.
- The kumquat listener is one thread for the life of the process.
- Verified by `lazy_compositor::*`: Wayland and X11-only cold start, idle
  stop, restart, and flat fd/thread counts over start/stop cycles.

## Wayland Protocols Implemented

- `wl_compositor`, `xdg_wm_base` (v6), `wl_shm`, `wl_seat`, `wl_output`
- `wp_viewporter` (required by Firefox/WebRender — see rendering notes)
- `xdg_decoration` and KDE server-decoration (desktop titlebars suppressed)
- `wl_data_device_manager` (clipboard/DnD, stub implementations)
- `zwp_text_input_v3` (custom impl bridging Android IME)
- `android_wlegl` (libhybris's standard GPU buffer sharing protocol; client-side
  allocation only — `get_server_buffer_handle` is rejected)

**Socket path:** `/data/data/me.phie.tawc/share/wayland-0` -- app's own data dir
under the `share/` subdir, which each install method binds at
`/usr/share/tawc` inside the rootfs (see notes/installation.md "/usr/share/tawc").
Uses `ListeningSocket::bind_absolute()`.

## Smithay Integration

Using Smithay 0.7 from a custom fork (`wmww/smithay`, branch `tawc-patches`),
pinned in `deps/deps.list` and consumed via `[patch.crates-io] smithay = { path =
"../deps/smithay" }` in `compositor/Cargo.toml`. Gradle's `setupSmithay` task
clones the checkout ahead of `buildRustLibrary*`; standalone `cd compositor &&
cargo build` requires running `scripts/ensure-deps.sh smithay` first.

**Features:** `wayland_frontend`, `renderer_gl`, `desktop`. Avoids all Linux-specific
backends (DRM, GBM, libinput, udev, libseat).

**Patches:** Android loads `libEGL.so` instead of `libEGL.so.1`
(`src/backend/egl/ffi.rs`), and TAWC adds a generic external-buffer renderer
hook so AHB-backed `wl_buffer`s can populate Smithay renderer state without
Smithay linking Android APIs.

**wayland-rs:** Pure Rust Wayland protocol implementation (do NOT enable `server_system`
feature -- no `libwayland-server.so` needed).

**Key APIs used:**
- `EGLContext::from_raw(display, config_id, context)` -- wraps pre-existing EGL context
- `GlesRenderer` -- composites GL textures. AHB import still lives in TAWC
  (`gl_import.rs`), exposed to Smithay as an external buffer import hook.
- `DisplayHandle::insert_client(stream, data)` -- accepts new Wayland client connections

**Native dependencies:**
| Dependency | Status |
|---|---|
| EGL/GLESv2 | Provided by Android NDK |
| libxkbcommon | Cross-compiled for aarch64-linux-android |
| libwayland | Not needed (pure Rust backend) |
| libdrm/libgbm | Not needed (disabled features) |

## Toplevel Lifecycle

Smithay owns xdg toplevel lifetime in `XdgShellState`; TAWC reads
`toplevel_surfaces()` when it needs to configure or close host-local windows and
uses `XdgShellHandler::toplevel_destroyed` for cleanup. Do not add a parallel
xdg toplevel list in `TawcState`.

## Known Risks and Constraints

| Risk | Status | Mitigation |
|---|---|---|
| libhybris can't load stock EGL/GLES | ✅ Solved | Battle-tested (Sailfish OS). Proven in Phase 2. |
| libhybris Vulkan varies by vendor | Open | Stretch goal. EGL/GLES covers most apps. Mali needs unmerged PRs (#604, #607). |
| Stock driver needs Binder/gralloc | ✅ Solved | Bind-mount `/vendor`, `/system`, `/system_ext`, `/dev/binderfs`. |
| `eglGetNativeClientBufferANDROID` not available | Low risk | Widely available Android 8+. Runtime-check required. |
| `android_wlegl` is libhybris-specific | Accepted | All chroot apps reach the GPU via libhybris; `wl_shm` is the standard fallback. |
| SELinux blocks socket without root | Documented | Root: direct connect. No-root: Binder fd passing. |
| Smithay on Android | ✅ Solved | `default-features = false`, patched EGL loader. Proven in Phase 1. |
| Phantom Process Killer (Android 12+) | Open | Users need Developer Options toggle for Termux processes. |
| Vendor-specific GPU quirks | Open | Architecture is vendor-neutral. Each vendor needs testing. `wl_shm` fallback. |
| Freeform windowing not universal | Accepted | Phones: fullscreen Activities. Freeform on DeX/ChromeOS/Android 15+. See [multi-activity.md](multi-activity.md). |

## References

- [wmww/libhybris](https://github.com/wmww/libhybris) -- our fork of libhybris (bionic compatibility layer), local checkout at `./deps/libhybris`
- [Smithay](https://github.com/Smithay/smithay) -- Rust Wayland compositor library
- [Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge)
- [AHardwareBuffer NDK docs](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [Vulkan Layer mechanism](https://vulkan.lunarg.com/doc/view/latest/linux/loader_and_layer_interface.html)
- [ndk crate](https://crates.io/crates/ndk) -- Rust NDK bindings
