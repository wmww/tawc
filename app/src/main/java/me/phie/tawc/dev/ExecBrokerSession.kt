package me.phie.tawc.dev

import android.net.LocalSocket
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * One accepted broker connection. Reads the header, spawns the child
 * with [ProcessBuilder], then runs three relay threads that move bytes
 * between the socket and the child's stdio. Closing the socket from
 * the host side cancels the child via SIGKILL.
 *
 * Protocol details: notes/exec-broker.md.
 */
internal class ExecBrokerSession(private val socket: LocalSocket) {

    private sealed class Request {
        /**
         * `opTitle` is the optional `OP_TITLE` header — when non-null,
         * the broker mirrors the process's stdio into a
         * [me.phie.tawc.ops.Operation] surfaced through
         * [me.phie.tawc.ops.LogScreenActivity] (in addition to streaming
         * back to the host as usual). See [BrokerOpMirror].
         */
        data class Exec(
            val argv: List<String>,
            val env: List<Pair<String, String>>?,
            val cwd: String?,
            val opTitle: String?,
        ) : Request()

        data class Action(
            val name: String,
            val args: Map<String, String>,
        ) : Request()

        /**
         * Run a command inside an installed chroot. The broker reads
         * the install's recorded method from `metadata.json`,
         * dispatches to [InstallationMethod.startInside], and streams
         * stdio back exactly like [Exec]. Single entry point for every
         * "enter the chroot" path — replaces the prior on-disk
         * `enter.sh` + ARGV-form Exec dance.
         *
         * `cmd == null` is interactive `bash -l`. `opTitle` semantics
         * match [Exec.opTitle].
         */
        data class RunInside(
            val installId: String,
            val cmd: String?,
            val opTitle: String?,
        ) : Request()
    }

    fun run() {
        val rawIn = socket.inputStream
        val sout = DataOutputStream(socket.outputStream)
        // Read the header byte-at-a-time so we don't over-buffer into the
        // first frame. After readHeader returns, the next byte on rawIn
        // is the start of the first frame.
        val req = try {
            readHeader(rawIn)
        } catch (t: Throwable) {
            sendErrorAndExit(sout, "header parse: ${t.message}")
            return
        }
        val sin = DataInputStream(rawIn)
        when (req) {
            is Request.Exec -> {
                Log.i(ExecBroker.TAG, "exec argv=${req.argv} cwd=${req.cwd}")
                runExec(req, sin, sout)
            }
            is Request.Action -> {
                Log.i(ExecBroker.TAG, "action name=${req.name} args=${req.args.keys}")
                runAction(req, sin, sout)
            }
            is Request.RunInside -> {
                Log.i(ExecBroker.TAG, "runInside install=${req.installId} cmd=${req.cmd?.take(64)}")
                runInside(req, sin, sout)
            }
        }
    }

    /**
     * Resolve the install's method via metadata.json and start the
     * subprocess via [InstallationMethod.startInside]. The streaming /
     * cancel / waitFor scaffolding is shared with [runExec] via
     * [streamProcess] — only the spawn primitive differs.
     */
    private fun runInside(req: Request.RunInside, sin: DataInputStream, sout: DataOutputStream) {
        val app = ExecBroker.appContext
        val store = me.phie.tawc.install.InstallationStore(app)
        val inst = store.load(req.installId)
        if (inst == null) {
            sendErrorAndExit(sout, "no install '${req.installId}' (state file missing)")
            return
        }
        val method = me.phie.tawc.install.InstallationMethod.forKey(app, inst.method)
        if (method == null) {
            sendErrorAndExit(sout, "unknown method '${inst.method}' for install '${req.installId}'")
            return
        }
        val rootfs = store.rootfsDir(req.installId).absolutePath
        val proc: Process = try {
            method.startInside(rootfs, req.cmd)
        } catch (t: Throwable) {
            sendErrorAndExit(sout, "startInside: ${t.javaClass.simpleName}: ${t.message}")
            return
        }
        val mirror = req.opTitle?.let { title ->
            BrokerOpMirror.create(app, title).also {
                it.start("[${req.installId}] ${req.cmd ?: "(interactive shell)"}")
            }
        }
        streamProcess(proc, sin, sout, mirror)
    }

