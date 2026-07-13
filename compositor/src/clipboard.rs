//! Text clipboard bridge between Android and Wayland selections.
//!
//! Smithay owns the Wayland data-device protocol mechanics. This module
//! owns tawc's platform bridge policy: text MIME selection, eager
//! mirroring of client-owned selections into Android, and paste-time
//! fetches for compositor-owned Android selections (announces are
//! content-free so Android's paste toast only fires on actual pastes).
//!
//! Mirroring is "latest selection wins". At most one pull is in flight,
//! owned by the event loop as `TawcState::clipboard_pull`; a newer
//! selection (or fresh Android text) cancels and replaces it. Clients
//! routinely set the clipboard more than once per copy — GTK3 apps
//! (Firefox, VTE terminals) immediately re-announce the selection with
//! `SAVE_TARGETS` appended — so the bridge must survive bursts and
//! mirror the final state.

use std::fs::File;
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Mutex,
};
use std::time::Duration;

use log::{debug, error, info, warn};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{
    channel, Interest, LoopHandle, Mode, PostAction, RegistrationToken,
};
use smithay::wayland::selection::SelectionTarget;

use crate::compositor::TawcState;

/// Hard cap for eager clipboard pulls into Android. A broken or malicious
/// Wayland/X11 client can keep writing forever; we stop once the transfer
/// crosses this boundary and leave Android's clipboard unchanged.
pub const MAX_TEXT_BYTES: usize = 1024 * 1024;

const PULL_TIMEOUT: Duration = Duration::from_secs(5);

/// Deadline for draining a paste into a client-supplied pipe. A client
/// that requests a paste and never reads must not pin a writer thread
/// (and the fd) for the life of the process.
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

const TEXT_MIMES: &[&str] = &[
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
];

#[derive(Clone)]
pub enum SelectionUserData {
    /// Android's clipboard owns the selection. Payloadless: the content
    /// is fetched from Android at paste time (see
    /// [`write_android_clipboard_to_fd`]) so the OS paste toast fires
    /// only when a client actually pastes.
    Android,
    X11(SelectionTarget),
}

/// Which side owns the selection a pull reads from.
#[derive(Clone, Copy)]
pub enum PullSource {
    Wayland,
    X11,
}

impl PullSource {
    fn label(self) -> &'static str {
        match self {
            PullSource::Wayland => "wayland",
            PullSource::X11 => "x11",
        }
    }
}

pub enum ClipboardEvent {
    /// Android's clipboard holds a text clip; only its description was
    /// read. `ts` is the clip's `ClipDescription.getTimestamp()` (0 on
    /// OEM builds that don't stamp clips); `own_write` means the clip
    /// was written by tawc's own Wayland→Android mirror.
    AndroidClipAvailable { ts: i64, own_write: bool },
    PullSelection {
        source: PullSource,
        mime_type: String,
    },
}

static CLIPBOARD_SENDER: Mutex<Option<channel::Sender<ClipboardEvent>>> = Mutex::new(None);
static NEXT_PULL_ID: AtomicU64 = AtomicU64::new(0);
static PULL_TIMEOUTS_TOTAL: AtomicU64 = AtomicU64::new(0);
static ANDROID_FETCHES_TOTAL: AtomicU64 = AtomicU64::new(0);
static WRITE_TIMEOUTS_TOTAL: AtomicU64 = AtomicU64::new(0);

pub fn debug_state() -> String {
    format!(
        "clipboard_pull_timeouts_total={} clipboard_android_fetches_total={} clipboard_write_timeouts_total={}",
        PULL_TIMEOUTS_TOTAL.load(Ordering::Relaxed),
        ANDROID_FETCHES_TOTAL.load(Ordering::Relaxed),
        WRITE_TIMEOUTS_TOTAL.load(Ordering::Relaxed)
    )
}

pub fn create_clipboard_channel() -> channel::Channel<ClipboardEvent> {
    let (sender, ch) = channel::channel();
    *CLIPBOARD_SENDER.lock().unwrap() = Some(sender);
    ch
}

pub fn clear_clipboard_sender() {
    *CLIPBOARD_SENDER.lock().unwrap() = None;
}

pub fn send_android_clip_available(ts: i64, own_write: bool) {
    if let Some(sender) = CLIPBOARD_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(ClipboardEvent::AndroidClipAvailable { ts, own_write });
    }
}

/// MIME no client ever offers, used by [`selection_exists`] to probe
/// selection state without requesting data.
const PROBE_MIME: &str = "x-tawc/selection-probe";

/// True when any clipboard selection is installed, client- or
/// compositor-owned. Smithay has no direct query, and it clears a dead
/// client source's selection internally without telling the handler, so
/// asking is the only way that survives client death. A request for a
/// MIME nobody offers fails with `NoSelection` (none) or
/// `InvalidMimetype` (some; Smithay checks the MIME before the
/// compositor-vs-client split) without transferring anything. If a
/// client ever did offer the probe MIME the request degrades to one
/// spurious `send` into /dev/null — still "a selection exists".
pub fn selection_exists(state: &TawcState) -> bool {
    use smithay::wayland::selection::data_device::{
        request_data_device_client_selection, SelectionRequestError,
    };
    let Ok(devnull) = File::options().write(true).open("/dev/null") else {
        return false;
    };
    !matches!(
        request_data_device_client_selection(&state.seat, PROBE_MIME.to_string(), devnull.into()),
        Err(SelectionRequestError::NoSelection)
    )
}

