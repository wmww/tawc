package me.phie.tawc.install

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.verticalLp

/**
 * "Install new distro" screen. Shows a read-only summary of what's
 * about to be installed (distro, detected CPU arch, install path) until
 * the user taps Install, then swaps to a live progress + log view
 * bound to [InstallationService] via [OperationLogPanel].
 *
 * `am start … --es autoStart true --es id <id>` skips the form and
 * triggers the install immediately (used by the `am start` install hook
 * documented in `notes/installation.md`). The autoStart fires at most
 * once per launch (`savedInstanceState == null`); a fresh `am start`
 * delivers a new launch intent and re-fires, but a process-death
 * recreation does not. Even if the gate ever leaked, the request would
 * be refused at the [InstallationService] level — this is the UX
 * shortcut, not the safety net.
 */
class InstallActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var formSection: LinearLayout
    private lateinit var installButton: MaterialButton
    private lateinit var panel: OperationLogPanel

    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH
        started = savedInstanceState?.getBoolean(KEY_STARTED) == true

        val scaffold = buildChildScreen("Install distro")

        val pad = (16 * resources.displayMetrics.density).toInt()
        formSection = buildFormSection(pad)
        scaffold.content.addView(formSection, verticalLp(MATCH_PARENT, WRAP_CONTENT))

        panel = OperationLogPanel(this)
        panel.view.visibility = if (started) View.VISIBLE else View.GONE
        if (started) formSection.visibility = View.GONE
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        setContentView(scaffold.root)

        // Fire autoStart only on the very first onCreate of this
        // activity instance. Re-creations restore [started]=true from
        // savedInstanceState and skip this path. A fresh `am start`
        // produces a null savedInstanceState (Android creates a new
        // activity instance), so the CLI keeps working.
        if (savedInstanceState == null && autoStartRequested(intent)) {
            beginInstall()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        targetId = intent.getStringExtra(EXTRA_ID) ?: targetId
        if (autoStartRequested(intent)) {
            beginInstall()
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(KEY_STARTED, started)
    }

    // `am start --es autoStart true` sends a string extra; `--ez autoStart
    // true` sends a boolean. Accept either so the CLI is forgiving.
    private fun autoStartRequested(intent: Intent?): Boolean {
        intent ?: return false
        if (intent.getBooleanExtra(EXTRA_AUTO_START, false)) return true
        return intent.getStringExtra(EXTRA_AUTO_START)?.equals("true", ignoreCase = true) == true
    }

    override fun onStart() {
        super.onStart()
        panel.bindToService()
    }

    override fun onStop() {
        super.onStop()
        panel.unbind()
    }

    private fun buildFormSection(pad: Int): LinearLayout {
        val s = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        // Resolve the host's default Distro so the form shows the
        // exact thing that's about to be installed (display name +
        // Linux arch label). If no Distro matches the host we still
        // render the form — the install button click is the
        // authoritative gate and will reject with a readable error.
        val distro = DistroRegistry.defaultForHost()
        val distroLabel = distro?.displayName ?: "(no supported distro for this device)"
        val archLabel = distro?.linuxArch ?: "(unknown)"

        s.addView(formRow("Distro:", distroLabel), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        s.addView(formRow("Architecture:", archLabel), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        s.addView(
            formRow("Install location:", store.installationDir(targetId).absolutePath),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )

        installButton = primaryButton("Install") { beginInstall() }
        // Mirror the service-level gate so the form makes the refusal
        // obvious before the user taps. The service is still the
        // source of truth — the button is just a hint.
        val current = store.load(targetId)
        if (current != null) {
            installButton.isEnabled = false
            installButton.text = when (current.state) {
                Installation.State.READY -> "Install (already installed — delete first)"
                Installation.State.INSTALLING -> "Install (in progress — delete to abort)"
                Installation.State.UNINSTALLING -> "Install (delete in progress)"
                Installation.State.FAILED -> "Install (failed — delete first)"
            }
        }
        s.addView(installButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        return s
    }

    private fun formRow(label: String, value: String): LinearLayout {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val l = TextView(this).apply { text = label; textSize = 14f }
        val v = TextView(this).apply { text = value; textSize = 14f; typeface = Typeface.MONOSPACE }
        row.addView(l, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginEnd = 16 })
        row.addView(v, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        return row
    }

    private fun beginInstall() {
        if (!Su.rootAvailable()) {
            // Surface the error in the log area so the form stays visible
            // for the user to read the install location alongside.
            formSection.visibility = View.GONE
            panel.view.visibility = View.VISIBLE
            panel.setStatus("ERROR: root (su) not available — Magisk must grant this app.")
            return
        }
        formSection.visibility = View.GONE
        panel.view.visibility = View.VISIBLE
        // [InstallationService] is the authoritative gate; we just hand
        // off and let it decide whether to run or reject. `started` only
        // tracks UI state (form vs panel) so a process-death recreate
        // restores the panel view.
        panel.appendLog(if (started) "[ui] re-requesting install of '$targetId'"
                        else "[ui] starting install of '$targetId'")
        started = true
        InstallationService.startInstall(this, targetId)
    }

    companion object {
        const val EXTRA_ID = "id"
        const val EXTRA_AUTO_START = "autoStart"
        private const val KEY_STARTED = "tawc.install.started"
    }
}
