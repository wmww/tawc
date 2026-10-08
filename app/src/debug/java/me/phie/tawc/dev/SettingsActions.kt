package me.phie.tawc.dev

import me.phie.tawc.AndoBrokers
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.PointerEmulation
import me.phie.tawc.Settings
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.InstallationStore

/**
 * Broker actions that read/write entries in [Settings]. Registered
 * from [me.phie.tawc.TawcApplication.onCreate] (debug builds only).
 *
 * Used for ad-hoc developer toggling. In normal app mode this writes
 * the persisted settings users edit from the in-app Settings screen. In
 * integration-test mode, `test-init` swaps [Settings] to an in-memory
 * store, so these actions only mutate that test process state.
 *
 * | Action | Args | Effect |
 * |--------|------|--------|
 * | `set-graphics-backend` | `value` ∈ {"libhybris","gfxstream","cpu"} | `Settings.graphicsBackend = …` |
 * | `get-graphics-backend` | — | prints current backend key on stdout |
 * | `set-output-scale` | `value` ∈ 0.5..4.0, snapped to 0.25 | save current setting and push live compositor scale |
 * | `get-output-scale` | — | prints current output scale |
 * | `set-xwayland` | `enabled` ∈ true|false | save current setting and start/stop Xwayland |
 * | `get-xwayland` | — | prints true/false |
 * | `set-pointer-emulation` | `value` ∈ none|hover|full | save current setting and push it live |
 * | `get-pointer-emulation` | — | prints none/hover/full |
 * | `set-ando` | `installId`, `enabled` ∈ true|false | set the test-mode ando override for the install and reconcile the broker |
 * | `get-ando` | `installId` | prints true/false (override or metadata) |
 *
 * `set-ando`/`get-ando` are per-distro (notes/ando.md), so unlike the
 * others they don't touch [Settings]: `set-ando` writes an in-memory
 * override in [InstallationStore] (mirroring `Settings.enterTestMode` —
 * no durable metadata write, discarded on process death) and reconciles
 * the native listeners via [AndoBrokers.refresh]. Integration tests use
 * them to flip ando per test without a real install.
 */
internal object SettingsActions {

    fun registerAll() {
        ActionRegistry.register("set-graphics-backend", SetGraphicsBackendAction)
        ActionRegistry.register("get-graphics-backend", GetGraphicsBackendAction)
        ActionRegistry.register("set-output-scale", SetOutputScaleAction)
        ActionRegistry.register("get-output-scale", GetOutputScaleAction)
        ActionRegistry.register("set-xwayland", SetXwaylandAction)
        ActionRegistry.register("get-xwayland", GetXwaylandAction)
        ActionRegistry.register("set-pointer-emulation", SetPointerEmulationAction)
        ActionRegistry.register("get-pointer-emulation", GetPointerEmulationAction)
        ActionRegistry.register("set-ando", SetAndoAction)
        ActionRegistry.register("get-ando", GetAndoAction)
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

    private object SetXwaylandAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["enabled"] ?: args["value"]
                ?: return ctx.fail("set-xwayland: --arg enabled=true|false required")
            val enabled = raw.toBooleanStrictOrNull()
                ?: return ctx.fail("set-xwayland: invalid boolean '$raw'")
            Settings.xwayland = enabled
            NativeBridge.nativeSetXwaylandEnabled(enabled)
            ctx.out(enabled.toString())
            return 0
        }
    }

    private object GetXwaylandAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(Settings.xwayland.toString())
            return 0
        }
    }

    private object SetPointerEmulationAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["value"]
                ?: return ctx.fail("set-pointer-emulation: --arg value=none|hover|full required")
            val mode = PointerEmulation.fromKeyOrNull(raw)
                ?: return ctx.fail("set-pointer-emulation: unknown value '$raw'")
            Settings.pointerEmulation = mode
            NativeBridge.nativeSetPointerEmulation(mode.ordinal)
            ctx.out(mode.key)
            return 0
        }
    }

    private object GetPointerEmulationAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(Settings.pointerEmulation.key)
            return 0
        }
    }

    private object SetAndoAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"]
                ?: return ctx.fail("set-ando: --arg installId=<id> required")
            val raw = args["enabled"] ?: args["value"]
                ?: return ctx.fail("set-ando: --arg enabled=true|false required")
            val enabled = raw.toBooleanStrictOrNull()
                ?: return ctx.fail("set-ando: invalid boolean '$raw'")
            InstallationStore.setAndoOverride(id, enabled)
            // Bring the per-distro listener up/down now (enable) or tear
            // it down + kill in-flight children (disable). Next rootfs
            // spawn picks up the matching bind.
            AndoBrokers.refresh(ctx.appContext)
            ctx.out(enabled.toString())
            return 0
        }
    }

    private object GetAndoAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"]
                ?: return ctx.fail("get-ando: --arg installId=<id> required")
            ctx.out(InstallationStore(ctx.appContext).andoEnabled(id).toString())
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
