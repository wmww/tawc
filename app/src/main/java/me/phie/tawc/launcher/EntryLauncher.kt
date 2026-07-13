package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.UserRootfsSession
import me.phie.tawc.terminal.TerminalActivity

/**
 * Shared fire-and-forget dispatch of a launcher entry into its rootfs —
 * the single point every launch surface goes through ([LauncherActivity]
 * today; home-screen shortcuts later).
 *
 * `Terminal=true` entries on tawcroot installs open [TerminalActivity]
 * with the entry's Exec as a command tab instead of a headless spawn —
 * a CLI program run to /dev/null would be invisible. The terminal is
 * tawcroot-only (see the gate in MainActivity), so proot/chroot keep
 * the headless launch with a logcat warn; those methods are debug-only.
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
        if (entry.terminal) {
            if (method is TawcrootMethod) {
                appContext.startActivity(
                    Intent(appContext, TerminalActivity::class.java)
                        .putExtra(TerminalActivity.EXTRA_ID, inst.id)
                        .putExtra(TerminalActivity.EXTRA_COMMAND, entry.exec)
                        .putExtra(TerminalActivity.EXTRA_LABEL, entry.name.ifEmpty { entry.id })
                        // Per-distro document URI — see the manifest
                        // comment on TerminalActivity.
                        .setData(Uri.parse("tawc://terminal/${inst.id}"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
                return
            }
            Log.w(TAG, "terminal entry ${entry.id}: native terminal is tawcroot-only, running headless")
        }
        val rootfs = InstallationStore(appContext).rootfsDir(inst.id).absolutePath
        val cmd = "${entry.exec} </dev/null >/dev/null 2>&1"
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
}
