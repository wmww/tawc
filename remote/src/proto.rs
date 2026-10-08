//! Agent <-> relay control protocol, v1 (see notes/remote-access.md).
//!
//! Control stream: newline-delimited JSON, strictly alternating
//! (challenge, hello, ready | error, later bye). Client streams start with
//! one JSON header line, then carry raw SSH bytes.

use serde::{Deserialize, Serialize};
use std::io;
use tokio::io::{AsyncBufRead, AsyncBufReadExt, AsyncWrite, AsyncWriteExt};

pub const TUNNEL_PATH: &str = "/v1/tunnel";
pub const VERSION: u32 = 1;
pub const HELLO_SIG_PREFIX: &[u8] = b"sshyeet hello v1\0";
pub const MAX_LINE: usize = 16 << 10;
pub const HEADER_FORCE_INSTANCE: &str = "fly-force-instance-id";
pub const ERR_ID_TAKEN: &str = "session id taken";

#[derive(Debug, Deserialize)]
pub struct Challenge {
    pub op: String,
    #[serde(default)]
    pub v: u32,
    #[serde(with = "b64")]
    pub nonce: Vec<u8>,
}

#[derive(Debug, Serialize)]
pub struct Hello<'a> {
    pub op: &'static str,
    pub v: u32,
    pub host_key: &'a str,
    #[serde(with = "b64")]
    pub sig: Vec<u8>,
    #[serde(skip_serializing_if = "is_zero")]
    pub ttl: u64,
    pub agent: &'a str,
    /// Words wanted in the session id (1..=8).
    pub id_words: usize,
}

fn is_zero(v: &u64) -> bool {
    *v == 0
}

#[derive(Debug, Default, Clone, Deserialize, Serialize, PartialEq)]
pub struct Ready {
    #[serde(default)]
    pub op: String,
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub jump: String,
    #[serde(default)]
    pub node: String,
    #[serde(default)]
    pub region: String,
    #[serde(default)]
    pub expires: i64,
    #[serde(default)]
    pub notice: String,
}

/// Peek at `op`/`msg`/bye fields of any control message.
#[derive(Debug, Default, Deserialize)]
pub struct Envelope {
    #[serde(default)]
    pub op: String,
    #[serde(default)]
    pub msg: String,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub reconnect: bool,
}

#[derive(Debug, Default, Deserialize, Serialize)]
pub struct ClientHeader {
    #[serde(default)]
    pub from: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub via: String,
}

/// Go's `[]byte` JSON encoding: standard base64 with padding.
mod b64 {
    use data_encoding::BASE64;
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(v: &[u8], s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&BASE64.encode(v))
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Vec<u8>, D::Error> {
        let s = String::deserialize(d)?;
        BASE64.decode(s.as_bytes()).map_err(serde::de::Error::custom)
    }
}

/// Read one line (without the newline), at most [`MAX_LINE`] bytes.
pub async fn read_line<R: AsyncBufRead + Unpin>(r: &mut R) -> io::Result<Vec<u8>> {
    let mut line = Vec::new();
    loop {
        let buf = r.fill_buf().await?;
        if buf.is_empty() {
            return Err(if line.is_empty() {
                io::ErrorKind::UnexpectedEof.into()
            } else {
                io::Error::new(io::ErrorKind::UnexpectedEof, "truncated line")
            });
        }
        let (chunk, done) = match buf.iter().position(|&b| b == b'\n') {
            Some(i) => (&buf[..i], Some(i + 1)),
            None => (buf, None),
        };
        if line.len() + chunk.len() > MAX_LINE {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "line too long"));
        }
        line.extend_from_slice(chunk);
        let used = done.unwrap_or(buf.len());
        r.consume(used);
        if done.is_some() {
            return Ok(line);
        }
    }
}

pub async fn write_json<W: AsyncWrite + Unpin, T: Serialize>(w: &mut W, v: &T) -> io::Result<()> {
    let mut b = serde_json::to_vec(v)?;
    b.push(b'\n');
    if b.len() > MAX_LINE {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "message too large"));
    }
    w.write_all(&b).await?;
    w.flush().await
}

/// `host`, `host:port`, IPv4[:port] or `[IPv6]:port`, and nothing a shell
/// cares about. Same grammar as upstream's `jumpRE`.
pub fn valid_jump(s: &str) -> bool {
    let (host, port) = if let Some(rest) = s.strip_prefix('[') {
        let Some(end) = rest.find(']') else { return false };
        let inner = &rest[..end];
        if !(2..=45).contains(&inner.len())
            || !inner.chars().all(|c| c.is_ascii_hexdigit() || c == ':' || c == '.')
        {
            return false;
        }
        (None, &rest[end + 1..])
    } else {
        let end = s.find(':').unwrap_or(s.len());
        (Some(&s[..end]), &s[end..])
    };
    if let Some(h) = host {
        if !(1..=253).contains(&h.len())
            || !h.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-')
        {
            return false;
        }
    }
    if port.is_empty() {
        return true;
    }
    let Some(digits) = port.strip_prefix(':') else { return false };
    (1..=5).contains(&digits.len()) && digits.chars().all(|c| c.is_ascii_digit())
}

fn valid_region(s: &str) -> bool {
    s.len() <= 16 && s.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
}