/// Queue a pull of a freshly set client selection into Android. Deferred
/// through the clipboard channel because Smithay invokes the selection
/// handlers before installing the new selection in seat state.
pub fn queue_selection_pull(source: PullSource, mime_types: &[String]) {
    let Some(mime_type) = preferred_text_mime(mime_types) else {
        return;
    };
    if let Some(sender) = CLIPBOARD_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(ClipboardEvent::PullSelection { source, mime_type });
    }
}

pub fn text_mime_types() -> Vec<String> {
    TEXT_MIMES.iter().map(|m| (*m).to_string()).collect()
}

pub fn preferred_text_mime(mime_types: &[String]) -> Option<String> {
    TEXT_MIMES
        .iter()
        .find(|mime| mime_types.iter().any(|candidate| candidate == **mime))
        .map(|m| (*m).to_string())
}

pub fn is_supported_text_mime(mime_type: &str) -> bool {
    TEXT_MIMES.contains(&mime_type)
}

pub fn pipe() -> std::io::Result<(OwnedFd, OwnedFd)> {
    let mut fds = [0; 2];
    let rc = unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC) };
    if rc < 0 {
        return Err(std::io::Error::last_os_error());
    }
    let read = unsafe { OwnedFd::from_raw_fd(fds[0]) };
    let write = unsafe { OwnedFd::from_raw_fd(fds[1]) };
    Ok((read, write))
}

/// The in-flight selection read, owned by the event loop. Both event
/// sources (pipe + timeout timer) are deregistered together on every
/// completion, cancellation, or failure path.
pub struct ActivePull {
    /// Defensive only: callbacks act only when their captured id matches
    /// the current pull's. Cancellation already removes both sources, and
    /// calloop's token versioning prevents stale dispatch, so a mismatch
    /// should be unreachable.
    id: u64,
    fd_token: RegistrationToken,
    timer_token: RegistrationToken,
    buf: Vec<u8>,
    label: &'static str,
}

/// Begin mirroring the selection readable on `fd` into Android,
/// canceling any pull already in flight.
pub fn start_pull(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
    fd: OwnedFd,
    source: PullSource,
) {
    let label = source.label();
    cancel_pull(handle, state);

    if let Err(e) = set_nonblocking(fd.as_raw_fd()) {
        warn!("clipboard: failed to set {} pull nonblocking: {}", label, e);
        return;
    }
    let id = NEXT_PULL_ID.fetch_add(1, Ordering::Relaxed);

    let fd_source = Generic::new(File::from(fd), Interest::READ, Mode::Level);
    let fd_token = match handle.insert_source(fd_source, move |_, file, data: &mut TawcState| {
        Ok(pull_readable(&data.loop_handle(), data, id, file.as_ref()))
    }) {
        Ok(token) => token,
        Err(e) => {
            warn!("clipboard: failed to register {} pull source: {}", label, e);
            return;
        }
    };

    let timer = Timer::from_duration(PULL_TIMEOUT);
    // If the deadline and the pipe's final readable event land in the same
    // poll batch with the timer ordered first, a completed transfer is
    // miscounted as a timeout. Accepted: it needs the source to finish in
    // the same wakeup as the 5s deadline, and costs one copy.
    let timer_token = match handle.insert_source(timer, move |_, _, data: &mut TawcState| {
        if let Some(pull) = take_pull_if_current(data, id) {
            data.loop_handle().remove(pull.fd_token);
            warn!("clipboard: timed out waiting for {} selection source", pull.label);
            PULL_TIMEOUTS_TOTAL.fetch_add(1, Ordering::Relaxed);
        }
        TimeoutAction::Drop
    }) {
        Ok(token) => token,
        Err(e) => {
            warn!("clipboard: failed to register {} pull timer: {}", label, e);
            handle.remove(fd_token);
            return;
        }
    };

    state.clipboard_pull = Some(ActivePull {
        id,
        fd_token,
        timer_token,
        buf: Vec::new(),
        label,
    });
}

/// Drop any in-flight pull. Closing the pipe is the cancellation: the
/// selection owner sees EPIPE and stops writing.
pub fn cancel_pull(handle: &LoopHandle<'static, TawcState>, state: &mut TawcState) {
    if let Some(pull) = state.clipboard_pull.take() {
        handle.remove(pull.fd_token);
        handle.remove(pull.timer_token);
    }
}

fn take_pull_if_current(state: &mut TawcState, id: u64) -> Option<ActivePull> {
    if state.clipboard_pull.as_ref().is_some_and(|p| p.id == id) {
        state.clipboard_pull.take()
    } else {
        None
    }
}

