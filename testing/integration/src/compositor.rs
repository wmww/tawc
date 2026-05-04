use std::io;
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

/// Panic with a clear message if the compositor isn't running. The
/// harness never starts the compositor itself — `run-integration-tests.sh`
/// launches it once before invoking `cargo test` and force-stops it
/// after the suite. A failure here means the script wasn't used (or
/// the compositor died mid-suite), and the right fix is to re-run
/// `bash testing/run-integration-tests.sh` rather than to start it
/// from inside a test.
pub fn assert_running() {
    match is_running() {
        Ok(true) => {}
        Ok(false) => panic!(
            "tawc compositor is not running on the device — run the suite via \
             `bash testing/run-integration-tests.sh` (which starts the compositor) \
             instead of invoking `cargo test` directly"
        ),
        Err(e) => panic!("failed to check whether tawc compositor is running: {e}"),
    }
}

/// True iff the tawc app process is alive AND the chroot-visible
/// Wayland socket exists. Both conditions matter: `am force-stop`
/// leaves the unix-domain socket file behind on disk even though no
/// process is listening, so the file alone would falsely indicate
/// readiness on the very next test run.
pub fn is_running() -> io::Result<bool> {
    // /tmp inside the chroot is the rootfs's /tmp dir; from outside,
    // that's /data/data/me.phie.tawc/distros/<id>/rootfs/tmp. The
    // compositor puts its socket at /data/data/me.phie.tawc/wayland-0
    // and 01-tawc.sh symlinks it to /tmp/wayland-0 inside the chroot —
    // the symlink is what we check here.
    let cmd = format!(
        "pidof me.phie.tawc >/dev/null && \
         su -c 'test -e /data/data/me.phie.tawc/distros/{}/rootfs/tmp/wayland-0' \
         && echo ready",
        crate::install_id(),
    );
    let output = adb::shell(&cmd)?;
    Ok(String::from_utf8_lossy(&output.stdout).contains("ready"))
}
