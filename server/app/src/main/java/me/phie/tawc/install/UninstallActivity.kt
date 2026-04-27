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
 * no in-page form: it kicks off the uninstall in `onCreate` and
 * watches the service from there. `am start … --es id <id>` is the
 * headless/test entry point.
 */
class UninstallActivity : AppCompatActivity() {

    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var panel: OperationLogPanel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH

        val scaffold = buildChildScreen("Delete")
        panel = OperationLogPanel(this)
        panel.view.visibility = View.VISIBLE
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        setContentView(scaffold.root)

        // Only kick off on the very first onCreate; a process-death
        // recreate restores from savedInstanceState and just re-binds
        // to the (still-running) service.
        if (savedInstanceState == null) {
            beginUninstall(reLaunch = false)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        targetId = intent.getStringExtra(EXTRA_ID) ?: targetId
        beginUninstall(reLaunch = true)
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
        if (!Su.rootAvailable()) {
            panel.setStatus("ERROR: root (su) not available — Magisk must grant this app.")
            return
        }
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
