//! End-to-end: agent <-> fake relay <-> the host's `ssh`, no network.
//! `TAWC_LIVE_RELAY=1` also runs `live_relay` against sshyeet.com.

mod support;

use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Command, Output, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use support::{wait_for, Relay};
use tawc_remote::event::Event;
use tawc_remote::spawn::HostShell;
use tawc_remote::agent::{Login, Transport};
use tawc_remote::{Agent, Config, Status};

const T: Duration = Duration::from_secs(20);

struct Harness {
    agent: Agent,
    events: Arc<Mutex<Vec<Event>>>,
}

impl Harness {
    fn start(relay_url: &str, idle_timeout: Option<Duration>) -> Harness {
        Self::start_with(Transport::Relay(relay_url.into()), Login::Secret, idle_timeout, None).unwrap()
    }

    fn start_keyed(relay_url: &str, key: &std::path::Path) -> Harness {
        Self::start_with(Transport::Relay(relay_url.into()), Login::Secret, None, Some(key)).unwrap()
    }

    fn start_with(
        transport: Transport,
        login: Login,
        idle_timeout: Option<Duration>,
        host_key: Option<&std::path::Path>,
    ) -> std::io::Result<Harness> {
        let events = Arc::new(Mutex::new(Vec::new()));
        let ev = events.clone();
        let agent = Agent::start(Config {
            transport,
            login,
            host_key: host_key.map(Into::into),
            idle_timeout,
            agent: "tawc/test linux/x".into(),
            version: "test".into(),
            launcher: Arc::new(HostShell { shell: "/bin/sh".into() }),
            events: Arc::new(move |e| ev.lock().unwrap().push(e)),
        })?;
        Ok(Harness { agent, events })
    }

    fn ready(&self) -> Status {
        self.wait(|s| s.state == "ready" && !s.id.is_empty())
    }

    fn wait(&self, f: impl Fn(&Status) -> bool) -> Status {
        wait_for("status", T, || Some(self.agent.status()).filter(|s| f(s)))
    }

    fn has_event(&self, kind: &str, msg_part: &str) -> bool {
        self.events.lock().unwrap().iter().any(|e| e.kind == kind && e.msg.contains(msg_part))
    }
}

fn ssh_cmd(port: u16, user: &str) -> Command {
    ssh_cmd_log(port, user, "ERROR")
}

/// ssh keeps the first value of an option, so the log level is set here.
fn ssh_cmd_log(port: u16, user: &str, level: &str) -> Command {
    let mut c = Command::new("ssh");
    c.args(["-F", "/dev/null", "-p", &port.to_string()])
        .args(["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"])
        .args(["-o", "BatchMode=yes", "-o", &format!("LogLevel={level}"), "-o", "ConnectTimeout=10"])
        .args(["-o", "IdentitiesOnly=yes", "-o", "IdentityFile=/dev/null"])
        .arg(format!("{user}@127.0.0.1"));
    c
}

fn ssh(port: u16, user: &str, args: &[&str], stdin: &[u8]) -> Output {
    let mut c = ssh_cmd(port, user);
    c.args(args).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = c.spawn().unwrap();
    child.stdin.take().unwrap().write_all(stdin).unwrap();
    child.wait_with_output().unwrap()
}

