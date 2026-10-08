//! Host-side driver for the in-app exec broker.
//!
//! Picks a free local TCP port, sets up `adb forward` to the device-side
//! `LocalServerSocket`, sends the protocol header, and multiplexes local
//! stdio over the socket. Exit code is the child's exit code.
//!
//! Wire protocol: notes/exec-broker.md.

use std::env;
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::os::unix::net::UnixStream;
#[cfg(unix)]
use std::os::unix::process::ExitStatusExt;
use std::process::{Command, ExitCode, Output, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

/// Broker socket: `<app package>.exec`, matching the app's ExecBroker.
fn socket_name() -> String {
    format!("{}.exec", crate::app_package())
}

const STREAM_STDIN: u8 = 0;
const STREAM_STDOUT: u8 = 1;
const STREAM_STDERR: u8 = 2;
const STREAM_EXIT: u8 = 3;
const STREAM_STDIN_EOF: u8 = 4;
const STREAM_ERR: u8 = 5;

pub fn print_usage() {
    eprintln!("usage: tawc-exec [--foreground-app] [--cwd DIR] [--env K=V ...] [--op-title TITLE] -- ARGV0 [ARG ...]");
    eprintln!("       tawc-exec [--foreground-app] --action NAME [--arg K=V ...]");
    eprintln!("       tawc-exec [--foreground-app] --in-rootfs ID [--graphics libhybris|gfxstream|cpu|libhybris-zink] [--op-title TITLE] [-- CMD ...]");
}

/// Top-level invocation kind. Mirrors the wire protocol: an ARGV-form
/// header for fork-exec, an ACTION-form header for an in-process
/// broker action, or a RUNINSIDE-form header for chroot dispatch.
/// Mutually exclusive — `parse_args` rejects mixes.
///
/// `op_title` (Exec / RunInside): when present, the broker mirrors
/// process stdio into an in-app log screen titled with this string.
/// `--op-title` on the host CLI controls it.
pub enum Request {
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
    /// interactive `bash -l`. `graphics` non-empty overrides the
    /// in-rootfs `GraphicsBackend` for this spawn (libhybris / gfxstream
    /// / cpu / libhybris-zink) without touching the user's persisted Settings pick;
    /// empty means "use Settings". Tests use this to run a single
    /// client under a specific backend.
    RunInside {
        install_id: String,
        cmd: String,
        op_title: Option<String>,
        graphics: Option<String>,
    },
}

pub struct Invocation {
    pub foreground_app: bool,
    pub request: Request,
}

pub fn parse_args(args: &[String]) -> Result<Invocation, String> {
    let mut env = Vec::new();
    let mut cwd: Option<String> = None;
    let mut action_name: Option<String> = None;
    let mut action_args: Vec<(String, String)> = Vec::new();
    let mut run_inside_id: Option<String> = None;
    let mut run_inside_graphics: Option<String> = None;
    let mut op_title: Option<String> = None;
    let mut foreground_app = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--" => {
                i += 1;
                break;
            }
            "--foreground-app" => {
                foreground_app = true;
                i += 1;
            }
            "--env" => {
                i += 1;
                let v = args.get(i).ok_or("--env needs argument")?;
                if !v.contains('=') {
                    return Err(format!("--env value must be K=V (got '{v}')"));
                }
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
                if v.is_empty() {
                    return Err("--action name must not be empty".into());
                }
                action_name = Some(v);
                i += 1;
            }
            "--arg" => {
                i += 1;
                let v = args.get(i).ok_or("--arg needs argument")?;
                let eq = v
                    .find('=')
                    .ok_or_else(|| format!("--arg value must be key=value (got '{v}')"))?;
                action_args.push((v[..eq].to_string(), v[eq + 1..].to_string()));
                i += 1;
            }
            "--in-rootfs" => {
                i += 1;
                let v = args.get(i).ok_or("--in-rootfs needs install id")?.clone();
                if v.is_empty() {
                    return Err("--in-rootfs id must not be empty".into());
                }
                run_inside_id = Some(v);
                i += 1;
            }
            "--graphics" => {
                i += 1;
                let v = args.get(i).ok_or("--graphics needs a backend key")?.clone();
                if v.is_empty() {
                    return Err("--graphics key must not be empty".into());
                }
                // Validation against the GraphicsBackend enum happens
                // device-side in ExecBrokerSession; we just forward the
                // string. Avoids duplicating the enum on the host.
                run_inside_graphics = Some(v);
                i += 1;
            }
            "--op-title" => {
                i += 1;
                let v = args.get(i).ok_or("--op-title needs argument")?.clone();
                if v.is_empty() {
                    return Err("--op-title must not be empty".into());
                }
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
        return Ok(Invocation {
            foreground_app,
            request: Request::RunInside {
                install_id: id,
                cmd,
                op_title,
                graphics: run_inside_graphics,
            },
        });
    }
    if run_inside_graphics.is_some() {
        return Err("--graphics is only valid with --in-rootfs".into());
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
        return Ok(Invocation {
            foreground_app,
            request: Request::Action {
                name,
                args: action_args,
            },
        });
    }
    // ARGV form. --action / --arg must not be present.
    if !action_args.is_empty() {
        return Err("--arg is for action mode; missing --action".into());
    }
    let argv: Vec<String> = args[i..].to_vec();
    if argv.is_empty() {
        return Err(
            "no command (use `-- ARGV0 ...`, `--action NAME`, or `--in-rootfs ID -- CMD`)"
                .to_string(),
        );
    }
    Ok(Invocation {
        foreground_app,
        request: Request::Exec {
            argv,
            env,
            cwd,
            op_title,
        },
    })
}

