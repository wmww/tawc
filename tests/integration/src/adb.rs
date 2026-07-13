//! Host-side helpers for driving the device.
//!
//! # Input rule: tests act as keyboards or apps, never inside the compositor
//!
//! Soft-IME helpers drive [`TawcInputConnection`] (via the broker `ic-*`
//! actions), the same Kotlin entrypoint Gboard / OpenBoard / AOSP-latin call
//! into. Hardware-key helpers use focused Activity/view key dispatch. There is
//! intentionally **no helper that pokes `NativeBridge.native*` directly** and
//! no broker action that does so either. Tests act as a keyboard or as a
//! wayland client (assertions go through `wayland-debug-app`'s observed
//! events). Tests assert the two public ends — Android contract results and
//! client-visible Wayland behavior — not private tawc state. See
//! `notes/text-input.md` ("Test infrastructure note") for the rationale.
//!
//! Tap / cursor events come from [`inject_touch`] / [`inject_touch_logical`],
//! which dispatch MotionEvents through the focused SurfaceView (at a stable
//! Wayland logical coordinate for the latter). Not an IC bypass — touches
//! don't go through the IC in production either.

use std::io;
use std::process::{Command, Output};
use std::thread;
use std::time::{Duration, Instant};

use crate::exec_broker::{self, BrokerChild, Invocation, Request};
use crate::GraphicsBackend;

/// Run an adb shell command, wait for completion, return output.
pub fn shell(cmd: &str) -> io::Result<Output> {
    Command::new("adb").args(["shell", cmd]).output()
}

/// Raw Android framebuffer screenshot from `adb exec-out screencap`.
pub struct RawScreenshot {
    pub width: u32,
    pub height: u32,
    pub format: u32,
    data_offset: usize,
    data: Vec<u8>,
}

impl RawScreenshot {
    pub fn pixel_rgba(&self, x: u32, y: u32) -> Option<[u8; 4]> {
        if x >= self.width || y >= self.height {
            return None;
        }
        let idx = self.data_offset + (((y * self.width + x) * 4) as usize);
        Some([
            *self.data.get(idx)?,
            *self.data.get(idx + 1)?,
            *self.data.get(idx + 2)?,
            *self.data.get(idx + 3)?,
        ])
    }
}

fn le_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(offset..offset + 4)?.try_into().ok()?))
}

/// Capture a raw screenshot. Android prefixes raw RGBA data with width,
/// height, pixel format, and on newer releases a dataspace word.
pub fn screencap_raw() -> io::Result<RawScreenshot> {
    let output = Command::new("adb").args(["exec-out", "screencap"]).output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "screencap failed: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    let data = output.stdout;
    let width = le_u32(&data, 0).ok_or_else(|| io::Error::other("screencap missing width"))?;
    let height = le_u32(&data, 4).ok_or_else(|| io::Error::other("screencap missing height"))?;
    let format = le_u32(&data, 8).ok_or_else(|| io::Error::other("screencap missing format"))?;
    let pixel_bytes = width as usize * height as usize * 4;
    let data_offset = if data.len() >= 16 + pixel_bytes {
        16
    } else if data.len() >= 12 + pixel_bytes {
        12
    } else {
        return Err(io::Error::other(format!(
            "screencap too short: {} bytes for {}x{}",
            data.len(),
            width,
            height
        )));
    };
    Ok(RawScreenshot {
        width,
        height,
        format,
        data_offset,
        data,
    })
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
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::Exec {
            argv: argv.iter().map(|s| (*s).to_string()).collect(),
            env: Vec::new(),
            cwd: None,
            op_title: None,
        },
    })
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
    rootfs_run_inner(None, cmd)
}

/// Same as [rootfs_run] but pins the in-rootfs [GraphicsBackend] for
/// this one spawn. Travels over the broker as a `GRAPHICS <key>` header
/// on the RUNINSIDE form (see notes/exec-broker.md); the persisted
/// `Settings.graphicsBackend` is untouched.
pub fn rootfs_run_with(backend: GraphicsBackend, cmd: &str) -> io::Result<Output> {
    rootfs_run_inner(Some(backend), cmd)
}

