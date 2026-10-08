package me.phie.tawc.launcher

import me.phie.tawc.GraphicsBackend
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.dev.ActionContext
import me.phie.tawc.dev.ActionRegistry
import me.phie.tawc.dev.BrokerAction
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import org.json.JSONArray
import org.json.JSONObject

/**
 * Broker actions for launcher test visibility, registered from
 * [me.phie.tawc.TawcApplication.onCreate] (debug builds only). Gives
 * integration tests a query surface for hidden filtering / scan shape
 * without screenshot scraping.
 *
 * | Action | Args | Effect |
 * |--------|------|--------|
 * | `launcher-list` | `installId`, optional `showHidden` ∈ true|false | print the launcher entry list as a JSON array on stdout |
 * | `set-entry-hidden` | `installId`, `entryId`, `hidden` ∈ true|false | persist hide/unhide through the same metadata write the UI uses |
 * | `set-entry-graphics` | `installId`, `entryId`, `backend` (a `GraphicsBackend.key`, or empty to clear) | persist the editor's per-entry graphics override |
 * | `launcher-icons` | `installId` | print the icon picker's name list (`[{name, user}]`) |
 * | `launcher-resolve-icon` | `installId`, `value` | print the PNG path an `Icon=` value resolves to (empty line if none) |
 * | `launcher-launch` | `installId`, `entryId`, optional `timeoutMs` | launch a GUI entry like a tap (splash + reserved host); print the launch id |
 * | `launch-state` | `launchId` | print `{state, code, message, taskId, log}` for a launch; `state` is `released` once its splash closed |
 *
 * `launcher-list` mirrors what [me.phie.tawc.launcher.AppsPane] renders: hidden
 * entries are filtered out unless `showHidden=true` (the UI's
 * "Show hidden" toggle), and includes the built-ins. Each element is
 * `{id, name, exec, terminal, iconPath, path, shadows, graphics, hidden, builtin}`
 * (`graphics`: the entry's stored override key, empty when none) — `iconPath` is
 * the resolved on-device PNG (empty when nothing resolved), which is
 * how icon-resolution tests see what `launcher.rs` picked.
 */
internal object LauncherActions {

    fun registerAll() {
        ActionRegistry.register("launcher-list", LauncherListAction)
        ActionRegistry.register("set-entry-hidden", SetEntryHiddenAction)
        ActionRegistry.register("set-entry-graphics", SetEntryGraphicsAction)
        ActionRegistry.register("launcher-icons", ListIconsAction)
        ActionRegistry.register("launcher-resolve-icon", ResolveIconAction)
        ActionRegistry.register("launcher-launch", LaunchAction)
        ActionRegistry.register("launch-state", LaunchStateAction)
    }

