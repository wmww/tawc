//! Host-side helpers for driving the device.
//!
//! # Input rule: tests act as keyboards or apps, never inside the compositor
//!
//! Every input helper here drives [`TawcInputConnection`] (via the broker
//! `ic-*` actions) — the same Kotlin entrypoint Gboard / OpenBoard /
//! AOSP-latin call into. There is intentionally **no helper that pokes
//! `NativeBridge.native*` directly** and no broker action that does so
//! either. Tests act as a keyboard (sending IME methods to the IC) or as
//! a wayland client (assertions go through `wayland-debug-app`'s observed
//! events). The IC's full state machine — `computeReplaceDeltas`, the
//! Editable mirror, the `composingRegionIsPreedit` short-circuit,
//! `unitsToKeyCounts` — runs in every test the same way it runs in
//! production. See `notes/text-input.md` ("Test infrastructure note") for
//! the rationale.
//!
//! Tap / cursor events ([`input_tap`]) come from the OS-level
//! `adb shell input tap` — they're real touch events from the
//! SurfaceView's perspective, the same as a finger on the screen. They
//! are not "IC-bypass" — touches don't go through the IC at all in
//! production either.

use std::io;
use std::process::{Command, Output, Stdio};
use std::sync::OnceLock;

use crate::GraphicsBackend;

/// Path to the host-side tawc-exec helper. Absolute path lets tests
/// invoke it without needing it on $PATH. Built by `scripts/tawc-exec.sh`.
/// Override via `TAWC_EXEC_BIN`.
fn tawc_exec_bin() -> &'static str {
    static B: OnceLock<String> = OnceLock::new();
    B.get_or_init(|| {
        if let Ok(env) = std::env::var("TAWC_EXEC_BIN") {
            return env;
        }
        // CARGO_MANIFEST_DIR is `tests/integration`; the binary lives
        // at <repo>/build/tawc-exec/tawc-exec.
        let repo = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(2)
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| std::path::PathBuf::from("."));
        repo.join("build")
            .join("tawc-exec")
            .join("tawc-exec")
            .to_string_lossy()
            .into_owned()
    })
    .as_str()
}

/// Run an adb shell command, wait for completion, return output.
pub fn shell(cmd: &str) -> io::Result<Output> {
    Command::new("adb").args(["shell", cmd]).output()
}

/// Execute a command on the device as the app uid (no `su`, no
/// `run-as`) via the dev exec broker. Same address-space + SELinux
/// domain (`untrusted_app`) as the running app.
///
/// `argv[0]` must be an absolute path or one resolvable on the
/// (empty-by-default) child PATH; pass `--cwd` / `--env` separately
/// if needed by using the broker directly. For these tests the helper
/// is mainly used to copy files into the rootfs.
pub fn rootfs_host_exec(argv: &[&str]) -> io::Result<Output> {
    Command::new(tawc_exec_bin()).args(argv).output()
}

/// Run a command inside the in-app Linux install on the phone.
///
/// Routed through the broker's RUNINSIDE handler, which dispatches
/// to the install's [InstallationMethod.startInside] (see
/// notes/exec-broker.md, notes/rootfs-sessions.md). Single entry point
/// for every install method — chroot handles its own `su` internally.
///
/// Uses whatever graphics backend the user picked in Settings. Tests
/// that want a specific backend should call [rootfs_run_with] instead.
pub fn rootfs_run(cmd: &str) -> io::Result<Output> {
    run_inside_argv(None, cmd).output()
}

/// Same as [rootfs_run] but pins the in-rootfs [GraphicsBackend] for
/// this one spawn. Travels over the broker as a `GRAPHICS <key>` header
/// on the RUNINSIDE form (see notes/exec-broker.md); the persisted
/// `Settings.graphicsBackend` is untouched.
pub fn rootfs_run_with(backend: GraphicsBackend, cmd: &str) -> io::Result<Output> {
    run_inside_argv(Some(backend), cmd).output()
}

