# Testing Infrastructure

## Overview

Automated integration tests for the compositor. Each `tests/<name>.rs`
file is a submodule of a single `tests/integration.rs` test binary, so
`cargo test` produces one combined libtest summary and selecting a
subset is just a libtest substring filter (`<module>::test_name`).

```
tests/
  apps/gtk4-debug-app/        C + GTK4, runs on phone in chroot, structured stdout
  integration/                Rust tests on host
    tests/integration.rs        single test binary, declares the submodules
    tests/<module>.rs           one file per group; see its docstring
scripts/
  build-debug-app.sh          Manual debug-app build script
  run-integration-tests.sh    Build everything, deploy, run integration tests
```

Each test module's own docstring documents what it covers and what its
prerequisites are. As of writing the modules are:

| Module          | Backend pin | Scope |
|-----------------|-------------|-------|
| `apps`          | `cpu`       | App-level smoke: program launches, maps a toplevel, and (optionally) does something simple. **No buffer-type assertions.** Pair tests here with a deeper one in the per-backend modules when a buffer-path regression is worth catching separately. |
| `hybris`        | `libhybris` | TLS / bionic-linker regressions plus every "X renders via hardware buffers through libhybris" smoke — `weston-simple-egl`, `vkcube`, GTK3/4, Firefox, supertuxkart, plus `vulkaninfo`/`eglinfo` sanity. |
| `libhybris_zink` | `libhybris-zink` | Representative libhybris+Zink coverage: Vulkan still through libhybris, EGL renderer must be Zink rather than llvmpipe, GTK4 should land as AHB when a capable device exists. Currently hardware-blocked on our Vulkan 1.1 Adreno devices; see [libhybris-zink.md](libhybris-zink.md). |
| `gfxstream`     | `gfxstream` | Experimental bridge-backend coverage plus an `eglinfo` software-fallback guard. Vulkan-native `vkcube` works today and runs unconditionally; GL/EGL app tests are gated on the remaining Zink-on-gfxstream work in [gfxstream-bridge.md](gfxstream-bridge.md). Surface ignored cases with `cargo test -- --ignored`. |
| `cpu_graphics`  | `cpu`       | Backend-agnostic SHM paths under software-only rendering: `weston-simple-shm`, GTK3 with `GDK_GL=disabled`, GTK4 with `GSK_RENDERER=cairo`, plus an `eglinfo` llvmpipe/swrast sanity. |
| `xwayland`      | `libhybris` | Anything that drives the bionic-built Xwayland binary — pure-X11 clients, TAWC-DRI AHB round-trips, libhybris's X11 EGL plugin. The Xwayland integration is libhybris-native; no analogue under gfxstream / cpu. |
| `input`         | `cpu`       | gtk4-debug-app driven through compositor input dispatch (text-input-v3, wl_keyboard, touch). Buffer type is irrelevant for input. |
| `tawcroot`      | n/a         | tawcroot device-side smokes (wraps the cleat-driven suite). |

The **backend pin** for each module is enforced at every spawn: tests
in `hybris::` call `RootfsProcess::spawn_with(GraphicsBackend::Libhybris, …)`
(and the corresponding `launch_and_wait_for_*` / `assert_renders_via_*`
variants), `libhybris_zink::` pins `LibhybrisZink`, `gfxstream::` pins
`Gfxstream`, `cpu_graphics::` / `apps::` / `input::` pin `Cpu`,
`xwayland::` pins `Libhybris`. The
broker carries the override through to `InstallationMethod.startInside`
on every spawn (`GRAPHICS <key>` header on RUNINSIDE, see
[exec-broker.md](exec-broker.md)) — the user's persisted
`Settings.graphicsBackend` (the in-app Settings screen pick) is left
untouched, so a single suite run exercises every backend without a
global flip.

**Where does this app go?** Apps that need both a launch smoke and a
buffer-path assertion get two tests — one in `apps::` (just maps a
window) and one in the matching deeper module. Real-toolkit AHB
smokes (Firefox / GTK / STK) live in **both** `hybris::` and
`gfxstream::` so a regression in one backend doesn't accidentally
hide behind the other; SHM smokes live only in `cpu_graphics::` (the
compositor's SHM plumbing doesn't depend on the chroot's graphics
env). Apps where buffer type is irrelevant (e.g. `lxterminal` driving
text input) get a single `apps::` entry.

## Debug App (`gtk4-debug-app`)

A small C program built against GTK4 that exposes a subcommand CLI and
emits structured `TAWC_DEBUG:` lines for the test harness to parse. Builds
inside the chroot with `gcc` + `pkg-config --cflags --libs gtk4`.

### Output Protocol

Every test-relevant line is prefixed `TAWC_DEBUG:` to filter from GTK/Wayland noise:
```
TAWC_DEBUG:READY                    Window mapped, text view focused
TAWC_DEBUG:TEXT_CHANGED:<text>      Full buffer contents after change
TAWC_DEBUG:CURSOR_POS:<offset>      Cursor position (character offset) after mark-set
TAWC_DEBUG:PREEDIT:<text>           Current composing/preedit string
TAWC_DEBUG:RENDERER:<class>         GSK renderer class name
TAWC_DEBUG:VULKAN_LOADED:yes|no     Whether libvulkan was mapped at READY time
```