fn scratch(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("tawc-remote-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn secret_login_exec_and_exit_status() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, Some(Duration::from_secs(3600)));
    let s = h.ready();
    assert_eq!(s.jump, format!("127.0.0.1:{}", relay.ssh_port));
    assert_eq!(s.command, format!("ssh -J {} {}@{}", s.jump, s.secret, s.id));
    assert_eq!(s.notice, "hello[31m");
    assert_eq!(s.region, "test");
    let hello = &relay.hellos()[0];
    assert!(!hello.has_comment);
    assert_eq!(hello.agent, "tawc/test linux/x");
    // The relay's maximum; the idle close is ours.
    assert_eq!(hello.ttl, 0);

    let out = ssh(relay.ssh_port, &s.secret, &["echo hi; echo $SSH_CONNECTION | cut -d' ' -f3-; exit 7"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "hi\n127.0.0.1 22\n");
    assert_eq!(out.status.code(), Some(7));

    // stdin EOF reaches the command; stderr is kept apart.
    let out = ssh(relay.ssh_port, &s.secret, &["tr a-z A-Z; echo oops >&2"], b"abc\n");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "ABC\n");
    assert_eq!(String::from_utf8_lossy(&out.stderr), "oops\n");
    assert!(h.has_event("login", "authenticated"));
    assert!(h.has_event("exit", "exit 7"));
    wait_for("logout", T, || h.has_event("logout", "").then_some(()));
    assert_eq!(h.agent.status().clients, 0);
}

#[test]
fn wrong_secrets_lock_out_until_refill() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, None);
    let s = h.ready();
    // Not secret-shaped: free, and still the usual refusal.
    let out = ssh(relay.ssh_port, "root", &["true"], b"");
    assert_eq!(out.status.code(), Some(255));
    assert!(String::from_utf8_lossy(&out.stderr).contains("Permission denied (publickey)"));
    // Burn the burst in parallel: concurrency must not buy extra guesses.
    let port = relay.ssh_port;
    let ts: Vec<_> = (0..12)
        .map(|i| std::thread::spawn(move || ssh(port, &format!("bold-cook-{}", tawc_remote::sid::word(100 + i)), &["true"], b"")))
        .collect();
    for t in ts {
        assert_eq!(t.join().unwrap().status.code(), Some(255));
    }
    let out = ssh(relay.ssh_port, &s.secret, &["true"], b"");
    assert_eq!(out.status.code(), Some(255), "right secret must be refused while throttled");
    assert!(h.has_event("warn", "too many wrong secrets"));
    // 10/min refill: one token back after 6 s.
    std::thread::sleep(Duration::from_secs(7));
    let out = ssh(relay.ssh_port, &s.secret, &["echo ok"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "ok\n");
}

/// Drive a pty session with the russh client: size from pty-req, then a
/// window-change, then exit status.
#[test]
fn pty_shell_and_window_change() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, None);
    let s = h.ready();
    let port = relay.ssh_port;
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    rt.block_on(async move {
        use russh::client;
        struct C;
        impl client::Handler for C {
            type Error = russh::Error;
            async fn check_server_key(&mut self, _k: &russh::keys::PublicKeyOrCertificate) -> Result<bool, Self::Error> {
                Ok(true)
            }
        }
        let cfg = Arc::new(client::Config::default());
        let mut sess = client::connect(cfg, ("127.0.0.1", port), C).await.unwrap();
        assert!(sess.authenticate_none(&s.secret).await.unwrap().success());
        let mut ch = sess.channel_open_session().await.unwrap();
        ch.request_pty(true, "xterm-256color", 80, 24, 0, 0, &[]).await.unwrap();
        ch.request_shell(true).await.unwrap();
        let mut out = Vec::new();
        let mut code = None;
        let mut step = 0;
        let deadline = tokio::time::Instant::now() + Duration::from_secs(20);
        ch.data(&b"echo T=$TERM; stty size; tty\n"[..]).await.unwrap();
        while code.is_none() {
            let msg = tokio::time::timeout_at(deadline, ch.wait()).await.expect("pty session timed out");
            match msg {
                Some(russh::ChannelMsg::Data { data }) => out.extend_from_slice(&data),
                Some(russh::ChannelMsg::ExitStatus { exit_status }) => code = Some(exit_status),
                None => break,
                _ => {}
            }
            let text = String::from_utf8_lossy(&out).to_string();
            if step == 0 && text.contains("24 80") && text.contains("/dev/pts/") {
                step = 1;
                ch.window_change(100, 30, 0, 0).await.unwrap();
                ch.data(&b"stty size\n"[..]).await.unwrap();
            } else if step == 1 && text.contains("30 100") {
                step = 2;
                ch.data(&b"exit 5\n"[..]).await.unwrap();
            }
        }
        let text = String::from_utf8_lossy(&out);
        assert!(text.contains("T=xterm-256color"), "{text}");
        assert_eq!(code, Some(5), "{text}");
    });
}

