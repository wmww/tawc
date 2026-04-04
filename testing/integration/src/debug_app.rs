use std::io::{self, BufRead, BufReader};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::chroot_process::ChrootProcess;

const PROTOCOL_PREFIX: &str = "TAWC_DEBUG:";

/// Minimum delay after each wait completes, so actions are visible on screen.
const MIN_ACTION_DELAY: Duration = Duration::from_millis(50);

/// A running instance of gtk3-debug-app with structured output capture.
pub struct DebugApp {
    process: ChrootProcess,
    /// All received protocol lines (without the TAWC_DEBUG: prefix).
    lines: Arc<Mutex<Vec<String>>>,
    /// Channel for new protocol lines as they arrive.
    line_rx: mpsc::Receiver<String>,
}

impl DebugApp {
    /// Start the debug app with the given subcommand.
    /// `binary_path` is the path inside the chroot (e.g. "/tmp/gtk3-debug-app/gtk3-debug-app").
    /// `gdk_gl` controls GTK3's GL usage: "gles:always" for hardware buffers, "disabled" for SHM.
    pub fn start(binary_path: &str, subcommand: &str, gdk_gl: &str) -> io::Result<Self> {
        let cmd = format!("GDK_GL={} {} {}", gdk_gl, binary_path, subcommand);
        let mut proc = ChrootProcess::spawn(&cmd)?;

        let stdout = proc.take_stdout().expect("stdout was piped");
        let lines = Arc::new(Mutex::new(Vec::new()));
        let lines_clone = lines.clone();
        let (tx, rx) = mpsc::channel();

        // Reader thread: continuously drain stdout, filter for protocol lines
        thread::spawn(move || {
            let reader = BufReader::new(stdout);
            for line in reader.lines() {
                match line {
                    Ok(line) => {
                        // adb shell may add \r
                        let line = line.trim_end_matches('\r').to_string();
                        if let Some(payload) = line.strip_prefix(PROTOCOL_PREFIX) {
                            let payload = payload.to_string();
                            lines_clone.lock().unwrap().push(payload.clone());
                            let _ = tx.send(payload);
                        }
                    }
                    Err(_) => break,
                }
            }
        });

        // Discover PGID now that stdout is being drained (so the process won't stall)
        proc.ensure_pgid();

        Ok(Self {
            process: proc,
            lines,
            line_rx: rx,
        })
    }