    /**
     * Run an in-process broker action: look up the handler in
     * [ActionRegistry], hand it an [ActionContext] backed by frame
     * writers, watch the socket for host disconnect to set the
     * cancelFlag, send the broker-style EXIT frame on completion.
     */
    private fun runAction(req: Request.Action, sin: DataInputStream, sout: DataOutputStream) {
        val handler = ActionRegistry.get(req.name)
        if (handler == null) {
            val available = ActionRegistry.names().joinToString(",")
            sendErrorAndExit(sout, "unknown action '${req.name}' (available: $available)")
            return
        }

        val cancelFlag = AtomicBoolean(false)
        // Watch the socket for host EOF — any read returning EOF (or any
        // frame at all, which would be a protocol violation since the
        // action protocol doesn't accept client → server frames) means
        // the host disconnected. We set cancelFlag so the handler can
        // bail at its next poll point. The thread itself exits when the
        // outer accept-loop in [ExecBroker] closes the per-session
        // socket in its `finally`.
        thread(name = "tawc-exec-action-watch", isDaemon = true) {
            try {
                while (true) {
                    val b = sin.read()
                    if (b == -1) break          // socket EOF — host gone
                    // Drain any framing the host sends; protocol says
                    // there should be none for actions, but reading
                    // keeps the buffer from filling and lets us notice
                    // EOF promptly.
                    val len = try { sin.readInt() } catch (_: Throwable) { break }
                    if (len < 0 || len > MAX_FRAME) break
                    // `readFully` is API 1+; `readNBytes` would be API 33.
                    if (len > 0) {
                        val drain = ByteArray(len)
                        try { sin.readFully(drain) } catch (_: Throwable) { break }
                    }
                }
            } catch (_: Throwable) { /* fall through */ }
            cancelFlag.set(true)
        }

        val app = ExecBroker.appContext
        val ctx = ActionContext(
            appContext = app,
            out = { line -> writeStreamFrame(sout, STREAM_STDOUT, "$line\n".toByteArray(Charsets.UTF_8)) },
            err = { line -> writeStreamFrame(sout, STREAM_STDERR, "$line\n".toByteArray(Charsets.UTF_8)) },
            cancelFlag = cancelFlag,
        )

        val exit = try {
            handler.run(req.args, ctx)
        } catch (t: Throwable) {
            Log.w(ExecBroker.TAG, "action '${req.name}' threw", t)
            try {
                writeStreamFrame(sout, STREAM_ERR,
                    "action threw: ${t.javaClass.simpleName}: ${t.message ?: ""}".toByteArray(Charsets.UTF_8))
            } catch (_: Throwable) {}
            -1
        }

        try {
            synchronized(sout) {
                sout.write(STREAM_EXIT)
                sout.writeInt(4)
                sout.writeInt(exit)
                sout.flush()
            }
        } catch (_: IOException) { /* host gone */ }

        // The watcher is parked on `sin.read()`, which is a blocking FD
        // I/O call that does NOT respond to Thread.interrupt(). The
        // outer accept-loop in [ExecBroker] closes `client` in its
        // per-session `finally`, which unblocks the read with EOF and
        // lets the daemon thread exit naturally. We don't need to wait
        // for that here — the EXIT frame above is the host-visible
        // completion signal.
    }

    private fun runExec(req: Request.Exec, sin: DataInputStream, sout: DataOutputStream) {
        val pb = ProcessBuilder(req.argv)
            .redirectErrorStream(false)
        // Always clear and rebuild env from the header — never leak the
        // broker's own (app-process) environment to children. Callers
        // pass every var they want explicitly. Matches the protocol
        // doc's "ENV replaces, doesn't merge" guarantee.
        pb.environment().clear()
        if (req.env != null) {
            for ((k, v) in req.env) pb.environment()[k] = v
        }
        if (req.cwd != null) pb.directory(File(req.cwd))

        val proc: Process = try {
            pb.start()
        } catch (t: Throwable) {
            sendErrorAndExit(sout, "spawn: ${t.message}")
            return
        }
        val mirror = req.opTitle?.let { title ->
            BrokerOpMirror.create(ExecBroker.appContext, title).also {
                it.start(req.argv.joinToString(" "))
            }
        }
        streamProcess(proc, sin, sout, mirror)
    }

