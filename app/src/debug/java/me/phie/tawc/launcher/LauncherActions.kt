package me.phie.tawc.launcher

import me.phie.tawc.GraphicsBackend
import me.phie.tawc.PointerEmulation
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.dev.ActionContext
import me.phie.tawc.dev.ActionRegistry
import me.phie.tawc.dev.BrokerAction
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import org.json.JSONArray
import org.json.JSONObject

/**
 * Broker actions for launcher test visibility, registered from
 * [me.phie.tawc.TawcApplication.onCreate] (debug builds only). Gives
 * integration tests a query surface for the merged entry list and the
 * [LauncherStore] without screenshot scraping.
 *
 * | Action | Args | Effect |
 * |--------|------|--------|
 * | `launcher-list` | `installId`, optional `showHidden` ∈ true|false | print the launcher entry list as a JSON array on stdout |
 * | `set-entry-override` | `installId`, `entryId`, `field`, optional `value` (absent = clear) | set one store field through the UI's write path ([LauncherEntries.withField]); `terminal`/`hidden` take true|false, `env` a JSON object |
 * | `put-shortcut` | `installId`, `fields` (JSON object), optional `entryId` (absent = a new `tawc:app:` id) | write a shortcut; print its id |
 * | `delete-shortcut` | `installId`, `entryId` | remove a shortcut |
 * | `launcher-icons` | `installId` | print the icon picker's name list (`[{name, user}]`) |
 * | `launcher-resolve-icon` | `installId`, `value` | print the PNG path an `Icon=` value resolves to (empty line if none) |
 * | `launcher-launch` | `installId`, `entryId`, optional `timeoutMs` | launch a GUI entry like a tap (splash + reserved host); print the launch id |
 * | `launch-state` | `launchId` | print `{state, code, message, taskId, log}` for a launch; `state` is `released` once its splash closed |
 *
 * `launcher-list` mirrors what [me.phie.tawc.launcher.AppsPane] renders: hidden
 * entries are filtered out unless `showHidden=true` (the UI's
 * "Show hidden" toggle), and includes the built-ins. Each element is
 * `{id, name, exec, terminal, icon, iconFile, iconPath, path, shadows,
 * source, overridden, packaged, env, graphics, pointer, hidden, builtin}`
 * — `iconPath` is the resolved on-device PNG (empty when nothing
 * resolved), which is how icon-resolution tests see what `launcher.rs`
 * picked.
 */
internal object LauncherActions {

    fun registerAll() {
        ActionRegistry.register("launcher-list", LauncherListAction)
        ActionRegistry.register("set-entry-override", SetEntryOverrideAction)
        ActionRegistry.register("put-shortcut", PutShortcutAction)
        ActionRegistry.register("delete-shortcut", DeleteShortcutAction)
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
            val entry = LauncherEntry.find(ctx.appContext, inst, entryId)
                ?: return ctx.fail("launcher-launch: no entry '$entryId'")
            if (entry.terminal) return ctx.fail("launcher-launch: '$entryId' is a terminal entry")
            val launch = EntryLauncher.launchGui(
                ctx.appContext,
                method,
                rootfs,
                entry,
                EntryLauncher.graphicsFor(entry),
                timeoutMs,
                EntryLauncher.pointerEmulationFor(entry),
            )
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
            val inst = InstallationStore(ctx.appContext).load(id)
                ?: return ctx.fail("launcher-list: no installation '$id'")
            val out = JSONArray()
            for (e in LauncherEntry.list(ctx.appContext, inst)) {
                if (e.hidden && !showHidden) continue
                out.put(JSONObject().apply {
                    put("id", e.id)
                    put("name", e.name)
                    put("exec", e.exec)
                    put("terminal", e.terminal)
                    put("icon", e.icon)
                    put("iconFile", e.iconFile)
                    put("iconPath", e.iconPath)
                    put("path", e.path)
                    put("shadows", e.shadows)
                    put("source", e.source.name.lowercase())
                    put("overridden", JSONArray(e.overridden.sorted()))
                    put("packaged", JSONObject(e.packaged))
                    put("env", JSONObject(e.env))
                    put("graphics", e.graphics ?: "")
                    put("pointer", e.pointer ?: "")
                    put("hidden", e.hidden)
                    put("builtin", e.builtin != null)
                })
            }
            ctx.out(out.toString())
            return 0
        }
    }

    /** Apply [mutate] to `installId`'s store and print [print] of the
     *  result, or report why not. */
    private fun ActionContext.updateStore(
        action: String,
        args: Map<String, String>,
        print: (LauncherEntries) -> String = { it.toJson() },
        mutate: (LauncherEntries) -> LauncherEntries,
    ): Int {
        val id = args["installId"] ?: return fail("$action: --arg installId=<id> required")
        val written = try {
            LauncherStore(appContext).update(id) { e, _ -> mutate(e) }
        } catch (e: Exception) {
            return fail("$action: ${e.message}")
        } ?: return fail("$action: no launcher store for '$id'")
        out(print(written))
        return 0
    }

    private object SetEntryOverrideAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val entryId = args["entryId"] ?: return ctx.fail("set-entry-override: --arg entryId=<id> required")
            val field = args["field"] ?: return ctx.fail("set-entry-override: --arg field=<name> required")
            val raw = args["value"]
            val value: Any? = when {
                raw == null -> null
                field == "terminal" || field == "hidden" -> raw.toBooleanStrictOrNull()
                    ?: return ctx.fail("set-entry-override: invalid boolean '$raw'")
                field == "env" -> runCatching { JSONObject(raw) }.getOrNull()
                    ?.let { o -> buildMap { for (k in o.keys()) put(k, o.get(k)) } }
                    ?: return ctx.fail("set-entry-override: env must be a JSON object")
                field == "graphics" && GraphicsBackend.fromKeyOrNull(raw) == null ->
                    return ctx.fail("set-entry-override: backend '$raw' not in this build")
                field == "pointer" && PointerEmulation.fromKeyOrNull(raw) == null ->
                    return ctx.fail("set-entry-override: unknown mode '$raw'")
                else -> raw
            }
            return ctx.updateStore("set-entry-override", args) { it.withField(entryId, field, value) }
        }
    }

    private object PutShortcutAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val raw = args["fields"] ?: return ctx.fail("put-shortcut: --arg fields=<JSON object> required")
            val fields = runCatching { EntryFields.fromJson(JSONObject(raw)) }.getOrNull()
                ?: return ctx.fail("put-shortcut: fields must be a JSON object")
            var id = args["entryId"]
            return ctx.updateStore("put-shortcut", args, print = { id!! }) { e ->
                val key = id ?: e.newShortcutId(fields.name ?: fields.exec ?: "")
                id = key
                e.withShortcut(key, fields)
            }
        }
    }

    private object DeleteShortcutAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val entryId = args["entryId"] ?: return ctx.fail("delete-shortcut: --arg entryId=<id> required")
            return ctx.updateStore("delete-shortcut", args) { it.withoutShortcut(entryId) }
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