/// Strip control characters (terminal escapes included) and truncate to
/// at most `max` bytes on a char boundary.
pub fn printable(s: &str, max: usize) -> String {
    let mut out = String::new();
    for c in s.chars() {
        let u = c as u32;
        if u < 0x20 || u == 0x7f || (0x80..0xa0).contains(&u) {
            continue;
        }
        if out.len() + c.len_utf8() > max {
            break;
        }
        out.push(c);
    }
    out
}

#[derive(Debug, PartialEq)]
pub enum ReadyError {
    /// Not a `ready` at all.
    Malformed,
    IdMismatch(String),
    BadJump(String),
}

/// Validate and sanitize the relay's `ready`: the id must be the short
/// one our host key derives at the `id_words` asked for, the jump host
/// must be shell-inert, free text is cleaned.
pub fn validate_ready(mut r: Ready, key_wire: &[u8], id_words: usize) -> Result<Ready, ReadyError> {
    if r.op != "ready" {
        return Err(ReadyError::Malformed);
    }
    if r.id != crate::sid::derive(key_wire, id_words) {
        return Err(ReadyError::IdMismatch(printable(&r.id, 64)));
    }
    if !valid_jump(&r.jump) {
        return Err(ReadyError::BadJump(printable(&r.jump, 64)));
    }
    if !valid_region(&r.region) {
        r.region.clear();
    }
    r.node = printable(&r.node, 64);
    r.notice = printable(&r.notice, 240);
    Ok(r)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jump_grammar() {
        for ok in ["sshyeet.com", "sshyeet.com:22", "1.2.3.4:2222", "[::1]:22", "[fe80::1]", "a-b.c"] {
            assert!(valid_jump(ok), "{ok}");
        }
        for bad in ["", "a b", "a;b", "host:", "host:123456", "[::1", "[zz]:1", "h:1:2", "$(x)", "a'b"] {
            assert!(!valid_jump(bad), "{bad}");
        }
        assert!(!valid_jump(&"a".repeat(254)));
    }

    #[test]
    fn printable_strips_and_truncates() {
        assert_eq!(printable("a\x1b[31mb\n\u{85}c", 10), "a[31mbc");
        assert_eq!(printable("héllo", 2), "h");
    }

    fn key() -> Vec<u8> {
        let mut w = Vec::new();
        w.extend_from_slice(&11u32.to_be_bytes());
        w.extend_from_slice(b"ssh-ed25519");
        w.extend_from_slice(&32u32.to_be_bytes());
        w.extend([7u8; 32]);
        w
    }

    #[test]
    fn ready_validation() {
        let k = key();
        let good = Ready {
            op: "ready".into(),
            id: crate::sid::derive(&k, 2),
            jump: "sshyeet.com".into(),
            region: "ams".into(),
            node: "n\x07ode".into(),
            notice: "hi\x1b]0;x\x07".into(),
            ..Default::default()
        };
        let r = validate_ready(good.clone(), &k, 2).unwrap();
        assert_eq!(r.node, "node");
        assert_eq!(r.notice, "hi]0;x");
        assert_eq!(
            validate_ready(Ready { id: "able-able-ant".into(), ..good.clone() }, &k, 2),
            Err(ReadyError::IdMismatch("able-able-ant".into()))
        );
        assert!(matches!(
            validate_ready(Ready { jump: "x;rm".into(), ..good.clone() }, &k, 2),
            Err(ReadyError::BadJump(_))
        ));
        let r = validate_ready(Ready { region: "BAD REGION".into(), ..good.clone() }, &k, 2).unwrap();
        assert_eq!(r.region, "");
        // Our words at another count, or the long form, don't pass.
        let long = Ready { id: crate::sid::derive_long(&k, 2), ..good.clone() };
        assert!(matches!(validate_ready(long, &k, 2), Err(ReadyError::IdMismatch(_))));
        let three = Ready { id: crate::sid::derive(&k, 3), ..good.clone() };
        assert!(matches!(validate_ready(three, &k, 2), Err(ReadyError::IdMismatch(_))));
        assert_eq!(validate_ready(Ready { op: "error".into(), ..good }, &k, 2), Err(ReadyError::Malformed));
    }

    #[tokio::test]
    async fn line_cap_and_split_reads() {
        use tokio::io::BufReader;
        let data = b"{\"a\":1}\nsecond\n".to_vec();
        // One byte per read: lines split across arbitrary chunks.
        let (mut w, r) = tokio::io::duplex(1);
        tokio::spawn(async move {
            w.write_all(&data).await.unwrap();
        });
        let mut r = BufReader::with_capacity(3, r);
        assert_eq!(read_line(&mut r).await.unwrap(), b"{\"a\":1}");
        assert_eq!(read_line(&mut r).await.unwrap(), b"second");
        assert_eq!(read_line(&mut r).await.unwrap_err().kind(), io::ErrorKind::UnexpectedEof);

        let long = vec![b'x'; MAX_LINE + 1];
        let mut r = BufReader::new(&long[..]);
        assert_eq!(read_line(&mut r).await.unwrap_err().kind(), io::ErrorKind::InvalidData);
        let mut exact = vec![b'x'; MAX_LINE];
        exact.push(b'\n');
        let mut r = BufReader::new(&exact[..]);
        assert_eq!(read_line(&mut r).await.unwrap().len(), MAX_LINE);
    }

    #[test]
    fn challenge_decodes_go_bytes() {
        let c: Challenge = serde_json::from_str(r#"{"op":"challenge","v":1,"nonce":"AAECAwQFBgcICQoLDA0ODw=="}"#).unwrap();
        assert_eq!(c.nonce, (0u8..16).collect::<Vec<_>>());
    }
}
