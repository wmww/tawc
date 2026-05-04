use std::io;
use std::process::ChildStdout;
use std::thread;
use std::time::{Duration, Instant};

use crate::adb;

/// How long to wait for the process group to die after SIGKILL.
const STOP_TIMEOUT: Duration = Duration::from_secs(10);

/// How long to wait for the pidfile to appear after spawning.
const PID_TIMEOUT: Duration = Duration::from_secs(10);

/// Path for the pidfile helper script inside the chroot.
const PIDFILE_HELPER_CHROOT: &str = "/tmp/tawc-pidfile-exec";

/// Filesystem root of the in-app install, as seen from outside.
/// Driven by [`crate::install_id`]; see [`adb`].
fn chroot_rootfs() -> String {
    format!("/data/data/me.phie.tawc/distros/{}/rootfs", crate::install_id())
}

/// Path for the pidfile helper script on the device filesystem (outside chroot).
fn pidfile_helper_device() -> String {
    format!("{}{}", chroot_rootfs(), PIDFILE_HELPER_CHROOT)
}

/// A process running inside the Android chroot with process-group-based lifecycle.
///
/// Uses a pidfile helper to capture the PID without relying on process name
/// lookup. The PGID is read from /proc/PID/stat on the host. `stop()` kills
/// the entire process tree via process group signals.
pub struct ChrootProcess {
    /// The local adb shell process.
    child: std::process::Child,
    /// Path to the pidfile on the device (accessible from host and chroot).
    pidfile_device: String,
    /// PID from the pidfile (the command process after exec).
    pid: Option<u32>,
    /// PGID of the process group leader (from /proc/PID/stat on the host).
    pgid: Option<u32>,
    stopped: bool,
}

impl ChrootProcess {
    /// Spawn a command in the chroot.
    ///
    /// Wraps the command with a pidfile helper that writes the process PID
    /// to a unique file, which is then used to discover the PGID from
    /// /proc/PID/stat. No process-name-based lookup is needed.
    ///
    /// Does NOT block waiting for the PGID — call `ensure_pgid()` later when
    /// you're ready to wait. This avoids blocking stdout reads.
    pub fn spawn(cmd: &str) -> io::Result<Self> {
        ensure_pidfile_helper()?;

        // Generate a unique pidfile path
        use std::sync::atomic::{AtomicU32, Ordering};
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pidfile_chroot = format!("/tmp/tawc-pid-{}-{}", std::process::id(), n);
        let pidfile_device = format!("{}{}", chroot_rootfs(), pidfile_chroot);

        // Wrap the command through the pidfile helper:
        // /tmp/tawc-pidfile-exec PIDFILE COMMAND...
        // The helper writes its PID to PIDFILE then exec's the command.
        let wrapped = format!(
            "{} {} {}",
            PIDFILE_HELPER_CHROOT, pidfile_chroot, cmd
        );
        let child = adb::chroot_spawn(&wrapped)?;

        Ok(Self {
            child,
            pidfile_device,
            pid: None,
            pgid: None,
            stopped: false,
        })
    }

    /// Wait for the pidfile to appear and discover the PID/PGID.
    /// Call this after setting up stdout reading so the process doesn't stall.
    pub fn ensure_pgid(&mut self) {
        let deadline = Instant::now() + PID_TIMEOUT;
        while Instant::now() < deadline {
            if let Some((pid, pgid)) = self.read_pid_and_pgid() {
                self.pid = Some(pid);
                self.pgid = Some(pgid);
                self.cleanup_pidfile();
                return;
            }
            thread::sleep(Duration::from_millis(100));
        }
        self.cleanup_pidfile();
        panic!(
            "Failed to read PID/PGID from pidfile {} within {:?}",
            self.pidfile_device, PID_TIMEOUT
        );
    }

    /// Take stdout from the underlying adb process (can only be called once).
    pub fn take_stdout(&mut self) -> Option<ChildStdout> {
        self.child.stdout.take()
    }

    /// Check if the chroot process is still running by probing /proc/PID.
    /// Returns false if the process has exited (crashed/closed).
    pub fn is_running(&self) -> bool {
        let Some(pid) = self.pid else { return false };
        let ok = adb::shell(&format!("su -c 'test -d /proc/{}'", pid));
        ok.map(|o| o.status.success()).unwrap_or(false)
    }

    /// Stop the process and all its children by killing the entire process tree.
    ///
    /// Walks the process tree to find all descendant PGIDs (handling children
    /// that created their own process groups like Firefox). Sends SIGTERM,
    /// waits briefly, then SIGKILL. Waits for all to exit.
    pub fn stop(&mut self) -> Result<(), String> {
        self.stopped = true;

        // Check if the chroot process already exited before we try to kill it.
        let already_gone = self.pid.is_some() && !self.is_running();

        // Kill the entire process tree (handles children with different PGIDs)
        if let Some(pid) = self.pid {
            kill_process_tree(pid, STOP_TIMEOUT)?;
        }

        // Also kill the local adb shell process
        let _ = self.child.kill();
        let _ = self.child.wait();

        if already_gone {
            return Err("Process exited/crashed before being stopped".to_string());
        }

        Ok(())
    }

