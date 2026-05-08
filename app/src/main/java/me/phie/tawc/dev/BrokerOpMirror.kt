package me.phie.tawc.dev

import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.flow.MutableSharedFlow
import me.phie.tawc.ops.LogScreenActivity
import me.phie.tawc.ops.MutableOperation
import me.phie.tawc.ops.OperationProgress
import me.phie.tawc.ops.OperationStage
import me.phie.tawc.ops.OperationsRegistry
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/**
 * Mirrors a broker-driven subprocess into an [me.phie.tawc.ops.Operation]
 * so the in-app [LogScreenActivity] can show its progress + log lines.
 *
 * Created by [ExecBrokerSession] when a request carries an `OP_TITLE`
 * header; [start] registers the op + opens the log screen, [appendOut] /
 * [appendErr] tee chunks of process stdout/stderr into the op log
 * (line-buffered so partial-line writes don't fragment the panel), and
 * [finish] publishes the terminal stage and unregisters the op.
 *
 * The host TTY is not affected — the broker keeps streaming raw chunks
 * back over the socket; this mirror only adds an in-app surface.
 */
internal class BrokerOpMirror private constructor(
    private val appContext: Context,
    private val opId: String,
    val op: MutableOperation,
    private val sink: MutableSharedFlow<String>,
    private val cancelHandler: AtomicReference<() -> Unit>,
) {

    private val outBuf = StringBuilder()
    private val errBuf = StringBuilder()
    private val finished = AtomicBoolean(false)

    fun start(initialMessage: String) {
        OperationsRegistry.register(op)
        op.publish(OperationProgress(OperationStage.RUNNING, initialMessage))
        sink.tryEmit("[broker] $initialMessage")
        try {
            val intent = LogScreenActivity.intentFor(appContext, opId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            appContext.startActivity(intent)
        } catch (t: Throwable) {
            // BAL refusal etc. — host TTY still gets the full feed,
            // and the per-op tray notification's tap action lets the
            // user reach the panel later. Op stays registered.
            Log.w(ExecBroker.TAG, "BrokerOpMirror open log screen failed: ${t.message}")
        }
    }

    /**
     * Plug a real cancel implementation in after [create] (the session
     * binds it to "kill the process tree"). Until this is called, the
     * panel's Cancel button is a no-op — but [start]→[finish] is fast
     * enough that the window doesn't matter in practice.
     */
    fun setCancelHandler(handler: () -> Unit) {
        cancelHandler.set(handler)
    }

    fun appendOut(buf: ByteArray, off: Int, len: Int) {
        appendInto(outBuf, buf, off, len)
    }

    fun appendErr(buf: ByteArray, off: Int, len: Int) {
        appendInto(errBuf, buf, off, len)
    }

    private fun appendInto(target: StringBuilder, buf: ByteArray, off: Int, len: Int) {
        // Decode as UTF-8 chunk-by-chunk. We accept that a multi-byte
        // codepoint split across chunks gets a replacement char — log
        // text is human-eyes-only and chunks are typically full lines.
        val chunk = String(buf, off, len, Charsets.UTF_8)
        synchronized(this) {
            target.append(chunk)
            while (true) {
                val nl = target.indexOf('\n')
                if (nl < 0) break
                val line = target.substring(0, nl).trimEnd('\r')
                target.delete(0, nl + 1)
                sink.tryEmit(line)
            }
        }
    }

    /**
     * Idempotent. Called on the structured exit path with the real
     * exit code, and again from [ExecBrokerSession.streamProcess]'s
     * outer finally with `-1` if anything threw between [start] and
     * the structured exit (so the op never leaks in the registry).
     * The first call wins.
     */
    fun finish(exit: Int) {
        if (!finished.compareAndSet(false, true)) return
        // Flush any remaining trailing bytes that didn't end in \n.
        synchronized(this) {
            if (outBuf.isNotEmpty()) {
                sink.tryEmit(outBuf.toString().trimEnd('\r'))
                outBuf.setLength(0)
            }
            if (errBuf.isNotEmpty()) {
                sink.tryEmit(errBuf.toString().trimEnd('\r'))
                errBuf.setLength(0)
            }
        }
        // Java Process.waitFor returns the wstatus value as an int —
        // for signal-killed children it's 128+sig, not negative. We
        // can't reliably distinguish "process exited 137" from
        // "process killed by SIGKILL"; the message just reports what
        // we've got.
        val (stage, msg) = if (exit == 0) {
            OperationStage.DONE to "Done"
        } else {
            OperationStage.FAILED to "Exited with code $exit"
        }
        op.publish(OperationProgress(stage, msg))
        OperationsRegistry.unregister(opId)
    }

    companion object {
        // Per-broker-process counter so concurrent ops never collide on
        // the registry id, even if the host sends two requests with the
        // same OP_TITLE.
        private val seq = AtomicLong(0)

        fun create(appContext: Context, title: String): BrokerOpMirror {
            val n = seq.incrementAndGet()
            val id = "broker-cmd:$n"
            val sink = MutableSharedFlow<String>(replay = 200, extraBufferCapacity = 1024)
            val cancelHandler = AtomicReference<() -> Unit>({})
            val op = MutableOperation(
                id = id,
                title = title,
                log = sink,
                cancelConfirmation = null,
                cancelHandler = { cancelHandler.get().invoke() },
            )
            return BrokerOpMirror(appContext, id, op, sink, cancelHandler)
        }
    }
}
