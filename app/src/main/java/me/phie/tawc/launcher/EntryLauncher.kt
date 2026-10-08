package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import me.phie.tawc.MainActivity
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
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
 * For GUI entries, stdio is redirected to /dev/null so a chatty program
 * can't fill the pipe back to the JVM (which we never read).
 *
 * No `setsid -f` detach: under proot's `--kill-on-exit` the detached
 * child gets SIGKILLed when the launcher bash exits, so the app dies
 * before it ever opens a Wayland window. Letting runInside block for
 * the program's whole lifetime is the correct behaviour anyway — the
 * program needs the JVM alive for the compositor's Wayland socket, so
 * there's nothing to gain from detaching.
 *
 * Spawn failures (compositor start, Wayland socket wait, the
 * fail-closed bind IOException from startInside) surface via
 * [LaunchErrorActivity] started from the application context — the
 * launching Activity is typically finished by the time they arrive. A
 * nonzero exit of the program itself returns normally and is
 * intentionally not surfaced.
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
        if (entry.terminal) {
            if (method is TawcrootMethod) {
                appContext.startActivity(
                    MainActivity.commandIntent(appContext, inst.id, entry.exec, entry.name.ifEmpty { entry.id })
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
                return
            }
            Log.w(TAG, "terminal entry ${entry.id}: native terminal is tawcroot-only, running headless")
        }
        val rootfs = InstallationStore(appContext).rootfsDir(inst.id).absolutePath
        val cmd = guiCommand(entry.exec)
        LAUNCH_SCOPE.launch {
            runCatching { UserRootfsSession.runInside(appContext, method, rootfs, cmd) }
                .onFailure { e ->
                    Log.w(TAG, "launch ${entry.id}: $e")
                    val title = appContext.getString(
                        R.string.launcher_launch_failed_title,
                        entry.name.ifEmpty { entry.id },
                    )
                    LaunchErrorActivity.start(appContext, title, e.message ?: e.javaClass.simpleName)
                }
        }
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
    internal fun guiCommand(exec: String): String {
        val tail = "</dev/null >/dev/null 2>&1"
        val argv0 = execArgv0(exec) ?: return "$exec $tail"
        val pak = "chrome_100_percent.pak"
        val probe = "_tawc_ns=; if _p=$(command -v -- ${Sh.quote(argv0)}) && " +
            "_p=$(readlink -f -- \"\$_p\") && " +
            "{ [ -e \"\${_p%/*}/$pak\" ] || [ -e \"/usr/lib/\${_p##*/}/$pak\" ]; }; " +
            "then _tawc_ns=--no-sandbox; fi; "
        return "$probe$exec \$_tawc_ns $tail"
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