### Commands

| Command | Description |
|---------|-------------|
| `text-input` | Opens a GtkTextView, reports text changes / cursor moves |

### Building

```bash
# From host (pushes source, builds in chroot):
scripts/build-debug-app.sh

# Or manually in chroot:
cd /tmp/gtk4-debug-app && ./build.sh
```

The Rust harness also calls `chroot::ensure_debug_app()` automatically with
freshness caching, so the explicit build step is only needed for ad-hoc
manual runs.

### Running Manually

```bash
scripts/rootfs-run.sh '/tmp/gtk4-debug-app/gtk4-debug-app text-input'
```

## Integration Tests

Rust tests using `std::process::Command` to call adb. Zero external
runtime dependencies on the host.

### Running

`scripts/run-integration-tests.sh` is the recommended entry point. It
picks the device via `scripts/lib/select-device.sh` (resolves
`./.tawctarget` / `TAWC_TARGET`; no auto-fallback to a single
connected target — see [emulator.md](emulator.md)), builds and
deploys everything, then runs cargo.

The script takes one optional positional arg — a libtest substring
filter forwarded to `cargo test` — plus `--no-build` / `-n`.

```bash
scripts/run-integration-tests.sh                 # everything
scripts/run-integration-tests.sh <module>::      # one module's tests
scripts/run-integration-tests.sh <test_name>     # one test by name
scripts/run-integration-tests.sh --no-build ...  # skip rebuild/redeploy
```

Direct `cargo test` invocations work too — they just need
`source scripts/lib/select-device.sh` first so cargo inherits
`ANDROID_SERIAL`. The source step also enforces the standing target,
so a stale `cargo test` from a `.tawctarget=none` checkout fails fast
instead of attaching to the wrong target.

Prerequisites: a phone (or emulator) connected via adb, the tawc app
installed, an in-app distro installed (see [installation.md](installation.md)),
and the test suite's chroot packages **and binaries** installed by
`scripts/install-test-deps.sh`. That script installs the runtime
package set (gtk3/gtk4/weston/mesa-utils/vulkan-tools/…) plus a C
toolchain, then compiles every in-rootfs test program from
`tests/apps/<name>/` into `/tmp/<name>/<name>` inside the rootfs.
**Re-run install-test-deps after editing any `tests/apps/<name>/*`
source** — tests check the binaries exist and refuse to run if not,
they do not compile anything at runtime. The suite auto-targets the
unique install if there's only one, otherwise pin via
`TAWC_INSTALL_ID=<id>`. Some modules have additional prerequisites (e.g.
libhybris on a real device for the GPU-rendering tests); see each
module's docstring.

### Test Input Mechanism

Tests inject input by acting as a **keyboard**: every input action calls a method on the active `TawcInputConnection` via the broker `ic-*` actions. There is intentionally no test path that pokes `NativeBridge.native*` directly — see `notes/text-input.md` "Test infrastructure note" for the rationale.

```bash
# Stop the system IME from reacting to updateSelection / showSoftInput.
scripts/tawc-exec.sh --action enable-test-input

# Drive the IC: commit text, set preedit, send a key, etc.
scripts/tawc-exec.sh --action ic-commit-text --arg text=hello
scripts/tawc-exec.sh --action ic-set-composing-text --arg text=wor
scripts/tawc-exec.sh --action ic-finish-composing
scripts/tawc-exec.sh --action ic-send-key-event --arg keycode=67  # Backspace
```

Every call goes through the same Kotlin entry points the system IMM uses to dispatch Gboard / OpenBoard / AOSP-latin events, so the IC's full state machine (Editable mirror, `computeReplaceDeltas`, the `composingRegionIsPreedit` short-circuit) runs on every test step.

Broker actions connect to an already-running `LocalServerSocket` and complete in <10ms each, vs. 100–300ms per `am broadcast` JVM cold start (the broadcast channel was retired entirely). More reliable than `adb shell input text` (which gets intercepted by the IME).

### Architecture

```
Host (cargo test)                    Phone
  │                                    │ (test programs already compiled
  │                                    │  by install-test-deps; the
  │                                    │  harness only checks they exist)
  ├─ adb shell (start client) ─────────┤──→ gtk4-debug-app  /  firefox  /  …
  │     └─ piped stdout ←──────────────┤     └─ TAWC_DEBUG:READY (debug app only)
  │                                    │
  ├─ tawc-exec --action ic-commit-text ┤──→ ExecBroker / InputActions
  │                                    │     └─ TawcInputConnection.commitText
  │                                    │       └─ nativeCommitText
  │                                    │         └─ text_input_v3
  │                                    │           └─ GTK text view
  │     └─ TAWC_DEBUG:TEXT_CHANGED ←───┤
  │                                    │
  ├─ adb shell input tap X Y ──────────┤──→ SurfaceView.onTouchEvent
  │                                    │     └─ nativeOnTouchEvent
  │                                    │       └─ wl_touch → GDK_TOUCH_BEGIN
  │                                    │         └─ GtkGestureMultiPress
  │                                    │           └─ cursor move
  │     └─ TAWC_DEBUG:CURSOR_POS ←─────┤
  │                                    │
  └─ assert text/cursor == expected    │
```

