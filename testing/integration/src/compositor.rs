use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use crate::adb;

/// Compositor state snapshot, parsed from COMPOSITOR_STATE log line.
#[derive(Debug, Clone)]
pub struct CompositorState {
    pub clients: u32,
    pub toplevels: u32,
    pub surfaces_wlegl: u32,
    pub surfaces_shm: u32,
    pub frames: u64,
    /// Number of toplevels visible in the last rendered frame.
    /// If this is non-zero while toplevels is zero, the screen shows a stale frame.
    pub rendered_toplevels: u32,
}

/// Query the compositor's current state via broadcast intent.
/// Does NOT clear logcat — counts existing COMPOSITOR_STATE lines and waits for a new one.
pub fn query_state(timeout: Duration) -> Result<CompositorState, String> {
    // Count existing COMPOSITOR_STATE lines so we can detect the new one
    let existing_logs = adb::logcat_dump_tawc()
        .map_err(|e| format!("Failed to dump logcat: {}", e))?;
    let existing_count = existing_logs.lines()
        .filter(|l| l.contains("COMPOSITOR_STATE:"))
        .count();

    adb::broadcast_query_state().map_err(|e| format!("Failed to send query: {}", e))?;

    let deadline = Instant::now() + timeout;
    loop {
        let logs = adb::logcat_dump_tawc()
            .map_err(|e| format!("Failed to dump logcat: {}", e))?;
        let state_lines: Vec<&str> = logs.lines()
            .filter(|l| l.contains("COMPOSITOR_STATE:"))
            .collect();
        if state_lines.len() > existing_count {
            // Parse the newest COMPOSITOR_STATE line
            if let Some(state) = parse_compositor_state_line(state_lines.last().unwrap()) {
                return Ok(state);
            }
        }
        if Instant::now() > deadline {
            return Err(format!(
                "Timeout waiting for COMPOSITOR_STATE in logcat (existing: {}, current: {})",
                existing_count, state_lines.len()
            ));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Wait until the compositor reports the expected number of clients and toplevels.
pub fn wait_for_state(
    expected_clients: u32,
    expected_toplevels: u32,
    timeout: Duration,
) -> Result<CompositorState, String> {
    let deadline = Instant::now() + timeout;
    loop {
        let state = query_state(Duration::from_secs(2))?;
        if state.clients == expected_clients && state.toplevels == expected_toplevels {
            return Ok(state);
        }
        if Instant::now() > deadline {
            return Err(format!(
                "Compositor state didn't reach clients={} toplevels={} within {:?} (last: {:?})",
                expected_clients, expected_toplevels, timeout, state
            ));
        }
        thread::sleep(Duration::from_millis(200));
    }
}

fn parse_compositor_state_line(line: &str) -> Option<CompositorState> {
    let idx = line.find("COMPOSITOR_STATE:")?;
    let payload = &line[idx + "COMPOSITOR_STATE:".len()..];
    let mut clients = None;
    let mut toplevels = None;
    let mut surfaces_wlegl = None;
    let mut surfaces_shm = None;
    let mut frames = None;
    let mut rendered_toplevels = None;
    for part in payload.split_whitespace() {
        if let Some((key, val)) = part.split_once('=') {
            match key {
                "clients" => clients = Some(val.parse().ok()?),
                "toplevels" => toplevels = Some(val.parse().ok()?),
                "surfaces_wlegl" => surfaces_wlegl = Some(val.parse().ok()?),
                "surfaces_shm" => surfaces_shm = Some(val.parse().ok()?),
                "frames" => frames = Some(val.parse().ok()?),
                "rendered_toplevels" => rendered_toplevels = Some(val.parse().ok()?),
                _ => {}
            }
        }
    }
    Some(CompositorState {
        clients: clients?,
        toplevels: toplevels?,
        surfaces_wlegl: surfaces_wlegl?,
        surfaces_shm: surfaces_shm?,
        frames: frames?,
        rendered_toplevels: rendered_toplevels?,
    })
}

/// Wait until the last rendered frame shows the expected number of toplevels.
/// This ensures the screen actually reflects the current state, not a stale frame.
pub fn wait_for_rendered_toplevels(
    expected: u32,
    timeout: Duration,
) -> Result<CompositorState, String> {
    let deadline = Instant::now() + timeout;
    loop {
        let state = query_state(Duration::from_secs(2))?;
        if state.rendered_toplevels == expected {
            return Ok(state);
        }
        if Instant::now() > deadline {
            return Err(format!(
                "Screen still shows {} toplevels (expected {}), compositor state: {:?}",
                state.rendered_toplevels, expected, state
            ));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

static STARTED: AtomicBool = AtomicBool::new(false);

/// Stop the compositor if we started it. Called from the test shutdown hook.
pub fn stop_if_started() {
    if STARTED.load(Ordering::Relaxed) {
        eprintln!("Stopping compositor...");
        let _ = adb::shell("am force-stop me.phie.tawc");
    }
}

/// Ensure the tawc compositor is running and visible on the phone.
/// Restarts it if not running or if backgrounded/paused.
pub fn ensure_running() -> io::Result<()> {
    let output = adb::shell("dumpsys activity activities | grep me.phie.tawc")?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    // Check for both presence AND visibility. A paused/backgrounded compositor
    // won't have a functioning Wayland socket or receive touch events.
    let is_running = stdout.contains("me.phie.tawc/.MainActivity");
    let is_visible = stdout.contains("visible=true");

    if is_running && is_visible {
        return Ok(());
    }

    if is_running {
        eprintln!("Compositor paused/backgrounded, restarting...");
        adb::shell("am force-stop me.phie.tawc")?;
        thread::sleep(Duration::from_millis(500));
    } else {
        eprintln!("Starting compositor...");
    }

    adb::shell("am start -n me.phie.tawc/.MainActivity")?;
    // Poll for the Wayland socket to appear (compositor is ready).
    // The socket symlink target is in the app's private data dir, so
    // we need root to follow it.
    let deadline = std::time::Instant::now() + Duration::from_secs(15);
    loop {
        let output = adb::shell("su -c 'test -e /data/local/arch-chroot/tmp/wayland-0 && echo ready'")?;
        if String::from_utf8_lossy(&output.stdout).contains("ready") {
            break;
        }
        if std::time::Instant::now() > deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Wayland socket did not appear within 15s",
            ));
        }
        thread::sleep(Duration::from_millis(100));
    }
    STARTED.store(true, Ordering::Relaxed);
    Ok(())
}
