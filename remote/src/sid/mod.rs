//! Session ids: a pure function of the host key, computed the same way
//! the relay does, so the agent can check the id the relay assigns.
//!
//! ```text
//! H     = SHA-256("sshyeet id v3\0" || ssh-wire(pubkey))
//! short = W[bits(H, 0:11)] "-" W[bits(H, 11:22)] "-" ...   (n words from H[0:16])
//! long  = short "-" base32(H[16:26])   (16 chars, lowercase, no padding)
//! ```
//!
//! `n` is the agent's choice (`id_words` in the hello; 2 by default).
//! Same as upstream's `agent/src/sid.rs` (golden vector below).

mod rfc1751;

use sha2::{Digest, Sha256};

pub const WORD_BITS: u32 = 11;
pub const MAX_WORDS: usize = 8;
pub const DEFAULT_ID_WORDS: usize = 2;

const HASH_DOMAIN: &[u8] = b"sshyeet id v3\0";
const WORD_BYTES: usize = 16;
const BIND_OFF: usize = 16;
const BIND_BYTES: usize = 10;

fn base32() -> data_encoding::Encoding {
    let mut spec = data_encoding::Specification::new();
    spec.symbols.push_str("abcdefghijklmnopqrstuvwxyz234567");
    spec.encoding().expect("valid base32 spec")
}

fn key_hash(key_wire: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(HASH_DOMAIN);
    h.update(key_wire);
    h.finalize().into()
}

/// The `i`-th 11-bit big-endian field of `h`.
fn word_index(h: &[u8], i: usize) -> usize {
    (i * WORD_BITS as usize..(i + 1) * WORD_BITS as usize)
        .fold(0, |v, b| v << 1 | ((h[b / 8] >> (7 - b % 8)) & 1) as usize)
}

/// Dictionary word at `i` (mod 2048).
pub fn word(i: usize) -> &'static str {
    rfc1751::WORDS[i & (rfc1751::WORDS.len() - 1)]
}

pub fn is_word(w: &str) -> bool {
    static SET: std::sync::OnceLock<std::collections::HashSet<&'static str>> = std::sync::OnceLock::new();
    SET.get_or_init(|| rfc1751::WORDS.iter().copied().collect()).contains(w)
}

/// Short (routing, typing) id of `n` words (clamped to 1..=8) for a host
/// key in SSH wire encoding.
pub fn derive(key_wire: &[u8], n: usize) -> String {
    let h = key_hash(key_wire);
    (0..n.clamp(1, MAX_WORDS)).map(|i| word(word_index(&h[..WORD_BYTES], i))).collect::<Vec<_>>().join("-")
}

/// Self-certifying long id.
pub fn derive_long(key_wire: &[u8], n: usize) -> String {
    let h = key_hash(key_wire);
    format!("{}-{}", derive(key_wire, n), base32().encode(&h[BIND_OFF..BIND_OFF + BIND_BYTES]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dictionary() {
        assert_eq!(rfc1751::WORDS.len(), 2048);
        let mut seen = std::collections::HashSet::new();
        for w in rfc1751::WORDS {
            assert!((1..=4).contains(&w.len()) && w.bytes().all(|c| c.is_ascii_lowercase()), "{w}");
            assert!(seen.insert(w), "duplicate {w}");
        }
        assert_eq!((word(0), word(2047), word(2048)), ("a", "yoke", "a"));
        assert!(is_word("yoke") && is_word("root") && !is_word("roots") && !is_word("Yoke"));
    }

    #[test]
    fn word_index_bits() {
        // Upstream's vector: alternating all-ones / all-zeros fields.
        let h = [0xff, 0xe0, 0x03, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        let got: Vec<usize> = (0..4).map(|i| word_index(&h, i)).collect();
        assert_eq!(got, [2047, 0, 2047, 0]);
    }

    // Ed25519 key with public bytes 0x00..0x1f. Expected values computed
    // independently (Python hashlib + the word lists).
    fn vector_key() -> Vec<u8> {
        let mut wire = Vec::new();
        wire.extend_from_slice(&11u32.to_be_bytes());
        wire.extend_from_slice(b"ssh-ed25519");
        wire.extend_from_slice(&32u32.to_be_bytes());
        wire.extend((0u8..32).collect::<Vec<_>>());
        wire
    }

    #[test]
    fn vectors() {
        let k = vector_key();
        assert_eq!(derive(&k, 1), "none");
        assert_eq!(derive(&k, 2), "none-rays");
        assert_eq!(derive(&k, 3), "none-rays-frey");
        assert_eq!(derive(&k, 8), "none-rays-frey-po-oust-lola-feed-news");
        assert_eq!(derive(&k, 0), "none");
        assert_eq!(derive(&k, 9), derive(&k, 8));
        assert_eq!(derive_long(&k, 2), "none-rays-hrcx6z2hk2si3szp");
    }

    // Upstream's golden (agent/src/sid.rs, internal/sid/golden_test.go).
    #[test]
    fn upstream_golden() {
        let mut wire = Vec::new();
        wire.extend_from_slice(&11u32.to_be_bytes());
        wire.extend_from_slice(b"ssh-ed25519");
        wire.extend_from_slice(&32u32.to_be_bytes());
        wire.extend([7u8; 32]);
        assert_eq!(derive_long(&wire, 3), "pep-weed-dash-ancalixtmrycqzex");
    }
}