    /// Wait until a protocol line starting with `prefix` appears.
    /// Returns the full payload (after TAWC_DEBUG:).
    pub fn wait_for(&self, prefix: &str, timeout: Duration) -> Result<String, String> {
        let deadline = Instant::now() + timeout;

        // Check already-received lines
        for line in self.lines.lock().unwrap().iter() {
            if line.starts_with(prefix) {
                return Ok(line.clone());
            }
        }

        // Wait for new lines
        loop {
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| {
                    let received = self.lines.lock().unwrap().clone();
                    format!(
                        "Timeout waiting for '{}' (received: {:?})",
                        prefix, received
                    )
                })?;

            match self.line_rx.recv_timeout(remaining) {
                Ok(line) if line.starts_with(prefix) => return Ok(line),
                Ok(_) => continue,
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    let received = self.lines.lock().unwrap().clone();
                    return Err(format!(
                        "Timeout after {:?} waiting for '{}' (received: {:?})",
                        timeout, prefix, received
                    ));
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    let received = self.lines.lock().unwrap().clone();
                    return Err(format!(
                        "Debug app exited while waiting for '{}' (received: {:?})",
                        prefix, received
                    ));
                }
            }
        }
    }

    /// Wait for the app to signal it's ready (window mapped, focused).
    pub fn wait_ready(&self) -> Result<(), String> {
        self.wait_for("READY", Duration::from_secs(30)).map(|_| ())
    }

    /// Get the value from the most recent TEXT_CHANGED line.
    pub fn last_text(&self) -> Option<String> {
        self.lines
            .lock()
            .unwrap()
            .iter()
            .rev()
            .find_map(|l| l.strip_prefix("TEXT_CHANGED:").map(|s| s.to_string()))
    }

    /// Get the most recent cursor position (character offset).
    pub fn last_cursor_pos(&self) -> Option<u32> {
        self.lines
            .lock()
            .unwrap()
            .iter()
            .rev()
            .find_map(|l| l.strip_prefix("CURSOR_POS:").and_then(|s| s.parse().ok()))
    }

    /// Get the text content as it was at the time of the most recent CURSOR_POS event.
    /// Useful for detecting if a click changed text content (it shouldn't).
    pub fn text_at_last_cursor_event(&self) -> (Option<String>, Option<u32>) {
        let lines = self.lines.lock().unwrap();
        let mut last_text = None;
        let mut last_cursor = None;
        for line in lines.iter() {
            if let Some(t) = line.strip_prefix("TEXT_CHANGED:") {
                last_text = Some(t.to_string());
            }
            if let Some(c) = line.strip_prefix("CURSOR_POS:") {
                if let Ok(pos) = c.parse::<u32>() {
                    last_cursor = Some(pos);
                }
            }
        }
        (last_text, last_cursor)
    }

    /// Count how many TEXT_CHANGED events have been received so far.
    pub fn text_changed_count(&self) -> usize {
        self.lines
            .lock()
            .unwrap()
            .iter()
            .filter(|l| l.starts_with("TEXT_CHANGED:"))
            .count()
    }

    /// Wait until `last_text()` equals `expected`, or timeout.
    pub fn wait_for_text(&self, expected: &str, timeout: Duration) -> Result<String, String> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(text) = self.last_text() {
                if text == expected {
                    thread::sleep(MIN_ACTION_DELAY);
                    return Ok(text);
                }
            }
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| {
                    format!(
                        "Timeout waiting for text '{}' (last: {:?})",
                        expected,
                        self.last_text()
                    )
                })?;
            // Wait for any new line, then re-check
            match self.line_rx.recv_timeout(remaining.min(Duration::from_millis(100))) {
                Ok(_) | Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    return Err(format!(
                        "Debug app exited waiting for text '{}' (last: {:?})",
                        expected,
                        self.last_text()
                    ));
                }
            }
        }
    }

    /// Wait until `text_changed_count()` exceeds `prev_count`, or timeout.
    /// Returns the new text value.
    pub fn wait_for_text_change(&self, prev_count: usize, timeout: Duration) -> Result<String, String> {
        let deadline = Instant::now() + timeout;
        loop {
            if self.text_changed_count() > prev_count {
                thread::sleep(MIN_ACTION_DELAY);
                return Ok(self.last_text().unwrap());
            }
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| {
                    format!(
                        "Timeout waiting for text change (count still {}, last: {:?})",
                        prev_count,
                        self.last_text()
                    )
                })?;
            match self.line_rx.recv_timeout(remaining.min(Duration::from_millis(100))) {
                Ok(_) | Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    return Err("Debug app exited waiting for text change".into());
                }
            }
        }
    }

    /// Wait until `cursor_pos_count()` exceeds `prev_count`, or timeout.
    pub fn wait_for_cursor_change(&self, prev_count: usize, timeout: Duration) -> Result<u32, String> {
        let deadline = Instant::now() + timeout;
        loop {
            if self.cursor_pos_count() > prev_count {
                thread::sleep(MIN_ACTION_DELAY);
                return Ok(self.last_cursor_pos().unwrap());
            }
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| {
                    format!("Timeout waiting for CURSOR_POS (count still {})", prev_count)
                })?;
            match self.line_rx.recv_timeout(remaining.min(Duration::from_millis(100))) {
                Ok(_) | Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    return Err("Debug app exited waiting for cursor change".into());
                }
            }
        }
    }

    /// Count how many CURSOR_POS events have been received so far.
    pub fn cursor_pos_count(&self) -> usize {
        self.lines
            .lock()
            .unwrap()
            .iter()
            .filter(|l| l.starts_with("CURSOR_POS:"))
            .count()
    }

    /// Stop the debug app and all its children, verify it didn't crash.
    pub fn stop(&mut self) -> Result<(), String> {
        self.process.stop()
    }
}