pub fn run_stdio(invocation: Invocation) -> io::Result<i32> {
    let (sock, _fwd) = connect(&invocation)?;
    pump_stdio(sock)
}

pub fn run_capture(invocation: Invocation) -> io::Result<Output> {
    let (sock, _fwd) = connect(&invocation)?;
    let (code, stdout, stderr) = pump_capture(sock)?;
    Ok(Output {
        status: exit_status_from_broker(code),
        stdout,
        stderr,
    })
}

/// Like [run_capture], but feed `input` to the child's stdin first
/// (stdin frames, then EOF). Used to deliver file contents to the
/// device through the broker (`sh -c 'cat > path'`) without adb push —
/// the write lands as the app uid, so the file is app-owned.
///
/// All input is written before any output is read, so this is only for
/// children that don't produce output until stdin is drained (`cat`,
/// `tar -x`); a chatty child could fill the socket buffers and deadlock.
pub fn run_capture_with_input(invocation: Invocation, input: &[u8]) -> io::Result<Output> {
    let (mut sock, _fwd) = connect(&invocation)?;
    for chunk in input.chunks(65536) {
        let mut frame = Vec::with_capacity(5 + chunk.len());
        frame.push(STREAM_STDIN);
        frame.extend_from_slice(&(chunk.len() as u32).to_be_bytes());
        frame.extend_from_slice(chunk);
        sock.write_all(&frame)?;
    }
    // pump_capture sends the stdin EOF frame before reading.
    let (code, stdout, stderr) = pump_capture(sock)?;
    Ok(Output {
        status: exit_status_from_broker(code),
        stdout,
        stderr,
    })
}

/// Stream a binary request: `stdin` (if any) is sent as stdin frames
/// from a helper thread while stdout bytes go to `stdout` and stderr
/// chunks to `on_stderr` as they arrive. Returns the exit code and all
/// stderr. For archive-sized payloads (distro export/import) that
/// [run_capture] / [run_capture_with_input] would hold in memory.
pub fn run_streaming(
    invocation: Invocation,
    stdin: Option<Box<dyn Read + Send>>,
    stdout: &mut dyn Write,
    mut on_stderr: impl FnMut(&[u8]),
) -> io::Result<(i32, Vec<u8>)> {
    let (sock, _fwd) = connect(&invocation)?;
    let mut tx = sock.try_clone()?;
    let feeder = thread::spawn(move || {
        if let Some(mut input) = stdin {
            let mut buf = vec![0u8; 65536];
            loop {
                let n = match input.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => n,
                };
                let mut frame = Vec::with_capacity(5 + n);
                frame.push(STREAM_STDIN);
                frame.extend_from_slice(&(n as u32).to_be_bytes());
                frame.extend_from_slice(&buf[..n]);
                if tx.write_all(&frame).is_err() {
                    return;
                }
            }
        }
        let _ = tx.write_all(&[STREAM_STDIN_EOF, 0, 0, 0, 0]);
    });
    let mut s = sock;
    let mut stderr = Vec::new();
    let mut exit_code = -2;
    loop {
        let mut hdr = [0u8; 5];
        match read_exact(&mut s, &mut hdr) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e),
        }
        let len = u32::from_be_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
        if len > 16 * 1024 * 1024 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, format!("frame too large: {len}")));
        }
        let mut payload = vec![0u8; len];
        read_exact(&mut s, &mut payload)?;
        match hdr[0] {
            STREAM_STDOUT => stdout.write_all(&payload)?,
            STREAM_STDERR | STREAM_ERR => {
                on_stderr(&payload);
                stderr.extend_from_slice(&payload);
            }
            STREAM_EXIT if payload.len() == 4 => {
                exit_code = i32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                break;
            }
            other => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unexpected server stream {other}"),
                ))
            }
        }
    }
    stdout.flush()?;
    // The feeder may be blocked on a socket write if the action quit
    // reading; shutting the socket down unblocks it.
    let _ = s.shutdown(Shutdown::Both);
    let _ = feeder.join();
    Ok((exit_code, stderr))
}

