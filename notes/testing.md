# Testing Infrastructure

## Overview

Automated integration tests for the compositor. Each `tests/<name>.rs`
file is a submodule of a single `tests/integration.rs` test binary, so
`cargo test` produces one combined libtest summary and selecting a
subset is just a libtest substring filter (`<module>::test_name`).

```
tests/
  apps/wayland-debug-app/     C + Wayland/Cairo protocol test client
  integration/                Rust tests on host
    tests/integration.rs        single test binary, declares the submodules
    tests/<module>.rs           one file per group; see its docstring
scripts/
  run-integration-tests.sh    Build everything, deploy, run integration tests
```

Each test module's own docstring documents what it covers and what its
prerequisites are. As of writing the modules are:

| Module          | Backend pin | Scope |
|-----------------|-------------|-------|
| `apps`          | `cpu`       | App-level smoke: program launches, maps a toplevel, and (optionally) does something simple. **No buffer-type assertions.** Pair tests here with a deeper one in the per-backend modules when a buffer-path regression is worth catching separately. |
| `libhybris`     | `libhybris` | TLS / bionic-linker regressions plus every "X renders via hardware buffers through libhybris" smoke — `weston-simple-egl`, `vkcube`, GTK3/4, Firefox, supertuxkart, plus `vulkaninfo`/`eglinfo` sanity. Skipped by the runner on x86 devices. |
| `libhybris_zink` | `libhybris-zink` | Representative libhybris+Zink coverage: Vulkan still through libhybris, EGL renderer must be Zink rather than llvmpipe, GTK4 should land as AHB when a capable device exists. Currently hardware-blocked on our Vulkan 1.1 Adreno devices; see [libhybris-zink.md](libhybris-zink.md). Skipped by the runner on x86 devices. |
| `gfxstream`     | `gfxstream` | Experimental bridge-backend coverage plus an `eglinfo` software-fallback guard. Vulkan-native `vkcube` works today on physical devices; GL/EGL app tests are gated on the remaining Zink-on-gfxstream work in [gfxstream-bridge.md](gfxstream-bridge.md). Surface ignored cases with `cargo test -- --ignored`. Skipped by the runner on emulator targets. |
| `cpu_graphics`  | `cpu`       | Backend-agnostic SHM paths under software-only rendering: `weston-simple-shm`, GTK3 with `GDK_GL=disabled`, GTK4 with `GSK_RENDERER=cairo`, plus an `eglinfo` llvmpipe/swrast sanity. |
| `xwayland`      | mixed       | Anything that drives the bionic-built Xwayland binary. Pure-X11 SHM smoke uses `cpu` and runs on x86; TAWC-DRI AHB round-trips and libhybris's X11 EGL plugin use `libhybris` and are skipped on x86 devices. |
| `text_input`    | `cpu`       | wayland-debug-app text-input-v3, wl_keyboard, clipboard, and cursor-tap coverage. Buffer type is irrelevant. |
| `touch_input`   | `cpu`       | wayland-debug-app wl_touch routing coverage, including subsurfaces and popups. Buffer type is irrelevant. |
| `settings`      | `cpu`       | Runtime settings coverage: output scale, configure-state policy, and GTK3 broken menus workaround. |
| `tawcroot`      | n/a         | tawcroot device-side smokes (wraps the cleat-driven suite). |

The **backend pin** for each module is enforced at every spawn: tests
in `libhybris::` call `RootfsProcess::spawn_with(GraphicsBackend::Libhybris, …)`
(and the corresponding `launch_and_wait_for_*` / `assert_renders_via_*`
variants), `libhybris_zink::` pins `LibhybrisZink`, `gfxstream::` pins
`Gfxstream`, `cpu_graphics::` / `apps::` / `settings::` /
`text_input::` / `touch_input::` pin `Cpu`, and `xwayland::` uses
`Cpu` for pure-X11 SHM plus `Libhybris` for AHB/EGL-on-X11. The
broker carries the override through to `InstallationMethod.startInside`
on every spawn (`GRAPHICS <key>` header on RUNINSIDE, see
[exec-broker.md](exec-broker.md)) — the user's persisted
`Settings.graphicsBackend` (the in-app Settings screen pick) is left
untouched, so a single suite run exercises every backend without a
global flip.

