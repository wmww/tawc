//! The agent: host key + login (secret or keys), a transport (the relay
//! tunnel with its reconnect loop, or listeners on the local network),
//! and the embedded sshd, on one thread with its own runtime. Nothing
//! exists until [`Agent::start`]; [`Agent::stop`] joins it all.

use std::io;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::Serialize;
use tokio::io::BufReader;
use tokio_util::sync::CancellationToken;
use tokio_util::task::TaskTracker;

use crate::event::{Events, Sink};
use crate::hostkey::HostKey;
use crate::proto::{self, ClientHeader, Envelope, Hello, Ready, ReadyError};
use crate::spawn::Launcher;
use russh::keys::PublicKey;
use crate::sshd::auth::{new_secret, parse_authorized_keys, Policy};
use crate::sshd::Server;
use crate::{mux, sid, tunnel};

/// How clients reach the sshd.
pub enum Transport {
    /// Through a relay: its base URL, e.g. `https://sshyeet.com`.
    Relay(String),
    /// Listen on these addresses (the device's local-network ones; the
    /// app picks them, never a cellular interface).
    Local(Vec<std::net::SocketAddr>),
}

/// Who may log in.
pub enum Login {
    /// A fresh three-word secret (`bold-cook-fern`) as the username.
    Secret,
    /// These public keys, any username. `source` names them for the
    /// screen (`github.com/<user>`, `pasted`).
    Keys { keys: Vec<PublicKey>, source: String },
}

pub struct Config {
    pub transport: Transport,
    pub login: Login,
    /// Keep the host key here (created on first use) so clients' known_hosts
    /// can check it and the relay id stays the same. None: a new key per
    /// Start.
    pub host_key: Option<std::path::PathBuf>,
    /// Stop once no one has been logged in for this long (counted from
    /// Start, and again from each last logout). None: until stopped.
    pub idle_timeout: Option<Duration>,
    /// Hello `agent` / HTTP User-Agent, e.g. `tawc/12 android/aarch64`.
    pub agent: String,
    /// Goes into the SSH server version string.
    pub version: String,
    pub launcher: Arc<dyn Launcher>,
    pub events: Sink,
}

#[derive(Debug, serde::Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum TransportRequest {
    Relay { relay: String },
    Local { addrs: Vec<String> },
}

#[derive(Debug, serde::Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum LoginRequest {
    Secret,
    /// authorized_keys text (a code host's `.keys`, or pasted).
    Keys { keys: String, source: String },
}

/// What the app passes to `nativeRemoteStart`, as JSON.
#[derive(Debug, serde::Deserialize)]
pub struct StartRequest {
    pub transport: TransportRequest,
    pub login: LoginRequest,
    /// Seconds; 0 = until stopped.
    #[serde(default)]
    pub idle_timeout: u64,
    /// Path of the persistent host key (see [`Config::host_key`]).
    #[serde(default)]
    pub host_key: Option<String>,
    pub agent: String,
    pub version: String,
    pub envelope: crate::spawn::Envelope,
}

