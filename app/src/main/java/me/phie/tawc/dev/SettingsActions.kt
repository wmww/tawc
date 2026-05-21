package me.phie.tawc.dev

import android.util.Log
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.Settings
import me.phie.tawc.compositor.NativeBridge

/**
 * Broker actions that read/write entries in [Settings]. Registered
 * from [me.phie.tawc.TawcApplication.onCreate] (debug builds only).
 *
 * Used for ad-hoc developer toggling that should persist — same code
 * path users hit by toggling the radio on the in-app Settings screen.
 * The integration test suite does NOT use this; tests pass a
 * `GRAPHICS` header on each broker RUNINSIDE spawn so the persisted
 * pref isn't disturbed (see `notes/exec-broker.md`).
 *
 * | Action | Args | Effect |
 * |--------|------|--------|
 * | `set-graphics-backend` | `value` ∈ {"libhybris","gfxstream","cpu"} | `Settings.graphicsBackend = …` |
 * | `get-graphics-backend` | — | prints current backend key on stdout |
 * | `set-output-scale` | `value` ∈ 0.5..4.0, snapped to 0.25 | persist and push live compositor scale |
 * | `get-output-scale` | — | prints current output scale |
 * | `set-gtk3-broken-menus-workaround` | `enabled` ∈ true|false | persist and push live workaround toggle |
 * | `get-gtk3-broken-menus-workaround` | — | prints true/false |
 */
internal object SettingsActions {

    private const val TAG = "tawc"

    fun registerAll() {
        ActionRegistry.register("set-graphics-backend", SetGraphicsBackendAction)
        ActionRegistry.register("get-graphics-backend", GetGraphicsBackendAction)
        ActionRegistry.register("set-output-scale", SetOutputScaleAction)
        ActionRegistry.register("get-output-scale", GetOutputScaleAction)
        ActionRegistry.register("set-gtk3-broken-menus-workaround", SetGtk3BrokenMenusWorkaroundAction)
        ActionRegistry.register("get-gtk3-broken-menus-workaround", GetGtk3BrokenMenusWorkaroundAction)
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

    private object SetOutputScaleAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["value"]
                ?: return ctx.fail("set-output-scale: --arg value=<scale> required")
            val parsed = raw.toFloatOrNull()
                ?: return ctx.fail("set-output-scale: invalid value '$raw'")
            val scale = Settings.snapOutputScale(parsed)
            Settings.outputScale = scale
            NativeBridge.nativeSetOutputScale(scale)
            Log.i(TAG, "set-output-scale: ${Settings.formatOutputScale(scale)}")
            ctx.out(String.format(java.util.Locale.US, "%.2f", scale))
            return 0
        }
    }

    private object GetOutputScaleAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(String.format(java.util.Locale.US, "%.2f", Settings.outputScale))
            return 0
        }
    }

    private object SetGtk3BrokenMenusWorkaroundAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["enabled"] ?: args["value"]
                ?: return ctx.fail("set-gtk3-broken-menus-workaround: --arg enabled=true|false required")
            val enabled = raw.toBooleanStrictOrNull()
                ?: return ctx.fail("set-gtk3-broken-menus-workaround: invalid boolean '$raw'")
            Settings.gtk3BrokenMenusWorkaround = enabled
            NativeBridge.nativeSetGtk3BrokenMenusWorkaround(enabled)
            Log.i(TAG, "set-gtk3-broken-menus-workaround: $enabled")
            ctx.out(enabled.toString())
            return 0
        }
    }

    private object GetGtk3BrokenMenusWorkaroundAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(Settings.gtk3BrokenMenusWorkaround.toString())
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
