package me.phie.tawc.dev

import android.util.Log
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.Settings

/**
 * Broker actions that read/write entries in [Settings]. Registered
 * from [me.phie.tawc.TawcApplication.onCreate] (debug builds only).
 *
 * Used today by `scripts/run-integration-tests.sh --graphics …` so the
 * test runner can flip the persisted graphics-driver pick before
 * spawning client processes — same code path users hit by toggling
 * the radio on the in-app Settings screen.
 *
 * | Action | Args | Effect |
 * |--------|------|--------|
 * | `set-graphics-backend` | `value` ∈ {"libhybris","gfxstream","cpu"} | `Settings.graphicsBackend = …` |
 * | `get-graphics-backend` | — | prints current backend key on stdout |
 */
internal object SettingsActions {

    private const val TAG = "tawc"

    fun registerAll() {
        ActionRegistry.register("set-graphics-backend", SetGraphicsBackendAction)
        ActionRegistry.register("get-graphics-backend", GetGraphicsBackendAction)
    }

    private object SetGraphicsBackendAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["value"]
                ?: return ctx.fail("set-graphics-backend: --arg value=<key> required")
            val picked = GraphicsBackend.entries.firstOrNull { it.key == raw }
                ?: return ctx.fail(
                    "set-graphics-backend: unknown value '$raw'. " +
                        "Valid: ${GraphicsBackend.entries.joinToString { it.key }}"
                )
            Settings.graphicsBackend = picked
            Log.i(TAG, "set-graphics-backend: ${picked.key}")
            return 0
        }
    }

    private object GetGraphicsBackendAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(Settings.graphicsBackend.key)
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
