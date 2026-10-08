# Remote access

⋮ → **Remote access** (home screen and drawer row, READY tawcroot installs
only — the terminal's gate) makes the open distro reachable over ssh,
either **over the local network** (default) or **from anywhere via a
relay**, [sshyeet.com](https://sshyeet.com) by default (the field takes a
self-hosted one):

```
ssh -p 2222 bold-cook-fern@192.168.1.23     # local network
ssh -J sshyeet.com bold-cook-fern@tidy-crab  # relay
```

Login is a fresh three-word passphrase (the username), or public
keys: fetched from GitHub, GitLab or Codeberg (`https://<host>/<user>.keys`,
directly, never via the relay) or pasted. With keys the command's user
is `root` (any name works).

TAWC ships its own Rust client (`remote/`, crate `tawc_remote`); the
relay only splices bytes. Nothing runs until Start; everything stops on
Stop, the idle close ("Close after 5 minutes with no connection", on by
default), the notification's Exit, or the distro's uninstall.

## Trust model

- The relay sees SSH ciphertext only. The secret is generated per Start
  from the OS CSPRNG and travels as the SSH *username* inside the
  encrypted session.
- **One host key per device**, `<filesDir>/remote/host_key` (OpenSSH
  format, 0600, written atomically; an unreadable file is replaced), for
  both modes. ssh asks once per device, then known_hosts checks it: a
  relay (or squatter) answering under our id with another key gets
  ssh's "host identification has changed" warning, not a quiet login.
  It also keeps the relay id, so with key login the whole command is
  stable. The cost: whoever saw the id can tell when the device is
  online on that relay (still only reaching a login prompt).
- Relay mode: one outbound `wss://<relay>/v1/tunnel` (`http://` relays
  give `ws://`; the SSH inside is end-to-end either way), no listener.
- Local mode: listens on port 2222 on the device's private IPv4
  addresses (Wi-Fi, Ethernet, its own hotspot, a VPN's private range)
  only. `LocalNetwork` excludes cellular interfaces by
  `ConnectivityManager` (needs `ACCESS_NETWORK_STATE`) and by name
  (`rmnet*`, `ccmni*`, clat `v4-*`), not by address: carriers hand out
  private 10.x addresses too. Not loopback. A busy port fails Start.
  `-p` is unavoidable: apps can't bind ports below 1024.
- Key login: `publickey` against the set (compared by key data, comments
  ignored; RSA, ECDSA, Ed25519 and `sk-` types), `none`/secret off.
  authorized_keys parsing skips comments, options and unknown lines,
  capped at 100 keys; zero usable keys fails Start.
- Everything in the relay's `ready` is validated before use: the id must
  derive from our host key (the relay can't hand out a name that certifies
  its own key), the jump host must match upstream's shell-inert grammar,
  free text is stripped of control characters and truncated. The
  browser-terminal link (`web`) and `latest` are ignored; the hello has no
  `comment`. The web terminal stays unused on purpose: it means trusting
  the website with the secret, which plain ssh doesn't.
- Anyone with the secret gets a root shell in the distro (app uid
  outside), like the in-app terminal. The idle screen says so.
- Secret: three words from the RFC 1751 dictionary (the relay ids'
  list, 11 bits each: 33 bits, `bold-cook-fern`), sshyeet's default.
  Online defence as upstream: constant-time compare with `none` auth;
  whole-agent token bucket, 10 burst / 10 per minute, taken *before* the
  compare and refunded only when right; 1 s stall per counted wrong guess;
  secret logins switch off after 300 wrong guesses (≈1 in 30 million)
  until the next Start. Usernames not shaped like a secret (three
  dictionary words) cost nothing.
  `publickey` refuses every key so a wrong secret reads "Permission
  denied (publickey)".
- Never logged: the agent's connection events (client addresses, logins)
  go nowhere but tests; the screen shows only the connection count. The
  debug broker's `remote-status` is the only other place the secret
  appears.
- Forwarding is always on, since a login can do the same from the shell
  (the distro shares the app's network namespace): `-L`/`-W` dial from
  the app process; `-R` listens on loopback only — `localhost`,
  `127.0.0.1`, `::1`; a wildcard (`*`, `""`, `0.0.0.0`) or another
  interface is refused — and each listener closes with its connection
  or Stop. Android's loopback is shared by all apps, like any port a
  shell opens.

## Layout

| Where | What |
|---|---|
| `remote/src/proto.rs` | control messages, 16 KiB line framing, `ready` validation |
| `remote/src/sid/` | session ids (below); the RFC 1751 dictionary (checked against the RFC text) |
| `remote/src/mux.rs` | the `yamux` crate behind a small open/accept/close handle (below) |
| `remote/src/tunnel.rs` | WebSocket dial (rustls + ring + webpki-roots), byte pump, pings |
| `remote/src/sshd/` | russh server: auth policy, session channels, `-L`/`-R` |
| `remote/src/spawn.rs` | `Launcher` trait; pty/pipe spawn in a new session |
| `remote/src/agent.rs` | transports (relay loop / local listeners), login, idle close, status, one thread |
| `remote/sftp-server/` | build of OpenSSH's `sftp-server` for the rootfs (below) |
| `compositor/src/remote_jni.rs` | `nativeRemoteStart/Stop/Status`, reverse `onRemoteEvent` |
| `app/…/remote/RemoteSession.kt` | process-wide state, `Reason.Remote` hold, control thread |
| `app/…/remote/RemoteAccessActivity.kt` | the screen |
| `app/…/remote/LocalNetwork.kt`, `KeyHosts.kt` | local listen addresses; `.keys` fetch |

The crate is pure Rust (no ndk/jni) with its own `Cargo.lock` for host
tests; the APK links it into `libcompositor.so` through
`compositor/Cargo.toml` and its lock. It runs on one thread
(`tawc-remote`, current-thread tokio runtime); child waits use blocking
threads (`tawc-remote-wait`). The compositor's panic hook doesn't abort
for `tawc-remote*` threads (tokio contains task panics; the agent thread
catches the rest and reports `failed`).

### Spawning

`TawcrootMethod.spawnEnvelope` returns the tawcroot argv up to and
including the guest env (`… -- /usr/bin/env -i K=V …`), the
resolved root shell, and the host-side `TMPDIR`/cwd; the terminal's
`ptyShellExec` is built on it too. Native appends `TERM`, `SSH_CLIENT`,
`SSH_CONNECTION` and any client `env` requests (well-formed names only),
then `<shell> -l`, `<command_shell> -lc <cmd>` (bash, or `/bin/sh` in a
rootfs without it; like every command spawn, see [terminal.md](terminal.md)), or `/usr/lib/tawc/sftp-server` (below;
falling back to a distro OpenSSH's for a rootfs not yet refreshed). The
envelope is built at Start, so bind edits apply from the next Start.

The child gets `setsid()` (rootfs-session invariant) and, with a pty,
`TIOCSCTTY`; every fd above stdio is marked close-on-exec first. Waits
are `waitpid` on that pid in a blocking thread — no SIGCHLD handler in
the app process. A closed channel, dropped connection or Stop hangs the
login up like a closed terminal: pty master closed plus
SIGHUP. Stragglers (`nohup`, `setsid`) fall to the session service's
stray tail.

### sftp-server

sftp/scp (OpenSSH ≥ 9's scp is sftp) run TAWC's own build of OpenSSH's
`sftp-server`, so no distro package is needed. It must run inside
tawcroot to see the guest's paths and binds, so it's **static bionic**
like `ando`: `remote/sftp-server/build.sh` builds it from the pinned
`openssh-portable` dep (release tag, `configure` included) with the NDK,
`--without-openssl --without-zlib`, into
`jniLibs/<abi>/libsftp-server.so`; `SftpServerInstallProvider` copies it
to `/usr/lib/tawc/sftp-server` in every rootfs.

Bionic fixes (all in `remote/sftp-server/`, no patches to OpenSSH):
newest-API headers (static `libc.a` has every symbol; older headers hide
declarations configure's link tests find); `recallocarray` forced off
(in `libc.a`, never declared); `__sentinel__` defined to itself (defines.h
would blank it and break bionic's `execle`); a force-included header
declaring `crypt` (only sshd's `xcrypt.o` uses it, never linked) and
turning bionic's macro `bzero` into a real function; openbsd-compat's
`getrrsetbyname` (ssh's SSHFP) left out.

**User lookups:** bionic's `getpw*`/`getgr*` don't read `/etc/passwd`;
they synthesize Android IDs (uid 1000 = "system", root's home `/`). So
`guest-passwd.c` provides `getpwuid/getpwnam/getgrgid/getgrnam/initgroups`
over the guest's `/etc/passwd` and `/etc/group` (sftp's start dir and
`ls -l` owners), and the build fails if lld's `--trace-symbol` shows
bionic defining any of them (it would also be a duplicate symbol). ~760
KB per ABI.

### Tunnel protocol (relay v1)

The relay-side spec (enough to write a replacement relay) is
[remote/README.md](../remote/README.md); this is the agent's view.

`GET wss://<relay>/v1/tunnel`, `User-Agent: tawc/<versionName>
android/<arch>`, `fly-force-instance-id: <node>` on reconnect to land on
the same relay machine. Every binary message is a chunk of one byte
stream carrying yamux; the agent is the yamux client.

Control stream (first stream, NDJSON, alternating): relay `challenge`
(`v`, base64 nonce ≥ 16 B; `v > 1` is fatal "update TAWC") → agent
`hello` (`host_key` authorized_keys form, `sig` = SSH wire signature
over `"sshyeet hello v1\0" ‖ nonce`, `ttl` 0 = the relay's maximum,
`agent`, `id_words`) → relay `ready` or `error`.

**Session ids**: `H = SHA-256("sshyeet id v3\0" ‖ wire(key))`,
`id_words` (default 2) RFC 1751 words from 11-bit fields of `H[0:16]`,
long form `-base32(H[16:26])`; `sid` carries upstream's golden vector.
`ready` is accepted only if its id is exactly that derivation of *our*
key at the count asked for; anything else is fatal.

`"session id taken"` (another key holds our words), as upstream's
persistent-key agent: ask again at once with one more word, same key (so
known_hosts and local mode stay valid), up to 8, then fail. The longer
id lasts this run; the next Start asks for 2 again. With no saved key
(tests only) it's upstream's per-run policy: new key and secret first,
then also a word per collision. Other errors are fatal. Later `bye`:
`reconnect: true` → back off (1 s doubling to 1 min, reset after a
minute up) and reconnect with the same identity; otherwise fatal. A
silent drop reconnects too; 5 failed attempts before the first `ready`
give up.

Client streams (relay-opened): one JSON header line `{"from","via"}`
(15 s deadline), then raw SSH.

**yamux**, as upstream's agent: libp2p's `yamux` crate (0.13, 256
streams max, read after close), driven by one task in `mux.rs` that also
serves opens. The crate opens streams lazily (SYN on the first data
frame), but the relay speaks first on the control stream, so `open`
writes an empty frame to send the SYN. Liveness is the WebSocket's: a
ping every 25 s, the tunnel drops after 75 s with nothing received
(`tunnel::byte_stream`).

### russh notes

- Replies to channel requests are sent inside the `Handler` callbacks:
  russh tracks `want_reply` per *channel*, so a reply from another task
  could answer a later request. Work happens in a per-channel task fed
  over a `Ctl` channel.
- `auth_rejection_time` is zero; the only stall is ours (counted wrong
  guesses). `inactivity_timeout` off; keepalive every 60 s, 3 misses.
- `-R` listeners open `forwarded-tcpip` channels through the session
  `Handle`; OpenSSH's "Allocated port" works for port 0.
- Logs: the compositor's logger filters `russh` to `error`.

## App side

`RemoteSession` owns one agent per process. Start builds the request
(envelope JSON, `transport` relay/local, `login` secret/keys, idle
timeout, agent string) off the main thread, fetching keys first,
calls `nativeRemoteStart` (null, or the error to show), and acquires
`Reason.Remote(distroId, clients)` — the notification reads e.g.
"remote access · 1 client" and the process stays a foreground service.
`status` events from native only *queue* a refresh on the
`tawc-remote-ctl` thread (`nativeRemoteStop` joins the agent thread, so
the callback must not call native synchronously); the refresh updates
the hold's client count, and when the agent ended on its own (idle
close, failure) releases the hold and reaps the thread. Other events
are ignored. Exit (`SessionExit`) and
`InstallationService.startUninstall` stop it.

**Idle close:** the agent stops once no one has been logged in for 5
min, counted from Start and again from each last logout; one timer across
tunnel reconnects. The relay URL and the checkbox persist in `Settings`
(`remoteRelay`, `remoteIdleClose`).

Screen, kept to what matters: idle ("SSH into this device…", mode radio
— local network / via a relay, the relay URL field only for the
latter —, "Public key from" radio — generated passphrase / GitHub / GitLab /
Codeberg (username field) / paste a public key (text box) —, idle-close
checkbox, the error if the last run failed, Start). Choices and drafts
persist in `Settings` as they change. Running: green "Ready" and the
connection count (or connecting/reconnecting), the command (monospace;
if one line doesn't fit, the destination goes on a `\`-continued second
line, and a still-too-long line scrolls sideways rather than wrapping),
tap to copy the one-line form as a sensitive clip, "Logs in with
keys from …" in key mode, host key fingerprint (own monospace line, scrolls rather than wraps), Stop. Or "running for
<other distro>" with Stop. State lives
in `RemoteSession`, so rotation and leaving the screen change nothing.

Screen off with the SoC suspended still stalls the tunnel unless "Keep
awake" is on ([session-service.md](session-service.md)). Wake on connect (push via
UnifiedPush/ntfy): [../plans/remote-wake.md](../plans/remote-wake.md).

## Testing

- `cd remote && cargo test` — unit tests (sid vector; the derivation
  itself is checked live, since the agent refuses any id the relay
  assigns that doesn't match; secret shape and budget; `ready`
  validation; framing; mux open/close and bulk transfer; pty/pipe spawn) and `tests/e2e.rs`
  against an in-process fake relay (WebSocket + yamux server + TCP port
  that becomes client streams) with the host's `ssh`/`scp` and a russh
  client: exec/exit status/stderr, throttling (parallel guesses share
  the budget), pty size + window-change, `-W` forwarding, `-R` (loopback
  binds, refused wildcards, closed on disconnect and Stop), id-taken
  (saved key: a word per collision, back to 2 next Start, fails past 8;
  unsaved key: new key, then words), saved key stable across Starts and modes,
  bye/drop reconnects keeping the id, fatal bye/v2/foreign id,
  unreachable relay, idle close (and a login holding it off), key login
  (Ed25519 + RSA, any user, others refused), local mode (secret and
  keys, command shape, busy port), Stop
  hanging up pty and pipe logins, the envelope launcher with sftp/scp
  both ways. No network.
- `TAWC_LIVE_RELAY=1 cargo test --test e2e live_relay` — host agent
  through sshyeet.com. Run before merging relay-facing changes.
- `cargo run --example relay-spike [RELAY] [IDLE_SECONDS]` — serve `/bin/sh`
  through a relay and print status; connect by hand.
- Device (`tests/integration/tests/remote.rs`): `remote::` checks the
  module is inert before any start, and local mode on the device's
  loopback through `adb forward` (secret login, then key login); the
  live test (start, `ssh -J` from
  the host, distro `ID=`, `Remote` hold, owner names and home over sftp,
  Stop hangs up a running login and drops the hold) runs only with `TAWC_LIVE_RELAY=1
  scripts/run-integration-tests.sh remote::`.
- Broker actions (debug): `remote-start [installId] [idle] [relay | addrs]
  [keys]`, `remote-status`, `remote-stop`.
- App unit tests: `RemoteSessionTest` (hold lifecycle, one agent at a
  time, `stopFor`).

When testing by hand, `-o` options don't reach the `-J` hop; use a
scratch `ssh -F` config with `UserKnownHostsFile` and `BatchMode`.

## Upstream

Protocol and behaviour follow upstream's Rust agent (`agent/`; public
source `https://sshyeet.com/dl/src.tar.gz`, version `npnvl5ppj3` =
espes/sshyeet `e556664`, reviewed 2026-10-08). That repo is private, so
`remote/` stays our own implementation rather than a vendored copy.
Deliberate differences: no `comment` in the hello, `web`/`latest`
ignored, `-R` forwarding, and a saved host key
by default. Open questions for the author: is v1 stable and will the
relay keep speaking it after a bump; is a non-upstream agent string
welcome; any per-node rate limits (the relay answers 429 on the dial,
reported as "relay is rate limiting us").
