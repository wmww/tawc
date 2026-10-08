package me.phie.tawc.session

import android.content.Context
import android.util.Log
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.ChrootMethod
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ops.OperationsRegistry
import me.phie.tawc.remote.RemoteSession
import me.phie.tawc.tasks.ProcessScanner
import me.phie.tawc.terminal.TerminalSessions
import me.phie.tawc.terminal.kill
import kotlin.concurrent.thread

/**
 * Notification "Exit": kill everything in every rootfs. One notification
 * stands for every reason, so a partial exit would leave it up. Holds are
 * not released here — each follows its own process/session/compositor
 * down, and [SessionService] stops once the last one is gone.
 */
internal object SessionExit {
    private const val TAG = "tawc"

    /** Main thread. */
    fun killEverything(context: Context) {
        Log.i(TAG, "Session exit requested")
        // Shells die through the normal path: the exit closes the tab
        // (or the detached client drops the entry) and releases the hold.
        for (session in TerminalSessions.all()) session.kill()
        RemoteSession.stop()
        CompositorService.stop()
        val app = context.applicationContext
        // Rescans stop once anything new takes a hold: whatever the user
        // starts right after Exit must not be caught by a later pass.
        val acquisitions = SessionHolds.acquisitions
        thread(name = "tawc-session-exit", isDaemon = true) {
            val store = InstallationStore(app)
            for (install in store.list()) {
                // An installer's processes are not ours to kill.
                if (install.state != Installation.State.READY || hasLiveInstallOp(install.id)) continue
                try {
                    ProcessScanner.killAllInRootfs(
                        rootfsPath = store.rootfsDir(install.id).absolutePath,
                        installId = install.id,
                        includeChroot = install.method == ChrootMethod.KEY,
                        keepGoing = { SessionHolds.acquisitions == acquisitions },
                        log = {},
                    )
                } catch (t: Throwable) {
                    Log.w(TAG, "exit: kill in ${install.id} failed", t)
                }
            }
            SessionService.rescanStrays()
        }
    }

    private fun hasLiveInstallOp(id: String): Boolean =
        OperationsRegistry.get("install:$id") != null ||
            OperationsRegistry.get("import:$id") != null ||
            OperationsRegistry.get("uninstall:$id") != null
}
