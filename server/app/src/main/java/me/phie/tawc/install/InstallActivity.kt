package me.phie.tawc.install

import android.content.DialogInterface
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import me.phie.tawc.R
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.verticalLp

/**
 * "Install new distro" screen. Form for picking distro, label, and
 * install method, then swaps to a live progress + log view bound to
 * [InstallationService] via [OperationLogPanel] once the user taps
 * Install.
 *
 * Each visit creates a fresh activity instance and lands on the form —
 * a slot already being installed at the same id is the
 * [InstallationService] gate's job to refuse, not this activity's job
 * to forward to. After a finished install the user backs out to the
 * home screen, so reopening this activity always starts a brand-new
 * form rather than re-showing the previous run's log.
 *
 * `am start … --es autoStart true --es id <id>` skips the form and
 * triggers the install immediately (used by the `am start` install hook
 * documented in `notes/installation.md`). The autoStart fires at most
 * once per launch (`savedInstanceState == null`); a fresh `am start`
 * delivers a new launch intent and re-fires, but a process-death
 * recreation does not.
 */
class InstallActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var selectedMethod: String? = null
    private var selectedDistro: String? = null
    private var labelEdited: Boolean = false

    private lateinit var formScroll: ScrollView
    private lateinit var formSection: LinearLayout
    private lateinit var methodGroup: RadioGroup
    private lateinit var distroGroup: RadioGroup
    private lateinit var labelField: EditText
    private lateinit var locationLabel: TextView
    private lateinit var installButton: MaterialButton
    private lateinit var panel: OperationLogPanel
    private lateinit var scaffold: me.phie.tawc.ui.Scaffold

    /**
     * Resolved id for the Install button. Tracks (label → slug → unique)
     * so the service-call site doesn't have to re-derive it; null when
     * the label is empty / unslugifiable / collides with an existing
     * install (Install button is also disabled in that state).
     */
    private var resolvedId: String? = null

    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Saved state wins over the launch intent so a user's radio
        // flip survives rotation: Android re-delivers the original
        // intent on recreation, which would otherwise shadow the
        // saved selection.
        selectedMethod = savedInstanceState?.getString(KEY_METHOD)
            ?: intent?.getStringExtra(EXTRA_METHOD)
        selectedDistro = savedInstanceState?.getString(KEY_DISTRO)
            ?: intent?.getStringExtra(EXTRA_DISTRO)
        labelEdited = savedInstanceState?.getBoolean(KEY_LABEL_EDITED) == true
        started = savedInstanceState?.getBoolean(KEY_STARTED) == true

        scaffold = buildChildScreen("Install")

        val pad = (16 * resources.displayMetrics.density).toInt()
        formSection = buildFormSection(pad, savedInstanceState?.getString(KEY_LABEL_TEXT))
        // Wrap the form in a ScrollView so the soft keyboard can lift
        // the EditText into view without ever covering the Install
        // button on a small phone. The scaffold's content column is
        // MATCH_PARENT, so the scroll view fills it; the inner
        // formSection is WRAP_CONTENT and grows naturally.
        formScroll = ScrollView(this).apply {
            isFillViewport = true
            addView(formSection, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        scaffold.content.addView(formScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        panel = OperationLogPanel(this)
        panel.view.visibility = if (started) View.VISIBLE else View.GONE
        if (started) formScroll.visibility = View.GONE
        // Cancel during install requires a confirm: it triggers a
        // follow-up uninstall (INSTALLING → FAILED → UNINSTALLING),
        // which wipes the freshly-laid-down rootfs. There's no user
        // data at risk yet (gate guarantees an empty slot at install
        // start) but the time loss alone is worth a confirm tap.
        //
        // After a cancelled install, the service flips into
        // UNINSTALLING for the follow-up wipe — at that point a
        // second tap of the still-visible Cancel button should behave
        // like UninstallActivity's Cancel: no confirm dialog, just
        // abort the wipe directly. Dispatch on the service's current
        // job kind so we do the right thing in both phases.
        panel.onCancelClicked = { dispatchCancel() }
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        setContentView(scaffold.root)

        // Fire autoStart only on the very first onCreate of this
        // activity instance. Re-creations restore [started]=true from
        // savedInstanceState and skip this path. A fresh `am start`
        // produces a null savedInstanceState (Android creates a new
        // activity instance), so the CLI keeps working.
        if (savedInstanceState == null && intent.requestsAutoStart()) {
            val explicitId = intent?.getStringExtra(EXTRA_ID)
            if (explicitId != null && Installation.isValidId(explicitId)) {
                resolvedId = explicitId
            }
            beginInstall()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.requestsAutoStart()) {
            val explicitId = intent.getStringExtra(EXTRA_ID)
            if (explicitId != null && Installation.isValidId(explicitId)) {
                resolvedId = explicitId
            }
            beginInstall()
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(KEY_STARTED, started)
        outState.putBoolean(KEY_LABEL_EDITED, labelEdited)
        // Guard the late-init lookup the same way [revalidate] does —
        // saving state can in principle fire before the form is built
        // if a future onCreate path bails early.
        if (::labelField.isInitialized) {
            outState.putString(KEY_LABEL_TEXT, labelField.text.toString())
        }
        selectedMethod?.let { outState.putString(KEY_METHOD, it) }
        selectedDistro?.let { outState.putString(KEY_DISTRO, it) }
    }

    override fun onStart() {
        super.onStart()
        panel.bindToService()
    }

    override fun onStop() {
        super.onStop()
        panel.unbind()
    }

    private fun buildFormSection(pad: Int, savedLabelText: String?): LinearLayout {
        val s = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        // List the distros that match the host's primary ABI. Empty
        // list means no Distro supports this device; render an
        // explanatory line rather than a dead radio group, and let
        // the service-level gate refuse the install if the user taps
        // anyway. The `--es distro …` extra / saved state nudges the
        // initial selection.
        val available = DistroRegistry.availableForHost()
        val initialDistro = (selectedDistro ?: available.firstOrNull()?.key)
        selectedDistro = initialDistro

        s.addView(buildDistroPicker(available, pad), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        s.addView(buildInstallDirField(available, savedLabelText), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        // Method picker. Defaults to tawcroot (the recommended method);
        // the `--es method ...` intent extra and saved instance state
        // both override.
        s.addView(buildMethodPicker(), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))

        // "What's the difference?" link to the install method info
        // page. Borderless text button so it reads as a help affordance,
        // not a primary action.
        s.addView(
            MaterialButton(this, null, com.google.android.material.R.attr.borderlessButtonStyle).apply {
                text = "What's the difference?"
                setTextColor(getColor(R.color.tawc_accent))
                setOnClickListener {
                    startActivity(Intent(this@InstallActivity, InstallMethodInfoActivity::class.java))
                }
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT, bottomMargin = pad),
        )

        installButton = primaryButton("Install") { beginInstall() }
        s.addView(installButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))

        // Initial validation pass — populates resolvedId, location row,
        // and Install button enabled-state from the default label.
        revalidate()
        return s
    }

    /**
     * Build the distro picker. Lists every [Distro] whose Android ABI
     * matches the host. Defaults to the first ABI-matching entry
     * unless `--es distro …` / saved state nudges otherwise. Hidden
     * when only one distro matches (the first-class case before
     * Manjaro) so we don't render a single-choice radio group.
     */
    private fun buildDistroPicker(available: List<Distro>, pad: Int): LinearLayout {
        val container = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = TextView(this).apply { text = "Distro"; textSize = 14f }
        container.addView(title, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        if (available.isEmpty()) {
            val msg = TextView(this).apply {
                text = "(no supported distro for this device)"
                textSize = 14f
                typeface = Typeface.MONOSPACE
            }
            container.addView(msg, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            return container
        }

        distroGroup = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
        val idsByKey = mutableMapOf<Int, String>()
        for (d in available) {
            val rid = View.generateViewId()
            idsByKey[rid] = d.key
            val rb = RadioButton(this).apply {
                id = rid
                text = d.displayName
            }
            distroGroup.addView(rb, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        }
        container.addView(distroGroup, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        val initialKey = selectedDistro?.takeIf { k -> available.any { it.key == k } }
            ?: available.first().key
        selectedDistro = initialKey
        idsByKey.entries.firstOrNull { it.value == initialKey }?.let { distroGroup.check(it.key) }

        distroGroup.setOnCheckedChangeListener { _, checkedId ->
            idsByKey[checkedId]?.let {
                selectedDistro = it
                // Auto-update the label default when the user hasn't
                // typed anything custom yet — flipping from "Arch Linux
                // (x86)" to "Arch Linux ARM" should follow.
                if (!labelEdited) {
                    val d = available.firstOrNull { it.key == selectedDistro }
                    if (d != null) setLabelTextSilently(d.defaultLabel)
                }
                revalidate()
            }
        }
        return container
    }

    /**
     * Build the merged Label / Install-directory block. The user-typed
     * string is the [Installation.label]; we slugify it into the on-disk
     * id and render the resulting absolute path on the line directly
     * below as a quieter monospace echo, which doubles as the hint
     * shown when the label is empty / unslugifiable / collides.
     */
    private fun buildInstallDirField(available: List<Distro>, savedLabelText: String?): LinearLayout {
        val container = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = TextView(this).apply { text = "Label"; textSize = 14f }
        container.addView(title, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        val initialDefault = available.firstOrNull { it.key == selectedDistro }?.defaultLabel ?: ""
        labelField = EditText(this).apply {
            setText(savedLabelText ?: initialDefault)
            isSingleLine = true
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                override fun afterTextChanged(s: Editable?) {
                    if (!suppressEditedFlag) labelEdited = true
                    revalidate()
                }
            })
        }
        container.addView(labelField, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Single quieter monospace line below the input — when valid,
        // it's the resolved absolute install path; when invalid (empty
        // / unslugifiable / collides), it's the explanation in the
        // same slot. One line of feedback instead of two.
        locationLabel = TextView(this).apply {
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant))
        }
        container.addView(locationLabel, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return container
    }

    /**
     * `setText(...)` from inside the activity (e.g. when the distro
     * radio flips) must not flip [labelEdited] back to true. Wrap
     * those updates with this guard.
     */
    private var suppressEditedFlag = false

    private fun setLabelTextSilently(text: String) {
        suppressEditedFlag = true
        try {
            labelField.setText(text)
        } finally {
            suppressEditedFlag = false
        }
    }

    /**
     * Recompute resolvedId, the location row, the hint, and the
     * Install button's enabled state from the current label. Called on
     * label edits and distro flips.
     */
    private fun revalidate() {
        if (!::labelField.isInitialized) return
        val rawLabel = labelField.text.toString().trim()
        val slug = if (rawLabel.isEmpty()) null else Installation.slugifyLabel(rawLabel)
        val collides = slug != null && store.installationDir(slug).exists()
        resolvedId = slug?.takeUnless { collides }

        if (::locationLabel.isInitialized) {
            locationLabel.text = when {
                rawLabel.isEmpty() -> "Label cannot be empty"
                slug == null -> "Label must contain at least one letter or digit"
                collides -> "Already installed at ${store.installationDir(slug).absolutePath}"
                else -> store.installationDir(slug).absolutePath
            }
        }

        if (::installButton.isInitialized) {
            installButton.isEnabled = (resolvedId != null)
            installButton.text = "Install"
        }
    }

    /**
     * Build the method picker (tawcroot / proot / chroot radio group),
     * vertical and in recommendation order: tawcroot first as the
     * default for new installs, proot as the established rootless
     * fallback, chroot last for rooted-only setups.
     */
    private fun buildMethodPicker(): LinearLayout {
        val container = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = TextView(this).apply { text = "Install method"; textSize = 14f }
        container.addView(title, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        methodGroup = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
        val rootAvailable = Su.rootAvailable()

        // Use generateViewId() rather than hand-picked constants —
        // any literal we'd reach for in the AAPT range collides with
        // future R.id.* once we add a layout XML.
        val tawcrootId = View.generateViewId()
        val prootId = View.generateViewId()
        val chrootId = View.generateViewId()

        val tawcroot = RadioButton(this).apply {
            id = tawcrootId
            text = "tawcroot (recommended)"
        }
        val proot = RadioButton(this).apply {
            id = prootId
            text = "proot"
        }
        val chroot = RadioButton(this).apply {
            id = chrootId
            text = "chroot (requires root)"
            // Greyed-out on un-rooted devices so the limitation is
            // visible at the form level — service-side gate will also
            // refuse but the user shouldn't reach the tap to find out.
            isEnabled = rootAvailable
        }
        methodGroup.addView(tawcroot, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        methodGroup.addView(proot, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        methodGroup.addView(chroot, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        container.addView(methodGroup, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Initial selection: explicit override → use it; else default
        // to tawcroot (the recommended method).
        val initial = selectedMethod ?: TawcrootMethod.KEY
        methodGroup.check(when (initial) {
            ChrootMethod.KEY    -> chrootId
            ProotMethod.KEY     -> prootId
            else                -> tawcrootId
        })
        selectedMethod = initial

        methodGroup.setOnCheckedChangeListener { _, checkedId ->
            selectedMethod = when (checkedId) {
                chrootId    -> ChrootMethod.KEY
                prootId     -> ProotMethod.KEY
                tawcrootId  -> TawcrootMethod.KEY
                else        -> selectedMethod
            }
        }
        return container
    }

    private fun beginInstall() {
        // Only the chroot path needs `su`. Proot/tawcroot are rootless
        // by definition, so a missing-root device fails this check only
        // if the user picked chroot anyway.
        val methodKey = selectedMethod ?: TawcrootMethod.KEY
        if (methodKey == ChrootMethod.KEY && !Su.rootAvailable()) {
            formSection.visibility = View.GONE
            panel.view.visibility = View.VISIBLE
            // Also append to the log: bindToService() will overwrite
            // the status text with the service's StateFlow ("Idle") as
            // soon as onStart fires, so a status-only error vanishes
            // and the user sees a misleading "Idle" with no
            // explanation. The log line is sticky.
            val msg = "ERROR: root (su) not available — pick proot or tawcroot, or grant Magisk root."
            panel.setStatus(msg)
            panel.appendLog(msg)
            return
        }
        val targetId = resolvedId
        if (targetId == null) {
            // Defensive: the Install button is disabled when resolvedId
            // is null, but autoStart can land here without going via
            // the form. Surface the refusal clearly.
            formSection.visibility = View.GONE
            panel.view.visibility = View.VISIBLE
            val msg = "ERROR: no valid label / id resolved — set a non-empty label that doesn't collide"
            panel.setStatus(msg)
            panel.appendLog(msg)
            return
        }
        formScroll.visibility = View.GONE
        panel.view.visibility = View.VISIBLE
        // Wipe any lines the panel may have collected from the
        // SharedFlow's replay cache between onStart and now (e.g. the
        // previous uninstall's tail when the user uninstalled then
        // came back to install again).
        panel.clearLog()
        // [InstallationService] is the authoritative gate; we just hand
        // off and let it decide whether to run or reject. `started` only
        // tracks UI state (form vs panel) so a process-death recreate
        // restores the panel view.
        val distroKey = selectedDistro
        val labelText = labelField.text.toString().trim().takeIf { it.isNotEmpty() }
        panel.appendLog(
            (if (started) "[ui] re-requesting install of '$targetId' via $methodKey"
             else "[ui] starting install of '$targetId' via $methodKey")
                + (distroKey?.let { " (distro=$it)" } ?: "")
                + (labelText?.let { " label='$it'" } ?: "")
        )
        started = true
        InstallationService.startInstall(this, targetId, methodKey, distroKey, labelText)
    }

    private fun dispatchCancel() {
        val service = panel.boundService
        if (service == null) {
            panel.appendLog("[ui] cancel ignored: service not bound yet")
            return
        }
        val targetId = resolvedId
        if (targetId == null) {
            panel.appendLog("[ui] cancel ignored: no resolved id")
            return
        }
        when (service.currentKind) {
            InstallationService.JobKind.INSTALL -> confirmCancelInstall(service, targetId)
            InstallationService.JobKind.UNINSTALL -> {
                // Follow-up uninstall phase from a previous cancel-
                // install (or the user opened the install activity
                // while an uninstall was already in flight). Match
                // UninstallActivity's behaviour: no confirm.
                panel.appendLog("[ui] cancelling in-flight uninstall")
                service.cancelUninstall(targetId)
            }
            null -> panel.appendLog("[ui] cancel: no active job")
        }
    }

    private fun confirmCancelInstall(service: InstallationService, targetId: String) {
        // Note: the "no data will be lost" wording is correct because
        // the install gate only runs against an empty slot (see
        // notes/installation.md). If a future refactor adds in-place
        // reconfigure/migration the message must be revisited.
        val message = "Cancelling will stop the install and remove the partially " +
            "extracted rootfs at\n${store.installationDir(targetId).absolutePath}.\n" +
            "Nothing of yours has been written there yet, so no data will be lost."
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Cancel install of '$targetId'?")
            .setMessage(message)
            .setNegativeButton("Keep installing", null)
            .setPositiveButton("Cancel install") { _, _ ->
                panel.appendLog("[ui] user confirmed cancel")
                service.cancelInstallAndUninstall(targetId)
            }
            .show()
        // Match the destructive-action coloring on DistroInfoActivity:
        // accent red on the destructive option, neutral on the keep-
        // going one so it doesn't compete.
        dialog.getButton(DialogInterface.BUTTON_POSITIVE)?.setTextColor(getColor(R.color.tawc_danger))
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
            btn.setTextColor(
                MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
        }
    }

    companion object {
        const val EXTRA_ID = "id"
        const val EXTRA_METHOD = "method"
        const val EXTRA_DISTRO = "distro"
        private const val KEY_STARTED = "tawc.install.started"
        private const val KEY_METHOD = "tawc.install.method"
        private const val KEY_DISTRO = "tawc.install.distro"
        private const val KEY_LABEL_EDITED = "tawc.install.labelEdited"
        private const val KEY_LABEL_TEXT = "tawc.install.labelText"
    }
}
