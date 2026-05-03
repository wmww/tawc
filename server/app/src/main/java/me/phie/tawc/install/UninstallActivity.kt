package me.phie.tawc.install

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import androidx.appcompat.app.AppCompatActivity
import android.widget.LinearLayout
import me.phie.tawc.ui.buildChildScreen

/**
 * Live "uninstall in progress" screen — just an [OperationLogPanel]
 * bound to [InstallationService]. The Are-You-Sure step lives on
 * [DistroInfoActivity] as an AlertDialog now, so this activity has
 * no in-page form: it kicks off the uninstall when launched with
 * `--es autoStart true` and watches the service from there.
 *
 * **Opening the activity is not a trigger.** Like [InstallActivity],
 * the uninstall fires only when the launching intent explicitly
 * carries `autoStart=true` (see [requestsAutoStart] / [EXTRA_AUTO_START]).
 * Otherwise the page just renders the panel showing the bound service's
 * last operation status — opening from the app switcher or a recents
 * card never re-runs an operation that already finished. The intent
 * extra is paired with `savedInstanceState == null` so a config-change
 * recreate doesn't re-fire either.
 *
 * `am start … --es autoStart true --es id <id>` is the headless / test
 * entry point and the same shape DistroInfoActivity's "Delete" dialog
 * uses internally.
 */
class UninstallActivity : AppCompatActivity() {

    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var panel: OperationLogPanel
    private lateinit var scaffold: me.phie.tawc.ui.Scaffold

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH
        // Reject hostile `--es id` extras early; UninstallActivity is
        // exported. [InstallationService] re-validates as the gate.
        if (!Installation.isValidId(targetId)) {
            android.util.Log.w("tawc-install", "UninstallActivity: rejected invalid id '$targetId'")
            finish()
            return
        }

        scaffold = buildChildScreen("Delete")
        panel = OperationLogPanel(this)
        panel.view.visibility = View.VISIBLE
        // No confirm dialog here. The user might be tapping Cancel
        // quickly to stop a delete they didn't mean to start; another
        // dialog in the way risks finishing the wipe before they can
        // confirm. The state machine catches this safely:
        // UNINSTALLING → FAILED leaves whatever survives on disk for
        // a manual recovery, and the two-pass wipe in
        // RootfsCleaner / ProotMethod keeps metadata.json around
        // until the rootfs is fully gone.
        panel.onCancelClicked = {
            val svc = panel.boundService
            if (svc == null) {
                panel.appendLog("[ui] cancel ignored: service not bound yet")
            } else {
                panel.appendLog("[ui] user requested uninstall cancel")
                svc.cancelUninstall(targetId)
            }
        }
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        setContentView(scaffold.root)

        // Fire the uninstall only on a first-time onCreate that's also
        // carrying an explicit autoStart trigger. Recreations (rotation,
        // process-death restore) re-deliver the launch intent — the
        // savedInstanceState gate is what stops the trigger from being
        // re-honoured.
        if (savedInstanceState == null && intent.requestsAutoStart()) {
            beginUninstall(reLaunch = false)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val newId = intent.getStringExtra(EXTRA_ID) ?: targetId
        if (!Installation.isValidId(newId)) {
            android.util.Log.w("tawc-install", "UninstallActivity: rejected invalid id '$newId' on re-intent")
            return
        }
        targetId = newId
        if (intent.requestsAutoStart()) {
            beginUninstall(reLaunch = true)
        }
    }

    override fun onStart() {
        super.onStart()
        panel.bindToService()
    }

    override fun onStop() {
        super.onStop()
        panel.unbind()
    }

    private fun beginUninstall(reLaunch: Boolean) {
        // su is only needed for chroot uninstalls (mount unwinding,
        // killing chroot processes by root-owned /proc/<pid>/root).
        // Proot installs are app-uid-owned end-to-end, so an
        // unprivileged uninstall works. Look at the recorded method
        // before deciding.
        val installation = InstallationStore(this).load(targetId)
        val method = installation?.method ?: Installation.METHOD_CHROOT
        if (method == Installation.METHOD_CHROOT && !Su.rootAvailable()) {
            panel.setStatus("ERROR: root (su) not available — chroot uninstalls need it.")
            return
        }
        // Drop any replay-cached lines from a previous operation that
        // bound between onStart and now (same reason as InstallActivity).
        panel.clearLog()
        // [InstallationService] is the authoritative gate; we just
        // hand off and let it decide whether to run or reject.
        panel.appendLog(if (reLaunch) "[ui] re-requesting uninstall of '$targetId'"
                        else "[ui] starting uninstall of '$targetId'")
        InstallationService.startUninstall(this, targetId)
    }

    companion object {
        const val EXTRA_ID = "id"
    }
}