impl Config {
    pub fn from_json(json: &str, events: Sink) -> io::Result<Config> {
        let r: StartRequest = serde_json::from_str(json).map_err(io::Error::other)?;
        let transport = match r.transport {
            TransportRequest::Relay { relay } => Transport::Relay(relay),
            TransportRequest::Local { addrs } => Transport::Local(
                addrs
                    .iter()
                    .map(|a| a.parse().map_err(|_| io::Error::other(format!("bad address {a:?}"))))
                    .collect::<io::Result<_>>()?,
            ),
        };
        let login = match r.login {
            LoginRequest::Secret => Login::Secret,
            LoginRequest::Keys { keys, source } => {
                let keys = parse_authorized_keys(&keys);
                if keys.is_empty() {
                    return Err(io::Error::other(format!("no usable public keys from {source}")));
                }
                Login::Keys { keys, source }
            }
        };
        Ok(Config {
            transport,
            login,
            host_key: r.host_key.map(Into::into),
            idle_timeout: (r.idle_timeout > 0).then(|| Duration::from_secs(r.idle_timeout)),
            agent: r.agent,
            version: r.version,
            launcher: Arc::new(r.envelope),
            events,
        })
    }
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Status {
    /// `connecting`, `ready`, `reconnecting`, `stopped`, `failed`.
    pub state: &'static str,
    /// Why `failed`/`stopped`/`reconnecting`; empty otherwise.
    pub error: String,
    /// `relay` or `local`.
    pub mode: &'static str,
    pub relay: String,
    pub id: String,
    pub id_long: String,
    pub jump: String,
    pub region: String,
    pub notice: String,
    /// Empty in key mode.
    pub secret: String,
    pub secret_disabled: bool,
    /// Key mode: where the keys came from, and how many.
    pub key_source: String,
    pub key_count: usize,
    pub host_key: String,
    pub fingerprint: String,
    pub command: String,
    /// Authenticated connections right now.
    pub clients: usize,
}

struct Shared {
    status: Mutex<Status>,
    server: Mutex<Option<Arc<Server>>>,
    events: Events,
}

impl Shared {
    fn update(&self, f: impl FnOnce(&mut Status)) {
        f(&mut self.status.lock().unwrap());
        self.events.emit("status", "", "");
    }

    fn snapshot(&self) -> Status {
        let mut s = self.status.lock().unwrap().clone();
        if let Some(srv) = self.server.lock().unwrap().as_ref() {
            s.clients = srv.clients();
            s.secret_disabled = srv.policy.disabled();
        }
        s
    }
}

pub struct Agent {
    cancel: CancellationToken,
    shared: Arc<Shared>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Agent {
    pub fn start(cfg: Config) -> io::Result<Agent> {
        let mut status = Status { state: "connecting", ..Default::default() };
        let (mut url, mut listeners) = (String::new(), Vec::new());
        match &cfg.transport {
            Transport::Relay(relay) => {
                url = tunnel::tunnel_url(relay).ok_or_else(|| {
                    io::Error::new(io::ErrorKind::InvalidInput, "relay must be an https:// (or http://) URL")
                })?;
                status.mode = "relay";
                status.relay = relay.clone();
            }
            Transport::Local(addrs) => {
                if addrs.is_empty() {
                    return Err(io::Error::other("no local network"));
                }
                // Bound here so a busy port is Start's error.
                for a in addrs {
                    let l = std::net::TcpListener::bind(a).map_err(|e| match e.kind() {
                        io::ErrorKind::AddrInUse => io::Error::other(format!("port {} is in use", a.port())),
                        _ => io::Error::other(format!("can't listen on {a}: {e}")),
                    })?;
                    l.set_nonblocking(true)?;
                    listeners.push(l);
                }
                status.mode = "local";
            }
        }
        let policy = match &cfg.login {
            Login::Secret => Policy::new(new_secret()?),
            Login::Keys { keys, source } => {
                status.key_source = source.clone();
                status.key_count = keys.len();
                Policy::with_keys(keys.clone())
            }
        };
        let events = Events::new(cfg.events.clone());
        let shared = Arc::new(Shared { status: Mutex::new(status), server: Mutex::new(None), events: events.clone() });
        let cancel = CancellationToken::new();
        let key = match &cfg.host_key {
            Some(path) => HostKey::load_or_create(path)?,
            None => HostKey::generate()?,
        };
        let ident = Identity { key, secret: policy.secret() };
        let (c2, s2) = (cancel.clone(), shared.clone());
        let thread = std::thread::Builder::new().name("tawc-remote".into()).spawn(move || {
            let rt = match tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .thread_name("tawc-remote-wait")
                .build()
            {
                Ok(rt) => rt,
                Err(e) => {
                    s2.update(|s| {
                        s.state = "failed";
                        s.error = e.to_string();
                    });
                    return;
                }
            };
            let s3 = s2.clone();
            let ran = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| rt.block_on(async {
                let policy = Arc::new(policy);
                let server = Arc::new(Server::new(
                    &ident.key,
                    &cfg.version,
                    policy.clone(),
                    cfg.launcher.clone(),
                    events.clone(),
                ));
                *s2.server.lock().unwrap() = Some(server.clone());
                let mut run = Run {
                    url,
                    cfg,
                    ident,
                    policy,
                    server,
                    shared: s2.clone(),
                    cancel: c2.clone(),
                    tracker: TaskTracker::new(),
                    last_node: None,
                    last_ready: None,
                    id_words: sid::DEFAULT_ID_WORDS,
                    collisions: 0,
                };
                if listeners.is_empty() {
                    run.run().await;
                } else {
                    run.run_local(listeners).await;
                }
                // Hang up every login (pty masters closed, SIGHUP) before
                // the runtime goes.
                c2.cancel();
                run.tracker.close();
                let _ = tokio::time::timeout(Duration::from_secs(3), run.tracker.wait()).await;
            })));
            if ran.is_err() {
                s3.update(|s| {
                    s.state = "failed";
                    s.error = "internal error".into();
                });
            }
            // Waits for exited logins are left to finish on their own.
            rt.shutdown_timeout(Duration::from_millis(500));
        })?;
        Ok(Agent { cancel, shared, thread: Some(thread) })
    }