    /**
     * Relay [proc]'s stdout/stderr to the socket as STREAM_STDOUT /
     * STREAM_STDERR frames, forward incoming socket frames to [proc]'s
     * stdin, send STREAM_EXIT with the exit code. Cancels the process
     * tree (proc + descendants) on host disconnect.
     *
     * Shared by [runExec] (generic argv form) and [runInside] (chroot
     * dispatch form) — they only differ in how the [Process] gets
     * spawned. When `mirror` is non-null, every chunk also tees into
     * the in-app log screen.
     */
    private fun streamProcess(
        proc: Process,
        sin: DataInputStream,
        sout: DataOutputStream,
        mirror: BrokerOpMirror?,
    ) {
        try {
            streamProcessInner(proc, sin, sout, mirror)
        } finally {
            // Best-effort idempotent finalisation. Any throw between
            // here and the structured exit path leaves the op stuck in
            // the registry forever (notification pinned, no terminal
            // stage). [BrokerOpMirror.finish] no-ops if already called
            // on the structured path, so calling it again here is safe.
            try { mirror?.finish(-1) } catch (_: Throwable) { /* swallow */ }
        }
    }

    private fun streamProcessInner(
        proc: Process,
        sin: DataInputStream,
        sout: DataOutputStream,
        mirror: BrokerOpMirror?,
    ) {
        val socketAlive = AtomicBoolean(true)

        // Pump child stdout / stderr → socket. Both threads are
        // daemons so they won't keep the JVM alive on shutdown.
        val outT = thread(name = "tawc-exec-stdout", isDaemon = true) {
            relayChildOutToSocket(proc.inputStream, STREAM_STDOUT, sout, socketAlive, mirror, true)
        }
        val errT = thread(name = "tawc-exec-stderr", isDaemon = true) {
            relayChildOutToSocket(proc.errorStream, STREAM_STDERR, sout, socketAlive, mirror, false)
        }

        // Two events race: socket EOF (host disconnected) or child
        // exit (work done). A latch lets the main thread wait for
        // whichever fires first; the loser is allowed to keep
        // running — its cleanup happens implicitly when we close the
        // socket / reap the child below.
        val finished = CountDownLatch(1)
        val hostDisconnected = AtomicBoolean(false)

        thread(name = "tawc-exec-stdin", isDaemon = true) {
            val cancelled = try {
                relaySocketToChildIn(sin, proc.outputStream)
            } catch (t: Throwable) {
                Log.w(ExecBroker.TAG, "socket→stdin error", t); true
            }
            if (cancelled) {
                hostDisconnected.set(true)
                finished.countDown()
            }
        }
        thread(name = "tawc-exec-waiter", isDaemon = true) {
            try { proc.waitFor() } catch (_: InterruptedException) {}
            finished.countDown()
        }

        // Wire the in-panel Cancel button (when present) to the same
        // kill path host disconnect uses. Unblocks `finished` so we
        // proceed to reap + send the exit frame.
        mirror?.setCancelHandler {
            if (proc.isAlive) {
                hostDisconnected.set(true)
                finished.countDown()
            }
        }

        finished.await()

        if (hostDisconnected.get() && proc.isAlive) {
            killProcessTree(proc, "cancel")
        }

        val exit = try { proc.waitFor() } catch (_: InterruptedException) { -1 }
        outT.join(2000)
        errT.join(2000)

        // Take the same lock as the stdout/stderr relays so we can't
        // interleave with a slow relay that hasn't finished its last
        // synchronized(sout) block yet (outT.join() can time out).
        try {
            synchronized(sout) {
                sout.write(STREAM_EXIT)
                sout.writeInt(4)
                sout.writeInt(exit)
                sout.flush()
            }
        } catch (_: IOException) {
            // Host already disconnected — nothing to do.
        }
        socketAlive.set(false)
        mirror?.finish(exit)
    }

    /**
     * SIGKILL [proc] + every captured descendant. Used both by the
     * host-disconnect path and by an in-panel Cancel tap. We can't
     * use `/proc/<pid>/task/<pid>/children` (requires
     * `CONFIG_PROC_CHILDREN`, missing on Android stock kernels), so
     * we walk every `/proc/<pid>/status` looking for matching `PPid:` lines —
     * see [collectDescendants]. Pids can in theory be reused across
     * the kill window, but on Android pid_max is 32k+ and this is
     * dev tooling.
     */
    private fun killProcessTree(proc: Process, reason: String) {
        val rootPid = pidOf(proc)
        val descendants = if (rootPid > 0) collectDescendants(rootPid) else emptyList()
        Log.i(ExecBroker.TAG, "$reason rootPid=$rootPid descendants=$descendants")
        proc.destroyForcibly()
        for (p in descendants) {
            try { Os.kill(p, OsConstants.SIGKILL) } catch (_: Throwable) {}
        }
    }

