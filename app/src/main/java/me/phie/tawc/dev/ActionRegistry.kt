package me.phie.tawc.dev

/**
 * Global mapping of action name → [BrokerAction] handler.
 *
 * Debug builds only. Populated from [me.phie.tawc.TawcApplication.onCreate]
 * (next to [ExecBroker.start]) so every host-driven entry point has the
 * full action set available. [ExecBrokerSession] looks handlers up by
 * the `ACTION <name>` header value when the host sends an action
 * invocation in place of an ARGV.
 *
 * Thread safety: [register] is expected to be called once per action at
 * app start, before any host connection arrives. Lookups happen from the
 * per-connection broker threads. The simple synchronized map is fine
 * for the tiny set sizes (handful of actions, never modified after
 * startup).
 */
internal object ActionRegistry {

    private val handlers = mutableMapOf<String, BrokerAction>()

    fun register(name: String, action: BrokerAction) {
        synchronized(handlers) { handlers[name] = action }
    }

    fun get(name: String): BrokerAction? =
        synchronized(handlers) { handlers[name] }

    fun names(): List<String> =
        synchronized(handlers) { handlers.keys.sorted() }
}
