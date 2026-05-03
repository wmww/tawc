use std::io;
use std::process::{Command, Output, Stdio};
use std::sync::OnceLock;

const PKG: &str = "me.phie.tawc";

/// Path to the on-device chroot wrapper rendered by the in-app
/// installer (see [`ChrootMounter.enterScript`] /
/// [`ProotMethod.renderEnterScript`]). Mount + chroot logic lives
/// there; these helpers just deliver the (base64-encoded) command via
/// the right adb-side wrapper.
fn enter_script() -> String {
    format!("/data/data/{}/distros/{}/enter.sh", PKG, crate::install_id())
}
fn meta_path() -> String {
    format!("/data/data/{}/distros/{}/metadata.json", PKG, crate::install_id())
}

/// Run an adb shell command, wait for completion, return output.
pub fn shell(cmd: &str) -> io::Result<Output> {
    Command::new("adb")
        .args(["shell", cmd])
        .output()
}

/// Run a command inside the in-app Arch chroot on the phone. Routes
/// through `su -c` for chroot installs (the mount + chroot syscalls
/// in `enter.sh` need root) and through `run-as` for proot installs
/// (proot tracees that run as root via `su` hit a bionic-loader TLS
/// abort while loading vendor GPU libs; running them as the app uid
/// matches the packaging context the libs expect). Method is read
/// from the install's `metadata.json` once per process.
pub fn chroot_run(cmd: &str) -> io::Result<Output> {
    shell(&chroot_invocation(cmd))
}

/// Spawn a command in the chroot with piped stdout/stderr (non-blocking).
/// Returns the Child process. Caller is responsible for reading output.
pub fn chroot_spawn(cmd: &str) -> io::Result<std::process::Child> {
    Command::new("adb")
        .args(["shell", &chroot_invocation(cmd)])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
}

/// Detected install method (`"chroot"` or `"proot"`), cached after
/// the first probe. Returning the string keeps callers free to pass
/// it through to log lines without round-tripping through an enum.
fn install_method() -> &'static str {
    static M: OnceLock<String> = OnceLock::new();
    M.get_or_init(|| {
        // `run-as` first because it doesn't need root and works on
        // both rooted and rootless devices for debuggable APKs (we
        // are debuggable). Fall back to `su` for non-debuggable
        // builds. If both fail the test will fail loudly later
        // anyway, so we don't try to be clever about reporting.
        let meta = meta_path();
        let probe = format!(
            "run-as {pkg} cat {meta} 2>/dev/null || su -c 'cat {meta}' 2>/dev/null",
            pkg = PKG,
            meta = meta,
        );
        let raw = Command::new("adb")
            .args(["shell", &probe])
            .output()
            .ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
            .unwrap_or_default();
        // Pull "method" out of the JSON without a serde dep — single
        // field, low schema risk. awk -F'"' on the matching line
        // mirrors what `client/tawc-chroot-run` does.
        for line in raw.lines() {
            if let Some(rest) = line.split_once("\"method\"") {
                if let Some(after_colon) = rest.1.split_once(':') {
                    let v = after_colon.1.trim().trim_start_matches('"');
                    if let Some(end) = v.find('"') {
                        return v[..end].to_string();
                    }
                }
            }
        }
        // No metadata yet (or unreadable) — assume chroot. Tests that
        // depend on chroot_run will fail with a clearer error than a
        // missing-method panic anyway.
        "chroot".to_string()
    }).as_str()
}

fn chroot_invocation(cmd: &str) -> String {
    // Base64 alphabet has no shell metacharacters, so embedding the
    // payload bare in the wrapped shell command is safe.
    let b64 = b64_encode(cmd.as_bytes());
    let enter = enter_script();
    match install_method() {
        m @ ("proot" | "tawcroot") => {
            // run-as switches to the app uid before exec'ing
            // enter.sh. mksh's default cwd under run-as is /data/local
            // where the app uid has no write access, which breaks
            // its here-doc temp-file logic; cd + TMPDIR to the app's
            // own cache dir first. Same trick the host launcher
            // (`client/tawc-chroot-run`) uses. tawcroot is the same
            // shape as proot here — runs as app uid, no extra setup
            // needed beyond TMPDIR.
            let scratch = format!("/data/data/{}/cache/{}-tmp", PKG, m);
            format!(
                "run-as {pkg} sh -c 'mkdir -p {scratch} && cd {scratch} && export TMPDIR={scratch} && exec {enter} {b64}'",
                pkg = PKG,
                scratch = scratch,
                enter = enter,
                b64 = b64,
            )
        }
        // Chroot (or unknown — treat as chroot) goes through su.
        _ => format!("su -c '{} {}'", enter, b64),
    }
}

/// Standard base64 (RFC 4648) with `=` padding, no line breaks. Inlined
/// rather than pulling a crate in for ~25 lines that the test harness
/// uses in exactly one place.
fn b64_encode(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(((data.len() + 2) / 3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = chunk.get(1).copied().unwrap_or(0) as u32;
        let b2 = chunk.get(2).copied().unwrap_or(0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(T[((n >> 18) & 63) as usize] as char);
        out.push(T[((n >> 12) & 63) as usize] as char);
        out.push(if chunk.len() > 1 { T[((n >> 6) & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[(n & 63) as usize] as char } else { '=' });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::b64_encode;
    #[test]
    fn b64_matches_reference() {
        // Spot-check against known values.
        assert_eq!(b64_encode(b""), "");
        assert_eq!(b64_encode(b"f"), "Zg==");
        assert_eq!(b64_encode(b"fo"), "Zm8=");
        assert_eq!(b64_encode(b"foo"), "Zm9v");
        assert_eq!(b64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(b64_encode(b"hello"), "aGVsbG8=");
        assert_eq!(b64_encode(b"\xff\xfe\xfd"), "//79");
    }
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