#[test]
fn stdio_forwarding_reaches_local_port() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, None);
    let s = h.ready();
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        let (mut c, _) = l.accept().unwrap();
        let mut b = [0u8; 5];
        c.read_exact(&mut b).unwrap();
        c.write_all(&b).unwrap();
        c.write_all(b" pong").unwrap();
    });
    // `ssh -W` is a direct-tcpip channel, like -L.
    let mut c = ssh_cmd(relay.ssh_port, &s.secret);
    c.args(["-W", &format!("127.0.0.1:{port}")]).stdin(Stdio::piped()).stdout(Stdio::piped());
    let mut child = c.spawn().unwrap();
    child.stdin.as_mut().unwrap().write_all(b"ping!").unwrap();
    let mut got = [0u8; 10];
    child.stdout.as_mut().unwrap().read_exact(&mut got).unwrap();
    assert_eq!(&got, b"ping! pong");
    drop(child.stdin.take());
    let _ = child.kill();
    let _ = child.wait();
    assert!(h.has_event("forward", &format!("127.0.0.1:{port}")));
}

/// Collisions with the saved key: each one asks for one more word, same
/// key (upstream's persistent-key policy); past 8 words Start fails.
#[test]
fn id_taken_grows_saved_key() {
    let relay = Relay::start();
    relay.behaviour().id_taken.store(3, std::sync::atomic::Ordering::SeqCst);
    let dir = scratch("taken");
    let key = dir.join("host_key");
    let h = Harness::start_keyed(&relay.url, &key);
    let s = h.ready();
    let hellos = relay.hellos();
    let words: Vec<u64> = hellos.iter().map(|h| h.id_words).collect();
    assert_eq!(words, [2, 3, 4, 5]);
    assert!(hellos.iter().all(|h| h.host_key == s.host_key));
    assert_eq!(s.id.split('-').count(), 5);
    assert!(s.id_long.starts_with(&format!("{}-", s.id)));
    assert!(h.has_event("warn", "session id was taken"));
    let out = ssh(relay.ssh_port, &s.secret, &["echo ok"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "ok\n");
    // The longer id lasts one run.
    drop(h);
    let s2 = Harness::start_keyed(&relay.url, &key).ready();
    assert_eq!(s2.id.split('-').count(), 2);

    relay.behaviour().id_taken.store(100, std::sync::atomic::Ordering::SeqCst);
    let h3 = Harness::start_keyed(&relay.url, &key);
    let s3 = h3.wait(|s| s.state == "failed");
    assert!(s3.error.contains("8 words"), "{}", s3.error);
    assert_eq!(relay.hellos().last().unwrap().id_words, 8);
    let _ = std::fs::remove_dir_all(&dir);
}

/// A per-Start key is replaced, secret too, on the first collision; from
/// the second the id grows as well.
#[test]
fn id_taken_rolls_unsaved_key() {
    let relay = Relay::start();
    relay.behaviour().id_taken.store(3, std::sync::atomic::Ordering::SeqCst);
    let h = Harness::start(&relay.url, None);
    let s = h.ready();
    let hellos = relay.hellos();
    let words: Vec<u64> = hellos.iter().map(|h| h.id_words).collect();
    assert_eq!(words, [2, 2, 3, 4]);
    let keys: std::collections::HashSet<_> = hellos.iter().map(|h| h.host_key.clone()).collect();
    assert_eq!(keys.len(), 4);
    assert_eq!(hellos[3].host_key, s.host_key);
    let out = ssh(relay.ssh_port, &s.secret, &["echo ok"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "ok\n");
}

/// One saved key: same fingerprint and id every Start, locally and on the
/// relay; the secret is new each time.
#[test]
fn saved_host_key_is_stable() {
    let relay = Relay::start();
    let dir = scratch("saved");
    let key = dir.join("host_key");
    let a = Harness::start_keyed(&relay.url, &key).ready();
    let b = Harness::start_keyed(&relay.url, &key).ready();
    assert_eq!((a.fingerprint.as_str(), a.id.as_str()), (b.fingerprint.as_str(), b.id.as_str()));
    assert_eq!(a.id.split('-').count(), 2);
    assert_ne!(a.secret, b.secret);
    assert_eq!(a.secret.split('-').count(), 3);
    let local = Harness::start_with(Transport::Local(vec!["127.0.0.1:0".parse().unwrap()]), Login::Secret, None, Some(&key))
        .unwrap()
        .ready();
    assert_eq!(local.fingerprint, a.fingerprint);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn bye_reconnect_keeps_identity() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, None);
    let s = h.ready();
    relay.bye("deploy", true);
    wait_for("second tunnel", T, || (relay.tunnels() >= 2).then_some(()));
    let s2 = h.ready();
    assert_eq!(s2.id, s.id);
    assert_eq!(s2.secret, s.secret);
    assert_eq!(relay.hellos()[1].host_key, s.host_key);
    wait_for("reconnect note", T, || h.has_event("info", "command unchanged").then_some(()));
    // A plain drop (no bye) reconnects too.
    relay.kill_tunnel();
    wait_for("third tunnel", T, || (relay.tunnels() >= 3).then_some(()));
    let out = ssh(relay.ssh_port, &s.secret, &["echo ok"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "ok\n");
}

#[test]
fn bye_without_reconnect_is_fatal() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, None);
    h.ready();
    relay.bye("banned", false);
    let s = h.wait(|s| s.state == "failed");
    assert_eq!(s.error, "relay closed the session: banned");
}