/// Spawn a command in the chroot with piped stdout/stderr (non-blocking).
/// Returns a broker child handle. Caller is responsible for reading output.
///
/// Same broker dispatch as [rootfs_run]. The rootfs-session invariant
/// (notes/rootfs-sessions.md) is enforced inside
/// [InstallationMethod.startInside] — no caller-side `setsid` needed.
pub fn rootfs_spawn(cmd: &str) -> io::Result<BrokerChild> {
    rootfs_spawn_inner(None, cmd)
}

/// Backend-pinned variant of [rootfs_spawn]. See [rootfs_run_with].
pub fn rootfs_spawn_with(backend: GraphicsBackend, cmd: &str) -> io::Result<BrokerChild> {
    rootfs_spawn_inner(Some(backend), cmd)
}

fn rootfs_run_inner(backend: Option<GraphicsBackend>, cmd: &str) -> io::Result<Output> {
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::RunInside {
            install_id: crate::install_id(),
            cmd: cmd.to_string(),
            op_title: None,
            graphics: backend.map(|b| b.as_key().to_string()),
        },
    })
}

fn rootfs_spawn_inner(backend: Option<GraphicsBackend>, cmd: &str) -> io::Result<BrokerChild> {
    exec_broker::spawn(Invocation {
        foreground_app: false,
        request: Request::RunInside {
            install_id: crate::install_id(),
            cmd: cmd.to_string(),
            op_title: None,
            graphics: backend.map(|b| b.as_key().to_string()),
        },
    })
}

/// Run a broker action. `args` are sent as `ARG key=value` header lines.
/// Returns the broker exit status + captured stdio,
/// same shape as [shell].
fn broker_action_raw(name: &str, args: &[(&str, &str)]) -> io::Result<Output> {
    exec_broker::run_capture(Invocation {
        foreground_app: false,
        request: Request::Action {
            name: name.to_string(),
            args: args
                .iter()
                .map(|(k, v)| ((*k).to_string(), (*v).to_string()))
                .collect(),
        },
    })
}

/// Run a broker action and fail the caller if the broker returned non-zero.
fn broker_action(name: &str, args: &[(&str, &str)]) -> io::Result<Output> {
    let output = broker_action_raw(name, args)?;
    if output.status.success() {
        Ok(output)
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        Err(io::Error::other(format!(
            "broker action {name} failed with {} stdout={:?} stderr={:?}",
            output.status, stdout, stderr
        )))
    }
}

// ---- Test-mode setup -----------------------------------------------------

/// Reset app-side test state before an integration test runs. This enters
/// in-memory test settings (factory defaults, no SharedPreferences writes),
/// swaps in RecordingImeOutput, clears the active IC, and asks existing
/// Wayland clients to close. Returns the number of client windows that were
/// asked to close.
pub fn test_init() -> io::Result<usize> {
    let install_id = crate::install_id();
    let output = broker_action("test-init", &[("installId", &install_id)])?;
    if !output.status.success() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "test-init failed: stdout={} stderr={}",
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            ),
        ));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    stdout
        .lines()
        .find_map(|line| line.strip_prefix("closed=")?.parse::<usize>().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::Other, format!("test-init missing closed count: {stdout:?}")))
}

pub fn cleanup_rootfs() -> io::Result<usize> {
    let install_id = crate::install_id();
    let output = broker_action("cleanup-rootfs", &[("installId", &install_id)])?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "cleanup-rootfs failed with {} stdout={:?} stderr={:?}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr),
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    stdout
        .lines()
        .find_map(|line| line.strip_prefix("rootfs_killed=")?.parse::<usize>().ok())
        .ok_or_else(|| io::Error::other(format!("cleanup-rootfs missing count: {stdout:?}")))
}

/// Wait until the focused compositor activity has an active
/// TawcInputConnection. In production the Android IMM creates and owns the
/// IC after showSoftInput; in test mode RecordingImeOutput does that without
/// waking a real third-party IME.
pub fn wait_for_active_input_connection(timeout: Duration) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    loop {
        let last = match broker_action_raw("input-ready", &[]) {
            Ok(output) if output.status.success() => return Ok(()),
            Ok(output) => {
                format!(
                    "{} stdout={:?} stderr={:?}",
                    output.status,
                    String::from_utf8_lossy(&output.stdout),
                    String::from_utf8_lossy(&output.stderr)
                )
            }
            Err(e) => e.to_string(),
        };
        if Instant::now() >= deadline {
            return Err(format!(
                "Timeout waiting for active input connection (last: {})",
                last
            ));
        }
        thread::sleep(Duration::from_millis(25));
    }
}

