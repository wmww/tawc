use std::io;
use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

use crate::adb;

/// Compositor state snapshot returned by the `query-state` broker action.
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
    pub hosts: u32,
    pub bound_hosts: u32,
    pub xwayland_running: bool,
    /// Live Xwayland process ids (compositor-side /proc walk, same
    /// enumeration the idle-stop kill sweep uses).
    pub xwayland_pids: Vec<u32>,
    pub x11_surfaces: u32,
    pub x11_surfaces_with_host: u32,
    pub wlegl_create_buffer_total: u64,
    pub wlegl_import_texture_total: u64,
    /// wl_buffer destroys for wlegl-backed buffers. Xwayland destroys a
    /// TAWC-DRI wl_buffer only after receiving the compositor's release,
    /// so this advancing during an X11 GL run proves buffer releases are
    /// served end-to-end.
    pub wlegl_buffer_destroy_total: u64,
    pub last_wlegl_width: u32,
    pub last_wlegl_height: u32,
    pub last_wlegl_format: u32,
    pub output_physical_w: i32,
    pub output_physical_h: i32,
    pub output_logical_w: i32,
    pub output_logical_h: i32,
    /// Vsync ticks the compositor has handled; idle compositors don't tick.
    pub vsync_ticks: u64,
    /// `CLOCK_MONOTONIC` ns of the last vsync tick.
    pub last_vsync_ns: i64,
    pub output_refresh_mhz: u32,
    /// Hosts reserved by launcher taps and not yet matched.
    pub pending_launches: u32,
    /// `<host>:<windows>` per registered host, comma-separated; `-` for none.
    pub host_windows: String,
}

impl CompositorState {
    /// Windows assigned to registered host `id`, or None if no such host.
    pub fn host_windows(&self, id: &str) -> Option<u32> {
        self.host_windows
            .split(',')
            .find_map(|entry| entry.strip_prefix(id)?.strip_prefix(':')?.parse().ok())
    }
}