#[test]
fn newer_protocol_is_fatal() {
    let relay = Relay::start();
    relay.behaviour().version.store(2, std::sync::atomic::Ordering::SeqCst);
    let h = Harness::start(&relay.url, None);
    let s = h.wait(|s| s.state == "failed");
    assert!(s.error.contains("protocol changed"), "{}", s.error);
}

#[test]
fn foreign_id_is_fatal() {
    let relay = Relay::start();
    relay.behaviour().wrong_id.store(true, std::sync::atomic::Ordering::SeqCst);
    let h = Harness::start(&relay.url, None);
    let s = h.wait(|s| s.state == "failed");
    assert!(s.error.contains("does not match our host key"), "{}", s.error);
    assert!(s.command.is_empty());
}

#[test]
fn unreachable_relay_gives_up() {
    // Nothing listens here; 5 attempts with 1+2+4+8 s backoff.
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}", l.local_addr().unwrap());
    drop(l);
    let h = Harness::start(&url, None);
    let s = wait_for("failure", Duration::from_secs(40), || Some(h.agent.status()).filter(|s| s.state == "failed"));
    assert!(s.error.starts_with("could not reach 127.0.0.1:"), "{}", s.error);
}

#[test]
fn idle_close_without_connections() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, Some(Duration::from_secs(2)));
    h.ready();
    let s = h.wait(|s| s.state == "stopped");
    assert_eq!(s.error, "closed after 2 s with no connection");
    wait_for("thread exit", T, || (!h.agent.is_running()).then_some(()));
}

#[test]
fn idle_close_waits_for_the_last_logout() {
    let relay = Relay::start();
    let h = Harness::start(&relay.url, Some(Duration::from_secs(3)));
    let s = h.ready();
    let dir = scratch("idle");
    let mut c = ssh_cmd(relay.ssh_port, &s.secret);
    c.arg(hup_probe(&dir)).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
    let mut child = c.spawn().unwrap();
    let mut buf = [0u8; 5];
    child.stdout.as_mut().unwrap().read_exact(&mut buf).unwrap();
    assert_eq!(h.agent.status().clients, 1);
    // Logged in past the timeout: still up.
    std::thread::sleep(Duration::from_secs(5));
    assert_eq!(h.agent.status().state, "ready");
    // The timer restarts at the logout.
    let _ = child.kill();
    let _ = child.wait();
    wait_for("logout", T, || (h.agent.status().clients == 0).then_some(()));
    std::thread::sleep(Duration::from_secs(1));
    assert_eq!(h.agent.status().state, "ready");
    let s = h.wait(|s| s.state == "stopped");
    assert!(s.error.starts_with("closed after"), "{}", s.error);
    let _ = std::fs::remove_dir_all(&dir);
}

/// Echo server on loopback; returns its port.
fn echo_server() -> u16 {
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for c in l.incoming() {
            let Ok(mut c) = c else { continue };
            std::thread::spawn(move || {
                let mut b = [0u8; 256];
                while let Ok(n) = c.read(&mut b) {
                    if n == 0 || c.write_all(&b[..n]).is_err() {
                        break;
                    }
                }
            });
        }
    });
    port
}

