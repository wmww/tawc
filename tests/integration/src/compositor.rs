use std::io;
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
    pub x11_surfaces: u32,
    pub x11_surfaces_with_host: u32,
    pub wlegl_create_buffer_total: u64,
    pub wlegl_import_texture_total: u64,
    pub last_wlegl_width: u32,
    pub last_wlegl_height: u32,
    pub last_wlegl_format: u32,
    pub output_physical_w: i32,
    pub output_physical_h: i32,
    pub output_logical_w: i32,
    pub output_logical_h: i32,
    pub output_advertised: bool,
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
                return Err(format!("query-state returned malformed stdout: {stdout:?}"));
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
    let mut x11_surfaces = None;
    let mut x11_surfaces_with_host = None;
    let mut wlegl_create_buffer_total = None;
    let mut wlegl_import_texture_total = None;
    let mut last_wlegl_width = None;
    let mut last_wlegl_height = None;
    let mut last_wlegl_format = None;
    let mut output_physical_w = None;
    let mut output_physical_h = None;
    let mut output_logical_w = None;
    let mut output_logical_h = None;
    let mut output_advertised = None;
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
                "x11_surfaces" => x11_surfaces = Some(val.parse().ok()?),
                "x11_surfaces_with_host" => x11_surfaces_with_host = Some(val.parse().ok()?),
                "wlegl_create_buffer_total" => wlegl_create_buffer_total = Some(val.parse().ok()?),
                "wlegl_import_texture_total" => wlegl_import_texture_total = Some(val.parse().ok()?),
                "last_wlegl_width" => last_wlegl_width = Some(val.parse().ok()?),
                "last_wlegl_height" => last_wlegl_height = Some(val.parse().ok()?),
                "last_wlegl_format" => last_wlegl_format = Some(val.parse().ok()?),
                "output_physical_w" => output_physical_w = Some(val.parse().ok()?),
                "output_physical_h" => output_physical_h = Some(val.parse().ok()?),
                "output_logical_w" => output_logical_w = Some(val.parse().ok()?),
                "output_logical_h" => output_logical_h = Some(val.parse().ok()?),
                "output_advertised" => output_advertised = Some(val.parse().ok()?),
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
        x11_surfaces: x11_surfaces.unwrap_or_default(),
        x11_surfaces_with_host: x11_surfaces_with_host.unwrap_or_default(),
        wlegl_create_buffer_total: wlegl_create_buffer_total.unwrap_or_default(),
        wlegl_import_texture_total: wlegl_import_texture_total.unwrap_or_default(),
        last_wlegl_width: last_wlegl_width.unwrap_or_default(),
        last_wlegl_height: last_wlegl_height.unwrap_or_default(),
        last_wlegl_format: last_wlegl_format.unwrap_or_default(),
        output_physical_w: output_physical_w.unwrap_or_default(),
        output_physical_h: output_physical_h.unwrap_or_default(),
        output_logical_w: output_logical_w.unwrap_or_default(),
        output_logical_h: output_logical_h.unwrap_or_default(),
        output_advertised: output_advertised.unwrap_or_default(),
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
        thread::sleep(Duration::from_millis(200));
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
/// launches it once before invoking `cargo test` and force-stops it
/// after the suite. A failure here means the script wasn't used (or
/// the compositor died mid-suite), and the right fix is to re-run
/// `scripts/run-integration-tests.sh` rather than to start it
/// from inside a test.
pub fn assert_running() {
    match is_running() {
        Ok(true) => {}
        Ok(false) => panic!(
            "tawc compositor is not running on the device — run the suite via \
             `scripts/run-integration-tests.sh` (which starts the compositor) \
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
    // The compositor binds its socket at <appData>/share/wayland-0;
    // pidof is shell-readable (process is in untrusted_app but `pidof`
    // just walks /proc, world-readable). The socket file lives in app
    // data, so probe it through the broker (runs as app uid).
    let pid_output = adb::shell("pidof me.phie.tawc")?;
    if pid_output.stdout.iter().filter(|b| b.is_ascii_digit()).count() == 0 {
        return Ok(false);
    }
    let exists = adb::rootfs_host_exec(&[
        "/system/bin/sh", "-c",
        "test -e /data/data/me.phie.tawc/share/wayland-0",
    ])?;
    Ok(exists.status.success())
}
