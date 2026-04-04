use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

use crate::adb;

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
    let output = adb::shell("dumpsys activity activities | grep me.phie.tawc/.MainActivity")?;
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
    // Poll for the Wayland socket to appear (compositor is ready)
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    loop {
        let output = adb::shell("test -e /data/local/arch-chroot/tmp/wayland-0 && echo ready")?;
        if String::from_utf8_lossy(&output.stdout).contains("ready") {
            break;
        }
        if std::time::Instant::now() > deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Wayland socket did not appear within 10s",
            ));
        }
        thread::sleep(Duration::from_millis(100));
    }
    STARTED.store(true, Ordering::Relaxed);
    Ok(())
}
