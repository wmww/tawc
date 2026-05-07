package me.phie.tawc.dev

import android.content.Context
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Handler for a broker `ACTION` invocation. Registered in
 * [ActionRegistry] at app start (debug builds only); dispatched to from
 * [ExecBrokerSession] when the host sends an `ACTION <name>` header.
 *
 * Handlers run on the per-connection broker thread (NOT a coroutine).
 * Long blocking work is fine — that's the whole point of the broker.
 * Cancellation is observed via [ActionContext.cancelFlag], which the
 * broker sets when the host disconnects (Ctrl-C of `tawc-exec`).
 */
internal interface BrokerAction {
    /**
     * Run the action with the given [args] (parsed from `ARG key=value`
     * header lines). Use [ctx.out] / [ctx.err] to write to the host's
     * stdout / stderr. Return the integer that should land as the
     * process's exit code on the host.
     */
    fun run(args: Map<String, String>, ctx: ActionContext): Int
}

/**
 * Per-invocation context handed to a [BrokerAction]. Encapsulates the
 * application context (for service starts, registry lookups, etc.),
 * plus channels for streaming output back to the host and observing
 * host-side cancellation.
 */
internal class ActionContext(
    val appContext: Context,
    /** Append a line to the host's stdout. The framing (newline, frame header, …) is handled. */
    val out: (String) -> Unit,
    /** Append a line to the host's stderr. Same conventions as [out]. */
    val err: (String) -> Unit,
    /**
     * Set to `true` when the host socket goes away. Long-running actions
     * should poll this between blocking calls and bail out promptly so
     * the broker can free the connection thread.
     */
    val cancelFlag: AtomicBoolean,
)