    pub fn status(&self) -> Status {
        self.shared.snapshot()
    }

    pub fn status_json(&self) -> String {
        serde_json::to_string(&self.status()).unwrap_or_default()
    }

    /// Stop and join. Idempotent.
    pub fn stop(&mut self) {
        self.cancel.cancel();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }

    pub fn is_running(&self) -> bool {
        self.thread.as_ref().is_some_and(|t| !t.is_finished())
    }
}

impl Drop for Agent {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Host key + secret (empty in key mode); replaced wholesale on a relay
/// id collision.
struct Identity {
    key: HostKey,
    secret: String,
}

enum Outcome {
    Stopped,
    Idle,
    Fatal(String),
    /// New identity; reconnect at once.
    RetryNow,
    Transient(String),
}

struct Run {
    url: String,
    cfg: Config,
    ident: Identity,
    policy: Arc<Policy>,
    server: Arc<Server>,
    shared: Arc<Shared>,
    cancel: CancellationToken,
    tracker: TaskTracker,
    last_node: Option<String>,
    last_ready: Option<Ready>,
    /// Words asked for in the id; grows on collisions.
    id_words: usize,
    collisions: u32,
}

/// Closes the yamux session when the attempt ends, however it ends.
struct CloseOnDrop(mux::Control);

impl Drop for CloseOnDrop {
    fn drop(&mut self) {
        self.0.close();
    }
}

/// Resolves once `clients` has been 0 for `timeout` straight. Never with
/// no timeout.
async fn idle(mut clients: tokio::sync::watch::Receiver<usize>, timeout: Option<Duration>) {
    let Some(timeout) = timeout else { return std::future::pending().await };
    loop {
        while *clients.borrow_and_update() > 0 {
            if clients.changed().await.is_err() {
                return std::future::pending().await;
            }
        }
        tokio::select! {
            _ = tokio::time::sleep(timeout) => return,
            r = clients.changed() => {
                if r.is_err() {
                    return std::future::pending().await;
                }
            }
        }
    }
}

/// "5 min", "90 s": for the idle-close message.
fn human(d: Duration) -> String {
    let s = d.as_secs();
    if s >= 60 && s % 60 == 0 { format!("{} min", s / 60) } else { format!("{s} s") }
}

const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(30);
const CLIENT_HEADER_TIMEOUT: Duration = Duration::from_secs(15);

impl Run {
    async fn run(&mut self) {
        let mut backoff = Duration::from_secs(1);
        let mut failures = 0u32;
        // One timer across reconnects: a tunnel blip doesn't reset it.
        let idle = idle(self.server.watch_clients(), self.cfg.idle_timeout);
        tokio::pin!(idle);
        loop {
            let started = Instant::now();
            let cancel = self.cancel.clone();
            let out = tokio::select! {
                o = self.once() => o,
                _ = cancel.cancelled() => Outcome::Stopped,
                _ = &mut idle => Outcome::Idle,
            };
            match out {
                Outcome::Stopped => {
                    self.shared.update(|s| {
                        s.state = "stopped";
                        s.error.clear();
                    });
                    return;
                }
                Outcome::Idle => return self.idle_closed(),
                Outcome::Fatal(msg) => {
                    self.shared.update(|s| {
                        s.state = "failed";
                        s.error = msg;
                    });
                    return;
                }
                // Bounded: every collision past the first grows the id.
                Outcome::RetryNow => continue,
                Outcome::Transient(msg) => {
                    if started.elapsed() > Duration::from_secs(60) {
                        backoff = Duration::from_secs(1);
                    }
                    failures += 1;
                    if self.last_ready.is_none() && failures > 4 {
                        let host = tunnel::relay_host(&self.url).to_string();
                        self.shared.update(|s| {
                            s.state = "failed";
                            s.error = format!("could not reach {host}: {msg}");
                        });
                        return;
                    }
                    self.shared.update(|s| {
                        s.state = if self.last_ready.is_some() { "reconnecting" } else { "connecting" };
                        s.error = msg;
                    });
                    tokio::select! {
                        _ = tokio::time::sleep(backoff) => {}
                        _ = self.cancel.cancelled() => {}
                        _ = &mut idle => return self.idle_closed(),
                    }
                    backoff = (backoff * 2).min(Duration::from_secs(60));
                }
            }
        }
    }