    /** The tap path for GUI entries, with a settable splash timeout. */
    private object LaunchAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"] ?: return ctx.fail("launcher-launch: --arg installId=<id> required")
            val entryId = args["entryId"] ?: return ctx.fail("launcher-launch: --arg entryId=<id> required")
            val timeoutMs = args["timeoutMs"]?.let {
                it.toLongOrNull() ?: return ctx.fail("launcher-launch: invalid timeoutMs '$it'")
            } ?: LaunchRegistry.DEFAULT_TIMEOUT_MS
            val store = InstallationStore(ctx.appContext)
            val inst = store.load(id) ?: return ctx.fail("launcher-launch: no installation '$id'")
            val method = InstallationMethod.forKey(ctx.appContext, inst.method)
                ?: return ctx.fail("launcher-launch: method '${inst.method}' not in this build")
            val rootfs = store.rootfsDir(id).absolutePath
            val entry = LauncherEntry.scan(rootfs).firstOrNull { it.id == entryId }
                ?: return ctx.fail("launcher-launch: no entry '$entryId'")
            if (entry.terminal) return ctx.fail("launcher-launch: '$entryId' is a terminal entry")
            val graphics = EntryLauncher.graphicsFor(inst, entry.id)
            val launch = EntryLauncher.launchGui(ctx.appContext, method, rootfs, entry, graphics, timeoutMs)
            ctx.out(launch.id)
            return 0
        }
    }

    private object LaunchStateAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["launchId"] ?: return ctx.fail("launch-state: --arg launchId=<id> required")
            val launch = LaunchRegistry.get(id)
            val out = JSONObject()
            if (launch == null) {
                out.put("state", "released")
            } else {
                when (val state = launch.state.value) {
                    is LaunchState.Exited -> out.put("state", "exited").put("code", state.code)
                    is LaunchState.Failed -> out.put("state", "failed").put("message", state.message)
                    else -> out.put("state", state.toString().lowercase())
                }
                out.put("taskId", launch.taskId)
                out.put("log", launch.logText())
            }
            ctx.out(out.toString())
            return 0
        }
    }

    private object ListIconsAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val rootfs = ctx.rootfs("launcher-icons", args) ?: return 2
            ctx.out(NativeBridge.nativeListIcons(rootfs))
            return 0
        }
    }

    private object ResolveIconAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val rootfs = ctx.rootfs("launcher-resolve-icon", args) ?: return 2
            val value = args["value"]
                ?: return ctx.fail("launcher-resolve-icon: --arg value=<Icon= value> required")
            ctx.out(NativeBridge.nativeResolveIcon(rootfs, value))
            return 0
        }
    }

    /** Rootfs of `installId`, or null after reporting why not. */
    private fun ActionContext.rootfs(action: String, args: Map<String, String>): String? {
        val id = args["installId"] ?: run { fail("$action: --arg installId=<id> required"); return null }
        val store = InstallationStore(appContext)
        if (store.load(id) == null) {
            fail("$action: no installation '$id'")
            return null
        }
        return store.rootfsDir(id).absolutePath
    }

    private object LauncherListAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"]
                ?: return ctx.fail("launcher-list: --arg installId=<id> required")
            val showHidden = args["showHidden"]?.let {
                it.toBooleanStrictOrNull()
                    ?: return ctx.fail("launcher-list: invalid boolean for showHidden '$it'")
            } ?: false
            val store = InstallationStore(ctx.appContext)
            val inst = store.load(id)
                ?: return ctx.fail("launcher-list: no installation '$id'")
            val rootfs = store.rootfsDir(id).absolutePath
            val hidden = inst.hiddenDesktopIds.toSet()
            val out = JSONArray()
            for (e in LauncherEntry.list(ctx.appContext, inst, rootfs)) {
                val isHidden = e.id in hidden
                if (isHidden && !showHidden) continue
                out.put(JSONObject().apply {
                    put("id", e.id)
                    put("name", e.name)
                    put("exec", e.exec)
                    put("terminal", e.terminal)
                    put("iconPath", e.iconPath)
                    put("path", e.path)
                    put("shadows", e.shadows)
                    put("graphics", inst.entryGraphics[e.id] ?: "")
                    put("hidden", isHidden)
                    put("builtin", e.builtin != null)
                })
            }
            ctx.out(out.toString())
            return 0
        }
    }

    private object SetEntryHiddenAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"]
                ?: return ctx.fail("set-entry-hidden: --arg installId=<id> required")
            val entryId = args["entryId"]
                ?: return ctx.fail("set-entry-hidden: --arg entryId=<id> required")
            val raw = args["hidden"]
                ?: return ctx.fail("set-entry-hidden: --arg hidden=true|false required")
            val hidden = raw.toBooleanStrictOrNull()
                ?: return ctx.fail("set-entry-hidden: invalid boolean '$raw'")
            val updated: Installation = InstallationStore(ctx.appContext)
                .update(id) { it.withEntryHidden(entryId, hidden) }
                ?: return ctx.fail("set-entry-hidden: no installation '$id'")
            ctx.out(updated.hiddenDesktopIds.joinToString(","))
            return 0
        }
    }

    private object SetEntryGraphicsAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val id = args["installId"]
                ?: return ctx.fail("set-entry-graphics: --arg installId=<id> required")
            val entryId = args["entryId"]
                ?: return ctx.fail("set-entry-graphics: --arg entryId=<id> required")
            val raw = args["backend"]
                ?: return ctx.fail("set-entry-graphics: --arg backend=<key>|'' required")
            val key = raw.ifEmpty { null }
            if (key != null && GraphicsBackend.fromKeyOrNull(key) == null) {
                return ctx.fail("set-entry-graphics: backend '$key' not in this build")
            }
            val updated: Installation = InstallationStore(ctx.appContext)
                .update(id) { it.withEntryGraphics(entryId, key) }
                ?: return ctx.fail("set-entry-graphics: no installation '$id'")
            ctx.out(updated.entryGraphics.entries.joinToString(",") { "${it.key}=${it.value}" })
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
