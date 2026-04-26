# Testing Infrastructure

## Overview

Automated integration tests for the compositor. Each `tests/<name>.rs`
file is a submodule of a single `tests/integration.rs` test binary, so
`cargo test` produces one combined libtest summary and selecting a
subset is just a libtest substring filter (`<module>::test_name`).

```
testing/
  gtk4-debug-app/             C + GTK4, runs on phone in chroot, structured stdout
  integration/                Rust tests on host
    tests/integration.rs        single test binary, declares the submodules
    tests/<module>.rs           one file per group; see its docstring
  build-debug-app.sh          Manual debug-app build script
```

Each test module's own docstring documents what it covers and what its
prerequisites are. As of writing the modules are `apps` (real desktop
clients — Firefox/STK/GTK demos/vulkaninfo, exercises the rendering /
GPU stack) and `input` (gtk4-debug-app driven through compositor input
dispatch).

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
bash testing/build-debug-app.sh

# Or manually in chroot:
cd /tmp/gtk4-debug-app && bash build.sh
```

The Rust harness also calls `chroot::ensure_debug_app()` automatically with
freshness caching, so the explicit build step is only needed for ad-hoc
manual runs.

### Running Manually

```bash
adb shell "/system/bin/sh /data/local/tmp/arch-chroot-run '/tmp/gtk4-debug-app/gtk4-debug-app text-input'"
```

## Integration Tests

Rust tests using `std::process::Command` to call adb. Zero external
runtime dependencies on the host.

### Running

`testing/run-integration-tests.sh` is the recommended entry point. It
picks the device via `client/select-device.sh` (`TAWC_TARGET=device|emulator`
if both are connected), builds and deploys everything, then runs cargo.

The script takes one optional positional arg — a libtest substring
filter forwarded to `cargo test` — plus `--no-build` / `-n`.

```bash
bash testing/run-integration-tests.sh                 # everything
bash testing/run-integration-tests.sh <module>::      # one module's tests
bash testing/run-integration-tests.sh <test_name>     # one test by name
bash testing/run-integration-tests.sh --no-build ...  # skip rebuild/redeploy
```

Direct `cargo test` invocations work too — they just need
`source client/select-device.sh` first so cargo inherits
`ANDROID_SERIAL`. Without it the harness's `adb` calls fail silently
when more than one target is connected.

Prerequisites: a phone (or emulator) connected via adb, the compositor
APK installed, `arch-chroot-run` pushed to the device. Some modules
have additional prerequisites (e.g. libhybris on a real device for the
GPU-rendering tests); see each module's docstring.

### Test Input Mechanism

Tests inject input via Android broadcast intents, not `adb shell input text`:

```bash
# Text input (goes through nativeCommitText -> text_input_v3):
adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"

# Key event (goes through nativeSendKeyEvent -> wl_keyboard):
adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67
```

This is more reliable than `adb shell input text` which gets intercepted by
the IME (Gboard) and may not reach the InputConnection. The broadcast
approach goes directly through the same JNI path as real IME input.

### Architecture

```
Host (cargo test)                    Phone
  │                                    │
  ├─ adb push source ──────────────────┤ (one-time build, cached by mtime stamp)
  ├─ adb shell (build in chroot) ──────┤
  │                                    │
  ├─ adb shell (start client) ─────────┤──→ gtk4-debug-app  /  firefox  /  …
  │     └─ piped stdout ←──────────────┤     └─ TAWC_DEBUG:READY (debug app only)
  │                                    │
  ├─ am broadcast TEXT_INPUT ──────────┤──→ BroadcastReceiver
  │                                    │     └─ nativeCommitText
  │                                    │       └─ text_input_v3
  │                                    │         └─ GTK text view
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

- **`adb.rs`**: Shell commands, chroot execution, broadcast-based input injection
- **`chroot.rs`**: `ensure_debug_app()` (build gtk4-debug-app, cached by mtime)
  and `ensure_pkgs()` (idempotent pacman install used to make sure
  upstream packages are present in the chroot)
- **`debug_app.rs`**: Start/stop lifecycle, stdout reader thread, `wait_for()` with timeout
- **`compositor.rs`**: Ensure compositor is running, query state via broadcast
- **`helpers.rs`**: Shared test helpers (`ensure_compositor`, `start_text_input`,
  `assert_compositor_clean`, `launch_and_wait_for_ahb`, `saw_ahb_import`,
  `saw_shm_import`). The OnceLock state means one-time setup
  (compositor start, debug-app build) runs once per `cargo test` invocation.

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
harness starts broadcasting text. If you add new commands that rely on
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
- **Broadcast intents over `adb shell input text`:** The IME (Gboard)
  intercepts `input text` key events and may buffer/autocorrect them.
  Broadcasts go directly through `nativeCommitText`, the same JNI path
  as real IME input.
- **Reader thread + mpsc channel:** adb stdout is a blocking stream.
  Thread drains it continuously, mpsc gives timeout-based waiting.
- **Process-group kill in `ChrootProcess`:** Process chain
  `adb -> su -> bash -> chroot -> bash -> app` doesn't propagate signals;
  Firefox spawns content processes in their own PGIDs. We track the root
  PID via a small pidfile helper, walk descendants, and signal each PGID.
- **`--test-threads=1`:** Tests share the phone and compositor and can't
  run in parallel.