/// Terminal-path helper for the fd-source callback: detach the pull from
/// state and deregister its timer. The caller publishes or drops it, and
/// removes the fd source itself by returning `PostAction::Remove`.
fn finish_pull(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
    id: u64,
) -> ActivePull {
    let pull = take_pull_if_current(state, id).unwrap();
    handle.remove(pull.timer_token);
    pull
}

fn pull_readable(
    handle: &LoopHandle<'static, TawcState>,
    state: &mut TawcState,
    id: u64,
    mut file: &File,
) -> PostAction {
    if !state.clipboard_pull.as_ref().is_some_and(|p| p.id == id) {
        return PostAction::Remove;
    }

    let mut chunk = [0u8; 8192];
    loop {
        match file.read(&mut chunk) {
            Ok(0) => {
                publish_to_android(finish_pull(handle, state, id));
                return PostAction::Remove;
            }
            Ok(n) => {
                let pull = state.clipboard_pull.as_mut().unwrap();
                pull.buf.extend_from_slice(&chunk[..n]);
                if pull.buf.len() > MAX_TEXT_BYTES {
                    let pull = finish_pull(handle, state, id);
                    warn!(
                        "clipboard: {} selection exceeded {} byte cap; dropping",
                        pull.label, MAX_TEXT_BYTES
                    );
                    return PostAction::Remove;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                return PostAction::Continue;
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => {
                let pull = finish_pull(handle, state, id);
                warn!("clipboard: failed reading {} selection: {}", pull.label, e);
                return PostAction::Remove;
            }
        }
    }
}

fn publish_to_android(pull: ActivePull) {
    match String::from_utf8(pull.buf) {
        Ok(text) => {
            info!(
                "clipboard: mirrored {} text to Android ({} bytes)",
                pull.label,
                text.len()
            );
            crate::set_android_clipboard_text(&text);
        }
        Err(e) => warn!("clipboard: {} selection was not valid UTF-8: {}", pull.label, e),
    }
}

/// Serve a paste of the compositor-owned Android selection: fetch the
/// clipboard content from Android (the real `getPrimaryClip()` read — the
/// OS paste toast moment) off the event loop, then write it to `fd`. A
/// failed fetch (no focus, non-text clip, over the size cap) drops the fd
/// so the pasting client sees EOF and an empty paste.
///
/// The counter counts paste requests routed through the Android fetch
/// path — including Kotlin-side cache hits and failed fetches — not raw
/// `getPrimaryClip()` reads.
pub fn write_android_clipboard_to_fd(fd: OwnedFd) {
    ANDROID_FETCHES_TOTAL.fetch_add(1, Ordering::Relaxed);
    if let Err(e) = std::thread::Builder::new()
        .name("clipboard-fetch-android".into())
        .spawn(move || {
            let Some(text) = crate::fetch_android_clipboard_text() else {
                warn!("clipboard: Android clipboard fetch failed; sending empty paste");
                return;
            };
            write_to_fd_with_deadline(fd, text.as_bytes());
        }) {
        error!("clipboard: failed to spawn android fetcher: {}", e);
    }
}

/// Drain `buf` into a client-supplied pipe, giving up at [`WRITE_TIMEOUT`].
/// The deadline is absolute: a reader that drains slower than the clip
/// size over 5s gets truncated too, like the pull side's `PULL_TIMEOUT`.
/// Write errors (e.g. EPIPE from a client that closed early) just drop the
/// fd: the client sees a truncated/empty paste.
fn write_to_fd_with_deadline(fd: OwnedFd, mut buf: &[u8]) {
    if let Err(e) = set_nonblocking(fd.as_raw_fd()) {
        warn!("clipboard: failed to set paste fd nonblocking: {}", e);
        return;
    }
    let timed_out = || {
        WRITE_TIMEOUTS_TOTAL.fetch_add(1, Ordering::Relaxed);
        warn!(
            "clipboard: paste client did not drain pipe within {:?}; dropping",
            WRITE_TIMEOUT
        );
    };
    let deadline = std::time::Instant::now() + WRITE_TIMEOUT;
    let mut file = File::from(fd);
    while !buf.is_empty() {
        match file.write(buf) {
            Ok(0) => return,
            Ok(n) => buf = &buf[n..],
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                let Some(remaining) = deadline.checked_duration_since(std::time::Instant::now())
                else {
                    timed_out();
                    return;
                };
                let mut pfd = libc::pollfd {
                    fd: file.as_raw_fd(),
                    events: libc::POLLOUT,
                    revents: 0,
                };
                let timeout_ms = remaining.as_millis().clamp(1, i32::MAX as u128) as i32;
                let rc = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };
                if rc < 0 {
                    let e = std::io::Error::last_os_error();
                    if e.kind() != std::io::ErrorKind::Interrupted {
                        warn!("clipboard: poll on paste fd failed: {}", e);
                        return;
                    }
                } else if rc == 0 {
                    timed_out();
                    return;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => {
                debug!("clipboard: paste fd write failed: {}", e);
                return;
            }
        }
    }
}

fn set_nonblocking(fd: i32) -> std::io::Result<()> {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags < 0 {
        return Err(std::io::Error::last_os_error());
    }
    let rc = unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) };
    if rc < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}
