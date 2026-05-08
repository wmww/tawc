use std::io;
use std::process::{Command, Output, Stdio};
use std::sync::OnceLock;

/// Path to the host-side tawc-exec helper. Absolute path lets tests
/// invoke it without needing it on $PATH. Built by
/// `scripts/build-tawc-exec.sh`. Override via `TAWC_EXEC_BIN`.
fn tawc_exec_bin() -> &'static str {
    static B: OnceLock<String> = OnceLock::new();
    B.get_or_init(|| {
        if let Ok(env) = std::env::var("TAWC_EXEC_BIN") { return env; }
        // CARGO_MANIFEST_DIR is `tests/integration`; the binary lives
        // at <repo>/build/tawc-exec/tawc-exec.
        let repo = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors().nth(2).map(|p| p.to_path_buf())
            .unwrap_or_else(|| std::path::PathBuf::from("."));
        repo.join("build").join("tawc-exec").join("tawc-exec")
            .to_string_lossy().into_owned()
    }).as_str()
}

/// Run an adb shell command, wait for completion, return output.
pub fn shell(cmd: &str) -> io::Result<Output> {
    Command::new("adb")
        .args(["shell", cmd])
        .output()
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
pub fn rootfs_run(cmd: &str) -> io::Result<Output> {
    Command::new(tawc_exec_bin())
        .args(["--in-rootfs", &crate::install_id(), "--", cmd])
        .output()
}

/// Spawn a command in the chroot with piped stdout/stderr (non-blocking).
/// Returns the Child process. Caller is responsible for reading output.
///
/// Same broker dispatch as [rootfs_run]. The rootfs-session invariant
/// (notes/rootfs-sessions.md) is enforced inside
/// [InstallationMethod.startInside] — no caller-side `setsid` needed.
pub fn rootfs_spawn(cmd: &str) -> io::Result<std::process::Child> {
    Command::new(tawc_exec_bin())
        .args(["--in-rootfs", &crate::install_id(), "--", cmd])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
}

/// Send text to the compositor via broadcast intent — equivalent to
/// Gboard calling `commitText(text, 1)`. Goes through:
/// BroadcastReceiver -> TawcInputConnection.commitText -> nativeCommitText
/// -> text_input_v3 -> Wayland client.
///
/// More reliable than `adb shell input text` which may be intercepted by
/// the IME before reaching the editor.
pub fn input_text(text: &str) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.TEXT_INPUT --es text '{}'",
        text.replace('\'', "'\\''")
    ))
}

/// Set composing/preedit text — equivalent to Gboard calling
/// `setComposingText(text, 1)`. The Wayland client should display this as
/// a preedit (highlighted, not yet committed). Successive calls replace the
/// previous preedit. Pass an empty string to clear the preedit.
pub fn input_set_composing(text: &str) -> io::Result<Output> {
    input_set_composing_with_delete(text, 0, 0)
}

/// `setComposingText` with explicit composing-region replacement deltas.
/// `delete_before` / `delete_after` are UTF-16 code units around the
/// cursor that should be removed from committed text before the new
/// preedit is set — i.e. simulating Gboard's `setComposingRegion(s, e)`
/// followed by `setComposingText(text)` flow without depending on an
/// actual IME service. (Test broadcasts bypass the system IME to avoid
/// non-determinism — see CompositorActivity.testInputReceiver.)
pub fn input_set_composing_with_delete(text: &str, delete_before: u32, delete_after: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.SET_COMPOSING_TEXT --es text '{}' --ei deleteBefore {} --ei deleteAfter {}",
        text.replace('\'', "'\\''"), delete_before, delete_after
    ))
}

/// `commitText` with explicit composing-region replacement deltas.
/// See [input_set_composing_with_delete] for delta semantics.
pub fn input_text_with_delete(text: &str, delete_before: u32, delete_after: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.TEXT_INPUT --es text '{}' --ei deleteBefore {} --ei deleteAfter {}",
        text.replace('\'', "'\\''"), delete_before, delete_after
    ))
}

/// Finalize the current preedit as committed text — equivalent to Gboard
/// calling `finishComposingText()`. The active preedit becomes part of the
/// editor's text and the preedit is cleared.
pub fn input_finish_composing() -> io::Result<Output> {
    shell("am broadcast -a me.phie.tawc.FINISH_COMPOSING_TEXT")
}

/// Delete `before` UTF-16 code units before the cursor and `after` after —
/// equivalent to Gboard calling `deleteSurroundingText(before, after)`.
pub fn input_delete_surrounding(before: u32, after: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.DELETE_SURROUNDING_TEXT --ei before {} --ei after {}",
        before, after
    ))
}

/// Send a key event to the compositor via broadcast intent.
/// Goes through: BroadcastReceiver -> nativeSendKeyEvent -> wl_keyboard.
pub fn input_keyevent(keycode: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode {}",
        keycode
    ))
}

/// Send a tap at physical screen coordinates (x, y).
/// The compositor divides by touch_scale (2) to get logical coordinates.
pub fn input_tap(x: u32, y: u32) -> io::Result<Output> {
    shell(&format!("input tap {} {}", x, y))
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

/// Trigger the compositor to log its current state.
pub fn broadcast_query_state() -> io::Result<Output> {
    shell("am broadcast -a me.phie.tawc.QUERY_STATE")
}

// ---- IC-driven test broadcasts ----
// These call methods on the active TawcInputConnection so tests can
// exercise the same code path real Gboard takes (composing-region
// delta computation, Editable mirror, etc.). Prefer the bypassing
// `input_*` helpers above for compositor-pipeline tests; use these
// only when the IC's own behaviour is what's under test.

/// Call `TawcInputConnection.commitText(text, 1)` on the active IC.
pub fn ic_commit_text(text: &str) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.IC_COMMIT_TEXT --es text '{}'",
        text.replace('\'', "'\\''")
    ))
}

/// Call `TawcInputConnection.setComposingText(text, 1)` on the active IC.
pub fn ic_set_composing_text(text: &str) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.IC_SET_COMPOSING_TEXT --es text '{}'",
        text.replace('\'', "'\\''")
    ))
}

/// Call `TawcInputConnection.setComposingRegion(start, end)` on the active IC.
/// Marks the given Editable range as composing without changing its content —
/// the bridge between Android's "composing region annotates committed text"
/// and Wayland's "preedit is overlay only".
pub fn ic_set_composing_region(start: u32, end: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.IC_SET_COMPOSING_REGION --ei start {} --ei end {}",
        start, end
    ))
}

/// Call `TawcInputConnection.finishComposingText()` on the active IC.
pub fn ic_finish_composing() -> io::Result<Output> {
    shell("am broadcast -a me.phie.tawc.IC_FINISH_COMPOSING")
}

/// Call `TawcInputConnection.setSelection(start, end)` on the active IC.
/// Moves the Editable's cursor without going through any wire protocol —
/// simulates an IME that "thinks" the cursor is somewhere different from
/// where the Wayland client has it. Bypasses our delta-propagation logic
/// because there's no wayland-side equivalent for "move the cursor".
pub fn ic_set_selection(start: u32, end: u32) -> io::Result<Output> {
    shell(&format!(
        "am broadcast -a me.phie.tawc.IC_SET_SELECTION --ei start {} --ei end {}",
        start, end
    ))
}

// Common Android keycodes (used with input_keyevent)
pub const KEYCODE_DEL: u32 = 67; // Backspace
pub const KEYCODE_ENTER: u32 = 66;
pub const KEYCODE_TAB: u32 = 61;