`scripts/run-integration-tests.sh` marks unsupported target/backend
combinations ignored via conditional libtest attributes: active
`gfxstream::` tests on emulator targets, and active libhybris-backed
tests on x86 devices. Existing per-test `#[ignore]` markers still gate
unfinished backend cases.

**Where does this app go?** Apps that need both a launch smoke and a
buffer-path assertion get two tests — one in `apps::` (just maps a
window) and one in the matching deeper module. Real-toolkit AHB
smokes (Firefox / GTK / STK) live in **both** `libhybris::` and
`gfxstream::` so a regression in one backend doesn't accidentally
hide behind the other; SHM smokes live only in `cpu_graphics::` (the
compositor's SHM plumbing doesn't depend on the chroot's graphics
env). Apps where buffer type is irrelevant (e.g. `lxterminal` driving
text input) get a single `apps::` entry.

## Debug App (`wayland-debug-app`)

A small C program built against libwayland-client and Cairo that exposes
a subcommand CLI and emits structured `TAWC_DEBUG:` lines for the test
harness to parse. It is cross-built on the host against
`build/sysroots/<distro>-<abi>/`.

### Output Protocol

Every test-relevant line is prefixed `TAWC_DEBUG:` to filter from client
and Wayland noise:
```
TAWC_DEBUG:READY                    Window mapped and initialized
TAWC_DEBUG:TEXT_CHANGED:<text>      Full buffer contents after change
TAWC_DEBUG:CURSOR_POS:<offset>      Cursor position (character offset)
TAWC_DEBUG:PREEDIT:<text>           Current composing/preedit string
TAWC_DEBUG:KEY:<name>               Keyboard event observed by the client
TAWC_DEBUG:TOUCH_DOWN:<id>:<x>:<y>:<active>
```

### Commands

| Command | Description |
|---------|-------------|
| `text-input` | Opens a Wayland toplevel with text-input-v3 enabled |
| `text-input-no-surrounding` | Text-input client that never sends surrounding text |
| `touch` / `subsurface` / `popup` | Touch routing scenes |
| `clipboard-copy` / `clipboard-paste` | Wayland/Android clipboard bridge probes |

### Building

```bash
# Build all test clients without copying:
make -C tests/apps ABI=aarch64 DISTRO=arch all
```

`scripts/run-integration-tests.sh` builds and deploys all integration
clients before cargo starts, so the explicit build step is only needed
for ad-hoc manual runs.

### Running Manually

```bash
scripts/rootfs-run.sh '/usr/local/bin/wayland-debug-app text-input'
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

Prerequisites: a phone (or emulator) connected via adb and an in-app
distro installed (see [installation.md](installation.md)). The runner
builds the APK, skips reinstalling it when the installed APK hash
matches, installs missing rootfs runtime packages, incrementally
cross-builds every test program from `tests/apps/<name>/` on the host,
and deploys only changed executables into `/usr/local/bin/` inside the
rootfs. The suite auto-targets the unique install if there's only one,
otherwise pin via `TAWC_INSTALL_ID=<id>`. Some modules have additional
prerequisites (e.g. libhybris on a real device for the GPU-rendering
tests); see each module's docstring.
`wayland-debug-app` is deliberately fail-fast test code: unsupported
protocol state, missing globals, truncation, and internal invariant
failures abort the process instead of being tolerated.

### Test Input Mechanism

Tests inject input through Android-facing entry points. Soft-IME scenarios call
methods on the active `TawcInputConnection` via broker `ic-*` actions. Hardware
keyboard scenarios dispatch `KeyEvent`s through the focused Activity/view via
`hardware-key`, matching Android's USB/Bluetooth keyboard path. There is
intentionally no test path that pokes `NativeBridge.native*` directly — see
`notes/text-input.md` "Test infrastructure note" for the rationale.

```bash
# Per-test reset: in-memory factory settings, RecordingImeOutput,
# active-IC cleanup, and Wayland client close requests.
scripts/tawc-exec.sh --action test-init

# Drive the IC: commit text, set preedit, send a key, etc.
scripts/tawc-exec.sh --action ic-commit-text --arg text=hello
scripts/tawc-exec.sh --action ic-set-composing-text --arg text=wor
scripts/tawc-exec.sh --action ic-finish-composing
scripts/tawc-exec.sh --action ic-send-key-event --arg keycode=67  # Backspace

