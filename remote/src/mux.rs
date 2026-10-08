//! The relay tunnel's yamux, over the `yamux` crate (as upstream's agent):
//! one task drives the connection; [`Control`] opens streams and closes
//! it, [`Incoming`] yields the peer's streams.
//!
//! The crate opens streams lazily (SYN rides on the first data frame), but
//! the relay speaks first on the control stream, so [`Control::open`]
//! writes an empty frame to send the SYN at once. Liveness is the
//! WebSocket's job ([`crate::tunnel::byte_stream`]).

use std::collections::VecDeque;
use std::io;
use std::task::Poll;
use std::time::Duration;

use tokio::sync::{mpsc, oneshot};
use tokio_util::compat::{Compat, FuturesAsyncReadCompatExt, TokioAsyncReadCompatExt};
use tokio_util::sync::CancellationToken;

pub use yamux::Mode;

/// A yamux stream, as tokio I/O.
pub type Stream = Compat<yamux::Stream>;

const MAX_STREAMS: usize = 256;
const INBOUND_BACKLOG: usize = 64;
const CLOSE_TIMEOUT: Duration = Duration::from_secs(2);

type OpenReply = oneshot::Sender<io::Result<yamux::Stream>>;

/// Handle to a session. Cheap to clone.
#[derive(Clone)]
pub struct Control {
    open: mpsc::UnboundedSender<OpenReply>,
    close: CancellationToken,
    dead: CancellationToken,
}

/// Streams opened by the peer.
pub struct Incoming {
    rx: mpsc::Receiver<Stream>,
}

impl Incoming {
    pub async fn accept(&mut self) -> Option<Stream> {
        self.rx.recv().await
    }
}

/// Run a session over `io` on a task of the current runtime; it ends with
/// the connection or [`Control::close`]. Streams beyond the backlog are
/// dropped (reset).
pub fn session<T>(io: T, mode: Mode) -> (Control, Incoming)
where
    T: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
    let mut cfg = yamux::Config::default();
    cfg.set_max_num_streams(MAX_STREAMS).set_read_after_close(true);
    let mut conn = yamux::Connection::new(io.compat(), cfg, mode);
    let (open_tx, mut open_rx) = mpsc::unbounded_channel::<OpenReply>();
    let (in_tx, in_rx) = mpsc::channel(INBOUND_BACKLOG);
    let (close, dead) = (CancellationToken::new(), CancellationToken::new());
    let (c2, d2) = (close.clone(), dead.clone());
    tokio::spawn(async move {
        let mut pending: VecDeque<OpenReply> = VecDeque::new();
        let drive = std::future::poll_fn(|cx| {
            while let Poll::Ready(Some(r)) = open_rx.poll_recv(cx) {
                pending.push_back(r);
            }
            while !pending.is_empty() {
                match conn.poll_new_outbound(cx) {
                    Poll::Ready(r) => {
                        let reply = pending.pop_front().unwrap();
                        let _ = reply.send(r.map_err(io::Error::other));
                    }
                    Poll::Pending => break,
                }
            }
            loop {
                match conn.poll_next_inbound(cx) {
                    Poll::Ready(Some(Ok(s))) => {
                        let _ = in_tx.try_send(s.compat());
                    }
                    Poll::Ready(_) => return Poll::Ready(()),
                    Poll::Pending => return Poll::Pending,
                }
            }
        });
        let closing = tokio::select! {
            _ = drive => false,
            _ = c2.cancelled() => true,
        };
        if closing {
            let _ = tokio::time::timeout(CLOSE_TIMEOUT, std::future::poll_fn(|cx| conn.poll_close(cx))).await;
        }
        d2.cancel();
    });
    (Control { open: open_tx, close, dead }, Incoming { rx: in_rx })
}

impl Control {
    /// Open a stream; the SYN goes out immediately.
    pub async fn open(&self) -> io::Result<Stream> {
        use futures::AsyncWriteExt;
        let (tx, rx) = oneshot::channel();
        self.open.send(tx).map_err(|_| io::Error::from(io::ErrorKind::NotConnected))?;
        let mut s = rx.await.map_err(|_| io::Error::from(io::ErrorKind::NotConnected))??;
        s.write(&[]).await?;
        s.flush().await?;
        Ok(s.compat())
    }

    /// Send GoAway and tear the session down.
    pub fn close(&self) {
        self.close.cancel();
    }

    /// Resolves once the session is gone (closed, peer EOF, error).
    pub async fn closed(&self) {
        self.dead.cancelled().await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    fn pair() -> ((Control, Incoming), (Control, Incoming)) {
        let (a, b) = tokio::io::duplex(4096);
        (session(a, Mode::Client), session(b, Mode::Server))
    }

    #[tokio::test]
    async fn open_is_eager_and_bidirectional() {
        let ((c, _ci), (_s, mut si)) = pair();
        let mut cs = c.open().await.unwrap();
        // The server sees the stream before the client writes anything,
        // and can speak first (the relay's challenge).
        let mut ss = si.accept().await.unwrap();
        ss.write_all(b"challenge\n").await.unwrap();
        let mut buf = [0u8; 10];
        cs.read_exact(&mut buf).await.unwrap();
        assert_eq!(&buf, b"challenge\n");
        cs.write_all(b"hi").await.unwrap();
        cs.shutdown().await.unwrap();
        let mut got = Vec::new();
        ss.read_to_end(&mut got).await.unwrap();
        assert_eq!(got, b"hi");
    }

    #[tokio::test]
    async fn bulk_transfer() {
        let ((_c, mut ci), (s, _si)) = pair();
        let mut ss = s.open().await.unwrap();
        let mut cs = ci.accept().await.unwrap();
        let data: Vec<u8> = (0..3 * 256 * 1024 + 12345).map(|i| (i * 7) as u8).collect();
        let d2 = data.clone();
        let w = tokio::spawn(async move {
            ss.write_all(&d2).await.unwrap();
            ss.shutdown().await.unwrap();
            ss
        });
        let mut got = Vec::new();
        cs.read_to_end(&mut got).await.unwrap();
        assert!(got == data);
        w.await.unwrap();
    }

    #[tokio::test]
    async fn close_ends_streams() {
        let ((c, _ci), (_s, mut si)) = pair();
        let mut cs = c.open().await.unwrap();
        let _ss = si.accept().await.unwrap();
        c.close();
        c.closed().await;
        let mut b = [0u8; 1];
        assert!(!matches!(cs.read(&mut b).await, Ok(1..)));
        assert!(c.open().await.is_err());
    }

    #[tokio::test]
    async fn peer_close_ends_session() {
        let ((c, _ci), (s, _si)) = pair();
        s.close();
        tokio::time::timeout(Duration::from_secs(5), c.closed()).await.unwrap();
    }
}
