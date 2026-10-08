//! Remote access (notes/remote-access.md). The idle check needs nothing.
//! The live one goes through sshyeet.com from both the target and this
//! host, so it is ignored unless the runner got `TAWC_LIVE_RELAY=1`
//! (`--cfg tawc_live_relay`).

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use tawc_integration::adb::{self, json_str};

fn wait<T>(what: &str, timeout: Duration, mut f: impl FnMut() -> Option<T>) -> T {
    let end = Instant::now() + timeout;
    loop {
        if let Some(v) = f() {
            return v;
        }
        assert!(Instant::now() < end, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(250));
    }
}

fn has_remote_hold() -> bool {
    adb::session_state().expect("session-state").iter().any(|r| r.starts_with("remote "))
}

#[test]
fn test_remote_inert_until_started() {
    tawc_integration::helpers::test_init();
    let status = adb::remote_status().expect("remote-status");
    let state = json_str(&status, "state").unwrap_or_default();
    assert!(state == "stopped" || state == "failed", "agent running outside a test: {status}");
    assert!(!has_remote_hold());
}

fn scratch(name: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("tawc-remote-it-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Local-network mode, without a network: the agent listens on the
/// device's loopback and `adb forward` carries the host's ssh there.
/// Secret login, then key login, through TAWC's sshd and sftp-server.
#[test]
fn test_remote_local_mode() {
    tawc_integration::helpers::test_init();
    adb::remote_stop().expect("remote-stop");
    const PORT: u16 = 42222;
    // adb picks the host port so parallel runs against other devices
    // don't collide on it.
    let out = Command::new("adb")
        .args(["forward", "tcp:0", &format!("tcp:{PORT}")])
        .output()
        .expect("adb forward");
    assert!(out.status.success(), "adb forward: {}", String::from_utf8_lossy(&out.stderr));
    let host_port: u16 = String::from_utf8_lossy(&out.stdout).trim().parse().expect("adb forward port");
    let fwd = format!("tcp:{host_port}");
    let dir = scratch("local");
    let ssh = |user: &str, key: Option<&std::path::Path>, cmd: &str| {
        let mut c = Command::new("ssh");
        c.args(["-F", "/dev/null", "-p", &host_port.to_string()])
            .args(["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"])
            .args(["-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "IdentitiesOnly=yes"]);
        match key {
            Some(k) => c.arg("-i").arg(k),
            None => c.args(["-o", "IdentityFile=/dev/null"]),
        };
        c.arg(format!("{user}@127.0.0.1")).arg(cmd).output().expect("ssh")
    };

    adb::remote_start_local(&format!("127.0.0.1:{PORT}"), None).expect("remote-start");
    let status = wait("local ready", Duration::from_secs(20), || {
        let s = adb::remote_status().ok()?;
        (json_str(&s, "state").as_deref() == Some("ready")).then_some(s)
    });
    assert_eq!(json_str(&status, "mode").as_deref(), Some("local"));
    let secret = json_str(&status, "secret").unwrap();
    let fingerprint = json_str(&status, "fingerprint").unwrap();
    assert_eq!(json_str(&status, "command").unwrap(), format!("ssh -p {PORT} {secret}@127.0.0.1"));
    assert_eq!(secret.split('-').count(), 3, "{secret}");
    let out = ssh(&secret, None, "grep ^ID= /etc/os-release");
    assert!(String::from_utf8_lossy(&out.stdout).starts_with("ID="), "{}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(ssh("acid-vowel-417", None, "true").status.code(), Some(255));
    adb::remote_stop().expect("remote-stop");

    // Key login: the key's holder gets in under any name; the secret is off.
    let key = dir.join("id");
    let out = Command::new("ssh-keygen").args(["-q", "-t", "ed25519", "-N", "", "-f"]).arg(&key).output().unwrap();
    assert!(out.status.success());
    let public = std::fs::read_to_string(key.with_extension("pub")).unwrap();
    adb::remote_start_local(&format!("127.0.0.1:{PORT}"), Some(public.trim())).expect("remote-start keys");
    let status = wait("keys ready", Duration::from_secs(20), || {
        let s = adb::remote_status().ok()?;
        (json_str(&s, "state").as_deref() == Some("ready")).then_some(s)
    });
    assert_eq!(json_str(&status, "secret").as_deref(), Some(""));
    // The device's saved host key: the same across Starts.
    assert_eq!(json_str(&status, "fingerprint").unwrap(), fingerprint);
    assert!(status.contains("\"key_count\":1"), "{status}");
    let out = ssh("root", Some(&key), "echo key-in");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "key-in\n", "{}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(ssh("root", None, "true").status.code(), Some(255));

    adb::remote_stop().expect("remote-stop");
    let _ = Command::new("adb").args(["forward", "--remove", &fwd]).output();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
#[cfg_attr(not(tawc_live_relay), ignore = "needs network on target and host: TAWC_LIVE_RELAY=1")]
fn test_remote_live_relay() {
    tawc_integration::helpers::test_init();
    adb::remote_stop().expect("remote-stop");
    adb::remote_start(0).expect("remote-start");
    let status = wait("relay ready", Duration::from_secs(60), || {
        let s = adb::remote_status().ok()?;
        match json_str(&s, "state").as_deref() {
            Some("ready") => Some(s),
            Some("failed") => panic!("agent failed: {s}"),
            _ => None,
        }
    });
    let (id, secret, jump, key) = (
        json_str(&status, "id").unwrap(),
        json_str(&status, "secret").unwrap(),
        json_str(&status, "jump").unwrap(),
        json_str(&status, "host_key").unwrap(),
    );
    let install = tawc_integration::install_id();
    assert!(adb::session_state().unwrap().contains(&format!("remote {install} 0")));

    let dir = std::env::temp_dir().join(format!("tawc-remote-it-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("known_hosts"), format!("{id} {key}\n")).unwrap();
    std::fs::write(
        dir.join("config"),
        format!(
            "Host *\n  UserKnownHostsFile {0}/known_hosts {0}/jump_known_hosts\n  StrictHostKeyChecking accept-new\n  BatchMode yes\n  ConnectTimeout 30\n",
            dir.display()
        ),
    )
    .unwrap();
    let ssh = || {
        let mut c = Command::new("ssh");
        c.args(["-F", &dir.join("config").to_string_lossy(), "-J", &jump]).arg(format!("{secret}@{id}"));
        c
    };

    let out = ssh().arg("grep ^ID= /etc/os-release; id -u").output().expect("ssh");
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(text.starts_with("ID=") && text.ends_with("\n0\n"), "{text}\n{}", String::from_utf8_lossy(&out.stderr));

    // sftp: TAWC's sftp-server, with names and home from the guest's
    // /etc/passwd (not bionic's Android IDs).
    std::fs::write(dir.join("batch"), "pwd\nls -l /etc/passwd\n").unwrap();
    let out = Command::new("sftp")
        .args(["-F", &dir.join("config").to_string_lossy(), "-J", &jump, "-b"])
        .arg(dir.join("batch"))
        .arg(format!("{secret}@{id}"))
        .output()
        .expect("sftp");
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(out.status.success(), "{text}\n{}", String::from_utf8_lossy(&out.stderr));
    assert!(text.contains("Remote working directory: /root"), "{text}");
    assert!(text.split_whitespace().collect::<Vec<_>>().windows(2).any(|w| w == ["root", "root"]), "{text}");

    // A login still running at Stop is hung up.
    let mut child = ssh()
        .arg("echo pid=$$; exec sleep 600")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("ssh");
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap()).read_line(&mut line).unwrap();
    let pid: u32 = line.trim().strip_prefix("pid=").and_then(|p| p.parse().ok()).expect("pid line");
    wait("one client", Duration::from_secs(10), || {
        (json_str(&adb::remote_status().ok()?, "state").as_deref() == Some("ready")
            && adb::remote_status().ok()?.contains("\"clients\":1"))
        .then_some(())
    });
    adb::remote_stop().expect("remote-stop");
    assert!(!has_remote_hold());
    wait("ssh client exit", Duration::from_secs(15), || child.try_wait().ok().flatten());
    wait("login gone", Duration::from_secs(10), || {
        let out = adb::shell(&format!("ps -o PID= -p {pid}")).ok()?;
        String::from_utf8_lossy(&out.stdout).trim().is_empty().then_some(())
    });
    let _ = std::fs::remove_dir_all(&dir);
}