    /// Read the PID from the pidfile, then get the PGID from /proc/PID/stat.
    /// Returns (pid, pgid).
    fn read_pid_and_pgid(&self) -> Option<(u32, u32)> {
        let output = adb::shell(&format!(
            "su -c 'cat {} 2>/dev/null'",
            self.pidfile_device
        ))
        .ok()?;
        let pid: u32 = String::from_utf8_lossy(&output.stdout)
            .trim()
            .parse()
            .ok()?;

        // Read PGID from /proc/PID/stat on the host.
        // Format: pid (comm) state ppid pgrp ...
        // comm can contain spaces, so find the last ')' to skip it.
        let stat_output = adb::shell(&format!("su -c 'cat /proc/{}/stat'", pid)).ok()?;
        let stat = String::from_utf8_lossy(&stat_output.stdout);
        let stat = stat.trim();
        let after_comm = stat.rfind(')')? + 2;
        // Fields after comm: state ppid pgrp ...
        // pgrp is the 3rd field (state=1, ppid=2, pgrp=3)
        let pgid: u32 = stat.get(after_comm..)?
            .split_whitespace()
            .nth(2)?
            .parse()
            .ok()?;
        Some((pid, pgid))
    }

    /// Remove the pidfile from the device.
    fn cleanup_pidfile(&self) {
        let _ = adb::shell(&format!("su -c 'rm -f {}'", self.pidfile_device));
    }
}

impl Drop for ChrootProcess {
    fn drop(&mut self) {
        if !self.stopped {
            // Fallback cleanup if stop() wasn't called (e.g. test panicked).
            if let Some(pid) = self.pid {
                for pgid in collect_descendant_pgids(pid) {
                    let _ = adb::chroot_run(&format!(
                        "kill -KILL -- -{} 2>/dev/null; true",
                        pgid
                    ));
                }
            }
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
        self.cleanup_pidfile();
    }
}

/// Ensure the pidfile helper script exists in the chroot.
/// Retries on failure (unlike Once, which would silently skip after a failed attempt).
fn ensure_pidfile_helper() -> io::Result<()> {
    use std::sync::Mutex;
    static PUSHED: Mutex<bool> = Mutex::new(false);
    let mut pushed = PUSHED.lock().unwrap();
    if *pushed {
        return Ok(());
    }
    let helper_src = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("tawc-pidfile-exec");
    let staging = format!("{}/tawc-pidfile-exec", crate::TAWC_SCRATCH);
    adb::shell(&format!("mkdir -p {}", crate::TAWC_SCRATCH))?;
    std::process::Command::new("adb")
        .args(["push", &helper_src.to_string_lossy(), &staging])
        .output()?;
    let helper_dev = pidfile_helper_device();
    adb::shell(&format!(
        "su -c 'cp {} {} && chmod +x {}'",
        staging, helper_dev, helper_dev
    ))?;
    *pushed = true;
    Ok(())
}

/// Kill a process tree rooted at `pid`, handling child processes that may
/// have created their own process groups (e.g. Firefox content processes).
///
/// Collects all unique PGIDs among descendants, then SIGTERM + SIGKILL all.
fn kill_process_tree(pid: u32, timeout: Duration) -> Result<(), String> {
    let pgids = collect_descendant_pgids(pid);
    if pgids.is_empty() {
        return Ok(());
    }

    // SIGTERM all process groups
    for &pgid in &pgids {
        let _ = adb::chroot_run(&format!("kill -TERM -- -{} 2>/dev/null; true", pgid));
    }
    thread::sleep(Duration::from_millis(500));

    // SIGKILL stragglers
    for &pgid in &pgids {
        let _ = adb::chroot_run(&format!("kill -KILL -- -{} 2>/dev/null; true", pgid));
    }

    // Wait until all process groups are dead
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        let all_gone = pgids.iter().all(|&pgid| {
            let output = adb::chroot_run(&format!(
                "kill -0 -- -{} 2>/dev/null && echo RUNNING || echo GONE",
                pgid
            ));
            output
                .map(|o| String::from_utf8_lossy(&o.stdout).contains("GONE"))
                .unwrap_or(true)
        });
        if all_gone {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(200));
    }
    Err(format!(
        "Process tree (PGIDs {:?}) did not exit within {:?}",
        pgids, timeout
    ))
}

/// Collect all unique PGIDs from a process and its descendants.
/// Runs `ps` on the Android host — this works because chroot shares the
/// host PID namespace (no PID namespace isolation).
fn collect_descendant_pgids(root_pid: u32) -> Vec<u32> {
    let output = match adb::shell("su -c 'ps -eo pid,ppid,pgid'") {
        Ok(o) => o,
        Err(_) => return vec![],
    };
    let stdout = String::from_utf8_lossy(&output.stdout);

    // Parse into a table (header line is skipped by parse failures)
    let mut children: std::collections::HashMap<u32, Vec<u32>> = std::collections::HashMap::new();
    let mut pgid_of: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
    for line in stdout.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() >= 3 {
            if let (Ok(pid), Ok(ppid), Ok(pgid)) = (
                parts[0].parse::<u32>(),
                parts[1].parse::<u32>(),
                parts[2].parse::<u32>(),
            ) {
                children.entry(ppid).or_default().push(pid);
                pgid_of.insert(pid, pgid);
            }
        }
    }

    // BFS from root_pid to find all descendants
    let mut pgids = std::collections::HashSet::new();
    let mut queue = vec![root_pid];
    while let Some(pid) = queue.pop() {
        if let Some(&pgid) = pgid_of.get(&pid) {
            pgids.insert(pgid);
        }
        if let Some(kids) = children.get(&pid) {
            queue.extend(kids);
        }
    }

    pgids.into_iter().collect()
}