    /**
     * Write one stream frame (used by [runAction] to push action
     * output to the host). Synchronizes on [sout] for the same reason
     * [relayChildOutToSocket] does — multiple writers can race on
     * stdout / stderr / exit.
     */
    private fun writeStreamFrame(sout: DataOutputStream, streamId: Int, payload: ByteArray) {
        if (payload.size > MAX_FRAME) {
            // Split oversize payloads. Action handlers typically write
            // one log line at a time, well under MAX_FRAME, so this is
            // defensive.
            var off = 0
            while (off < payload.size) {
                val n = minOf(MAX_FRAME, payload.size - off)
                writeStreamFrame(sout, streamId, payload.copyOfRange(off, off + n))
                off += n
            }
            return
        }
        try {
            synchronized(sout) {
                sout.write(streamId)
                sout.writeInt(payload.size)
                sout.write(payload)
                sout.flush()
            }
        } catch (_: IOException) {
            // Host disconnected. The session's run() will observe this
            // via its socket-watcher thread; nothing we can do here.
        }
    }

    private fun sendErrorAndExit(sout: DataOutputStream, msg: String) {
        try {
            val bytes = msg.toByteArray(Charsets.UTF_8)
            sout.write(STREAM_ERR)
            sout.writeInt(bytes.size)
            sout.write(bytes)
            sout.write(STREAM_EXIT)
            sout.writeInt(4)
            sout.writeInt(-1)
            sout.flush()
        } catch (_: IOException) {}
    }

    /**
     * Read the LF-terminated header until an empty line. Reads
     * byte-at-a-time so the next byte on [stream] is the start of the
     * first binary frame — using BufferedReader here would silently
     * absorb frame bytes into its internal buffer.
     *
     * Three header shapes are accepted, mutually exclusive:
     *   - **ARGV-form** (fork-exec): one or more `ARGV <s>` lines plus
     *     optional `ENV K=V` / `CWD <p>`. The session forks the named
     *     process and relays stdio.
     *   - **ACTION-form** (in-process): one `ACTION <name>` line plus
     *     zero or more `ARG <key>=<value>` lines. The session looks
     *     [name] up in [ActionRegistry] and runs the handler in-process,
     *     streaming its output back on stdout/stderr.
     *   - **RUNINSIDE-form** (chroot dispatch): one `RUNINSIDE <id>`
     *     line plus an optional `CMD <command>` line. The session
     *     resolves the install's method, calls
     *     [InstallationMethod.startInside], and streams stdio.
     *     `CMD` absent = interactive `bash -l`.
     */
    private fun readHeader(stream: InputStream): Request {
        val firstLine = readHeaderLine(stream)
            ?: throw IOException("connection closed before header")
        if (firstLine != "TAWCEXEC 1") {
            throw IOException("bad magic: '$firstLine'")
        }
        val argv = mutableListOf<String>()
        var env: MutableList<Pair<String, String>>? = null
        var cwd: String? = null
        var actionName: String? = null
        val actionArgs = mutableMapOf<String, String>()
        var runInsideId: String? = null
        var runInsideCmd: String? = null
        var opTitle: String? = null
        while (true) {
            val line = readHeaderLine(stream) ?: throw IOException("EOF in header")
            if (line.isEmpty()) break
            val sp = line.indexOf(' ')
            val key = if (sp < 0) line else line.substring(0, sp)
            val value = if (sp < 0) "" else line.substring(sp + 1)
            when (key) {
                "ARGV" -> argv += value
                "ENV"  -> {
                    if (env == null) env = mutableListOf()
                    val eq = value.indexOf('=')
                    if (eq < 0) throw IOException("malformed ENV: '$value'")
                    env += value.substring(0, eq) to value.substring(eq + 1)
                }
                "CWD"  -> cwd = value
                "ACTION" -> {
                    if (actionName != null) throw IOException("duplicate ACTION line")
                    if (value.isEmpty()) throw IOException("ACTION needs a name")
                    actionName = value
                }
                "ARG" -> {
                    val eq = value.indexOf('=')
                    if (eq < 0) throw IOException("malformed ARG: '$value' (must be key=value)")
                    actionArgs[value.substring(0, eq)] = value.substring(eq + 1)
                }
                "RUNINSIDE" -> {
                    if (runInsideId != null) throw IOException("duplicate RUNINSIDE line")
                    if (value.isEmpty()) throw IOException("RUNINSIDE needs an install id")
                    runInsideId = value
                }
                "CMD" -> {
                    // Empty CMD value is allowed (means "no cmd, interactive
                    // shell"), so don't reject "" — just store it.
                    if (runInsideCmd != null) throw IOException("duplicate CMD line")
                    runInsideCmd = value
                }
                "OP_TITLE" -> {
                    if (opTitle != null) throw IOException("duplicate OP_TITLE line")
                    if (value.isEmpty()) throw IOException("OP_TITLE needs a non-empty title")
                    opTitle = value
                }
                else -> throw IOException("unknown header key: '$key'")
            }
        }
        val isAction = actionName != null
        val isExec = argv.isNotEmpty()
        val isRunInside = runInsideId != null
        val forms = listOf(isAction, isExec, isRunInside).count { it }
        if (forms > 1) {
            throw IOException("ARGV / ACTION / RUNINSIDE are mutually exclusive in one header")
        }
        if (isAction) {
            if (opTitle != null) throw IOException("OP_TITLE is not allowed with ACTION")
            return Request.Action(actionName!!, actionArgs)
        }
        if (isRunInside) return Request.RunInside(runInsideId!!, runInsideCmd, opTitle)
        if (!isExec) throw IOException("no ARGV / ACTION / RUNINSIDE in header")
        return Request.Exec(argv, env, cwd, opTitle)
    }