/// Spawn a command in the chroot with piped stdout/stderr (non-blocking).
/// Returns the Child process. Caller is responsible for reading output.
///
/// Same broker dispatch as [rootfs_run]. The rootfs-session invariant
/// (notes/rootfs-sessions.md) is enforced inside
/// [InstallationMethod.startInside] — no caller-side `setsid` needed.
pub fn rootfs_spawn(cmd: &str) -> io::Result<std::process::Child> {
    run_inside_argv(None, cmd)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
}

/// Backend-pinned variant of [rootfs_spawn]. See [rootfs_run_with].
pub fn rootfs_spawn_with(backend: GraphicsBackend, cmd: &str) -> io::Result<std::process::Child> {
    run_inside_argv(Some(backend), cmd)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
}

/// Build the `tawc-exec --in-rootfs … [--graphics …] -- <cmd>` Command.
/// Shared by `rootfs_run` / `rootfs_run_with` / spawn variants — only
/// the stdio piping and `.output()` vs `.spawn()` differs.
fn run_inside_argv(backend: Option<GraphicsBackend>, cmd: &str) -> Command {
    let mut c = Command::new(tawc_exec_bin());
    c.args(["--in-rootfs", &crate::install_id()]);
    if let Some(b) = backend {
        c.args(["--graphics", b.as_key()]);
    }
    c.args(["--", cmd]);
    c
}

/// Run a broker action via tawc-exec. `args` are passed as repeated
/// `--arg key=value`. Returns the helper's exit status + captured stdio,
/// same shape as [shell].
fn broker_action(name: &str, args: &[(&str, &str)]) -> io::Result<Output> {
    let mut cmd = Command::new(tawc_exec_bin());
    cmd.args(["--action", name]);
    for (k, v) in args {
        cmd.args(["--arg", &format!("{k}={v}")]);
    }
    cmd.output()
}

// ---- Test-mode setup -----------------------------------------------------

/// Swap the production [me.phie.tawc.compositor.RealImeOutput] for a
/// recording impl so the system IME (Gboard / OpenBoard / AOSP-latin)
/// never sees `updateSelection` / `showSoftInput` and never reacts. The
/// recorder is only on the **outbound** boundary IC ⇒ system-IMM — it
/// doesn't bypass anything in our state machine, it just stops a
/// third-party from amplifying events into the IC. Pair with
/// [disable_test_input]; both are no-ops on release builds (no broker).
/// Called by [`crate::helpers::start_text_input`] on every test.
pub fn enable_test_input() -> io::Result<Output> {
    broker_action("enable-test-input", &[])
}

/// Restore the production ImeOutput. Symmetric counterpart to
/// [enable_test_input]. Process death also resets, so test crashes
/// can't leave the recorder pinned in place across `cargo test` runs.
pub fn disable_test_input() -> io::Result<Output> {
    broker_action("disable-test-input", &[])
}

// ---- IC drivers ----------------------------------------------------------
//
// These mirror the [android.view.inputmethod.InputConnection] surface that
// the system IMM dispatches Gboard events through. The IC's full state
// machine runs on every call — Editable mirror update, composing-region
// delta computation, the `composingRegionIsPreedit` short-circuit, key
// translation in `unitsToKeyCounts`, etc. The wire output is what real
// Gboard would produce given the same sequence of method calls.

/// Call `TawcInputConnection.commitText(text, 1)` on the active IC.
/// Equivalent of Gboard's `commitText` — autocorrect commit, finished
/// word, single-character commit, etc.
pub fn ic_commit_text(text: &str) -> io::Result<Output> {
    broker_action("ic-commit-text", &[("text", text)])
}

/// Call `TawcInputConnection.setComposingText(text, 1)` on the active
/// IC. Equivalent of Gboard's `setComposingText` — preedit-as-you-type.
/// Successive calls replace the previous preedit; pass an empty string
/// to clear it.
pub fn ic_set_composing_text(text: &str) -> io::Result<Output> {
    broker_action("ic-set-composing-text", &[("text", text)])
}