fn echo_through(port: u16) -> std::io::Result<String> {
    let mut c = std::net::TcpStream::connect(("127.0.0.1", port))?;
    c.set_read_timeout(Some(Duration::from_secs(10)))?;
    c.write_all(b"ping")?;
    let mut b = [0u8; 4];
    c.read_exact(&mut b)?;
    Ok(String::from_utf8_lossy(&b).into())
}

/// `ssh -N -R <bind>:0:127.0.0.1:<echo>`; returns the child and the
/// allocated port once ssh reports it.
fn reverse(port: u16, secret: &str, bind: &str, echo: u16) -> (std::process::Child, Option<u16>) {
    let mut c = ssh_cmd_log(port, secret, "INFO");
    let spec = if bind.is_empty() { format!("0:127.0.0.1:{echo}") } else { format!("{bind}:0:127.0.0.1:{echo}") };
    c.args(["-N", "-o", "ExitOnForwardFailure=yes", "-R", &spec])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped());
    let mut child = c.spawn().unwrap();
    let mut err = std::io::BufReader::new(child.stderr.take().unwrap());
    let mut line = String::new();
    loop {
        line.clear();
        if std::io::BufRead::read_line(&mut err, &mut line).unwrap_or(0) == 0 {
            return (child, None);
        }
        if let Some(p) = line.strip_prefix("Allocated port ") {
            let port = p.split_whitespace().next().and_then(|p| p.parse().ok());
            std::thread::spawn(move || std::io::copy(&mut err, &mut std::io::sink()));
            return (child, port);
        }
    }
}

#[test]
fn reverse_forward_on_loopback_only() {
    let relay = Relay::start();
    let mut h = Harness::start(&relay.url, None);
    let s = h.ready();
    let echo = echo_server();
    for bind in ["", "localhost", "127.0.0.1"] {
        let (mut child, port) = reverse(relay.ssh_port, &s.secret, bind, echo);
        let port = port.unwrap_or_else(|| panic!("-R with bind {bind:?} failed"));
        assert_eq!(echo_through(port).unwrap(), "ping");
        // Gone with the connection.
        let _ = child.kill();
        let _ = child.wait();
        wait_for("listener closed", T, || echo_through(port).is_err().then_some(()));
    }
    assert!(h.has_event("forward", "listening on 127.0.0.1:"));
    // Wildcard and non-loopback binds are refused.
    for bind in ["0.0.0.0", "*", "192.0.2.1"] {
        let (mut child, port) = reverse(relay.ssh_port, &s.secret, bind, echo);
        assert_eq!(port, None, "bind {bind:?} was allowed");
        assert_ne!(child.wait().unwrap().code(), Some(0));
    }
    // Stop closes listeners too.
    let (mut child, port) = reverse(relay.ssh_port, &s.secret, "", echo);
    let port = port.unwrap();
    assert_eq!(echo_through(port).unwrap(), "ping");
    h.agent.stop();
    assert!(echo_through(port).is_err());
    let _ = child.kill();
    let _ = child.wait();
}

fn hup_probe(dir: &PathBuf) -> String {
    format!(
        "trap 'echo hup > {}/hup; exit 0' HUP; echo ready; while :; do sleep 0.1; done",
        dir.display()
    )
}

