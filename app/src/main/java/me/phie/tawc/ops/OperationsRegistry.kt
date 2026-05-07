package me.phie.tawc.ops

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/**
 * Application-singleton map of currently-running operations, keyed by
 * [Operation.id]. The single source of truth observers look at when
 * they want to know "what's happening right now":
 *
 *   - [OperationsNotificationCenter] posts/cancels notifications based
 *     on the registry contents.
 *   - [LogScreenActivity] looks up the op named in its launching intent.
 *   - A future "in-progress operations" list UI just collects [ops].
 *
 * Per the project's "unregister immediately on terminal" choice, an
 * Operation is in the registry exactly while its work is in flight.
 * Terminal Operations vanish; any still-bound viewer keeps its last-
 * rendered state frozen, but a fresh lookup finds nothing.
 *
 * Thread safety: operations are typically registered from a service's
 * coroutine scope and unregistered from a `finally` on the same job;
 * observers collect on Main. The underlying [MutableStateFlow] handles
 * the cross-thread visibility.
 */
object OperationsRegistry {

    private val state = MutableStateFlow<Map<String, Operation>>(emptyMap())

    /** Live snapshot of all registered operations. */
    val ops: StateFlow<Map<String, Operation>> = state.asStateFlow()

    /**
     * Register [op] under [Operation.id]. An existing entry with the
     * same id is replaced — concrete operation owners are expected to
     * have already unregistered the previous one in their `finally`,
     * but a defensive overwrite means a leaked Operation can't wedge
     * the slot.
     */
    fun register(op: Operation) {
        state.update { it + (op.id to op) }
    }

    fun unregister(id: String) {
        state.update { it - id }
    }

    fun get(id: String): Operation? = state.value[id]
}
