//! Host-side driver for the in-app exec broker.
//!
//! Picks a free local TCP port, sets up `adb forward` to the device-side
//! `LocalServerSocket`, sends the protocol header, and multiplexes local
//! stdio over the socket. Exit code is the child's exit code.
//!
//! Wire protocol: notes/exec-broker.md.

use std::env;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::{Command, ExitCode, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;

const SOCKET_NAME: &str = "me.phie.tawc.exec";

const STREAM_STDIN: u8 = 0;
const STREAM_STDOUT: u8 = 1;
const STREAM_STDERR: u8 = 2;
const STREAM_EXIT: u8 = 3;
const STREAM_STDIN_EOF: u8 = 4;
const STREAM_ERR: u8 = 5;

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let parsed = match parse_args(&args) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("tawc-exec: {e}");
            eprintln!("usage: tawc-exec [--cwd DIR] [--env K=V ...] [--op-title TITLE] -- ARGV0 [ARG ...]");
            eprintln!("       tawc-exec --action NAME [--arg K=V ...]");
            eprintln!("       tawc-exec --in-rootfs ID [--op-title TITLE] [-- CMD ...]");
            return ExitCode::from(2);
        }
    };

    match run(parsed) {
        Ok(code) => map_exit(code),
        Err(e) => {
            eprintln!("tawc-exec: {e}");
            ExitCode::from(255)
        }
    }
}

/// Top-level invocation kind. Mirrors the wire protocol: an ARGV-form
/// header for fork-exec, an ACTION-form header for an in-process
/// broker action, or a RUNINSIDE-form header for chroot dispatch.
/// Mutually exclusive — `parse_args` rejects mixes.
///
/// `op_title` (Exec / RunInside): when present, the broker mirrors
/// process stdio into an in-app log screen titled with this string.
/// `--op-title` on the host CLI controls it.
enum Parsed {
    Exec {
        argv: Vec<String>,
        env: Vec<String>,
        cwd: Option<String>,
        op_title: Option<String>,
    },
    Action {
        name: String,
        args: Vec<(String, String)>,
    },
    /// Run a command inside an installed chroot. The broker dispatches
    /// to the install's [InstallationMethod.startInside]. `cmd` empty =
    /// interactive `bash -l`.
    RunInside {
        install_id: String,
        cmd: String,
        op_title: Option<String>,
    },
}

