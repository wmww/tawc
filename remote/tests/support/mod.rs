//! In-process fake relay: WebSocket + yamux server speaking the control
//! protocol, plus a TCP port whose connections become client streams
//! (with a `from` header), like `ssh -J` through the real relay.

#![allow(dead_code)]

use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use data_encoding::BASE64;
use ring::rand::{SecureRandom, SystemRandom};
use serde_json::{json, Value};
use tawc_remote::{mux, proto, sid, tunnel};
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;

#[derive(Debug, Clone)]
pub struct HelloRec {
    pub host_key: String,
    pub ttl: u64,
    pub agent: String,
    pub has_comment: bool,
    pub id_words: u64,
}

#[derive(Default)]
pub struct Behaviour {
    /// Answer this many hellos with "session id taken".
    pub id_taken: AtomicUsize,
    /// Challenge `v`.
    pub version: AtomicU32,
    /// Assign an id that doesn't match the key.
    pub wrong_id: AtomicBool,
}

enum Cmd {
    Bye { reason: String, reconnect: bool },
}

struct State {
    beh: Behaviour,
    hellos: Mutex<Vec<HelloRec>>,
    tunnels: AtomicUsize,
    current: Mutex<Option<(mux::Control, mpsc::UnboundedSender<Cmd>)>>,
}

pub struct Relay {
    pub url: String,
    pub ssh_port: u16,
    state: Arc<State>,
    rt: tokio::runtime::Runtime,
}

impl Relay {
    pub fn start() -> Relay {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().unwrap();
        let state = Arc::new(State {
            beh: Behaviour { version: AtomicU32::new(1), ..Default::default() },
            hellos: Mutex::new(Vec::new()),
            tunnels: AtomicUsize::new(0),
            current: Mutex::new(None),
        });
        let (ws_l, ssh_l) = rt.block_on(async {
            (TcpListener::bind("127.0.0.1:0").await.unwrap(), TcpListener::bind("127.0.0.1:0").await.unwrap())
        });
        let url = format!("http://{}", ws_l.local_addr().unwrap());
        let ssh_port = ssh_l.local_addr().unwrap().port();
        let st = state.clone();
        rt.spawn(async move {
            while let Ok((tcp, _)) = ws_l.accept().await {
                tokio::spawn(tunnel_conn(tcp, st.clone(), ssh_port));
            }
        });
        let st = state.clone();
        rt.spawn(async move {
            while let Ok((tcp, peer)) = ssh_l.accept().await {
                tokio::spawn(client_conn(tcp, peer, st.clone()));
            }
        });
        Relay { url, ssh_port, state, rt }
    }

    pub fn behaviour(&self) -> &Behaviour {
        &self.state.beh
    }

    pub fn hellos(&self) -> Vec<HelloRec> {
        self.state.hellos.lock().unwrap().clone()
    }

    /// Tunnels that completed the handshake.
    pub fn tunnels(&self) -> usize {
        self.state.tunnels.load(Ordering::SeqCst)
    }

    pub fn bye(&self, reason: &str, reconnect: bool) {
        if let Some((_, tx)) = self.state.current.lock().unwrap().as_ref() {
            let _ = tx.send(Cmd::Bye { reason: reason.into(), reconnect });
        }
    }

    /// Drop the tunnel without a bye (network blip).
    pub fn kill_tunnel(&self) {
        if let Some((ctl, _)) = self.state.current.lock().unwrap().take() {
            ctl.close();
        }
    }
}

fn verify_hello(hello: &Value, nonce: &[u8]) -> Option<Vec<u8>> {
    let key = hello["host_key"].as_str()?;
    let mut parts = key.split(' ');
    if parts.next()? != "ssh-ed25519" {
        return None;
    }
    let wire = BASE64.decode(parts.next()?.as_bytes()).ok()?;
    let sig = BASE64.decode(hello["sig"].as_str()?.as_bytes()).ok()?;
    // string "ssh-ed25519" || string sig(64)
    if sig.len() != 4 + 11 + 4 + 64 || &sig[4..15] != b"ssh-ed25519" || wire.len() != 51 {
        return None;
    }
    let mut msg = proto::HELLO_SIG_PREFIX.to_vec();
    msg.extend_from_slice(nonce);
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, &wire[19..])
        .verify(&msg, &sig[19..])
        .ok()?;
    Some(wire)
}