pub fn focused_activity_id() -> io::Result<String> {
    let output = broker_action("focused-activity-id", &[])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let id = stdout.trim();
    if id.is_empty() {
        Err(io::Error::other("focused-activity-id returned empty stdout"))
    } else {
        Ok(id.to_string())
    }
}

pub fn focus_activity(activity_id: &str) -> io::Result<Output> {
    broker_action("focus-activity", &[("activityId", activity_id)])
}

pub fn focused_editor_info() -> io::Result<(i32, i32)> {
    let output = broker_action("focused-editor-info", &[])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut input_type = None;
    let mut ime_options = None;
    for part in stdout.split_whitespace() {
        if let Some((key, value)) = part.split_once('=') {
            match key {
                "inputType" => input_type = value.parse().ok(),
                "imeOptions" => ime_options = value.parse().ok(),
                _ => {}
            }
        }
    }
    match (input_type, ime_options) {
        (Some(input_type), Some(ime_options)) => Ok((input_type, ime_options)),
        _ => Err(io::Error::other(format!("parse focused-editor-info: {stdout:?}"))),
    }
}

/// One recorded `ImeOutput.updateSelection` call — what the editor told
/// the (test-recorded) IME about selection and composing region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImeSelectionUpdate {
    pub sel_start: i32,
    pub sel_end: i32,
    pub composing_start: i32,
    pub composing_end: i32,
}

impl ImeSelectionUpdate {
    /// The "composition ended" shape real IMEs (Gboard, OpenBoard) react
    /// to with a defensive `finishComposingText`.
    pub fn signals_composition_ended(&self) -> bool {
        self.composing_start == -1 && self.composing_end == -1
    }
}

/// Dump every `updateSelection` call recorded by the test `ImeOutput`
/// since `test-init`, oldest first. This is the editor→IME boundary the
/// system IME watches; test-init removed that IME from the loop, so tests
/// use this to assert (or model a reaction to) what it would have seen.
pub fn ime_selection_updates() -> io::Result<Vec<ImeSelectionUpdate>> {
    let output = broker_action("ime-selection-updates", &[])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    stdout
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(|line| {
            let fields: Vec<i32> = line
                .split(',')
                .map(|v| v.trim().parse())
                .collect::<Result<_, _>>()
                .map_err(|e| io::Error::other(format!("parse ime-selection-updates {line:?}: {e}")))?;
            if fields.len() != 4 {
                return Err(io::Error::other(format!(
                    "ime-selection-updates line {line:?}: expected 4 fields"
                )));
            }
            Ok(ImeSelectionUpdate {
                sel_start: fields[0],
                sel_end: fields[1],
                composing_start: fields[2],
                composing_end: fields[3],
            })
        })
        .collect()
}

// ---- IC drivers ----------------------------------------------------------
//
// These mirror the [android.view.inputmethod.InputConnection] surface that
// the system IMM dispatches Gboard events through. The wire output is what
// real Gboard would produce given the same sequence of method calls.

/// Call `TawcInputConnection.commitText(text, 1)` on the active IC.
/// Equivalent of Gboard's `commitText` — autocorrect commit, finished
/// word, single-character commit, etc.
pub fn ic_commit_text(text: &str) -> io::Result<Output> {
    broker_action("ic-commit-text", &[("text", text)])
}

/// Try `TawcInputConnection.commitText(text, 1)` and return the raw broker
/// result so tests can assert Android-contract rejection without treating
/// the expected `false` as a harness failure.
pub fn ic_commit_text_raw(text: &str) -> io::Result<Output> {
    broker_action_raw("ic-commit-text", &[("text", text)])
}

/// Call `TawcInputConnection.commitCompletion(...)` on the active IC.
/// Equivalent of tapping a suggestion from an IME completion row such as
/// Gboard's center autocomplete candidate.
pub fn ic_commit_completion(text: &str) -> io::Result<Output> {
    broker_action("ic-commit-completion", &[("text", text)])
}

/// Call `TawcInputConnection.commitCorrection(CorrectionInfo(...))` on
/// the active IC.
pub fn ic_commit_correction(offset: u32, old_text: &str, new_text: &str) -> io::Result<Output> {
    let offset = offset.to_string();
    broker_action(
        "ic-commit-correction",
        &[("offset", &offset), ("old", old_text), ("new", new_text)],
    )
}

