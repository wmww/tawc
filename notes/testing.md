# Testing Infrastructure

## Overview

Automated integration tests for the compositor. Two test groups, run as
separate cargo `tests/` targets so they can be exercised independently.

```
testing/
  gtk4-debug-app/         C + GTK4, runs on phone in chroot, structured stdout
  integration/            Rust tests on host
    tests/apps.rs           real-app coverage (Firefox, STK, GTK demos, vulkaninfo)
    tests/input.rs          input-dispatch coverage (uses gtk4-debug-app)
  build-debug-app.sh      Manual debug-app build script
```

## Test Groups

### `apps` — what runs on which toolkit

Launches real existing programs (Firefox, supertuxkart, `gtk3-demo-application`,
`gtk4-demo-application`, vulkaninfo) and verifies they don't crash, render
through the expected buffer path (AHB / SHM), and look sane to the
compositor. **No debug app involved** — these tests catch regressions in
the rendering / GPU pipeline against real-world clients.

These tests need libhybris and an Android GPU driver, so they only pass on
a real device.

### `input` — how the compositor dispatches input

Uses `gtk4-debug-app` to drive text-input-v3 commits, key events, and
touch taps, asserting the client observes the expected results. **No
buffer-type assertions** — input dispatch is independent of the rendering
path, so these tests force `GSK_RENDERER=cairo` and run equally well on
the device or the emulator. This means we can iterate on input/IME work
on the emulator without paying the cost of GPU/AHB setup.

## Debug App (`gtk4-debug-app`)

A small C program built against GTK4 that exposes a subcommand CLI and
emits structured `TAWC_DEBUG:` lines for the test harness to parse. Builds
inside the chroot with `gcc` + `pkg-config --cflags --libs gtk4`.

We used to also have a `gtk3-debug-app`, but it was redundant: GTK3-vs-GTK4
toolkit coverage now lives in the `apps` group via the upstream
demo apps.

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

Rust tests using `std::process::Command` to call adb. The only runtime
dependency is `libc` (for an `atexit` shutdown hook).

### Running

`testing/run-integration-tests.sh` is the recommended entry point for
both full runs and narrowed-down ones. It picks the device via
`client/select-device.sh` (`TAWC_TARGET=device|emulator` if both are
connected), builds and deploys everything, then runs cargo with the
right `--test` / libtest filter.

```bash
# Everything (both groups):
bash testing/run-integration-tests.sh

# One group:
bash testing/run-integration-tests.sh apps
bash testing/run-integration-tests.sh input

# One test by name (libtest substring filter):
bash testing/run-integration-tests.sh test_firefox_launches_with_hardware_buffers
bash testing/run-integration-tests.sh input test_text_input_and_backspace

# Skip the rebuild/redeploy phase when iterating on the tests
# themselves (APK, libhybris, chroot helpers are reused as-is):
bash testing/run-integration-tests.sh --no-build input
```

Direct `cargo test` invocations work too — they just need
`source client/select-device.sh` first so cargo inherits
`ANDROID_SERIAL`. Without it the harness's `adb` calls fail silently
when more than one target is connected.

Tests require:
- Phone (or emulator, for `input` group) connected via adb
- Compositor APK installed and running (the harness force-restarts if needed)
- `arch-chroot-run` pushed to phone
- For the `apps` group: libhybris already built (see `client/build-libhybris`)

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
  and `ensure_pkgs()` (idempotent pacman install, used by application tests
  to make sure `gtk3` / `gtk4` are present in the chroot)
- **`debug_app.rs`**: Start/stop lifecycle, stdout reader thread, `wait_for()` with timeout
- **`compositor.rs`**: Ensure compositor is running, query state via broadcast
- **`helpers.rs`**: Shared test helpers (`ensure_compositor`, `start_text_input`,
  `assert_compositor_clean`, `launch_and_wait_for_ahb`, `saw_ahb_import`,
  `saw_shm_import`). Each test binary gets its own copy of the OnceLock
  state, so per-binary one-time setup runs once per `cargo test` invocation.

## Adding New Tests

Pick a group:

- **Render / toolkit / GPU regressions** → `tests/apps.rs`. Spawn a
  real existing program via `ChrootProcess::spawn` (or the
  `launch_and_wait_for_ahb` helper) and assert on logcat / compositor
  state. Use `chroot::ensure_pkgs(&["..."])` to install any missing
  packages from the chroot's Arch repos. Don't reach for the debug app.
- **Input / IME / focus regressions** → `tests/input.rs`. Use
  `helpers::start_text_input(env)` to launch the gtk4-debug-app and drive
  it via `adb::input_text` / `input_keyevent` / `input_tap`. Avoid
  asserting on buffer types so the test stays emulator-friendly.

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

- **Two test targets, not one:** Splitting `apps` and `input` into
  separate `tests/*.rs` files means each group is its own cargo test
  binary, so `cargo test --test input` runs only the input tests.
  Different groups have different prerequisites (input works on emulator,
  apps needs libhybris) and different iteration cadences (input is fast,
  apps coverage is slow because of Firefox/STK).
- **C for debug app:** Sub-second builds, no cargo on phone, `base-devel` already in chroot.
- **One debug app, not one per toolkit:** GTK3 vs GTK4 toolkit coverage is
  the *apps* group's job, exercised through the upstream demo programs.
  The debug app exists only to give us programmatic stdout hooks for
  input-dispatch tests; one toolkit (GTK4) is enough for that.
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
- **Full buffer in TEXT_CHANGED:** Tests assert exact equality without
  tracking incremental changes.
- **`--test-threads=1`:** Tests share the phone and compositor and can't
  run in parallel within a binary. Cargo runs the two test binaries
  sequentially by default.