fn parse_args(args: &[String]) -> Result<Parsed, String> {
    let mut env = Vec::new();
    let mut cwd: Option<String> = None;
    let mut action_name: Option<String> = None;
    let mut action_args: Vec<(String, String)> = Vec::new();
    let mut run_inside_id: Option<String> = None;
    let mut op_title: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--" => { i += 1; break; }
            "--env" => {
                i += 1;
                let v = args.get(i).ok_or("--env needs argument")?;
                if !v.contains('=') { return Err(format!("--env value must be K=V (got '{v}')")); }
                env.push(v.clone());
                i += 1;
            }
            "--cwd" => {
                i += 1;
                cwd = Some(args.get(i).ok_or("--cwd needs argument")?.clone());
                i += 1;
            }
            "--action" => {
                i += 1;
                let v = args.get(i).ok_or("--action needs argument")?.clone();
                if v.is_empty() { return Err("--action name must not be empty".into()); }
                action_name = Some(v);
                i += 1;
            }
            "--arg" => {
                i += 1;
                let v = args.get(i).ok_or("--arg needs argument")?;
                let eq = v.find('=').ok_or_else(||
                    format!("--arg value must be key=value (got '{v}')"))?;
                action_args.push((v[..eq].to_string(), v[eq+1..].to_string()));
                i += 1;
            }
            "--in-rootfs" => {
                i += 1;
                let v = args.get(i).ok_or("--in-rootfs needs install id")?.clone();
                if v.is_empty() { return Err("--in-rootfs id must not be empty".into()); }
                run_inside_id = Some(v);
                i += 1;
            }
            "--op-title" => {
                i += 1;
                let v = args.get(i).ok_or("--op-title needs argument")?.clone();
                if v.is_empty() { return Err("--op-title must not be empty".into()); }
                if v.contains('\n') { return Err("--op-title may not contain LF".into()); }
                op_title = Some(v);
                i += 1;
            }
            "-h" | "--help" => return Err("see notes/exec-broker.md".to_string()),
            other if other.starts_with("--") => {
                return Err(format!("unknown flag '{other}'"));
            }
            // First non-flag argument: treat as the start of argv. The
            // explicit `--` separator is still accepted but optional, so
            // callers don't need to remember it.
            _ => break,
        }
    }
    if let Some(id) = run_inside_id {
        // RUNINSIDE form. Positional args after `--` (or directly) are
        // joined with spaces and become the bash -lc command. Empty
        // (no positional args) means interactive `bash -l`.
        if action_name.is_some() || !action_args.is_empty() {
            return Err("--action / --arg can't be combined with --in-rootfs".into());
        }
        if !env.is_empty() || cwd.is_some() {
            return Err("--env / --cwd are ARGV-form only; not allowed with --in-rootfs".into());
        }
        let cmd = if i < args.len() {
            args[i..].join(" ")
        } else {
            String::new()
        };
        if id.contains('\n') || cmd.contains('\n') {
            return Err("install id / cmd may not contain LF".into());
        }
        return Ok(Parsed::RunInside { install_id: id, cmd, op_title });
    }
    if let Some(name) = action_name {
        // ACTION form. ARGV must be empty; --env / --cwd are also
        // ARGV-only (ENV replaces the inherited env for the forked
        // child, has no meaning in-process).
        if i < args.len() {
            return Err("--action takes no positional ARGV".into());
        }
        if !env.is_empty() {
            return Err("--env is for fork-exec mode; not allowed with --action".into());
        }
        if cwd.is_some() {
            return Err("--cwd is for fork-exec mode; not allowed with --action".into());
        }
        if op_title.is_some() {
            return Err("--op-title is not allowed with --action (the action's own log screen handles this)".into());
        }
        if name.contains('\n') || action_args.iter().any(|(k, v)| k.contains('\n') || v.contains('\n')) {
            return Err("--action name / --arg values may not contain LF".into());
        }
        return Ok(Parsed::Action { name, args: action_args });
    }
    // ARGV form. --action / --arg must not be present.
    if !action_args.is_empty() {
        return Err("--arg is for action mode; missing --action".into());
    }
    let argv: Vec<String> = args[i..].to_vec();
    if argv.is_empty() {
        return Err("no command (use `-- ARGV0 ...`, `--action NAME`, or `--in-rootfs ID -- CMD`)".to_string());
    }
    if argv.iter().any(|a| a.contains('\n')) {
        return Err("argv may not contain LF".to_string());
    }
    if env.iter().any(|a| a.contains('\n')) {
        return Err("env may not contain LF".to_string());
    }
    Ok(Parsed::Exec { argv, env, cwd, op_title })
}

fn run(p: Parsed) -> io::Result<i32> {
    // Pick a free port on 127.0.0.1, drop the listener immediately so
    // adbd can bind. Race window is tiny and the port is otherwise
    // unbound; if it loses we error loudly.
    let port = {
        let l = TcpListener::bind("127.0.0.1:0")?;
        l.local_addr()?.port()
    };
    let serial = env::var("ANDROID_SERIAL").ok();
    let fwd = AdbForward::start(serial.as_deref(), port)?;

    // Make sure the app process is actually up before trying to use the
    // forward. `adb forward localabstract:foo tcp:N` succeeds whether
    // or not the device-side abstract socket has been bound yet —
    // we'd notice only when the TCP connection got RST mid-handshake,
    // which is hard to distinguish from "broker disconnected normally".
    // Cheaper to ask up front.
    if !app_running(serial.as_deref()) {
        eprintln!("tawc-exec: app process down, starting MainActivity...");
        let mut start = Command::new("adb");
        if let Some(s) = serial.as_deref() { start.args(["-s", s]); }
        start.args(["shell", "am", "start", "-n", "me.phie.tawc/.MainActivity"]);
        start.stdout(Stdio::null()).stderr(Stdio::null());
        let _ = start.status();
        // Wait up to ~10s for Application.onCreate -> ExecBroker.start
        // to actually bind the abstract socket.
        let mut waited = 0;
        while waited < 50 {
            std::thread::sleep(std::time::Duration::from_millis(200));
            if app_running(serial.as_deref()) { break; }
            waited += 1;
        }
        if !app_running(serial.as_deref()) {
            return Err(io::Error::other(
                "app process didn't come up; is the debug APK installed?"));
        }
        // Once pidof reports it, Application.onCreate has launched but
        // ExecBroker.start spawns the listener thread asynchronously —
        // give it a moment to actually bind the abstract socket. ~500ms
        // is generous; the bind itself is microseconds.
        std::thread::sleep(std::time::Duration::from_millis(500));
    }

    let mut sock = TcpStream::connect(("127.0.0.1", port))?;
    sock.set_nodelay(true)?;

    write_header(&mut sock, &p)?;
    // Make sure the header lands as a single TCP segment before any
    // frame bytes follow. flush() doesn't actually push past Nagle on
    // its own, but TCP_NODELAY above + a tiny header means the kernel
    // sends it now.
    sock.flush()?;

    let exit = pump(sock)?;
    drop(fwd);
    Ok(exit)
}