pub type BrokerPipe = UnixStream;

pub struct BrokerChild {
    stdout: Option<BrokerPipe>,
    stderr: Option<BrokerPipe>,
    control: Option<TcpStream>,
    exit_rx: mpsc::Receiver<io::Result<i32>>,
    reader: Option<thread::JoinHandle<()>>,
    cancel: Arc<AtomicBool>,
    _fwd: Option<AdbForward>,
}

impl BrokerChild {
    pub fn take_stdout(&mut self) -> Option<BrokerPipe> {
        self.stdout.take()
    }

    pub fn take_stderr(&mut self) -> Option<BrokerPipe> {
        self.stderr.take()
    }

    pub fn kill(&mut self) -> io::Result<()> {
        self.cancel.store(true, Ordering::Relaxed);
        if let Some(control) = self.control.take() {
            control.shutdown(Shutdown::Both)
        } else {
            Ok(())
        }
    }

    pub fn wait(&mut self) -> io::Result<std::process::ExitStatus> {
        let code = self
            .exit_rx
            .recv()
            .map_err(|_| io::Error::other("broker session ended without an exit status"))??;
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
        Ok(exit_status_from_broker(code))
    }

    pub fn wait_timeout(
        &mut self,
        timeout: Duration,
    ) -> io::Result<Option<std::process::ExitStatus>> {
        match self.exit_rx.recv_timeout(timeout) {
            Ok(code) => {
                let code = code?;
                if let Some(reader) = self.reader.take() {
                    let _ = reader.join();
                }
                Ok(Some(exit_status_from_broker(code)))
            }
            Err(mpsc::RecvTimeoutError::Timeout) => Ok(None),
            Err(mpsc::RecvTimeoutError::Disconnected) => Err(io::Error::other(
                "broker session ended without an exit status",
            )),
        }
    }

    pub fn try_wait(&mut self) -> io::Result<Option<std::process::ExitStatus>> {
        match self.exit_rx.try_recv() {
            Ok(code) => {
                let code = code?;
                if let Some(reader) = self.reader.take() {
                    let _ = reader.join();
                }
                Ok(Some(exit_status_from_broker(code)))
            }
            Err(mpsc::TryRecvError::Empty) => Ok(None),
            Err(mpsc::TryRecvError::Disconnected) => Err(io::Error::other(
                "broker session ended without an exit status",
            )),
        }
    }
}

pub fn spawn(invocation: Invocation) -> io::Result<BrokerChild> {
    let (mut sock, fwd) = connect(&invocation)?;
    let control = sock.try_clone()?;
    sock.set_read_timeout(Some(std::time::Duration::from_millis(50)))?;
    let (stdout_read, stdout_write) = UnixStream::pair()?;
    let (stderr_read, stderr_write) = UnixStream::pair()?;
    let (exit_tx, exit_rx) = mpsc::channel();
    let cancel = Arc::new(AtomicBool::new(false));
    let reader_cancel = cancel.clone();

    let eof = [STREAM_STDIN_EOF, 0, 0, 0, 0];
    let _ = sock.write_all(&eof);

    let reader = thread::spawn(move || {
        let result = pump_pipes(sock, stdout_write, stderr_write, reader_cancel);
        let _ = exit_tx.send(result);
    });

    Ok(BrokerChild {
        stdout: Some(stdout_read),
        stderr: Some(stderr_read),
        control: Some(control),
        exit_rx,
        reader: Some(reader),
        cancel,
        _fwd: fwd,
    })
}

