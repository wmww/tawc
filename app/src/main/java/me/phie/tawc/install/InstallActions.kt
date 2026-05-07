package me.phie.tawc.install

import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import me.phie.tawc.dev.ActionContext
import me.phie.tawc.dev.ActionRegistry
import me.phie.tawc.dev.BrokerAction
import me.phie.tawc.ops.LogScreenActivity
import me.phie.tawc.ops.Operation
import me.phie.tawc.ops.OperationStage
import me.phie.tawc.ops.OperationsRegistry

/**
 * Broker actions for install / uninstall, registered from
 * [me.phie.tawc.TawcApplication.onCreate] (debug builds only).
 *
 * The actions are thin shims: they validate args, hand off to
 * [InstallationService.startInstall] / [InstallationService.startUninstall]
 * (which already enforces the install state-machine gate, owns the
 * worker coroutine, and registers the [Operation] adapter), and then
 * mirror the registered op's progress + log flows to the broker's
 * stdout / stderr until the op terminates. Host disconnect → the
 * broker session sets `ctx.cancelFlag`, which we observe and translate
 * into [Operation.cancel].
 *
 * Because the action is just a viewer onto an already-foreground
 * service job, it does not block the service's lifetime: if the host
 * disconnects without sending cancel, the install / uninstall keeps
 * running to completion under the FGS — exactly like what would happen
 * if the user closed the in-app log screen mid-job.
 */
internal object InstallActions {

    private const val TAG = "tawc-install"

    fun registerAll() {
        ActionRegistry.register("install", InstallAction)
        ActionRegistry.register("uninstall", UninstallAction)
    }

    private object InstallAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["id"] ?: return ctx.fail("install: --arg id=<id> is required")
            if (!Installation.isValidId(id)) {
                return ctx.fail("install: invalid id '$id' (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            }
            val method = args["method"]
            val distro = args["distro"]
            val label = args["label"]
            val mirrorProxy = args["mirrorProxy"]

            val opId = "install:$id"
            tryOpenLogScreen(ctx.appContext, opId)

            ctx.out("[action] install id=$id method=${method ?: "(default)"} " +
                "distro=${distro ?: "(default)"} label=${label ?: "(default)"}" +
                (mirrorProxy?.let { " mirrorProxy=$it" } ?: ""))

            InstallationService.startInstall(ctx.appContext, id, method, distro, label, mirrorProxy)
            return mirrorOperation(opId, ctx)
        }
    }

    private object UninstallAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["id"] ?: return ctx.fail("uninstall: --arg id=<id> is required")
            if (!Installation.isValidId(id)) {
                return ctx.fail("uninstall: invalid id '$id' (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            }

            val opId = "uninstall:$id"
            tryOpenLogScreen(ctx.appContext, opId)
            ctx.out("[action] uninstall id=$id")

            InstallationService.startUninstall(ctx.appContext, id)
            return mirrorOperation(opId, ctx)
        }
    }

    /**
     * Wait for [opId] to appear in [OperationsRegistry] (with a short
     * timeout to catch validation rejects that never produce an op),
     * then collect [Operation.progress] + [Operation.log] until either
     * the progress reaches a terminal stage or the op leaves the
     * registry. Returns 0 on DONE, 1 on FAILED.
     *
     * Cancellation: polled via [ActionContext.cancelFlag]. Once set,
     * we call [Operation.cancel] and let the service's cancel-and-
     * cleanup flow run; we keep mirroring until terminal so the host
     * sees the failure status and the cleanup log lines.
     */
    private fun mirrorOperation(opId: String, ctx: ActionContext): Int = runBlocking {
        // The service's startInstall/startUninstall is dispatched via
        // startForegroundService -> onStartCommand on the main looper,
        // so the registry entry appears asynchronously. Wait briefly.
        // Validation rejects also surface here as a transient op (see
        // [InstallationService.rejectAsTransientOp]); the timeout
        // window must comfortably exceed that helper's
        // TRANSIENT_REJECT_HOLD_MS (2s) so a fast reject is still
        // caught before its op is unregistered.
        val op = withTimeoutOrNull(5_000) {
            OperationsRegistry.ops
                .map { it[opId] }
                .filterNotNull()
                .first()
        }
        if (op == null) {
            ctx.err("error: op '$opId' never appeared in registry")
            return@runBlocking -1
        }

        coroutineScope {
            val logJob = launch {
                op.log.collect { line -> ctx.out(line) }
            }
            val cancelJob = launch {
                while (true) {
                    if (ctx.cancelFlag.get()) {
                        ctx.err("[action] host disconnected; requesting cancel")
                        op.cancel()
                        break
                    }
                    delay(200)
                }
            }

            // Wait for the op to publish a terminal stage. The service
            // writes directly to the per-op StateFlow synchronously
            // (no projection, no scheduler hop), so by the time the
            // worker's `finally` unregisters, the terminal value is
            // already visible. Each `[stage:FOO]` transition is a log
            // line via [InstallationService.publishProgress] → op.log →
            // [logJob], so we observe progress silently for the
            // terminal trigger.
            val finalStage = op.progress.first { it.stage.isTerminal }.stage

            // Drain any log lines that the SharedFlow had buffered but
            // [logJob]'s collector hasn't dispatched yet. Without this
            // a fast-rejecting op (where publishProgress fires
            // synchronously before logJob has run a single collect
            // callback) would exit before its rejection log line ever
            // hits the host TTY. 100ms is plenty for the dispatcher
            // to pump pending replays; the action exit is already
            // governed by the operation's terminal, not this delay.
            delay(100)

            logJob.cancel()
            cancelJob.cancel()

            if (finalStage == OperationStage.DONE) 0 else 1
        }
    }

    private fun tryOpenLogScreen(ctx: Context, opId: String) {
        try {
            val intent = LogScreenActivity.intentFor(ctx, opId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            ctx.startActivity(intent)
        } catch (t: Throwable) {
            // BAL refusal or similar — log but don't fail. The host TTY
            // still gets the full progress/log feed.
            Log.w(TAG, "tryOpenLogScreen failed: ${t.message}")
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