/// Call `TawcInputConnection.setComposingRegion(start, end)` on the
/// active IC. Marks the given Editable range as composing without
/// changing its content — the bridge between Android's "composing
/// region annotates committed text" and Wayland's "preedit is overlay
/// only". The IC's `computeReplaceDeltas` uses this on the next
/// commit/setComposing to emit a `delete_surrounding_text` covering
/// the marked range.
pub fn ic_set_composing_region(start: u32, end: u32) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action("ic-set-composing-region", &[("start", &s), ("end", &e)])
}

/// Call `TawcInputConnection.finishComposingText()` on the active IC.
/// Equivalent of Gboard's `finishComposingText` — clears the composing
/// flag without changing content. The wayland-side preedit, if any,
/// gets committed by the compositor's done-ordering.
pub fn ic_finish_composing() -> io::Result<Output> {
    broker_action("ic-finish-composing", &[])
}

/// Call `TawcInputConnection.setSelection(start, end)` on the active
/// IC. Moves the Editable's cursor without any wayland-side equivalent
/// — there's no `set_selection` in text-input-v3, so this simulates an
/// IME that "thinks" the cursor is somewhere different from where the
/// Wayland client has it. The IC's `lastSyncedCursor` divergence guard
/// is the production code path that handles this.
pub fn ic_set_selection(start: u32, end: u32) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action("ic-set-selection", &[("start", &s), ("end", &e)])
}

/// Call `TawcInputConnection.deleteSurroundingText(before, after)` on
/// the active IC. Equivalent of Gboard's `deleteSurroundingText` — the
/// IC translates this to UTF-16 unit counts via [unitsToKeyCounts] and
/// emits Backspace × `before` + Forward-Delete × `after` key events on
/// `wl_keyboard`. Use this for tests that assert wire-side key
/// translation; for tests that just want a Backspace, prefer
/// [ic_send_key_event] with `KEYCODE_DEL`.
pub fn ic_delete_surrounding_text(before: u32, after: u32) -> io::Result<Output> {
    let b = before.to_string();
    let a = after.to_string();
    broker_action(
        "ic-delete-surrounding-text",
        &[("before", &b), ("after", &a)],
    )
}

/// Call `TawcInputConnection.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode))`
/// on the active IC. The IC drops everything that isn't `ACTION_DOWN`
/// (production) so we only send down-events. Used for plain wl_keyboard
/// keys that the IME pokes through directly — Backspace, Tab, Enter when
/// no composing is in flight, etc.
pub fn ic_send_key_event(keycode: u32) -> io::Result<Output> {
    let kc = keycode.to_string();
    broker_action("ic-send-key-event", &[("keycode", &kc)])
}

/// Call `TawcInputConnection.sendKeyEvent` with a modifier meta-state.
/// Used for real GTK shortcuts such as Ctrl+A/C/V, where the focused
/// Wayland client must see the modifier held while the key is delivered.
pub fn ic_send_modified_key_event(
    keycode: u32,
    ctrl: bool,
    alt: bool,
    shift: bool,
) -> io::Result<Output> {
    let kc = keycode.to_string();
    let ctrl = if ctrl { "true" } else { "false" };
    let alt = if alt { "true" } else { "false" };
    let shift = if shift { "true" } else { "false" };
    broker_action(
        "ic-send-modified-key-event",
        &[
            ("keycode", &kc),
            ("ctrl", ctrl),
            ("alt", alt),
            ("shift", shift),
        ],
    )
}

/// Set Android's real ClipboardManager text through tawc's debug broker.
/// This is intentionally app-side rather than `adb shell` clipboard poking:
/// Android exposes clipboard APIs to the foreground app, while shell access
/// varies across OS versions and device builds.
pub fn clipboard_set_text(text: &str) -> io::Result<Output> {
    broker_action("clipboard-set-text", &[("text", text)])
}