fn connect(invocation: &Invocation) -> io::Result<(TcpStream, Option<AdbForward>)> {
    let serial = env::var("ANDROID_SERIAL").ok();

    // Suite mode: scripts/run-integration-tests.sh started the app,
    // owns the forward, and no test force-stops the app — so skip the
    // per-request pidof probe and the implicit RUNINSIDE am-start
    // entirely. An explicit foreground_app request (install/uninstall
    // helpers need the foreground-app BAL allowance) is still honored.
    // If the app died mid-suite (or the suite was run without the
    // script), fail loudly instead of papering over it with a restart.
    // Note adb accepts the host TCP connection before dialing the
    // device-side socket, so a dead app usually surfaces as a reset on
    // the header write, not at connect — wrap both.
    if let Ok(port) = env::var("TAWC_EXEC_BROKER_PORT") {
        let port = port.parse::<u16>().map_err(|e| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("invalid TAWC_EXEC_BROKER_PORT={port:?}: {e}"),
            )
        })?;
        if invocation.foreground_app {
            start_main_activity(serial.as_deref())?;
        }
        let suite_err = |e: io::Error| {
            io::Error::new(
                e.kind(),
                format!(
                    "exec broker request on suite port {port} failed: {e}; the app \
                     died mid-suite or the forward is gone — re-run \
                     scripts/run-integration-tests.sh"
                ),
            )
        };
        let mut sock = match TcpStream::connect(("127.0.0.1", port)) {
            // adb drops a device's forwards when its USB link resets;
            // the app outlives that, so put the forward back once.
            Err(e) if e.kind() == io::ErrorKind::ConnectionRefused => {
                restore_suite_forward(serial.as_deref(), port).map_err(suite_err)?;
                TcpStream::connect(("127.0.0.1", port)).map_err(suite_err)?
            }
            r => r.map_err(suite_err)?,
        };
        sock.set_nodelay(true)?;
        write_header(&mut sock, &invocation.request)
            .and_then(|()| sock.flush())
            .map_err(suite_err)?;
        return Ok((sock, None));
    }

    // CLI path (scripts/tawc-exec.sh): must keep working against a cold
    // app, so check the process and start MainActivity if needed.
    ensure_broker_ready(invocation, serial.as_deref())?;

    // Pick a free port on 127.0.0.1, drop the listener immediately so
    // adbd can bind. Race window is tiny and the port is otherwise
    // unbound; if it loses we error loudly.
    let port = {
        let l = TcpListener::bind("127.0.0.1:0")?;
        l.local_addr()?.port()
    };
    let fwd = AdbForward::start(serial.as_deref(), port)?;

    let mut sock = TcpStream::connect(("127.0.0.1", port))?;
    sock.set_nodelay(true)?;

    write_header(&mut sock, &invocation.request)?;
    // Make sure the header lands as a single TCP segment before any
    // frame bytes follow. flush() doesn't actually push past Nagle on
    // its own, but TCP_NODELAY above + a tiny header means the kernel
    // sends it now.
    sock.flush()?;

    Ok((sock, Some(fwd)))
}