    /// The login name in the command: the secret, or `root` with keys
    /// (any name works; it's root inside anyway).
    fn user(&self) -> &str {
        if self.ident.secret.is_empty() { "root" } else { &self.ident.secret }
    }

    /// Local network: serve whatever connects to `listeners` until Stop
    /// or the idle close.
    async fn run_local(&mut self, listeners: Vec<std::net::TcpListener>) {
        let mut addrs = Vec::new();
        for l in listeners {
            let l = match tokio::net::TcpListener::from_std(l) {
                Ok(l) => l,
                Err(e) => {
                    let msg = e.to_string();
                    return self.shared.update(|s| {
                        s.state = "failed";
                        s.error = msg;
                    });
                }
            };
            if let Ok(a) = l.local_addr() {
                addrs.push(a);
            }
            let (srv, cancel, tracker) = (self.server.clone(), self.cancel.clone(), self.tracker.clone());
            self.tracker.spawn(async move {
                loop {
                    let (tcp, peer) = tokio::select! {
                        a = l.accept() => match a {
                            Ok(a) => a,
                            Err(_) => continue,
                        },
                        _ = cancel.cancelled() => return,
                    };
                    let _ = tcp.set_nodelay(true);
                    let (srv, cancel) = (srv.clone(), cancel.clone());
                    tracker.spawn(async move { srv.serve(tcp, peer.to_string(), cancel).await });
                }
            });
        }
        let Some(first) = addrs.first().copied() else { return };
        let k = &self.ident.key;
        // The persistent key means known_hosts recognises us next Start too.
        let id = sid::derive(&k.wire, sid::DEFAULT_ID_WORDS);
        let command = format!("ssh -p {} {}@{}", first.port(), self.user(), first.ip());
        let (fingerprint, host_key, secret) = (k.fingerprint.clone(), k.openssh.clone(), self.ident.secret.clone());
        self.shared.update(|s| {
            s.state = "ready";
            s.id = id;
            s.secret = secret;
            s.host_key = host_key;
            s.fingerprint = fingerprint;
            s.command = command;
        });
        let idle = idle(self.server.watch_clients(), self.cfg.idle_timeout);
        tokio::select! {
            _ = self.cancel.cancelled() => self.shared.update(|s| {
                s.state = "stopped";
                s.error.clear();
            }),
            _ = idle => self.idle_closed(),
        }
    }

    fn idle_closed(&self) {
        let msg = format!("closed after {} with no connection", human(self.cfg.idle_timeout.unwrap_or_default()));
        self.shared.update(|s| {
            s.state = "stopped";
            s.error = msg;
        });
    }