/// Read Android's real ClipboardManager text through tawc's debug broker.
pub fn clipboard_get_text() -> io::Result<String> {
    let output = broker_action("clipboard-get-text", &[])?;
    Ok(String::from_utf8_lossy(&output.stdout)
        .trim_end_matches(['\r', '\n'])
        .to_string())
}

// ---- Touch / observation -------------------------------------------------

/// Send a tap at physical screen coordinates (x, y) via the OS-level
/// `input tap` command. Real touch event from the SurfaceView's
/// perspective — same path a finger on the screen takes. The compositor
/// divides by the current output scale to get logical coordinates.
pub fn input_tap(x: u32, y: u32) -> io::Result<Output> {
    shell(&format!("input tap {} {}", x, y))
}

/// Ask the debug broker to inject a normalized touch sequence into the
/// focused compositor SurfaceView. `kind` is one of `tap`, `drag`, or
/// `multitouch`; coordinates are chosen as fractions of the current view
/// size on device, so callers do not need to know the physical screen size.
pub fn inject_touch(kind: &str) -> io::Result<Output> {
    broker_action("inject-touch", &[("kind", kind)])
}

/// Clear the logcat buffer so subsequent reads only show new messages.
pub fn logcat_clear() -> io::Result<Output> {
    Command::new("adb").args(["logcat", "-c"]).output()
}

/// Dump logcat lines matching the tawc-native tag (compositor Rust logs).
pub fn logcat_dump_tawc() -> io::Result<String> {
    logcat_dump("tawc-native")
}

/// Dump logcat lines matching a specific tag.
pub fn logcat_dump(tag: &str) -> io::Result<String> {
    let output = Command::new("adb")
        .args(["logcat", "-d", "-s", tag])
        .output()?;
    Ok(String::from_utf8_lossy(&output.stdout).to_string())
}

/// Trigger the compositor to log its current state via the broker
/// `query-state` action. Reads as a `COMPOSITOR_STATE …` line under the
/// `tawc-native` logcat tag. Observational only — doesn't change input
/// state.
pub fn broadcast_query_state() -> io::Result<Output> {
    broker_action("query-state", &[])
}

/// Dynamically update the compositor output scale through the same broker
/// action used by Settings tests. Value is snapped by the app to 0.25x.
pub fn set_output_scale(scale: f32) -> io::Result<Output> {
    let value = format!("{scale:.2}");
    broker_action("set-output-scale", &[("value", &value)])
}

/// Read the persisted output scale from the app settings.
pub fn get_output_scale() -> io::Result<f32> {
    let output = broker_action("get-output-scale", &[])?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "get-output-scale failed: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    stdout
        .trim()
        .parse::<f32>()
        .map_err(|e| io::Error::other(format!("parse output scale: {e}; stdout={stdout:?}")))
}

/// Dynamically toggle the contained GTK3 broken menus workaround through
/// the same broker action used by Settings.
pub fn set_gtk3_broken_menus_workaround(enabled: bool) -> io::Result<Output> {
    let enabled = if enabled { "true" } else { "false" };
    broker_action("set-gtk3-broken-menus-workaround", &[("enabled", enabled)])
}

/// Read the persisted GTK3 broken menus workaround setting.
pub fn get_gtk3_broken_menus_workaround() -> io::Result<bool> {
    let output = broker_action("get-gtk3-broken-menus-workaround", &[])?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "get-gtk3-broken-menus-workaround failed: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    match stdout.trim() {
        "true" => Ok(true),
        "false" => Ok(false),
        other => Err(io::Error::other(format!(
            "parse gtk3 broken menus workaround: stdout={other:?}"
        ))),
    }
}

// Common Android keycodes (used with [ic_send_key_event]).
pub const KEYCODE_DEL: u32 = 67; // Backspace
pub const KEYCODE_FORWARD_DEL: u32 = 112; // Delete (forward delete)
pub const KEYCODE_ENTER: u32 = 66;
pub const KEYCODE_TAB: u32 = 61;
pub const KEYCODE_A: u32 = 29;
pub const KEYCODE_C: u32 = 31;
pub const KEYCODE_V: u32 = 50;