/// Wait for the device to come back after a USB reset and re-add the
/// suite's forward (the run script removes it at the end).
fn restore_suite_forward(serial: Option<&str>, port: u16) -> io::Result<()> {
    eprintln!("tawc-exec: suite forward on port {port} is gone (USB reset?); restoring it");
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
    loop {
        let mut cmd = Command::new("adb");
        if let Some(s) = serial {
            cmd.args(["-s", s]);
        }
        let out = cmd.arg("get-state").stderr(Stdio::null()).output()?;
        if String::from_utf8_lossy(&out.stdout).trim() == "device" {
            break;
        }
        if std::time::Instant::now() >= deadline {
            return Err(io::Error::other("device did not come back within 30s"));
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    std::mem::forget(AdbForward::start(serial, port)?);
    Ok(())
}

fn ensure_broker_ready(invocation: &Invocation, serial: Option<&str>) -> io::Result<()> {
    // Make sure the app process is actually up before trying to use the
    // forward. `adb forward localabstract:foo tcp:N` succeeds whether
    // or not the device-side abstract socket has been bound yet —
    // we'd notice only when the TCP connection got RST mid-handshake,
    // which is hard to distinguish from "broker disconnected normally".
    // Cheaper to ask up front.
    let was_running = app_running(serial.as_deref());
    let foreground_app =
        invocation.foreground_app || matches!(invocation.request, Request::RunInside { .. });
    if foreground_app || !was_running {
        if !was_running {
            eprintln!("tawc-exec: app process down, starting MainActivity...");
        }
        start_main_activity(serial.as_deref())?;
    }
    if !was_running {
        // Wait up to ~10s for Application.onCreate -> ExecBroker.start
        // to actually bind the abstract socket.
        let mut waited = 0;
        while waited < 50 {
            std::thread::sleep(std::time::Duration::from_millis(200));
            if app_running(serial.as_deref()) {
                break;
            }
            waited += 1;
        }
        if !app_running(serial.as_deref()) {
            return Err(io::Error::other(
                "app process didn't come up; is the debug APK installed?",
            ));
        }
        // Once pidof reports it, Application.onCreate has launched but
        // ExecBroker.start spawns the listener thread asynchronously —
        // give it a moment to actually bind the abstract socket. ~500ms
        // is generous; the bind itself is microseconds.
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    Ok(())
}

fn start_main_activity(serial: Option<&str>) -> io::Result<()> {
    // One retry with a short pause: a single `am start` can fail
    // transiently (adb hiccup, activity manager busy right after a
    // force-stop) and a whole suite run shouldn't die on that.
    let mut last = String::new();
    for attempt in 0..2 {
        if attempt > 0 {
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        let mut start = Command::new("adb");
        if let Some(s) = serial {
            start.args(["-s", s]);
        }
        start.args(["shell", "am", "start", "-n", &crate::main_activity()]);
        let out = start.output()?;
        // `am start` exits 0 but prints `Error:` for some failures
        // (e.g. unresolvable intent); treat those as failures too.
        let text = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        if out.status.success() && !text.contains("Error") {
            return Ok(());
        }
        last = format!("{}: {}", out.status, text.trim());
    }
    Err(io::Error::other(format!(
        "failed to start MainActivity (2 attempts): {last}"
    )))
}

fn write_header(s: &mut TcpStream, p: &Request) -> io::Result<()> {
    let mut h = String::new();
    h.push_str("TAWCEXEC 1\n");
    // Action and install id are programmatic identifiers — registered
    // names / `[a-z0-9_-]{1,32}` slugs — so no encoding is needed and
    // no LF can sneak in. Every other value is potentially user-supplied
    // and goes through [encode_value] so a `\n` doesn't terminate the
    // header line early. The device side decodes in [decodeValue].
    match p {
        Request::Exec {
            argv,
            env,
            cwd,
            op_title,
        } => {
            for a in argv {
                h.push_str("ARGV ");
                h.push_str(&encode_value(a));
                h.push('\n');
            }
            for e in env {
                h.push_str("ENV ");
                h.push_str(&encode_value(e));
                h.push('\n');
            }
            if let Some(c) = cwd {
                h.push_str("CWD ");
                h.push_str(&encode_value(c));
                h.push('\n');
            }
            if let Some(t) = op_title {
                h.push_str("OP_TITLE ");
                h.push_str(&encode_value(t));
                h.push('\n');
            }
        }
        Request::Action { name, args } => {
            h.push_str("ACTION ");
            h.push_str(name);
            h.push('\n');
            for (k, v) in args {
                h.push_str("ARG ");
                h.push_str(k);
                h.push('=');
                h.push_str(&encode_value(v));
                h.push('\n');
            }
        }
        Request::RunInside {
            install_id,
            cmd,
            op_title,
            graphics,
        } => {
            h.push_str("RUNINSIDE ");
            h.push_str(install_id);
            h.push('\n');
            // Empty cmd means interactive shell — omit the CMD line.
            if !cmd.is_empty() {
                h.push_str("CMD ");
                h.push_str(&encode_value(cmd));
                h.push('\n');
            }
            // GRAPHICS key is a programmatic identifier (libhybris /
            // gfxstream / cpu / libhybris-zink); no encoding needed.
            if let Some(g) = graphics {
                h.push_str("GRAPHICS ");
                h.push_str(g);
                h.push('\n');
            }
            if let Some(t) = op_title {
                h.push_str("OP_TITLE ");
                h.push_str(&encode_value(t));
                h.push('\n');
            }
        }
    }
    h.push('\n');
    s.write_all(h.as_bytes())
}

/// Encode a header-line value so it survives the LF-terminated header.
/// Applied uniformly to every value-bearing field (ARGV / ENV / CWD /
/// ARG / CMD / OP_TITLE) so a literal `\n` in any of them — tests
/// inject Enter via `ic-commit-text` with `text="\n"`, shell glue
/// passes `--env FOO=bar\nbaz`, etc. — doesn't end the line early.
/// Mirror in `ExecBrokerSession.kt::decodeValue`.
///
/// Encoding (small, reversible):
///   `\\` -> `\\\\`  (escape the escape char first)
///   `\n` -> `\\n`
///   `\r` -> `\\r`
///
/// No-op for any value that contains none of those three chars, so
/// normal text passes through unchanged.
fn encode_value(v: &str) -> String {
    let mut out = String::with_capacity(v.len());
    for c in v.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            _ => out.push(c),
        }
    }
    out
}

/// Stream local stdio against the socket, returning the broker's
/// reported exit code. -1 if the broker errored out before exit, -2 if
/// the socket closed without any exit frame.
fn pump_stdio(sock: TcpStream) -> io::Result<i32> {
    let alive = Arc::new(AtomicBool::new(true));

    // stdin → frame stream
    let stdin_sock = sock.try_clone()?;
    let stdin_alive = alive.clone();
    let stdin_thread = thread::spawn(move || -> io::Result<()> {
        let mut buf = [0u8; 4096];
        let mut s = stdin_sock;
        let mut stdin = io::stdin().lock();
        loop {
            if !stdin_alive.load(Ordering::Relaxed) {
                break;
            }
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
            if s.write_all(&frame).is_err() {
                break;
            }
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
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("frame too large: stream={stream} len={len}"),
            ));
        }
        let mut payload = vec![0u8; len];
        read_exact(&mut s, &mut payload)?;
        match stream {
            STREAM_STDOUT => {
                stdout.write_all(&payload)?;
                stdout.flush()?;
            }
            STREAM_STDERR => {
                stderr.write_all(&payload)?;
                stderr.flush()?;
            }
            STREAM_ERR => {
                eprintln!(
                    "tawc-exec: broker error: {}",
                    String::from_utf8_lossy(&payload)
                );
            }
            STREAM_EXIT => {
                if payload.len() != 4 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("exit frame payload len={}", payload.len()),
                    ));
                }
                exit_code = i32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                break 'recv;
            }
            other => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unexpected server stream {other}"),
                ));
            }
        }
    }
    alive.store(false, Ordering::Relaxed);
    // Don't wait forever for stdin to drain — once we got an exit, the
    // child is gone and stdin is moot.
    drop(stdin_thread);
    Ok(exit_code)
}