fn write_header(s: &mut TcpStream, p: &Parsed) -> io::Result<()> {
    let mut h = String::new();
    h.push_str("TAWCEXEC 1\n");
    match p {
        Parsed::Exec { argv, env, cwd, op_title } => {
            for a in argv { h.push_str("ARGV "); h.push_str(a); h.push('\n'); }
            for e in env  { h.push_str("ENV ");  h.push_str(e); h.push('\n'); }
            if let Some(c) = cwd { h.push_str("CWD "); h.push_str(c); h.push('\n'); }
            if let Some(t) = op_title { h.push_str("OP_TITLE "); h.push_str(t); h.push('\n'); }
        }
        Parsed::Action { name, args } => {
            h.push_str("ACTION "); h.push_str(name); h.push('\n');
            for (k, v) in args {
                h.push_str("ARG ");
                h.push_str(k);
                h.push('=');
                h.push_str(v);
                h.push('\n');
            }
        }
        Parsed::RunInside { install_id, cmd, op_title } => {
            h.push_str("RUNINSIDE "); h.push_str(install_id); h.push('\n');
            // Empty cmd means interactive shell — omit the CMD line.
            if !cmd.is_empty() {
                h.push_str("CMD "); h.push_str(cmd); h.push('\n');
            }
            if let Some(t) = op_title { h.push_str("OP_TITLE "); h.push_str(t); h.push('\n'); }
        }
    }
    h.push('\n');
    s.write_all(h.as_bytes())
}

/// Stream local stdio against the socket, returning the broker's
/// reported exit code. -1 if the broker errored out before exit, -2 if
/// the socket closed without any exit frame.
fn pump(sock: TcpStream) -> io::Result<i32> {
    let alive = Arc::new(AtomicBool::new(true));

    // stdin → frame stream
    let stdin_sock = sock.try_clone()?;
    let stdin_alive = alive.clone();
    let stdin_thread = thread::spawn(move || -> io::Result<()> {
        let mut buf = [0u8; 4096];
        let mut s = stdin_sock;
        let mut stdin = io::stdin().lock();
        loop {
            if !stdin_alive.load(Ordering::Relaxed) { break; }
            let n = match stdin.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            };
            // Single write of header+payload to keep frames atomic on
            // the wire — synchronizing with the read side isn't needed
            // because TCP is bytewise FIFO.
            let mut frame = Vec::with_capacity(5 + n);
            frame.push(STREAM_STDIN);
            frame.extend_from_slice(&(n as u32).to_be_bytes());
            frame.extend_from_slice(&buf[..n]);
            if s.write_all(&frame).is_err() { break; }
        }
        // Send stdin EOF frame; ignore if socket is gone.
        let eof = [STREAM_STDIN_EOF, 0, 0, 0, 0];
        let _ = s.write_all(&eof);
        Ok(())
    });

    // socket → stdout/stderr/exit
    let mut s = sock;
    let mut exit_code: i32 = -2;
    let mut stdout = io::stdout().lock();
    let mut stderr = io::stderr().lock();
    'recv: loop {
        let mut hdr = [0u8; 5];
        match read_exact(&mut s, &mut hdr) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e),
        }
        let stream = hdr[0];
        let len = u32::from_be_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
        if len > 16 * 1024 * 1024 {
            return Err(io::Error::new(io::ErrorKind::InvalidData,
                format!("frame too large: stream={stream} len={len}")));
        }
        let mut payload = vec![0u8; len];
        read_exact(&mut s, &mut payload)?;
        match stream {
            STREAM_STDOUT => { stdout.write_all(&payload)?; stdout.flush()?; }
            STREAM_STDERR => { stderr.write_all(&payload)?; stderr.flush()?; }
            STREAM_ERR => {
                eprintln!("tawc-exec: broker error: {}",
                    String::from_utf8_lossy(&payload));
            }
            STREAM_EXIT => {
                if payload.len() != 4 {
                    return Err(io::Error::new(io::ErrorKind::InvalidData,
                        format!("exit frame payload len={}", payload.len())));
                }
                exit_code = i32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                break 'recv;
            }
            other => {
                return Err(io::Error::new(io::ErrorKind::InvalidData,
                    format!("unexpected server stream {other}")));
            }
        }
    }
    alive.store(false, Ordering::Relaxed);
    // Don't wait forever for stdin to drain — once we got an exit, the
    // child is gone and stdin is moot.
    drop(stdin_thread);
    Ok(exit_code)
}

