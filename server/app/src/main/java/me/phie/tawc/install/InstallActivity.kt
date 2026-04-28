package me.phie.tawc.install

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
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
    private var selectedMethod: String? = null

    private lateinit var formSection: LinearLayout
    private lateinit var methodGroup: RadioGroup
    private lateinit var installButton: MaterialButton
    private lateinit var panel: OperationLogPanel

    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH
        // Reject hostile `--es id` extras early — the activity is
        // exported, so any installed app can launch it. The
        // [InstallationService] gate also enforces this, but failing
        // here keeps `installationDir(id)` (which is rendered in the
        // form) out of attacker reach.
        if (!Installation.isValidId(targetId)) {
            android.util.Log.w("tawc-install", "InstallActivity: rejected invalid id '$targetId'")
            finish()
            return
        }
        // Saved state wins over the launch intent so a user's radio
        // flip survives rotation: Android re-delivers the original
        // intent on recreation, which would otherwise shadow the
        // saved selection.
        selectedMethod = savedInstanceState?.getString(KEY_METHOD)
            ?: intent?.getStringExtra(EXTRA_METHOD)
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
        val newId = intent.getStringExtra(EXTRA_ID) ?: targetId
        if (!Installation.isValidId(newId)) {
            android.util.Log.w("tawc-install", "InstallActivity: rejected invalid id '$newId' on re-intent")
            return
        }
        targetId = newId
        if (autoStartRequested(intent)) {
            beginInstall()
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(KEY_STARTED, started)
        selectedMethod?.let { outState.putString(KEY_METHOD, it) }
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

        // Method picker. The radio default reflects the host: chroot if
        // `su` is available, proot otherwise. The `--es method ...`
        // intent extra (or saved instance state) overrides.
        s.addView(buildMethodPicker(), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

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

    /**
     * Build the method picker (chroot / proot radio group). Default
     * follows host capability — `su` available picks chroot, otherwise
     * proot. The intent extra `method` and saved-instance state both
     * override the default; the user can still flip the radio after.
     */
    private fun buildMethodPicker(): LinearLayout {
        val container = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = TextView(this).apply { text = "Install method:"; textSize = 14f }
        container.addView(title, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        methodGroup = RadioGroup(this).apply { orientation = RadioGroup.HORIZONTAL }
        val rootAvailable = Su.rootAvailable()

        // Use generateViewId() rather than hand-picked constants —
        // any literal we'd reach for in the AAPT range collides with
        // future R.id.* once we add a layout XML.
        val chrootId = View.generateViewId()
        val prootId = View.generateViewId()

        val chroot = RadioButton(this).apply {
            id = chrootId
            text = "chroot (root)"
            // The chroot path needs su; greyed-out on devices without
            // root makes the limitation visible without surprising
            // the user mid-install.
            isEnabled = rootAvailable
        }
        val proot = RadioButton(this).apply {
            id = prootId
            text = "proot (rootless)"
        }
        methodGroup.addView(chroot, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginEnd = 16 })
        methodGroup.addView(proot, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        container.addView(methodGroup, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Initial selection: explicit override → use it; else default
        // for host (chroot if su works, proot if not).
        val initial = selectedMethod
            ?: if (rootAvailable) ChrootMethod.KEY else ProotMethod.KEY
        methodGroup.check(if (initial == ChrootMethod.KEY) chrootId else prootId)
        selectedMethod = initial

        methodGroup.setOnCheckedChangeListener { _, checkedId ->
            selectedMethod = when (checkedId) {
                chrootId -> ChrootMethod.KEY
                prootId -> ProotMethod.KEY
                else -> selectedMethod
            }
        }
        return container
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
        // Only the chroot path needs `su`. Proot is rootless by
        // definition, so a missing-root device fails this check only
        // if the user picked chroot anyway. We surface the "no su"
        // message specifically; other "method not available" reasons
        // are caught at service-gate level.
        val methodKey = selectedMethod ?: ChrootMethod.KEY
        if (methodKey == ChrootMethod.KEY && !Su.rootAvailable()) {
            formSection.visibility = View.GONE
            panel.view.visibility = View.VISIBLE
            panel.setStatus("ERROR: root (su) not available — pick proot, or grant Magisk root.")
            return
        }
        formSection.visibility = View.GONE
        panel.view.visibility = View.VISIBLE
        // [InstallationService] is the authoritative gate; we just hand
        // off and let it decide whether to run or reject. `started` only
        // tracks UI state (form vs panel) so a process-death recreate
        // restores the panel view.
        panel.appendLog(if (started) "[ui] re-requesting install of '$targetId' via $methodKey"
                        else "[ui] starting install of '$targetId' via $methodKey")
        started = true
        InstallationService.startInstall(this, targetId, methodKey)
    }

    companion object {
        const val EXTRA_ID = "id"
        const val EXTRA_METHOD = "method"
        const val EXTRA_AUTO_START = "autoStart"
        private const val KEY_STARTED = "tawc.install.started"
        private const val KEY_METHOD = "tawc.install.method"
    }
}