/// Run a non-interactive broker request and collect stdout/stderr.
fn pump_capture(mut sock: TcpStream) -> io::Result<(i32, Vec<u8>, Vec<u8>)> {
    let eof = [STREAM_STDIN_EOF, 0, 0, 0, 0];
    let _ = sock.write_all(&eof);

    let mut exit_code: i32 = -2;
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    'recv: loop {
        let mut hdr = [0u8; 5];
        match read_exact(&mut sock, &mut hdr) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e),
        }
        let stream = hdr[0];
        let len = u32::from_be_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
        if len > 16 * 1024 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("frame too large: stream={stream} len={len}"),
            ));
        }
        let mut payload = vec![0u8; len];
        read_exact(&mut sock, &mut payload)?;
        match stream {
            STREAM_STDOUT => stdout.extend_from_slice(&payload),
            STREAM_STDERR | STREAM_ERR => stderr.extend_from_slice(&payload),
            STREAM_EXIT => {
                if payload.len() != 4 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("exit frame payload len={}", payload.len()),
                    ));
                }
                exit_code = i32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                break 'recv;
            }
            other => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unexpected server stream {other}"),
                ));
            }
        }
    }
    Ok((exit_code, stdout, stderr))
}

fn pump_pipes(
    mut sock: TcpStream,
    mut stdout: UnixStream,
    mut stderr: UnixStream,
    cancel: Arc<AtomicBool>,
) -> io::Result<i32> {
    let mut exit_code: i32 = -2;
    'recv: loop {
        if cancel.load(Ordering::Relaxed) {
            return Ok(-1);
        }
        let mut hdr = [0u8; 5];
        match read_exact_cancelable(&mut sock, &mut hdr, &cancel) {
            Ok(()) => {}
            // Cancel first: kill() shuts the socket down, which can
            // surface as EOF, ECONNRESET, or the Interrupted marker
            // depending on where the read was — all mean "cancelled".
            Err(_) if cancel.load(Ordering::Relaxed) => return Ok(-1),
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e),
        }
        let stream = hdr[0];
        let len = u32::from_be_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
        if len > 16 * 1024 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("frame too large: stream={stream} len={len}"),
            ));
        }
        let mut payload = vec![0u8; len];
        match read_exact_cancelable(&mut sock, &mut payload, &cancel) {
            Ok(()) => {}
            // kill() can land mid-payload (chatty children make this
            // likely): both the cancel flag and the socket shutdown it
            // triggers must count as a normal cancel, not an error.
            Err(_) if cancel.load(Ordering::Relaxed) => return Ok(-1),
            Err(e) => return Err(e),
        }
        match stream {
            STREAM_STDOUT => stdout.write_all(&payload)?,
            STREAM_STDERR | STREAM_ERR => stderr.write_all(&payload)?,
            STREAM_EXIT => {
                if payload.len() != 4 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("exit frame payload len={}", payload.len()),
                    ));
                }
                exit_code = i32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                break 'recv;
            }
            other => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unexpected server stream {other}"),
                ));
            }
        }
    }
    Ok(exit_code)
}

