package me.phie.tawc.ops

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Default [Operation] implementation: a freely-mutable progress
 * [StateFlow], a caller-supplied log [SharedFlow], a caller-supplied
 * cancel handler. The publish side (progress writes, log emits) lives
 * with whoever's running the work; viewers (the panel, the broker
 * mirror, notifications) just collect from the [Operation] surface.
 *
 * Used by [me.phie.tawc.install.InstallationService] for install /
 * uninstall jobs and by the broker's RUNINSIDE / ARGV handlers when an
 * `OP_TITLE` header asks for a log-screen mirror.
 *
 * Synchronous progress writes — [publish] lands on whatever thread the
 * caller is on, no scheduler hop, so a service's terminal-then-
 * unregister sequence is observable in order without race windows.
 */
internal class MutableOperation(
    override val id: String,
    override val title: String,
    log: SharedFlow<String>,
    override val cancelConfirmation: CancelConfirmation? = null,
    private val cancelHandler: () -> Unit = {},
) : Operation {

    private val _progress = MutableStateFlow(OperationProgress(OperationStage.IDLE, ""))
    override val progress: StateFlow<OperationProgress> = _progress.asStateFlow()

    override val log: SharedFlow<String> = log

    override fun cancel() = cancelHandler()

    fun publish(p: OperationProgress) {
        _progress.value = p
    }
}