/// True if the tawc app process is alive on the device. Uses `pidof`
/// over plain `adb shell` (no privilege needed — pidof walks /proc).
fn app_running(serial: Option<&str>) -> bool {
    let mut cmd = Command::new("adb");
    if let Some(s) = serial { cmd.args(["-s", s]); }
    cmd.args(["shell", "pidof", "me.phie.tawc"]);
    cmd.output()
        .map(|o| o.stdout.iter().any(|b| b.is_ascii_digit()))
        .unwrap_or(false)
}

fn read_exact(s: &mut TcpStream, buf: &mut [u8]) -> io::Result<()> {
    let mut filled = 0;
    while filled < buf.len() {
        match s.read(&mut buf[filled..]) {
            Ok(0) => {
                if filled == 0 {
                    return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "eof at frame boundary"));
                }
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "eof mid-frame"));
            }
            Ok(n) => filled += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// RAII wrapper for `adb forward`. Removes the forward on `Drop`, so
/// normal exit + panics + `Result<_, ?>` early returns all clean up.
/// **Caveat**: `Drop` doesn't run on signal-driven exits (SIGINT /
/// SIGKILL / etc.) — those leak the forward until adbd restarts. Not
/// worth a signal handler for a dev tool; the leftover forward is
/// harmless and `adb forward --remove-all` clears it.
struct AdbForward {
    serial: Option<String>,
    port: u16,
}

impl AdbForward {
    fn start(serial: Option<&str>, port: u16) -> io::Result<Self> {
        let mut cmd = Command::new("adb");
        if let Some(s) = serial { cmd.args(["-s", s]); }
        cmd.args([
            "forward",
            &format!("tcp:{port}"),
            &format!("localabstract:{SOCKET_NAME}"),
        ]);
        cmd.stdout(Stdio::null()).stderr(Stdio::piped());
        let out = cmd.output()?;
        if !out.status.success() {
            return Err(io::Error::other(format!(
                "adb forward failed: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            )));
        }
        Ok(AdbForward { serial: serial.map(String::from), port })
    }
}

impl Drop for AdbForward {
    fn drop(&mut self) {
        let mut cmd = Command::new("adb");
        if let Some(s) = &self.serial { cmd.args(["-s", s]); }
        cmd.args(["forward", "--remove", &format!("tcp:{}", self.port)]);
        let _ = cmd.stdout(Stdio::null()).stderr(Stdio::null()).status();
    }
}

/// Map the broker's i32 exit code into a process exit code.
/// >=0: normal exit, take the low 8 bits. <0: signal; mimic shell's
/// 128 + signum so callers can detect.
fn map_exit(code: i32) -> ExitCode {
    if code < 0 {
        let n = (-code) as u8;
        ExitCode::from(128u8.saturating_add(n))
    } else {
        ExitCode::from((code as u32 & 0xff) as u8)
    }
}