/// Call `TawcInputConnection.replaceText(start, end, text, 1, null)` on
/// the active IC. This is the Android API shape for an IME/editor
/// replacement of a known range, such as accepting a suggestion for the
/// current word.
pub fn ic_replace_text(start: u32, end: u32, text: &str) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action(
        "ic-replace-text",
        &[("start", &s), ("end", &e), ("text", text)],
    )
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

/// Finish composing on the hidden test InputConnection retained by
/// RecordingImeOutput after keyboard hide. This is intentionally separate
/// from [ic_finish_composing], which requires the current focused IC; use
/// it only to model stale IME callbacks after text-input focus leave.
pub fn ic_finish_hidden_composing() -> io::Result<Output> {
    broker_action("ic-finish-hidden-composing", &[])
}

/// Call `TawcInputConnection.setSelection(start, end)` on the active
/// IC. text-input-v3 has no wayland-side equivalent, so tawc rejects
/// cursor movement unless the request is already a no-op.
pub fn ic_set_selection(start: u32, end: u32) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action("ic-set-selection", &[("start", &s), ("end", &e)])
}

/// Try `TawcInputConnection.setSelection(start, end)` and return the raw
/// broker result so tests can assert expected rejection.
pub fn ic_set_selection_raw(start: u32, end: u32) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action_raw("ic-set-selection", &[("start", &s), ("end", &e)])
}

/// Call `TawcInputConnection.deleteSurroundingText(before, after)` on
/// the active IC. Equivalent of Gboard's `deleteSurroundingText` — the
/// IC translates the UTF-16 unit range around its wire cursor to
/// Backspace / Forward-Delete key counts on `wl_keyboard`. Use this for
/// tests that assert wire-side key
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

/// Call `TawcInputConnection.deleteSurroundingTextInCodePoints(before, after)`
/// on the active IC. Same Android boundary as [ic_delete_surrounding_text],
/// but the counts are Unicode code points instead of UTF-16 code units.
pub fn ic_delete_surrounding_text_codepoints(before: u32, after: u32) -> io::Result<Output> {
    let b = before.to_string();
    let a = after.to_string();
    broker_action(
        "ic-delete-surrounding-text-codepoints",
        &[("before", &b), ("after", &a)],
    )
}