#[test]
fn stop_hangs_up_logins() {
    let relay = Relay::start();
    let mut h = Harness::start(&relay.url, None);
    let s = h.ready();
    let dir = scratch("stop");
    for (tty, name) in [(true, "pty"), (false, "pipe")] {
        let d = dir.join(name);
        std::fs::create_dir_all(&d).unwrap();
        let mut c = ssh_cmd(relay.ssh_port, &s.secret);
        if tty {
            c.arg("-tt");
        }
        c.arg(hup_probe(&d)).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
        let mut child = c.spawn().unwrap();
        let mut buf = [0u8; 5];
        child.stdout.as_mut().unwrap().read_exact(&mut buf).unwrap();
        assert_eq!(&buf, b"ready");
        std::mem::forget(child);
    }
    h.agent.stop();
    assert_eq!(h.agent.status().state, "stopped");
    for name in ["pty", "pipe"] {
        let f = dir.join(name).join("hup");
        wait_for(&format!("{name} SIGHUP"), T, || std::fs::read_to_string(&f).ok().filter(|s| s == "hup\n"));
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// The device launcher's shape (`env -i` envelope, `bash -lc`, an
/// sftp-server from the rootfs) with `/` as the "rootfs": scp both ways.
#[test]
fn envelope_launcher_and_sftp() {
    if std::fs::metadata("/usr/lib/ssh/sftp-server").is_err() || std::fs::metadata("/bin/bash").is_err() {
        eprintln!("skipped: needs /usr/lib/ssh/sftp-server and /bin/bash");
        return;
    }
    let relay = Relay::start();
    let dir = scratch("sftp");
    let events = Arc::new(Mutex::new(Vec::new()));
    let ev = events.clone();
    let agent = Agent::start(Config {
        transport: Transport::Relay(relay.url.clone()),
        login: Login::Secret,
        host_key: None,
        idle_timeout: None,
        agent: "tawc/test".into(),
        version: "test".into(),
        launcher: Arc::new(tawc_remote::spawn::Envelope {
            argv: vec!["/usr/bin/env".into(), "-i".into(), format!("HOME={}", dir.display()), "PATH=/usr/bin:/bin".into()],
            shell: "/bin/sh".into(),
            command_shell: "/bin/sh".into(),
            host_env: vec![],
            cwd: dir.to_string_lossy().into(),
            rootfs: String::new(),
        }),
        events: Arc::new(move |e| ev.lock().unwrap().push(e)),
    })
    .unwrap();
    let h = Harness { agent, events };
    let s = h.ready();
    // Only the envelope's env plus SSH's own reaches the login.
    let out = ssh(relay.ssh_port, &s.secret, &["env | cut -d= -f1 | sort | tr '\\n' ' '"], b"");
    let vars = String::from_utf8_lossy(&out.stdout).to_string();
    assert!(vars.contains("HOME ") && vars.contains("SSH_CONNECTION ") && !vars.contains("CARGO"), "{vars}");
    std::fs::write(dir.join("up.txt"), "payload\n").unwrap();
    let scp = |args: &[String]| {
        Command::new("scp")
            .args(["-F", "/dev/null", "-P", &relay.ssh_port.to_string()])
            .args(["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "BatchMode=yes", "-o", "LogLevel=ERROR"])
            .args(args)
            .output()
            .unwrap()
    };
    let out = scp(&[dir.join("up.txt").to_string_lossy().into(), format!("{}@127.0.0.1:copied.txt", s.secret)]);
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(std::fs::read_to_string(dir.join("copied.txt")).unwrap(), "payload\n");
    let out = scp(&[format!("{}@127.0.0.1:copied.txt", s.secret), dir.join("down.txt").to_string_lossy().into()]);
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(std::fs::read_to_string(dir.join("down.txt")).unwrap(), "payload\n");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A fresh client key pair via ssh-keygen: (private key path, public line).
fn keygen(dir: &PathBuf, name: &str, kind: &str) -> (PathBuf, String) {
    let path = dir.join(name);
    let out = Command::new("ssh-keygen")
        .args(["-q", "-t", kind, "-N", "", "-C", "test@host", "-f"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    let public = std::fs::read_to_string(path.with_extension("pub")).unwrap();
    (path, public)
}

fn ssh_key(port: u16, key: &PathBuf, user: &str, cmd: &str) -> Output {
    let mut c = Command::new("ssh");
    c.args(["-F", "/dev/null", "-p", &port.to_string()])
        .args(["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"])
        .args(["-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "IdentitiesOnly=yes", "-i"])
        .arg(key)
        .arg(format!("{user}@127.0.0.1"))
        .arg(cmd);
    c.output().unwrap()
}

#[test]
fn key_login_through_the_relay() {
    let relay = Relay::start();
    let dir = scratch("keys");
    let (ed, ed_pub) = keygen(&dir, "ed", "ed25519");
    let (rsa, rsa_pub) = keygen(&dir, "rsa", "rsa");
    let (other, _) = keygen(&dir, "other", "ed25519");
    let keys = tawc_remote::sshd::auth::parse_authorized_keys(&format!("{ed_pub}{rsa_pub}"));
    assert_eq!(keys.len(), 2);
    let h = Harness::start_with(
        Transport::Relay(relay.url.clone()),
        Login::Keys { keys, source: "github.com/test".into() },
        None,
        None,
    )
    .unwrap();
    let s = h.ready();
    assert!(s.secret.is_empty());
    assert_eq!((s.key_source.as_str(), s.key_count), ("github.com/test", 2));
    assert!(s.command.ends_with(&format!(" root@{}", s.id)), "{}", s.command);
    for key in [&ed, &rsa] {
        let out = ssh_key(relay.ssh_port, key, "root", "echo in");
        assert_eq!(String::from_utf8_lossy(&out.stdout), "in\n", "{}", String::from_utf8_lossy(&out.stderr));
    }
    // Any username with a listed key; nothing without one.
    assert_eq!(ssh_key(relay.ssh_port, &ed, "whoever", "true").status.code(), Some(0));
    let out = ssh_key(relay.ssh_port, &other, "root", "true");
    assert!(String::from_utf8_lossy(&out.stderr).contains("Permission denied"), "{}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(ssh(relay.ssh_port, "bold-cook-fern", &["true"], b"").status.code(), Some(255));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn local_network_mode() {
    let h = Harness::start_with(Transport::Local(vec!["127.0.0.1:0".parse().unwrap()]), Login::Secret, None, None).unwrap();
    let s = h.ready();
    assert_eq!(s.mode, "local");
    // ssh -p <port> <secret>@127.0.0.1
    let parts: Vec<&str> = s.command.split(' ').collect();
    assert_eq!(parts.len(), 4, "{}", s.command);
    assert_eq!(parts[..2], ["ssh", "-p"]);
    assert_eq!(parts[3], format!("{}@127.0.0.1", s.secret));
    let port: u16 = parts[2].parse().unwrap();
    let out = ssh(port, &s.secret, &["echo local"], b"");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "local\n");
    assert_eq!(ssh(port, "bold-cook-fern", &["true"], b"").status.code(), Some(255));

    // Keys work locally too.
    let dir = scratch("local-keys");
    let (ed, ed_pub) = keygen(&dir, "ed", "ed25519");
    let k = Harness::start_with(
        Transport::Local(vec!["127.0.0.1:0".parse().unwrap()]),
        Login::Keys { keys: tawc_remote::sshd::auth::parse_authorized_keys(&ed_pub), source: "pasted".into() },
        None,
        None,
    )
    .unwrap();
    let ks = k.ready();
    let kport: u16 = ks.command.split(' ').nth(2).unwrap().parse().unwrap();
    assert_eq!(String::from_utf8_lossy(&ssh_key(kport, &ed, "root", "echo k").stdout), "k\n");

    // A busy port is Start's error.
    let err = Harness::start_with(Transport::Local(vec![format!("127.0.0.1:{port}").parse().unwrap()]), Login::Secret, None, None)
        .err()
        .expect("second bind must fail");
    assert_eq!(err.to_string(), format!("port {port} is in use"));
    let _ = std::fs::remove_dir_all(&dir);
}

/// Opt-in: `TAWC_LIVE_RELAY=1 cargo test --test e2e live_relay`.
#[test]
fn live_relay() {
    if std::env::var("TAWC_LIVE_RELAY").as_deref() != Ok("1") {
        eprintln!("skipped (TAWC_LIVE_RELAY=1 to run)");
        return;
    }
    let h = Harness::start("https://sshyeet.com", Some(Duration::from_secs(300)));
    let s = wait_for("live ready", Duration::from_secs(60), || Some(h.agent.status()).filter(|s| s.state == "ready"));
    let dir = scratch("live");
    std::fs::write(dir.join("known_hosts"), format!("{} {}\n", s.id, s.host_key)).unwrap();
    std::fs::write(
        dir.join("config"),
        format!(
            "Host *\n  UserKnownHostsFile {0}/known_hosts {0}/jump_known_hosts\n  StrictHostKeyChecking accept-new\n  BatchMode yes\n  ConnectTimeout 20\n",
            dir.display()
        ),
    )
    .unwrap();
    let out = Command::new("ssh")
        .args(["-F", &dir.join("config").to_string_lossy(), "-J", &s.jump])
        .arg(format!("{}@{}", s.secret, s.id))
        .arg("echo live-ok")
        .output()
        .unwrap();
    assert_eq!(String::from_utf8_lossy(&out.stdout), "live-ok\n", "{}", String::from_utf8_lossy(&out.stderr));
    let _ = std::fs::remove_dir_all(&dir);
}