    /**
     * Extract the kernel pid of [proc]. `Process.pid()` is the public
     * Java 9+ API but it isn't exposed at the Android API surface we
     * compile against (verified: `proc.pid()` doesn't resolve on
     * compileSdk=34 / minSdk=29). The underlying `ProcessImpl` /
     * `UNIXProcess` carries a `pid` int field we can pluck off via
     * reflection. Returns -1 on any failure.
     */
    private fun pidOf(proc: Process): Int {
        return try {
            val f = proc.javaClass.getDeclaredField("pid")
            f.isAccessible = true
            f.getInt(proc)
        } catch (_: Throwable) {
            -1
        }
    }

    /**
     * Collect pids of every descendant of [rootPid] by scanning each
     * `/proc/<pid>/status` for matching `PPid:` lines. Built bottom-up:
     * walk the entire ppid map once, then BFS from root. Returns
     * leaves-first so callers can SIGKILL in topological order.
     *
     * Why not `/proc/<pid>/task/<pid>/children`? That requires kernel
     * `CONFIG_PROC_CHILDREN`, which Android's emulator (and many real
     * devices) ship without — the file doesn't exist.
     */
    private fun collectDescendants(rootPid: Int): List<Int> {
        // Build pid → list of children once.
        val children = HashMap<Int, MutableList<Int>>()
        val procDir = File("/proc")
        val entries = procDir.list() ?: return emptyList()
        for (name in entries) {
            val pid = name.toIntOrNull() ?: continue
            val ppid = readPpid(pid) ?: continue
            children.getOrPut(ppid) { mutableListOf() }.add(pid)
        }
        val out = mutableListOf<Int>()
        fun visit(pid: Int) {
            val kids = children[pid] ?: return
            for (k in kids) visit(k)
            out += kids
        }
        visit(rootPid)
        return out
    }

    private fun readPpid(pid: Int): Int? {
        val f = File("/proc/$pid/status")
        return try {
            f.useLines { lines ->
                for (line in lines) {
                    if (line.startsWith("PPid:")) {
                        return@useLines line.substringAfter(':').trim().toIntOrNull()
                    }
                }
                null
            }
        } catch (_: Throwable) { null }
    }

