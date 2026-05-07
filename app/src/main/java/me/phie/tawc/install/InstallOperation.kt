package me.phie.tawc.install

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import me.phie.tawc.ops.CancelConfirmation
import me.phie.tawc.ops.Operation
import me.phie.tawc.ops.OperationProgress
import me.phie.tawc.ops.OperationStage

/**
 * Adapts [InstallationService]'s install-shaped progress / log / cancel
 * surface to the generic [Operation] contract that the ops layer
 * speaks. One instance per service-managed install or uninstall job
 * (or per refused-by-gate transient — see
 * [InstallationService.rejectAsTransientOp]); registered in
 * [me.phie.tawc.ops.OperationsRegistry] for the lifetime of the work.
 *
 * Owns its own progress [MutableStateFlow] so the service's
 * `publishProgress` writes land synchronously on the same thread that
 * called them — no upstream-collector scope, no projection-race window
 * during the unregister-on-terminal handoff.
 *
 * The [log] flow is shared with the service (one buffer; the service's
 * gate guarantees at most one job at a time, and the `replay` cache is
 * cleared at the start of every job).
 */
internal class InstallOperation(
    override val id: String,
    override val title: String,
    serviceLog: SharedFlow<String>,
    override val cancelConfirmation: CancelConfirmation?,
    private val cancelHandler: () -> Unit,
) : Operation {

    private val _progress = MutableStateFlow(OperationProgress(OperationStage.IDLE, ""))
    override val progress: StateFlow<OperationProgress> = _progress.asStateFlow()

    override val log: SharedFlow<String> = serviceLog

    override fun cancel() = cancelHandler()

    /**
     * Publish a new progress snapshot. Called by [InstallationService]
     * from `publishProgress` whenever the worker pipeline reports a
     * new stage / message. Synchronous — the caller's write is visible
     * to subscribers immediately, no scheduler hop.
     */
    fun publish(p: OperationProgress) {
        _progress.value = p
    }
}

internal fun InstallProgress.toOperationProgress(): OperationProgress =
    OperationProgress(
        stage = when (stage) {
            InstallStage.IDLE -> OperationStage.IDLE
            InstallStage.DONE -> OperationStage.DONE
            InstallStage.FAILED -> OperationStage.FAILED
            else -> OperationStage.RUNNING
        },
        message = message,
        percent = percent,
    )
