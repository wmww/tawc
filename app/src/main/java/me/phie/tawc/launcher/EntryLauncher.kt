package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.MainActivity
import me.phie.tawc.R
import me.phie.tawc.compositor.CompositorActivity
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.MethodRunHelper
import me.phie.tawc.install.Sh
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.UserRootfsSession
import me.phie.tawc.install.distro.DistroRegistry

/**
 * Shared fire-and-forget dispatch of a launcher entry into its rootfs —
 * the single point every launch surface goes through (the home
 * screen's [AppsPane] and pinned shortcuts via [ShortcutLaunchActivity]).
 *
 * `Terminal=true` entries on tawcroot installs open a new terminal tab
 * on the home screen ([MainActivity.commandIntent]) with the entry's
 * Exec as its command instead of a headless spawn — a CLI program run
 * to /dev/null would be invisible. The terminal is tawcroot-only, so
 * proot/chroot keep the headless launch with a logcat warn; those
 * methods are debug-only. The terminal built-ins (TAWC Term, Update
 * packages) open a tab the same way.
 *
 * GUI entries open their window's task at once: a [CompositorActivity]
 * splash whose host is reserved for the program's first window
 * ([LaunchRegistry], notes/launcher.md "Launch splash"). The program's
 * stdout/stderr feed the splash's log view until a window matches, and
 * are drained for the program's whole lifetime after that — a full pipe
 * would block it.
 *
 * No `setsid -f` detach: under proot's `--kill-on-exit` the detached
 * child gets SIGKILLed when the launcher bash exits, so the app dies
 * before it ever opens a Wayland window. Letting runInside block for
 * the program's whole lifetime is the correct behaviour anyway — the
 * program needs the JVM alive for the compositor's Wayland socket, so
 * there's nothing to gain from detaching.
 *
 * Spawn failures (the fail-closed bind IOException from startInside)
 * go to the splash's log view, or, with no splash up, to
 * [LaunchErrorActivity] started from the application context.
 */
object EntryLauncher {

    private const val TAG = "tawc-launcher"