    /// Someone else's key holds our words on the relay; upstream's
    /// policy. The saved key can't change, so each collision asks for one
    /// more word: same key and known_hosts entry, longer name. A per-Start
    /// key is replaced (with the secret) instead, and from the second
    /// collision the id grows too.
    fn on_collision(&mut self) -> Result<(), String> {
        self.collisions += 1;
        let saved = self.cfg.host_key.is_some();
        let grow = saved || self.collisions >= 2;
        if grow && self.id_words >= sid::MAX_WORDS {
            return Err(format!("session ids are taken on the relay even at {} words; try again later", sid::MAX_WORDS));
        }
        if !saved {
            self.reroll()?;
        }
        if grow {
            self.id_words += 1;
        }
        self.last_ready = None;
        Ok(())
    }

    /// New key and secret. Logins made with the old secret keep running,
    /// as upstream.
    fn reroll(&mut self) -> Result<(), String> {
        let secret = if self.ident.secret.is_empty() { String::new() } else { new_secret().map_err(|e| e.to_string())? };
        let new = Identity { key: HostKey::generate().map_err(|e| e.to_string())?, secret };
        if !new.secret.is_empty() {
            self.policy.set_secret(new.secret.clone());
        }
        self.server.set_host_key(&new.key);
        self.ident = new;
        self.last_ready = None;
        self.shared.update(|s| {
            s.state = "connecting";
            s.id.clear();
            s.command.clear();
        });
        Ok(())
    }

    async fn once(&mut self) -> Outcome {
        let ws = match tunnel::dial(&self.url, &self.cfg.agent, self.last_node.as_deref()).await {
            Ok(ws) => ws,
            Err(e) => return Outcome::Transient(io::Error::from(e).to_string()),
        };
        let (io, _pumps) = tunnel::byte_stream(ws);
        let (ctl, mut incoming) = mux::session(io, mux::Mode::Client);
        let ctl = CloseOnDrop(ctl);
        let control = match ctl.0.open().await {
            Ok(s) => s,
            Err(e) => return Outcome::Transient(e.to_string()),
        };
        let mut br = BufReader::new(control);
        let ready = match tokio::time::timeout(HANDSHAKE_TIMEOUT, self.handshake(&mut br)).await {
            Err(_) => return Outcome::Transient("relay handshake timed out".into()),
            Ok(Err(o)) => return o,
            Ok(Ok(r)) => r,
        };
        self.announce(ready);

        loop {
            tokio::select! {
                line = proto::read_line(&mut br) => match line {
                    Ok(l) => {
                        let env: Envelope = serde_json::from_slice(&l).unwrap_or_default();
                        if env.op == "bye" {
                            let reason = proto::printable(&env.reason, 200);
                            return if env.reconnect {
                                Outcome::Transient(format!("relay says: {reason}"))
                            } else {
                                Outcome::Fatal(format!("relay closed the session: {reason}"))
                            };
                        }
                    }
                    Err(_) => return Outcome::Transient("tunnel closed".into()),
                },
                s = incoming.accept() => match s {
                    Some(stream) => {
                        let (srv, cancel) = (self.server.clone(), self.cancel.clone());
                        self.tracker.spawn(client(stream, srv, cancel));
                    }
                    None => return Outcome::Transient("tunnel closed".into()),
                },
                _ = ctl.0.closed() => return Outcome::Transient("tunnel closed".into()),
            }
        }
    }