fn read_exact_cancelable(
    s: &mut TcpStream,
    buf: &mut [u8],
    cancel: &AtomicBool,
) -> io::Result<()> {
    let mut filled = 0;
    while filled < buf.len() {
        if cancel.load(Ordering::Relaxed) {
            return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled"));
        }
        match s.read(&mut buf[filled..]) {
            Ok(0) => {
                if filled == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "eof at frame boundary",
                    ));
                }
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "eof mid-frame",
                ));
            }
            Ok(n) => filled += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut =>
            {
                continue
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// True if the TAWC app process is alive on the device. Uses `pidof`
/// over plain `adb shell` (no privilege needed — pidof walks /proc).
fn app_running(serial: Option<&str>) -> bool {
    let mut cmd = Command::new("adb");
    if let Some(s) = serial {
        cmd.args(["-s", s]);
    }
    cmd.args(["shell", "pidof", &crate::app_package()]);
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
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "eof at frame boundary",
                    ));
                }
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "eof mid-frame",
                ));
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
        if let Some(s) = serial {
            cmd.args(["-s", s]);
        }
        cmd.args([
            "forward",
            &format!("tcp:{port}"),
            &format!("localabstract:{}", socket_name()),
        ]);
        cmd.stdout(Stdio::null()).stderr(Stdio::piped());
        let out = cmd.output()?;
        if !out.status.success() {
            return Err(io::Error::other(format!(
                "adb forward failed: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            )));
        }
        Ok(AdbForward {
            serial: serial.map(String::from),
            port,
        })
    }
}

impl Drop for AdbForward {
    fn drop(&mut self) {
        let mut cmd = Command::new("adb");
        if let Some(s) = &self.serial {
            cmd.args(["-s", s]);
        }
        cmd.args(["forward", "--remove", &format!("tcp:{}", self.port)]);
        let _ = cmd.stdout(Stdio::null()).stderr(Stdio::null()).status();
    }
}

/// Map the broker's i32 exit code into a process exit code.
/// >=0: normal exit, take the low 8 bits. <0: signal; mimic shell's
/// 128 + signum so callers can detect.
fn exit_status_from_broker(code: i32) -> std::process::ExitStatus {
    #[cfg(unix)]
    {
        if code < 0 {
            std::process::ExitStatus::from_raw(-code)
        } else {
            std::process::ExitStatus::from_raw((code as i32 & 0xff) << 8)
        }
    }
    #[cfg(not(unix))]
    {
        let _ = code;
        unimplemented!("TAWC integration tests require a Unix host")
    }
}

pub fn map_exit(code: i32) -> ExitCode {
    if code < 0 {
        let n = (-code) as u8;
        ExitCode::from(128u8.saturating_add(n))
    } else {
        ExitCode::from((code as u32 & 0xff) as u8)
    }
}
