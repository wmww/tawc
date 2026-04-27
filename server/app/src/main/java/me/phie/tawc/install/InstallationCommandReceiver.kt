package me.phie.tawc.install

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Lets external tooling (adb shell, integration tests, …) drive the
 * installation system without going through the UI. Output is sent to
 * logcat under the `tawc-install` tag.
 *
 * INSTALL / UNINSTALL are deliberately NOT broadcasts: they need a
 * foreground-service start, which background broadcast receivers can't
 * do reliably on Android 14+ (BAL_BLOCK from cold). Use `am start`
 * straight into [ManageInstallationsActivity] — Activities have full
 * FGS-launch privileges:
 *
 *   adb shell am start \
 *       -n me.phie.tawc/.install.ManageInstallationsActivity \
 *       --es autoAction install --es id arch
 *
 *   adb shell am start \
 *       -n me.phie.tawc/.install.ManageInstallationsActivity \
 *       --es autoAction uninstall --es id arch
 *
 * The cheap, synchronous operations stay on the receiver:
 *
 *   adb shell am broadcast -W \
 *       -n me.phie.tawc/.install.InstallationCommandReceiver \
 *       -a me.phie.tawc.install.LIST
 *
 *   adb shell am broadcast -W \
 *       -n me.phie.tawc/.install.InstallationCommandReceiver \
 *       -a me.phie.tawc.install.RUN --es id arch --es cmd 'uname -m'
 *
 * RUN uses `goAsync()` so the chroot command can outlive the receiver's
 * ~10s ANR budget. Mounts come up inside the RUN's su shell and
 * disappear with it — same model as `client/arch-chroot-run`.
 */
class InstallationCommandReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val app = context.applicationContext
        val id = intent.getStringExtra("id") ?: Installation.DISTRO_ARCH
        val store = InstallationStore(app)
        val rootfs = store.rootfsDir(id).absolutePath
        Log.i(TAG, "command: ${intent.action} id=$id")

        when (intent.action) {
            ACTION_LIST -> {
                val installs = store.list()
                if (installs.isEmpty()) {
                    Log.i(TAG, "LIST: no installations")
                    setResult(0, "no installations", null)
                } else {
                    val text = installs.joinToString("\n") {
                        "${it.id} (distro=${it.distro} arch=${it.arch} method=${it.method})"
                    }
                    Log.i(TAG, "LIST:\n$text")
                    setResult(0, text, null)
                }
            }

            ACTION_RUN -> {
                val cmd = intent.getStringExtra("cmd")
                if (cmd.isNullOrEmpty()) {
                    Log.w(TAG, "RUN missing --es cmd '<command>'")
                    setResult(1, "missing --es cmd", null)
                    return
                }
                // BroadcastReceiver.onReceive runs on the main thread with a
                // ~10s ANR budget; ChrootRunner.run can block much longer
                // (pacman, builds, …). goAsync hands the result back when
                // the worker thread finishes so `am broadcast -W` still
                // sees the exit code + output.
                val pending = goAsync()
                Thread({
                    try {
                        val r = ChrootRunner.run(rootfs, cmd)
                        Log.i(TAG, "RUN exit=${r.exitCode}\n${r.output}")
                        pending.setResult(if (r.ok) 0 else 1, r.output, null)
                    } catch (t: Throwable) {
                        Log.e(TAG, "RUN failed", t)
                        pending.setResult(1, "RUN failed: ${t.message}", null)
                    } finally {
                        pending.finish()
                    }
                }, "tawc-install-run").start()
            }

            else -> Log.w(TAG, "Unknown action: ${intent.action}")
        }
    }

    companion object {
        private const val TAG = "tawc-install"
        const val ACTION_LIST = "me.phie.tawc.install.LIST"
        const val ACTION_RUN = "me.phie.tawc.install.RUN"
        const val EXTRA_ID = "id"
        // Read by ManageInstallationsActivity when launched via `am start
        // … --es autoAction install|uninstall`. Lives here so the receiver
        // and the activity agree on the contract.
        const val EXTRA_AUTO_ACTION = "autoAction"
    }
}