### Key Modules

- **`adb.rs`**: Shell commands, chroot execution, broker-action-based input injection (`input_text`, `ic_commit_text`, `enable_test_input`, …; all routed through `tawc-exec --action`)
- **`rootfs.rs`**: `ensure_debug_app` / `ensure_tawc_dri_test` /
  `ensure_eglx11_test` — each one just probes for `/tmp/<name>/<name>`
  inside the rootfs and returns its path, errorring with a pointer at
  `scripts/install-test-deps.sh` if the binary is missing. Tests do
  **not** compile anything; they also do not install chroot packages.
  Both happen up-front in `scripts/install-test-deps.sh`.
- **`debug_app.rs`**: Start/stop lifecycle, stdout reader thread, `wait_for()` with timeout
- **`compositor.rs`**: Check whether the compositor is running (`is_running`,
  `assert_running`) and query its state via the broker `query-state` action. The compositor itself
  is launched by `run-integration-tests.sh` before `cargo test` runs — the
  Rust harness never starts it, only asserts it's there.
- **`helpers.rs`**: Shared test helpers. Every spawn helper takes an
  explicit `GraphicsBackend` so the in-rootfs env is hermetic — the
  user's UI pick never leaks in.
  - `require_compositor`, `assert_compositor_clean`, `saw_ahb_import`,
    `saw_shm_import` — observation primitives.
  - `start_text_input` / `start_text_input_no_surrounding` — for
    `input::`.
  - `launch_and_wait_for_toplevel(backend, …)` — for `apps::`. Waits
    until the client has committed its first frame regardless of
    buffer type.
  - `launch_and_wait_for_ahb(backend, …)` — for the per-backend
    hardware-buffer tests that need to keep the process alive after
    first paint (e.g. Firefox's steady-state surface-count check,
    `vkcube`'s animating check). Returns the still-running
    `RootfsProcess`.
  - `assert_renders_via_shm(backend, cmd, name, timeout)` — one-call
    SHM smoke: spawn, wait for SHM import, assert no AHB, assert ≥1
    toplevel, stop cleanly, assert clean. The body of every
    forced-SHM test reduces to this single call.
  - `assert_renders_via_ahb(backend, cmd, name, timeout)` — same
    shape for the AHB fast path. Bespoke tests that need extra
    steady-state checks (Firefox, vkcube) keep using
    `launch_and_wait_for_ahb` directly.

  `require_compositor` panics with a clear message if the compositor
  isn't running, telling the developer to use the run script instead
  of invoking `cargo test` directly. The OnceLock state means
  one-time setup (debug-app build) runs once per `cargo test` invocation.

## Adding New Tests

Add to an existing `tests/<module>.rs` if it fits an existing group, or
create a new module: drop `tests/<new>.rs` next to the others and add a
`mod <new>;` line to `tests/integration.rs`. Tests pick up the module
prefix automatically and the run script's substring filter just works.

If a new compositor protocol is needed, extend `gtk4-debug-app.c`:

1. Add a new command (new function + entry in `commands[]`).
2. Define protocol messages (`TAWC_DEBUG:YOUR_EVENT:value`) and a matching
   parser in `debug_app.rs`.
3. Use the same `DebugApp` harness (`start`, `wait_ready`, `wait_for`).

**GTK4 gotcha:** `gtk4-debug-app` defers its `READY` emission to the next
idle so the IM context has time to enable `zwp_text_input_v3` before the
harness starts injecting text. If you add new commands that rely on
text input, keep the deferred-READY pattern in `on_map()`.

## Design Decisions

- **One test binary, submodules per group:** Each group lives in its
  own file but they all compile into the same `tests/integration.rs`
  binary, so libtest prints one combined `test result: ...` summary
  and the run script picks subsets via the normal substring filter
  (`<module>::`). Avoids the per-binary summary fragmentation cargo
  produces when each `tests/*.rs` is its own target. `Cargo.toml` has
  `autotests = false` plus an explicit `[[test]]` entry so the
  per-group files aren't auto-discovered as separate binaries.
- **C for debug app:** Sub-second builds, no cargo on phone, `base-devel` already in chroot.
- **Broker `ic-*` actions over `adb shell input text`:** The system IME
  can intercept `input text` key events and buffer/autocorrect them.
  Broker actions drive `TawcInputConnection`, the same Kotlin state
  machine real IME input uses, without starting a broadcast JVM for
  every operation.
- **Reader thread + mpsc channel:** adb stdout is a blocking stream.
  Thread drains it continuously, mpsc gives timeout-based waiting.
- **Process-group kill in `ChrootProcess`:** Process chain
  `adb -> su -> bash -> chroot -> bash -> app` doesn't propagate signals;
  Firefox spawns content processes in their own PGIDs. We track the root
  PID via a small pidfile helper, walk descendants, and signal each PGID.
- **`--test-threads=1`:** Tests share the phone and compositor and can't
  run in parallel.