# Drive hardware-key dispatch through the focused view key path.
scripts/tawc-exec.sh --action hardware-key --arg keycode=67
```

Every call goes through the same Kotlin entry points Android uses to dispatch
Gboard / OpenBoard / AOSP-latin or physical keyboard events. Tests assert
Android contract results and `wayland-debug-app` observations, not private
tawc Rust/Kotlin state.

Broker actions connect to an already-running `LocalServerSocket` and complete in <10ms each, vs. 100–300ms per `am broadcast` JVM cold start (the broadcast channel was retired entirely). More reliable than `adb shell input text` (which gets intercepted by the IME).

### Architecture

```
Host (cargo test)                    Phone
  │                                    │ (test programs already compiled
  │                                    │  and deployed by the runner; the
  │                                    │  harness only checks they exist)
  ├─ adb shell (start client) ─────────┤──→ wayland-debug-app / stock apps / …
  │     └─ piped stdout ←──────────────┤     └─ TAWC_DEBUG:READY (debug app only)
  │                                    │
  ├─ broker action ic-commit-text ─────┤──→ ExecBroker / InputActions
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

- **`adb.rs`**: Shell commands, chroot execution, broker-action-based test reset and input injection (`test_init`, `ic_commit_text`, …; routed through the shared broker client)
- **`rootfs.rs`**: `ensure_wayland_debug_app` / `ensure_tawc_dri_test` /
  `ensure_eglx11_test` — each one just probes for `/usr/local/bin/<name>`
  inside the rootfs and returns its path. Tests do **not** compile
  anything; package install, host builds, and changed-artifact deploys
  happen up-front in `scripts/run-integration-tests.sh`.
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
  - `start_wayland_debug_text_input` and related Wayland debug app
    launchers — for `text_input::` and `touch_input::`.
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
  one-time setup checks run once per `cargo test` invocation.

## Adding New Tests

Add to an existing `tests/<module>.rs` if it fits an existing group, or
create a new module: drop `tests/<new>.rs` next to the others and add a
`mod <new>;` line to `tests/integration.rs`. Tests pick up the module
prefix automatically and the run script's substring filter just works.

If a new compositor protocol is needed, extend `wayland-debug-app.c`:

1. Add a new command (new function + entry in `commands[]`).
2. Define protocol messages (`TAWC_DEBUG:YOUR_EVENT:value`) and a matching
   parser in `debug_app.rs`.
3. Use the same `DebugApp` harness (`start`, `wait_ready`, `wait_for`).

Commands that rely on text input should emit `READY` only after the
client has enabled `zwp_text_input_v3`; otherwise the harness can inject
text before the compositor accepts it.

## Design Decisions

- **One test binary, submodules per group:** Each group lives in its
  own file but they all compile into the same `tests/integration.rs`
  binary, so libtest prints one combined `test result: ...` summary
  and the run script picks subsets via the normal substring filter
  (`<module>::`). Avoids the per-binary summary fragmentation cargo
  produces when each `tests/*.rs` is its own target. `Cargo.toml` has
  `autotests = false` plus an explicit `[[test]]` entry so the
  per-group files aren't auto-discovered as separate binaries.
- **C for debug app:** Host-side cross-builds are quick after the sysroot exists; no compiler or `base-devel` is needed in the device rootfs.
- **Broker input actions over `adb shell input text`:** The system IME
  can intercept `input text` key events and buffer/autocorrect them.
  Broker actions drive `TawcInputConnection` for soft-IME input and
  focused-view dispatch for hardware-key input, without starting
  a broadcast JVM for every operation.
- **Reader thread + mpsc channel:** adb stdout is a blocking stream.
  Thread drains it continuously, mpsc gives timeout-based waiting.
- **App-side reset owns guest cleanup:** Per-test isolation goes
  through the broker `test-init` action. It resets in-memory settings,
  input state, compositor clients, and runs `ProcessScanner` against the
  target rootfs. `RootfsProcess` is only a broker-session convenience for
  mid-test stdout/stderr and stop requests; it does not use host pidfiles,
  `ps`, PGID reads, or host-side `kill`.
- **`--test-threads=1`:** Tests share the phone and compositor and can't
  run in parallel.