/// Query the compositor's current state via the in-app broker.
pub fn query_state(timeout: Duration) -> Result<CompositorState, String> {
    let deadline = Instant::now() + timeout;
    loop {
        match adb::query_state() {
            Ok(output) if output.status.success() => {
                let stdout = String::from_utf8_lossy(&output.stdout);
                if let Some(state) = parse_compositor_state(stdout.trim()) {
                    return Ok(state);
                }
                if stdout.trim() != STOPPED {
                    return Err(format!("query-state returned malformed stdout: {stdout:?}"));
                }
                // Stopped (no clients): keep waiting for a start.
                if Instant::now() > deadline {
                    return Err("compositor is stopped".to_string());
                }
            }
            Ok(output) => {
                let stderr = String::from_utf8_lossy(&output.stderr);
                if Instant::now() > deadline {
                    return Err(format!(
                        "query-state failed with {} stderr={stderr:?}",
                        output.status
                    ));
                }
            }
            Err(e) => {
                if Instant::now() > deadline {
                    return Err(format!("query-state failed: {e}"));
                }
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// `query-state`'s whole answer while no compositor thread exists. The
/// action never starts one.
const STOPPED: &str = "stopped";

/// True iff the app answers and reports the compositor stopped.
pub fn is_stopped() -> Result<bool, String> {
    let output = adb::query_state().map_err(|e| e.to_string())?;
    Ok(output.status.success() && String::from_utf8_lossy(&output.stdout).trim() == STOPPED)
}

/// Wait for the idle rule (or an exit) to stop the compositor.
pub fn wait_for_stopped(timeout: Duration) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    loop {
        if is_stopped()? {
            return Ok(());
        }
        if Instant::now() > deadline {
            return Err(format!(
                "compositor still running after {timeout:?}: {:?}",
                query_state_once()
            ));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Query state once and return true if the app answers. Used by readiness
/// checks that need to prove the compositor loop is dispatching.
pub fn query_state_once() -> Result<CompositorState, String> {
    let output = adb::query_state().map_err(|e| e.to_string())?;
    if !output.status.success() {
        return Err(format!(
            "query-state failed with {} stderr={:?}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    parse_compositor_state(stdout.trim())
        .ok_or_else(|| format!("query-state returned malformed stdout: {stdout:?}"))
}

fn parse_compositor_state(line: &str) -> Option<CompositorState> {
    parse_compositor_state_payload(line)
}

fn parse_compositor_state_payload(payload: &str) -> Option<CompositorState> {
    let mut clients = None;
    let mut toplevels = None;
    let mut surfaces_wlegl = None;
    let mut surfaces_shm = None;
    let mut frames = None;
    let mut rendered_toplevels = None;
    let mut hosts = None;
    let mut bound_hosts = None;
    let mut xwayland_running = None;
    let mut xwayland_pids = None;
    let mut x11_surfaces = None;
    let mut x11_surfaces_with_host = None;
    let mut wlegl_create_buffer_total = None;
    let mut wlegl_import_texture_total = None;
    let mut wlegl_buffer_destroy_total = None;
    let mut last_wlegl_width = None;
    let mut last_wlegl_height = None;
    let mut last_wlegl_format = None;
    let mut output_physical_w = None;
    let mut output_physical_h = None;
    let mut output_logical_w = None;
    let mut output_logical_h = None;
    let mut vsync_ticks = None;
    let mut last_vsync_ns = None;
    let mut output_refresh_mhz = None;
    let mut pending_launches = None;
    let mut host_windows = None;
    for part in payload.split_whitespace() {
        if let Some((key, val)) = part.split_once('=') {
            match key {
                "clients" => clients = Some(val.parse().ok()?),
                "toplevels" => toplevels = Some(val.parse().ok()?),
                "surfaces_wlegl" => surfaces_wlegl = Some(val.parse().ok()?),
                "surfaces_shm" => surfaces_shm = Some(val.parse().ok()?),
                "frames" => frames = Some(val.parse().ok()?),
                "rendered_toplevels" => rendered_toplevels = Some(val.parse().ok()?),
                "hosts" => hosts = Some(val.parse().ok()?),
                "bound_hosts" => bound_hosts = Some(val.parse().ok()?),
                "xwayland_running" => xwayland_running = Some(val.parse().ok()?),
                "xwayland_pids" => {
                    xwayland_pids = Some(
                        val.split(',')
                            .filter(|s| !s.is_empty())
                            .map(|s| s.parse().ok())
                            .collect::<Option<Vec<u32>>>()?,
                    )
                }
                "x11_surfaces" => x11_surfaces = Some(val.parse().ok()?),
                "x11_surfaces_with_host" => x11_surfaces_with_host = Some(val.parse().ok()?),
                "wlegl_create_buffer_total" => wlegl_create_buffer_total = Some(val.parse().ok()?),
                "wlegl_import_texture_total" => wlegl_import_texture_total = Some(val.parse().ok()?),
                "wlegl_buffer_destroy_total" => wlegl_buffer_destroy_total = Some(val.parse().ok()?),
                "last_wlegl_width" => last_wlegl_width = Some(val.parse().ok()?),
                "last_wlegl_height" => last_wlegl_height = Some(val.parse().ok()?),
                "last_wlegl_format" => last_wlegl_format = Some(val.parse().ok()?),
                "output_physical_w" => output_physical_w = Some(val.parse().ok()?),
                "output_physical_h" => output_physical_h = Some(val.parse().ok()?),
                "output_logical_w" => output_logical_w = Some(val.parse().ok()?),
                "output_logical_h" => output_logical_h = Some(val.parse().ok()?),
                "vsync_ticks" => vsync_ticks = Some(val.parse().ok()?),
                "last_vsync_ns" => last_vsync_ns = Some(val.parse().ok()?),
                "output_refresh_mhz" => output_refresh_mhz = Some(val.parse().ok()?),
                "pending_launches" => pending_launches = Some(val.parse().ok()?),
                "host_windows" => host_windows = Some(val.to_string()),
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
        hosts: hosts.unwrap_or_default(),
        bound_hosts: bound_hosts.unwrap_or_default(),
        xwayland_running: xwayland_running.unwrap_or_default(),
        xwayland_pids: xwayland_pids.unwrap_or_default(),
        x11_surfaces: x11_surfaces.unwrap_or_default(),
        x11_surfaces_with_host: x11_surfaces_with_host.unwrap_or_default(),
        wlegl_create_buffer_total: wlegl_create_buffer_total.unwrap_or_default(),
        wlegl_import_texture_total: wlegl_import_texture_total.unwrap_or_default(),
        wlegl_buffer_destroy_total: wlegl_buffer_destroy_total.unwrap_or_default(),
        last_wlegl_width: last_wlegl_width.unwrap_or_default(),
        last_wlegl_height: last_wlegl_height.unwrap_or_default(),
        last_wlegl_format: last_wlegl_format.unwrap_or_default(),
        output_physical_w: output_physical_w.unwrap_or_default(),
        output_physical_h: output_physical_h.unwrap_or_default(),
        output_logical_w: output_logical_w.unwrap_or_default(),
        output_logical_h: output_logical_h.unwrap_or_default(),
        vsync_ticks: vsync_ticks.unwrap_or_default(),
        last_vsync_ns: last_vsync_ns.unwrap_or_default(),
        output_refresh_mhz: output_refresh_mhz.unwrap_or_default(),
        pending_launches: pending_launches.unwrap_or_default(),
        host_windows: host_windows.unwrap_or_default(),
    })
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
        thread::sleep(Duration::from_millis(50));
    }
}

/// Wait until the compositor is fully clean: no clients, toplevels,
/// surfaces, or Android hosts. Host teardown goes through an async
/// Activity `finish()` round trip, so the host counts can legitimately
/// trail the surface counts by a moment — poll them all together
/// instead of asserting the host counts point-in-time.
pub fn wait_for_clean_state(timeout: Duration) -> Result<CompositorState, String> {
    let deadline = Instant::now() + timeout;
    loop {
        let state = query_state(Duration::from_secs(2))?;
        if state.clients == 0
            && state.toplevels == 0
            && state.surfaces_wlegl == 0
            && state.surfaces_shm == 0
            && state.hosts == 0
            && state.bound_hosts == 0
        {
            return Ok(state);
        }
        if Instant::now() > deadline {
            return Err(format!(
                "Compositor did not reach a clean state within {timeout:?} (last: {state:?})"
            ));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Wait until the last rendered frame shows at least `min` toplevels —
/// i.e. the newly mapped window has actually reached the screen, not
/// just the compositor's client state.
pub fn wait_for_rendered_toplevels_at_least(
    min: u32,
    timeout: Duration,
) -> Result<CompositorState, String> {
    let deadline = Instant::now() + timeout;
    loop {
        let state = query_state(Duration::from_secs(2))?;
        if state.rendered_toplevels >= min {
            return Ok(state);
        }
        if Instant::now() > deadline {
            return Err(format!(
                "Screen shows {} toplevels (expected >= {}), compositor state: {:?}",
                state.rendered_toplevels, min, state
            ));
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Wait until the compositor's monotonic `frames` counter has advanced
/// by at least `min_delta` beyond `baseline`. The counter ticks on
/// every client commit, so this is the "the client committed again"
/// signal. Returns the observed delta, which is less than `min_delta`
/// only when the deadline passed — callers that treat the extra
/// commits as optional (grace-period replacements) can ignore the
/// shortfall, callers that require progress should assert on it.
pub fn wait_for_frames_advance(baseline: u64, min_delta: u64, timeout: Duration) -> u64 {
    let deadline = Instant::now() + timeout;
    let mut last = 0;
    loop {
        if let Ok(state) = query_state(Duration::from_secs(2)) {
            last = state.frames.saturating_sub(baseline);
            if last >= min_delta {
                return last;
            }
        }
        if Instant::now() > deadline {
            return last;
        }
        thread::sleep(Duration::from_millis(50));
    }
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
/// pins it running (`compositor-hold`) before invoking `cargo test` and
/// force-stops the app after the suite. A failure here means the script wasn't used (or
/// the compositor died mid-suite), and the right fix is to re-run
/// `scripts/run-integration-tests.sh` rather than to start it
/// from inside a test.
pub fn assert_running() {
    match is_running() {
        Ok(true) => {}
        Ok(false) => panic!(
            "TAWC compositor is not running on the device — run the suite via \
             `scripts/run-integration-tests.sh` (which starts the compositor) \
             instead of invoking `cargo test` directly"
        ),
        Err(e) => panic!("failed to check whether TAWC compositor is running: {e}"),
    }
}

/// True iff the compositor loop answers a `query-state` round-trip AND
/// the chroot-visible Wayland socket exists. The broker round-trip is
/// stronger than a pidof check — it proves the app process is up and
/// the compositor event loop is dispatching. The socket is bound once
/// per app process (before anything is spawned into a rootfs), so a
/// success is cached and later calls collapse to the single query-state
/// round-trip.
pub fn is_running() -> io::Result<bool> {
    static SOCKET_SEEN: OnceLock<()> = OnceLock::new();
    if query_state_once().is_err() {
        return Ok(false);
    }
    if SOCKET_SEEN.get().is_some() {
        return Ok(true);
    }
    // The socket file lives in app data, so probe it through the
    // broker (runs as app uid).
    let probe = format!("test -e {}/share/wayland-0", crate::app_data_dir());
    let exists = adb::rootfs_host_exec(&["/system/bin/sh", "-c", &probe])?;
    if exists.status.success() {
        let _ = SOCKET_SEEN.set(());
    }
    Ok(exists.status.success())
}