    /**
     * Read a UTF-8, LF-terminated line. Returns null on stream EOF.
     * Capped at MAX_FRAME bytes so a peer can't OOM the JVM by
     * streaming an LF-less header forever.
     */
    private fun readHeaderLine(stream: InputStream): String? {
        val buf = ByteArrayOutputStream()
        while (true) {
            val b = stream.read()
            if (b == -1) return if (buf.size() == 0) null
                                else buf.toByteArray().toString(Charsets.UTF_8)
            if (b == '\n'.code) return buf.toByteArray().toString(Charsets.UTF_8)
            if (buf.size() >= MAX_FRAME) {
                throw IOException("header line exceeded $MAX_FRAME bytes")
            }
            buf.write(b)
        }
    }

    /**
     * Read frames from the socket until socket EOF, forwarding stdin
     * frames to the child and closing child stdin on a stdin EOF
     * frame. **Keeps reading after the stdin EOF frame** so a host
     * disconnect (Ctrl-C / SIGKILL of `tawc-exec`) is observable even
     * for commands that never read stdin — without this, every test
     * that backgrounds tawc-exec would leak orphans because the host
     * helper sends stdin EOF immediately when /dev/null is the local
     * stdin, and then the broker would stop watching for cancellation.
     *
     * Returns true if we exited because the socket dropped (cancel
     * the child); false on protocol error (also cancel, but logged
     * differently).
     */
    private fun relaySocketToChildIn(sin: DataInputStream, childIn: OutputStream): Boolean {
        var stdinClosed = false
        try {
            while (true) {
                val streamId = sin.read()
                if (streamId == -1) return true
                val len = sin.readInt()
                when (streamId) {
                    STREAM_STDIN -> {
                        if (len <= 0 || len > MAX_FRAME) {
                            throw IOException("bad stdin frame length $len")
                        }
                        val buf = ByteArray(len)
                        sin.readFully(buf)
                        if (!stdinClosed) {
                            try { childIn.write(buf); childIn.flush() }
                            catch (_: IOException) { stdinClosed = true }
                        }
                    }
                    STREAM_STDIN_EOF -> {
                        if (len != 0) throw IOException("stdin EOF frame has len $len")
                        if (!stdinClosed) {
                            try { childIn.close() } catch (_: IOException) {}
                            stdinClosed = true
                        }
                        // Continue reading so we still observe socket EOF.
                    }
                    else -> throw IOException("unexpected client stream $streamId")
                }
            }
        } catch (e: IOException) {
            if (e is java.io.EOFException) return true
            Log.w(ExecBroker.TAG, "socket→stdin: ${e.message}")
            return true
        }
    }

    /**
     * Read child stdout/stderr in 4 KB chunks; emit one frame per read.
     * Stops on stream EOF (child closed) or on socket I/O error. Frame
     * writes are synchronized on [sout] so stdout / stderr / exit don't
     * interleave at the byte level.
     *
     * When [mirror] is non-null, each chunk is also tee'd into the
     * in-app log screen. We keep teeing even after the socket dies so
     * the user sees the rest of a job whose host TTY went away.
     */
    private fun relayChildOutToSocket(
        from: InputStream,
        streamId: Int,
        sout: DataOutputStream,
        socketAlive: AtomicBoolean,
        mirror: BrokerOpMirror?,
        isStdout: Boolean,
    ) {
        val buf = ByteArray(4096)
        while (true) {
            val n = try { from.read(buf) } catch (_: IOException) { -1 }
            if (n <= 0) break
            if (mirror != null) {
                if (isStdout) mirror.appendOut(buf, 0, n) else mirror.appendErr(buf, 0, n)
            }
            if (!socketAlive.get()) continue
            synchronized(sout) {
                try {
                    sout.write(streamId)
                    sout.writeInt(n)
                    sout.write(buf, 0, n)
                    sout.flush()
                } catch (_: IOException) {
                    socketAlive.set(false)
                }
            }
        }
    }

    companion object {
        // Stream IDs — kept in sync with notes/exec-broker.md and
        // tools/tawc-exec/src/main.rs.
        const val STREAM_STDIN = 0
        const val STREAM_STDOUT = 1
        const val STREAM_STDERR = 2
        const val STREAM_EXIT = 3
        const val STREAM_STDIN_EOF = 4
        const val STREAM_ERR = 5
        const val MAX_FRAME = 1 shl 20  // 1 MB safety cap
    }
}