    /**
     * Process-wide scope for fire-and-forget launches. Outlives the
     * launching Activity so closing it doesn't tear down the program
     * the user just started. [SupervisorJob] keeps one failed launch
     * from cancelling sibling launches. [UserRootfsSession.runInside]
     * blocks until the program exits, so each launch pins one IO
     * thread for the program's lifetime.
     */
    private val LAUNCH_SCOPE = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Fire-and-forget launch of [entry] in [inst]'s rootfs. */
    fun launch(appContext: Context, inst: Installation, entry: LauncherEntry) {
        val method = InstallationMethod.forKey(appContext, inst.method)
        if (method == null) {
            // E.g. a proot install opened by a release build (which
            // ships tawcroot only). A silent return here reads as a
            // dead tap; say what's wrong instead.
            Log.w(TAG, "launch ${entry.id}: method '${inst.method}' not in this build")
            LaunchErrorActivity.start(
                appContext,
                appContext.getString(R.string.launcher_launch_failed_title, entry.name.ifEmpty { entry.id }),
                appContext.getString(R.string.launcher_method_unavailable, inst.method),
            )
            return
        }
        val builtin = entry.builtin
        if (builtin != null) {
            launchBuiltin(appContext, inst, entry, builtin, method)
            return
        }
        val graphics = graphicsFor(inst, entry.id)
        if (entry.terminal) {
            if (method is TawcrootMethod) {
                appContext.startActivity(
                    MainActivity.commandIntent(appContext, inst.id, entry.exec, entry.name.ifEmpty { entry.id }, graphics)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
                return
            }
            Log.w(TAG, "terminal entry ${entry.id}: native terminal is tawcroot-only, running headless")
        }
        launchGui(appContext, method, InstallationStore(appContext).rootfsDir(inst.id).absolutePath, entry, graphics)
    }

    /** GUI launch behind a splash; returns its [Launch] (for the dev broker). */
    internal fun launchGui(
        appContext: Context,
        method: InstallationMethod,
        rootfs: String,
        entry: LauncherEntry,
        graphics: GraphicsBackend?,
        timeoutMs: Long = LaunchRegistry.DEFAULT_TIMEOUT_MS,
    ): Launch {
        val name = entry.name.ifEmpty { entry.id }
        val launch = LaunchRegistry.create(entry.id, name, entry.iconPath, entry.terminal, timeoutMs)
        val splash = runCatching { appContext.startActivity(CompositorActivity.launchIntent(appContext, launch.id)) }
            .onFailure { Log.w(TAG, "launch ${entry.id}: no splash: $it") }
            .isSuccess
        if (!splash) LaunchRegistry.release(launch.id)
        LAUNCH_SCOPE.launch {
            val token = if (splash) LaunchRegistry.reserve(appContext, launch) else null
            if (token == null) LaunchRegistry.dispatch(launch.id, LaunchEvent.Detached)
            val proc = try {
                UserRootfsSession.startInside(appContext, method, rootfs, guiCommand(entry.exec, token), graphics)
            } catch (e: Exception) {
                Log.w(TAG, "launch ${entry.id}: $e")
                val message = e.message ?: e.javaClass.simpleName
                if (token != null && launch.attached) {
                    LaunchRegistry.dispatch(launch.id, LaunchEvent.Failed(message))
                } else {
                    LaunchRegistry.dispatch(launch.id, LaunchEvent.Detached)
                    LaunchErrorActivity.start(
                        appContext,
                        appContext.getString(R.string.launcher_launch_failed_title, name),
                        message,
                    )
                }
                return@launch
            }
            // startInside wraps the launch in setsid, which execs (the JVM
            // child is no group leader), so the session id is the pid.
            val sid = MethodRunHelper.pidOf(proc)
            if (token != null && sid > 0) NativeBridge.nativeUpdateLaunch(launch.id, sid, false)
            val code = runCatching {
                MethodRunHelper.collectProcess(proc, launch::appendLog, keepOutput = false).exitCode
            }.getOrElse { -1 }
            if (token != null) NativeBridge.nativeUpdateLaunch(launch.id, sid, true)
            val alive = sid > 0 && LaunchRegistry.sessionAlive(sid)
            LaunchRegistry.dispatch(launch.id, LaunchEvent.Exited(code, alive))
        }
        return launch
    }

    /**
     * Shell for a headless GUI launch. Chromium-family apps (Chromium,
     * Chrome, ChatGPT, Electron) refuse to run as root without
     * `--no-sandbox`, and their sandbox can't come up here anyway; only
     * Electron reads `ELECTRON_DISABLE_SANDBOX`. So resolve argv0 in the
     * guest and append the flag when the real binary sits next to
     * `chrome_100_percent.pak`, or `/usr/lib/<name>/` holds one (distro
     * chromium wrappers).
     */
    internal fun guiCommand(exec: String, activationToken: String? = null): String {
        val tail = "</dev/null 2>&1"
        val env = activationToken?.let {
            val t = Sh.quote(it)
            "export XDG_ACTIVATION_TOKEN=$t DESKTOP_STARTUP_ID=$t; "
        } ?: ""
        // Probe the program, not a leading `env K=V` (the editor's variables).
        val argv0 = execArgv0(DesktopEntryFile.splitExec(exec).command) ?: return "$env$exec $tail"
        val pak = "chrome_100_percent.pak"
        val probe = "_tawc_ns=; if _p=$(command -v -- ${Sh.quote(argv0)}) && " +
            "_p=$(readlink -f -- \"\$_p\") && " +
            "{ [ -e \"\${_p%/*}/$pak\" ] || [ -e \"/usr/lib/\${_p##*/}/$pak\" ]; }; " +
            "then _tawc_ns=--no-sandbox; fi; "
        return "$env$probe$exec \$_tawc_ns $tail"
    }

    /** First word of a desktop-entry Exec line (spec quoting), or null. */
    internal fun execArgv0(exec: String): String? {
        val s = exec.trimStart()
        if (s.isEmpty()) return null
        if (s[0] != '"') return s.takeWhile { !it.isWhitespace() }
        val out = StringBuilder()
        var i = 1
        while (i < s.length) {
            val c = s[i]
            when {
                c == '\\' && i + 1 < s.length -> { out.append(s[i + 1]); i += 2 }
                c == '"' -> return out.toString().ifEmpty { null }
                else -> { out.append(c); i++ }
            }
        }
        return null
    }

    /**
     * [inst]'s graphics override for [entryId] ([Installation.entryGraphics]),
     * or null for the global setting — also when the stored backend
     * isn't in this build.
     */
    fun graphicsFor(inst: Installation, entryId: String): GraphicsBackend? =
        GraphicsBackend.fromKeyOrNull(inst.entryGraphics[entryId])

    /** A terminal built-in: a new tab (a plain shell for TAWC Term). Add
     *  entry is the apps pane's own (it wants the editor's result). */
    private fun launchBuiltin(
        appContext: Context,
        inst: Installation,
        entry: LauncherEntry,
        builtin: LauncherEntry.Builtin,
        method: InstallationMethod,
    ) {
        val exec = when (builtin) {
            LauncherEntry.Builtin.TERM -> null
            LauncherEntry.Builtin.UPDATE -> DistroRegistry.forInstallation(inst)?.upgradeCommand
            LauncherEntry.Builtin.ADD_ENTRY -> return
        }
        if (method !is TawcrootMethod || (builtin == LauncherEntry.Builtin.UPDATE && exec == null)) {
            LaunchErrorActivity.start(
                appContext,
                appContext.getString(R.string.launcher_launch_failed_title, entry.name),
                appContext.getString(R.string.launcher_builtin_unavailable),
            )
            return
        }
        val label = if (builtin == LauncherEntry.Builtin.TERM) null else entry.name
        appContext.startActivity(
            MainActivity.commandIntent(appContext, inst.id, exec, label).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }
}
