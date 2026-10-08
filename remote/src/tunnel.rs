//! WebSocket transport to the relay: every binary message is a chunk of
//! one byte stream, which carries yamux. Pings keep idle proxies from
//! dropping it and find a dead one.

use std::io;
use std::sync::Arc;
use std::time::Duration;

use futures::{SinkExt, StreamExt};
use tokio::io::{AsyncReadExt, AsyncWriteExt, DuplexStream};
use tokio::task::JoinHandle;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::{Error as WsError, Message};
use tokio_tungstenite::Connector;

use crate::proto;

const DIAL_TIMEOUT: Duration = Duration::from_secs(45);
/// Upstream's: ping every 25 s, give up after 75 s with nothing received.
const PING_EVERY: Duration = Duration::from_secs(25);
const DEAD_AFTER: Duration = Duration::from_secs(75);

/// `https://host[:port][/base]` -> `wss://host[:port]/base/v1/tunnel`
/// (`http` -> `ws`, for local test relays).
pub fn tunnel_url(relay: &str) -> Option<String> {
    let (scheme, rest) = if let Some(r) = relay.strip_prefix("https://") {
        ("wss", r)
    } else if let Some(r) = relay.strip_prefix("http://") {
        ("ws", r)
    } else {
        return None;
    };
    let (authority, path) = rest.split_at(rest.find('/').unwrap_or(rest.len()));
    if authority.is_empty() || !proto::valid_jump(authority) {
        return None;
    }
    if !path.bytes().all(|b| b.is_ascii_alphanumeric() || b"/-._~".contains(&b)) {
        return None;
    }
    Some(format!("{scheme}://{authority}{}{}", path.trim_end_matches('/'), proto::TUNNEL_PATH))
}

/// Host part of a relay URL, for messages.
pub fn relay_host(relay: &str) -> &str {
    let rest = relay.split_once("://").map(|x| x.1).unwrap_or(relay);
    rest.split('/').next().unwrap_or(rest)
}

fn tls() -> Arc<rustls::ClientConfig> {
    static CFG: std::sync::OnceLock<Arc<rustls::ClientConfig>> = std::sync::OnceLock::new();
    CFG.get_or_init(|| {
        // An Android app has no /etc/ssl; ship Mozilla's roots.
        let mut roots = rustls::RootCertStore::empty();
        roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        let cfg = rustls::ClientConfig::builder_with_provider(Arc::new(rustls::crypto::ring::default_provider()))
            .with_safe_default_protocol_versions()
            .expect("ring supports the default protocol versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
        Arc::new(cfg)
    })
    .clone()
}

pub enum DialError {
    RateLimited,
    Other(String),
}

type Ws = tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

async fn dial_once(url: &str, user_agent: &str, node: Option<&str>) -> Result<Ws, DialError> {
    let mut req = url.into_client_request().map_err(|e| DialError::Other(e.to_string()))?;
    let h = req.headers_mut();
    h.insert("User-Agent", HeaderValue::from_str(user_agent).map_err(|e| DialError::Other(e.to_string()))?);
    if let Some(n) = node {
        if let Ok(v) = HeaderValue::from_str(n) {
            h.insert(proto::HEADER_FORCE_INSTANCE, v);
        }
    }
    let cfg = WebSocketConfig::default().max_message_size(None).max_frame_size(None);
    let fut = tokio_tungstenite::connect_async_tls_with_config(req, Some(cfg), true, Some(Connector::Rustls(tls())));
    match tokio::time::timeout(DIAL_TIMEOUT, fut).await {
        Err(_) => Err(DialError::Other("timed out connecting".into())),
        Ok(Ok((ws, _))) => Ok(ws),
        Ok(Err(WsError::Http(resp))) if resp.status().as_u16() == 429 => Err(DialError::RateLimited),
        Ok(Err(WsError::Http(resp))) => Err(DialError::Other(format!("relay answered HTTP {}", resp.status()))),
        Ok(Err(e)) => Err(DialError::Other(e.to_string())),
    }
}

/// Dial, preferring the relay node we were on before (it may be gone:
/// then any node will do).
pub async fn dial(url: &str, user_agent: &str, node: Option<&str>) -> Result<Ws, DialError> {
    if let Some(n) = node.filter(|n| !n.is_empty()) {
        if let Ok(ws) = dial_once(url, user_agent, Some(n)).await {
            return Ok(ws);
        }
    }
    dial_once(url, user_agent, None).await
}

/// Aborts the pump tasks when dropped.
pub struct Pumps(Vec<JoinHandle<()>>);

impl Drop for Pumps {
    fn drop(&mut self) {
        for h in &self.0 {
            h.abort();
        }
    }
}

/// A byte stream over the WebSocket's binary messages. (Generic so the
/// test relay can use it on the accepting side.)
pub fn byte_stream<S>(ws: tokio_tungstenite::WebSocketStream<S>) -> (DuplexStream, Pumps)
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
    let (ours, theirs) = tokio::io::duplex(256 * 1024);
    let (mut sink, mut stream) = ws.split();
    let (mut rd, mut wr) = tokio::io::split(theirs);
    let down = tokio::spawn(async move {
        while let Ok(Some(Ok(msg))) = tokio::time::timeout(DEAD_AFTER, stream.next()).await {
            match msg {
                Message::Binary(b) => {
                    if wr.write_all(&b).await.is_err() {
                        break;
                    }
                }
                Message::Close(_) => break,
                _ => {}
            }
        }
        let _ = wr.shutdown().await;
    });
    let up = tokio::spawn(async move {
        let mut buf = vec![0u8; 64 * 1024];
        let mut ping = tokio::time::interval_at(tokio::time::Instant::now() + PING_EVERY, PING_EVERY);
        ping.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            let msg = tokio::select! {
                r = rd.read(&mut buf) => match r {
                    Ok(0) | Err(_) => break,
                    Ok(n) => Message::Binary(buf[..n].to_vec().into()),
                },
                _ = ping.tick() => Message::Ping(Vec::new().into()),
            };
            if sink.send(msg).await.is_err() {
                break;
            }
        }
        let _ = tokio::time::timeout(Duration::from_secs(2), sink.close()).await;
    });
    (ours, Pumps(vec![down, up]))
}

impl From<DialError> for io::Error {
    fn from(e: DialError) -> io::Error {
        match e {
            DialError::RateLimited => io::Error::other("relay is rate limiting us"),
            DialError::Other(s) => io::Error::other(s),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn urls() {
        assert_eq!(tunnel_url("https://sshyeet.com").unwrap(), "wss://sshyeet.com/v1/tunnel");
        assert_eq!(tunnel_url("https://sshyeet.com/").unwrap(), "wss://sshyeet.com/v1/tunnel");
        assert_eq!(tunnel_url("http://127.0.0.1:8123/x/").unwrap(), "ws://127.0.0.1:8123/x/v1/tunnel");
        assert!(tunnel_url("ftp://x").is_none());
        assert!(tunnel_url("https://").is_none());
        assert!(tunnel_url("https://a b").is_none());
        assert!(tunnel_url("https://a/?q").is_none());
        assert_eq!(relay_host("https://sshyeet.com/x"), "sshyeet.com");
    }
}