    async fn handshake(&mut self, br: &mut BufReader<mux::Stream>) -> Result<Ready, Outcome> {
        let transient = |e: io::Error| Outcome::Transient(format!("handshake: {e}"));
        let line = proto::read_line(br).await.map_err(transient)?;
        self.check_error(&line)?;
        let ch: proto::Challenge = serde_json::from_slice(&line)
            .map_err(|_| Outcome::Transient("handshake: unexpected first message from relay".into()))?;
        if ch.op != "challenge" || ch.nonce.len() < 16 {
            return Err(Outcome::Transient("handshake: unexpected first message from relay".into()));
        }
        if ch.v > proto::VERSION {
            return Err(Outcome::Fatal("relay protocol changed; update TAWC".into()));
        }
        let mut msg = proto::HELLO_SIG_PREFIX.to_vec();
        msg.extend_from_slice(&ch.nonce);
        let hello = Hello {
            op: "hello",
            v: proto::VERSION,
            host_key: &self.ident.key.openssh,
            sig: self.ident.key.sign_ssh(&msg),
            // The relay's maximum; the idle close is ours.
            ttl: 0,
            agent: &self.cfg.agent,
            id_words: self.id_words,
        };
        proto::write_json(br.get_mut(), &hello).await.map_err(transient)?;
        let line = proto::read_line(br).await.map_err(transient)?;
        self.check_error(&line)?;
        let ready: Ready = serde_json::from_slice(&line)
            .map_err(|_| Outcome::Transient("handshake: malformed ready message".into()))?;
        match proto::validate_ready(ready, &self.ident.key.wire, self.id_words) {
            Ok(r) => Ok(r),
            Err(ReadyError::Malformed) => Err(Outcome::Transient("handshake: malformed ready message".into())),
            Err(ReadyError::IdMismatch(id)) => Err(Outcome::Fatal(format!(
                "relay assigned session id {id:?}, which does not match our host key; refusing to continue"
            ))),
            Err(ReadyError::BadJump(j)) => Err(Outcome::Fatal(format!("relay sent a malformed jump address {j:?}"))),
        }
    }

    /// `{"op":"error"}` from the relay: an id collision is handled by
    /// [`Run::on_collision`], anything else is fatal.
    fn check_error(&mut self, line: &[u8]) -> Result<(), Outcome> {
        let env: Envelope = serde_json::from_slice(line).unwrap_or_default();
        if env.op != "error" {
            return Ok(());
        }
        if env.msg == proto::ERR_ID_TAKEN {
            self.on_collision().map_err(Outcome::Fatal)?;
            self.shared.events.emit("warn", "", "session id was taken on the relay; trying another");
            return Err(Outcome::RetryNow);
        }
        Err(Outcome::Fatal(format!("relay: {}", proto::printable(&env.msg, 200))))
    }

    fn announce(&mut self, r: Ready) {
        self.last_node = Some(r.node.clone()).filter(|n| !n.is_empty());
        if let Some(prev) = &self.last_ready {
            if prev.id == r.id && prev.jump == r.jump {
                self.shared.events.emit("info", "", "reconnected; command unchanged");
            } else {
                self.shared.events.emit("warn", "", "reconnected to a different relay node: the command changed");
            }
        }
        let k = &self.ident.key;
        let secret = &self.ident.secret;
        let dest = format!("{}@{}", self.user(), r.id);
        let command = format!("ssh -J {} {dest}", r.jump);
        let (id_long, fingerprint, host_key) = (sid::derive_long(&k.wire, self.id_words), k.fingerprint.clone(), k.openssh.clone());
        let secret = secret.clone();
        self.shared.update(|s| {
            s.state = "ready";
            s.error.clear();
            s.id = r.id.clone();
            s.id_long = id_long;
            s.jump = r.jump.clone();
            s.region = r.region.clone();
            s.notice = r.notice.clone();
            s.secret = secret;
            s.host_key = host_key;
            s.fingerprint = fingerprint;
            s.command = command;
        });
        self.last_ready = Some(r);
    }
}

async fn client(stream: mux::Stream, srv: Arc<Server>, cancel: CancellationToken) {
    let mut br = BufReader::new(stream);
    let hdr = match tokio::time::timeout(CLIENT_HEADER_TIMEOUT, proto::read_line(&mut br)).await {
        Ok(Ok(l)) => serde_json::from_slice::<ClientHeader>(&l).ok(),
        _ => None,
    };
    let Some(hdr) = hdr else { return };
    let from = proto::printable(&hdr.from, 64);
    let from = if from.is_empty() { "unknown".to_string() } else { from };
    srv.serve(br, from, cancel).await;
}
