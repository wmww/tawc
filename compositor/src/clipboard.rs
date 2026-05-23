//! Text clipboard bridge between Android and Wayland selections.
//!
//! Smithay owns the Wayland data-device protocol mechanics. This module
//! owns tawc's platform bridge policy: text MIME selection, bounded eager
//! reads for mirroring client-owned selections into Android, and async
//! writes for compositor-owned Android selections.

use std::fs::File;
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Mutex,
};
use std::time::{Duration, Instant};

use log::{error, info, warn};
use smithay::reexports::calloop::channel;
use smithay::wayland::selection::SelectionTarget;

/// Hard cap for eager clipboard pulls into Android. A broken or malicious
/// Wayland/X11 client can keep writing forever; we stop once the transfer
/// crosses this boundary and leave Android's clipboard unchanged.
pub const MAX_TEXT_BYTES: usize = 1024 * 1024;

const PULL_TIMEOUT: Duration = Duration::from_secs(5);

const TEXT_MIMES: &[&str] = &[
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
];

#[derive(Clone)]
pub enum SelectionUserData {
    AndroidText(String),
    X11(SelectionTarget),
}

pub enum ClipboardEvent {
    AndroidText(String),
    PullWaylandSelection { mime_type: String },
}

static CLIPBOARD_SENDER: Mutex<Option<channel::Sender<ClipboardEvent>>> = Mutex::new(None);
static READ_ACTIVE: AtomicBool = AtomicBool::new(false);
static READ_GENERATION: AtomicU64 = AtomicU64::new(0);
static PULL_TIMEOUTS_TOTAL: AtomicU64 = AtomicU64::new(0);

pub fn debug_state() -> String {
    format!(
        "clipboard_pull_timeouts_total={}",
        PULL_TIMEOUTS_TOTAL.load(Ordering::Relaxed)
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

pub fn send_android_text(text: String) {
    if text.len() > MAX_TEXT_BYTES {
        warn!(
            "clipboard: dropping Android clipboard text over cap: {} bytes",
            text.len()
        );
        return;
    }
    if let Some(sender) = CLIPBOARD_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(ClipboardEvent::AndroidText(text));
    }
}

pub fn queue_wayland_pull(mime_types: Vec<String>) {
    let Some(mime_type) = preferred_text_mime(&mime_types) else {
        return;
    };
    if let Some(sender) = CLIPBOARD_SENDER.lock().unwrap().as_ref() {
        let _ = sender.send(ClipboardEvent::PullWaylandSelection { mime_type });
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

pub fn read_fd_for_android(fd: OwnedFd, label: &'static str) {
    let generation = READ_GENERATION.fetch_add(1, Ordering::AcqRel) + 1;
    if READ_ACTIVE.swap(true, Ordering::AcqRel) {
        return;
    }

    if let Err(e) = std::thread::Builder::new()
        .name(format!("clipboard-read-{label}"))
        .spawn(move || {
            let result = read_capped_utf8(fd);
            READ_ACTIVE.store(false, Ordering::Release);
            if generation != READ_GENERATION.load(Ordering::Acquire) {
                return;
            }
            match result {
                Ok(Some(text)) => {
                    info!("clipboard: mirrored {} text to Android ({} bytes)", label, text.len());
                    crate::set_android_clipboard_text(&text);
                }
                Ok(None) => {}
                Err(e) => warn!("clipboard: failed reading {} selection: {}", label, e),
            }
        }) {
        READ_ACTIVE.store(false, Ordering::Release);
        error!("clipboard: failed to spawn reader: {}", e);
    }
}

pub fn write_text_to_fd(fd: OwnedFd, text: String) {
    if let Err(e) = std::thread::Builder::new()
        .name("clipboard-write-android".into())
        .spawn(move || {
            let mut file = File::from(fd);
            let _ = file.write_all(text.as_bytes());
        }) {
        error!("clipboard: failed to spawn writer: {}", e);
    }
}

fn read_capped_utf8(fd: OwnedFd) -> std::io::Result<Option<String>> {
    set_nonblocking(fd.as_raw_fd())?;
    let raw_fd = fd.as_raw_fd();
    let mut file = File::from(fd);
    let deadline = Instant::now() + PULL_TIMEOUT;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 8192];

    loop {
        match file.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.len() > MAX_TEXT_BYTES {
                    warn!(
                        "clipboard: selection exceeded {} byte cap; dropping",
                        MAX_TEXT_BYTES
                    );
                    return Ok(None);
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                let now = Instant::now();
                if now >= deadline {
                    warn!("clipboard: timed out waiting for selection source");
                    PULL_TIMEOUTS_TOTAL.fetch_add(1, Ordering::Relaxed);
                    return Ok(None);
                }
                poll_readable(raw_fd, deadline - now)?;
            }
            Err(e) => return Err(e),
        }
    }

    match String::from_utf8(buf) {
        Ok(text) => Ok(Some(text)),
        Err(e) => {
            warn!("clipboard: selection was not valid UTF-8: {}", e);
            Ok(None)
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

fn poll_readable(fd: i32, timeout: Duration) -> std::io::Result<()> {
    let mut pfd = libc::pollfd {
        fd,
        events: libc::POLLIN | libc::POLLHUP | libc::POLLERR,
        revents: 0,
    };
    let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
    let rc = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };
    if rc < 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}
