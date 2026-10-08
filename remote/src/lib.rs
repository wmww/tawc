//! Remote access: TAWC's own client for the sshyeet.com relay. Makes the
//! open distro reachable with `ssh -J sshyeet.com <secret>@<id>`; the relay
//! only splices SSH ciphertext. See notes/remote-access.md.
//!
//! Pure Rust and host-buildable (no Android deps): the compositor crate
//! links it and drives it over JNI (`remote_jni.rs`).

pub mod agent;
pub mod event;
pub mod hostkey;
pub mod mux;
pub mod proto;
pub mod sid;
pub mod spawn;
pub mod sshd;
pub mod tunnel;

pub use agent::{Agent, Config, Status};
