package me.phie.tawc.session

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Why something in a rootfs is alive. Carries what the notification shows. */
sealed interface Reason {
    data class Terminal(val distroId: String) : Reason
    data class Command(val label: String) : Reason
    data class Compositor(val windowCount: Int) : Reason
    /** Remote access running for [distroId] with [clients] logged in. */
    data class Remote(val distroId: String, val clients: Int) : Reason
    /** Guest processes that outlived every explicit hold. Only
     *  [SessionService] produces this; it is never acquired. */
    data class Stray(val count: Int) : Reason
}

/** One acquired [Reason]. [release] is idempotent and thread-safe. */
interface Hold {
    /** Swap the reason this hold reports (e.g. a new window count). */
    fun update(reason: Reason)
    fun release()
}

/**
 * Process-wide registry of everything alive in a rootfs. While it is
 * non-empty [SessionService] runs in the foreground; see
 * notes/session-service.md.
 *
 * Callable from any thread. The registry never touches Android itself:
 * [starter] (set once by [SessionService.install]) is invoked, outside
 * the lock, on every acquire that finds no service alive. The starter
 * must serialise with the service's stop on the main thread — see
 * [SessionService.install].
 */
object SessionHolds {
    private val lock = Any()
    private val holds = LinkedHashMap<Hold, Reason>()
    private val state = MutableStateFlow<List<Reason>>(emptyList())
    /** The live [SessionService] instance, as an opaque token. */
    private var service: Any? = null

    @Volatile
    var starter: (() -> Unit)? = null

    val reasons: StateFlow<List<Reason>> = state.asStateFlow()

    /** Bumped by every [acquire]: lets Exit notice something new started. */
    @Volatile
    var acquisitions = 0L
        private set

    fun acquire(reason: Reason): Hold {
        val hold = HoldImpl()
        val start = synchronized(lock) {
            acquisitions++
            holds[hold] = reason
            publish()
            service == null
        }
        if (start) starter?.invoke()
        return hold
    }

    /** Service side: [token]'s `onCreate`. */
    internal fun serviceStarted(token: Any) = synchronized(lock) { service = token }

    /**
     * Service side: if nothing is held, mark the service dead and return
     * true — the caller must then stop. Atomic with [acquire], so an
     * acquire racing the stop either keeps the service or starts a new one.
     */
    internal fun serviceStopIfIdle(token: Any): Boolean = synchronized(lock) {
        if (holds.isNotEmpty()) return false
        if (service === token) service = null
        true
    }

    /**
     * Service side: [token]'s `onDestroy`. Only clears its own
     * registration — a successor may already be up.
     */
    internal fun serviceStopped(token: Any) = synchronized(lock) {
        if (service === token) service = null
    }

    private fun publish() {
        state.value = holds.values.toList()
    }

    private class HoldImpl : Hold {
        override fun update(reason: Reason) = synchronized(lock) {
            if (holds.containsKey(this)) {
                holds[this] = reason
                publish()
            }
        }

        override fun release() = synchronized(lock) {
            if (holds.remove(this) != null) publish()
        }
    }

    /** Unit tests only: forget everything. */
    internal fun resetForTest() = synchronized(lock) {
        holds.clear()
        service = null
        starter = null
        publish()
    }
}

/** What the notification says, reduced from a list of reasons. */
data class SessionSummary(
    val terminals: Int,
    val windows: Int,
    val commands: List<String>,
    val strays: Int,
    /** Remote access on (at most one agent), and its logged-in clients. */
    val remote: Boolean = false,
    val remoteClients: Int = 0,
) {
    companion object {
        fun of(reasons: List<Reason>): SessionSummary {
            var terminals = 0
            var windows = 0
            val commands = ArrayList<String>()
            var strays = 0
            var remote = false
            var remoteClients = 0
            for (r in reasons) {
                when (r) {
                    is Reason.Terminal -> terminals++
                    is Reason.Command -> commands += r.label
                    is Reason.Compositor -> windows += r.windowCount
                    is Reason.Stray -> strays += r.count
                    is Reason.Remote -> {
                        remote = true
                        remoteClients += r.clients
                    }
                }
            }
            return SessionSummary(terminals, windows, commands, strays, remote, remoteClients)
        }
    }
}