async fn line(w: &mut (impl AsyncWriteExt + Unpin), v: Value) {
    let mut b = serde_json::to_vec(&v).unwrap();
    b.push(b'\n');
    let _ = w.write_all(&b).await;
}

async fn tunnel_conn(tcp: TcpStream, st: Arc<State>, ssh_port: u16) {
    let Ok(ws) = tokio_tungstenite::accept_async(tcp).await else { return };
    let (io, _pumps) = tunnel::byte_stream(ws);
    let (ctl, mut incoming) = mux::session(io, mux::Mode::Server);
    let Some(control) = incoming.accept().await else { return };
    let mut br = BufReader::new(control);
    let mut nonce = [0u8; 24];
    SystemRandom::new().fill(&mut nonce).unwrap();
    let v = st.beh.version.load(Ordering::SeqCst);
    line(br.get_mut(), json!({"op": "challenge", "v": v, "nonce": BASE64.encode(&nonce)})).await;
    let Ok(l) = proto::read_line(&mut br).await else { return };
    let hello: Value = serde_json::from_slice(&l).unwrap_or_default();
    let Some(wire) = verify_hello(&hello, &nonce) else {
        line(br.get_mut(), json!({"op": "error", "msg": "bad hello"})).await;
        return;
    };
    st.hellos.lock().unwrap().push(HelloRec {
        host_key: hello["host_key"].as_str().unwrap().to_string(),
        ttl: hello["ttl"].as_u64().unwrap_or(0),
        agent: hello["agent"].as_str().unwrap_or("").to_string(),
        has_comment: hello.get("comment").is_some(),
        id_words: hello["id_words"].as_u64().unwrap_or(0),
    });
    if st.beh.id_taken.load(Ordering::SeqCst) > 0 {
        st.beh.id_taken.fetch_sub(1, Ordering::SeqCst);
        line(br.get_mut(), json!({"op": "error", "msg": proto::ERR_ID_TAKEN})).await;
        tokio::time::sleep(Duration::from_millis(100)).await;
        ctl.close();
        return;
    }
    let id = if st.beh.wrong_id.load(Ordering::SeqCst) {
        "able-able-ant".to_string()
    } else {
        // Upstream: 0 = its default (2), else 1..=8.
        let n = hello["id_words"].as_u64().unwrap_or(0) as usize;
        sid::derive(&wire, if n == 0 { sid::DEFAULT_ID_WORDS } else { n })
    };
    line(
        br.get_mut(),
        json!({"op": "ready", "id": id, "jump": format!("127.0.0.1:{ssh_port}"), "node": "n1", "region": "test",
               "expires": 0, "notice": "hello\u{1b}[31m", "latest": "x", "web": "https://example.invalid"}),
    )
    .await;
    let (tx, mut rx) = mpsc::unbounded_channel();
    *st.current.lock().unwrap() = Some((ctl.clone(), tx));
    st.tunnels.fetch_add(1, Ordering::SeqCst);
    loop {
        tokio::select! {
            c = rx.recv() => match c {
                Some(Cmd::Bye { reason, reconnect }) => {
                    line(br.get_mut(), json!({"op": "bye", "reason": reason, "reconnect": reconnect})).await;
                    tokio::time::sleep(Duration::from_millis(100)).await;
                    ctl.close();
                    break;
                }
                None => break,
            },
            _ = ctl.closed() => break,
        }
    }
}

async fn client_conn(mut tcp: TcpStream, peer: SocketAddr, st: Arc<State>) {
    let ctl = st.current.lock().unwrap().as_ref().map(|(c, _)| c.clone());
    let Some(ctl) = ctl else { return };
    let Ok(mut s) = ctl.open().await else { return };
    line(&mut s, json!({"from": peer.to_string(), "via": "test"})).await;
    let _ = tokio::io::copy_bidirectional(&mut s, &mut tcp).await;
}

pub fn wait_for<T>(what: &str, timeout: Duration, mut f: impl FnMut() -> Option<T>) -> T {
    let end = Instant::now() + timeout;
    loop {
        if let Some(v) = f() {
            return v;
        }
        if Instant::now() > end {
            panic!("timed out waiting for {what}");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}