/// Try `TawcInputConnection.replaceText(...)` and return the raw broker
/// result so tests can assert Android-contract rejection without treating
/// the expected `false` as a harness failure.
pub fn ic_replace_text_raw(start: u32, end: u32, text: &str) -> io::Result<Output> {
    let s = start.to_string();
    let e = end.to_string();
    broker_action_raw(
        "ic-replace-text",
        &[("start", &s), ("end", &e), ("text", text)],
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

/// Dispatch a hardware-key press through the focused Activity/view path.
/// This mirrors Android's USB/Bluetooth keyboard path
/// (`SurfaceView.dispatchKeyEvent`) rather than the soft-IME
/// `InputConnection` path.
pub fn hardware_key_press(keycode: u32) -> io::Result<Output> {
    hardware_key("press", keycode, 0)
}

pub fn hardware_key_down(keycode: u32) -> io::Result<Output> {
    hardware_key("down", keycode, 0)
}

pub fn hardware_key_up(keycode: u32) -> io::Result<Output> {
    hardware_key("up", keycode, 0)
}

pub fn hardware_key_down_repeat(keycode: u32, repeat: u32) -> io::Result<Output> {
    hardware_key("down", keycode, repeat)
}

fn hardware_key(action: &str, keycode: u32, repeat: u32) -> io::Result<Output> {
    let kc = keycode.to_string();
    let repeat = repeat.to_string();
    broker_action(
        "hardware-key",
        &[("action", action), ("keycode", &kc), ("repeat", &repeat)],
    )
}

/// Set Android's real ClipboardManager text through tawc's debug broker.
/// This is intentionally app-side rather than `adb shell` clipboard poking:
/// Android exposes clipboard APIs to the foreground app, while shell access
/// varies across OS versions and device builds.
pub fn clipboard_set_text(text: &str) -> io::Result<Output> {
    broker_action("clipboard-set-text", &[("text", text)])
}

/// Set an HTML clip (no text/plain MIME in the description), as
/// Firefox/Gecko does when copying web content.
pub fn clipboard_set_html_text(text: &str) -> io::Result<Output> {
    broker_action("clipboard-set-text", &[("text", text), ("html", "1")])
}

/// Read Android's real ClipboardManager text through tawc's debug broker.
pub fn clipboard_get_text() -> io::Result<String> {
    let output = broker_action("clipboard-get-text", &[])?;
    Ok(String::from_utf8_lossy(&output.stdout)
        .trim_end_matches(['\r', '\n'])
        .to_string())
}

pub fn clipboard_pull_timeouts_total() -> io::Result<u64> {
    clipboard_debug_counter("clipboard_pull_timeouts_total")
}

/// Paste requests served through the lazy Android→client fetch path
/// (cache hits and failed fetches included; not raw clipboard reads).
pub fn clipboard_android_fetches_total() -> io::Result<u64> {
    clipboard_debug_counter("clipboard_android_fetches_total")
}

/// Paste pipe drains that hit the write deadline (client never read).
pub fn clipboard_write_timeouts_total() -> io::Result<u64> {
    clipboard_debug_counter("clipboard_write_timeouts_total")
}

fn clipboard_debug_counter(key: &str) -> io::Result<u64> {
    let output = broker_action("clipboard-debug-state", &[])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let prefix = format!("{key}=");
    stdout
        .split_whitespace()
        .find_map(|part| part.strip_prefix(&prefix).and_then(|v| v.parse().ok()))
        .ok_or_else(|| io::Error::other(format!("missing clipboard counter {key}: {stdout:?}")))
}

// ---- Touch / observation -------------------------------------------------

/// Dispatch Android Back through the focused compositor activity's
/// back-press path (broker `back` action) — the same entry the system
/// OnBackInvoked callback routes into. Activity-level rather than
/// system input dispatch; the compositor Back handling tests assert
/// lives below that boundary either way.
///
/// Retries briefly while no CompositorActivity has window focus: tests
/// press Back right after a client's first configure, and Android
/// grants window focus to the freshly launched activity slightly later.
/// (The system input path this replaced waited implicitly.)
pub fn back() -> io::Result<Output> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let output = broker_action_raw("back", &[])?;
        if output.status.success() {
            return Ok(output);
        }
        let stderr = String::from_utf8_lossy(&output.stderr);
        if !stderr.contains("no focused CompositorActivity") || Instant::now() >= deadline {
            return Err(io::Error::other(format!(
                "broker action back failed with {} stdout={:?} stderr={:?}",
                output.status,
                String::from_utf8_lossy(&output.stdout),
                stderr
            )));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Ask the debug broker to inject a normalized touch sequence into the
/// focused compositor SurfaceView. `kind` is one of `tap`, `drag`, or
/// `multitouch`; coordinates are chosen as fractions of the current view
/// size on device, so callers do not need to know the physical screen size.
pub fn inject_touch(kind: &str) -> io::Result<Output> {
    inject_touch_inner(&[("kind", kind)])
}

/// Inject a tap through the focused compositor SurfaceView at Wayland logical
/// coordinates. This keeps tests out of Android screen-space/window-animation
/// races while still exercising the Activity MotionEvent -> compositor path.
pub fn inject_touch_logical(x: f32, y: f32) -> io::Result<Output> {
    let x = format!("{x:.2}");
    let y = format!("{y:.2}");
    inject_touch_inner(&[("kind", "tap-logical"), ("x", &x), ("y", &y)])
}

fn inject_touch_inner(args: &[(&str, &str)]) -> io::Result<Output> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let output = broker_action_raw("inject-touch", args)?;
        if output.status.success() {
            return Ok(output);
        }
        let stderr = String::from_utf8_lossy(&output.stderr);
        if !stderr.contains("no focused CompositorActivity") || Instant::now() >= deadline {
            return Err(io::Error::other(format!(
                "broker action inject-touch failed with {} stdout={:?} stderr={:?}",
                output.status,
                String::from_utf8_lossy(&output.stdout),
                stderr
            )));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Query compositor state via the broker `query-state` action.
/// Observational only — doesn't change input state.
pub fn query_state() -> io::Result<Output> {
    broker_action("query-state", &[])
}

/// The app's `nativeLibraryDir` (where the APK's jniLibs land on this
/// device), via the broker `app-info` action. The tawcroot prod-env
/// tests exec `libtawcroot.so` from there — the one app-readable
/// location `untrusted_app` may execve.
pub fn native_lib_dir() -> io::Result<String> {
    let output = broker_action("app-info", &[])?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    stdout
        .lines()
        .find_map(|line| line.strip_prefix("nativeLibraryDir="))
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .ok_or_else(|| io::Error::other(format!("app-info missing nativeLibraryDir: {stdout:?}")))
}

/// Dynamically update the compositor output scale through the same broker
/// action used by Settings tests. Value is snapped by the app to 0.25x.
pub fn set_output_scale(scale: f32) -> io::Result<Output> {
    let value = format!("{scale:.2}");
    broker_action("set-output-scale", &[("value", &value)])
}

/// Read the current output scale from the app settings facade.
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

/// Dynamically start/stop Xwayland through the same broker action used by Settings.
pub fn set_xwayland(enabled: bool) -> io::Result<Output> {
    let enabled = if enabled { "true" } else { "false" };
    broker_action("set-xwayland", &[("enabled", enabled)])
}

/// Read the current Xwayland setting.
pub fn get_xwayland() -> io::Result<bool> {
    let output = broker_action("get-xwayland", &[])?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "get-xwayland failed: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    match stdout.trim() {
        "true" => Ok(true),
        "false" => Ok(false),
        other => Err(io::Error::other(format!(
            "parse xwayland setting: stdout={other:?}"
        ))),
    }
}

/// Dynamically toggle the contained GTK3 broken menus workaround through
/// the same broker action used by Settings.
pub fn set_gtk3_broken_menus_workaround(enabled: bool) -> io::Result<Output> {
    let enabled = if enabled { "true" } else { "false" };
    broker_action("set-gtk3-broken-menus-workaround", &[("enabled", enabled)])
}

/// Read the current GTK3 broken menus workaround setting.
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

/// Set the per-distro ando enable state for the standing install via the
/// test-mode `set-ando` override (notes/ando.md). Reconciles the broker
/// synchronously: enable brings the listener up (the next rootfs spawn
/// gets the bind); disable tears it down and SIGKILLs in-flight ando
/// children. In-memory only — discarded on app-process death and cleared
/// by `test-init`.
pub fn set_ando(enabled: bool) -> io::Result<Output> {
    let id = crate::install_id();
    let enabled = if enabled { "true" } else { "false" };
    broker_action("set-ando", &[("installId", &id), ("enabled", enabled)])
}

/// Read the effective ando enable state for the standing install
/// (override if set, else metadata).
pub fn get_ando() -> io::Result<bool> {
    let id = crate::install_id();
    let output = broker_action("get-ando", &[("installId", &id)])?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "get-ando failed: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    match stdout.trim() {
        "true" => Ok(true),
        "false" => Ok(false),
        other => Err(io::Error::other(format!("parse ando setting: stdout={other:?}"))),
    }
}

/// Print the launcher entry list for the standing install via the debug
/// `launcher-list` broker action. Returns the raw JSON array string —
/// the crate has no JSON dep, so callers assert on substrings/objects.
/// `show_hidden = true` mirrors the UI's "Show hidden" toggle: hidden
/// entries are included with `"hidden":true` instead of filtered out.
pub fn launcher_list(show_hidden: bool) -> io::Result<String> {
    let id = crate::install_id();
    let show = if show_hidden { "true" } else { "false" };
    let output = broker_action("launcher-list", &[("installId", &id), ("showHidden", show)])?;
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

/// Persist launcher hide/unhide for `entry_id` on the standing install
/// via the `set-entry-hidden` broker action — the same locked metadata
/// write the launcher UI performs. Durable across app restarts, so
/// tests must unhide in cleanup.
pub fn set_entry_hidden(entry_id: &str, hidden: bool) -> io::Result<Output> {
    let id = crate::install_id();
    let hidden = if hidden { "true" } else { "false" };
    broker_action(
        "set-entry-hidden",
        &[("installId", &id), ("entryId", entry_id), ("hidden", hidden)],
    )
}

// Common Android keycodes (used with [ic_send_key_event]).
pub const KEYCODE_DEL: u32 = 67; // Backspace
pub const KEYCODE_FORWARD_DEL: u32 = 112; // Delete (forward delete)
pub const KEYCODE_ENTER: u32 = 66;
pub const KEYCODE_TAB: u32 = 61;
pub const KEYCODE_A: u32 = 29;
pub const KEYCODE_C: u32 = 31;
pub const KEYCODE_V: u32 = 50;
pub const KEYCODE_CTRL_LEFT: u32 = 113;
