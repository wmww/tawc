package me.phie.tawc.ops

import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * A long-running task with an observable progress + log surface. The
 * generic abstraction the [LogScreenActivity] / [OperationLogPanel] /
 * [OperationsNotificationCenter] all hang off — none of them know
 * anything about install/uninstall (or any other concrete action),
 * they just collect from this interface.
 *
 * Lifetime:
 *   - The owner registers an Operation in [OperationsRegistry] when the
 *     work begins, and unregisters when it terminates (DONE / FAILED).
 *     Per the project's "unregister immediately on terminal" choice,
 *     a freshly-opened [LogScreenActivity] looking up a no-longer-
 *     registered op shows "no such operation" — only currently-bound
 *     viewers keep their last-rendered state frozen.
 *
 * The default impl is [MutableOperation] — see [InstallationService] for
 * the canonical use, and [me.phie.tawc.dev.ExecBrokerSession]'s OP_TITLE
 * mirror path for a broker-driven one.
 */
interface Operation {
    /**
     * Stable identifier. Convention: `<verb>:<target>`, e.g.
     * `install:arch`, `uninstall:arch`. Used as the registry key, the
     * notification id derivation, and the [LogScreenActivity] intent
     * extra.
     */
    val id: String

    /** Human-readable title shown in the LogScreen toolbar / notification. */
    val title: String

    /** Latest progress snapshot. Cold subscribers receive [progress.value] immediately. */
    val progress: StateFlow<OperationProgress>

    /** Log line stream. Implementations should configure a replay buffer so a late binder sees recent history. */
    val log: SharedFlow<String>

    /**
     * UX hint for the LogScreen Cancel button. `null` means "tap is
     * the confirmation"; non-null means "show this dialog first".
     * Notification cancel button always bypasses the dialog (a user
     * tapping a notification action has already deliberately decided).
     */
    val cancelConfirmation: CancelConfirmation?

    /**
     * Request cancellation. Idempotent. Implementations should guarantee
     * the operation eventually moves to a terminal state and is
     * unregistered.
     */
    fun cancel()
}

/**
 * Optional confirmation prompt shown by [LogScreenActivity] when its
 * Cancel button is tapped. Only the in-UI Cancel button consults this;
 * the notification's Cancel action always cancels straight through.
 */
data class CancelConfirmation(
    val title: String,
    val message: String,
    val confirmLabel: String = "Cancel",
    val keepLabel: String = "Keep going",
)
