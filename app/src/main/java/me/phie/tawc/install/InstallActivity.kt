package me.phie.tawc.install

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
import android.widget.CheckBox
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import me.phie.tawc.R
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ops.LogScreenActivity
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.verticalLp

/**
 * "Install new distro" screen. Form-only: distro / label / method /
 * cache-proxy controls plus an Install button. Tapping Install kicks
 * off [InstallationService] and hands the user off to
 * [LogScreenActivity] for the live progress view, then finishes itself
 * — so the back stack is `home → log`, not `home → form → log`.
 *
 * Mutating an installation never happens as a side-effect of opening
 * this screen. The button press is the only trigger; CLI install /
 * uninstall lives on the dev exec broker (see [InstallActions] +
 * `scripts/install-distro.sh`). This was the
 * `install-uninstall-trigger-via-activity-launch` issue's resolution.
 */
class InstallActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var selectedMethod: String? = null
    private var selectedDistro: String? = null
    private var labelEdited: Boolean = false

    /**
     * Tri-state for the "Use cache proxy" checkbox:
     *   - null: not yet initialised (will be seeded from build type).
     *   - true / false: user-overridden value, persisted across rotations.
     */
    private var useCacheProxy: Boolean? = null
    private lateinit var cacheProxyCheckbox: CheckBox

    private lateinit var formScroll: ScrollView
    private lateinit var formSection: LinearLayout
    private lateinit var methodGroup: RadioGroup
    private lateinit var distroGroup: RadioGroup
    private lateinit var labelField: EditText
    private lateinit var locationLabel: TextView
    private lateinit var installButton: MaterialButton
    private lateinit var scaffold: me.phie.tawc.ui.Scaffold

    /**
     * Resolved id for the Install button. Tracks (label → slug → unique)
     * so the service-call site doesn't have to re-derive it; null when
     * the label is empty / unslugifiable / collides with an existing
     * install (Install button is also disabled in that state).
     */
    private var resolvedId: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        selectedMethod = savedInstanceState?.getString(KEY_METHOD)
        selectedDistro = savedInstanceState?.getString(KEY_DISTRO)
        labelEdited = savedInstanceState?.getBoolean(KEY_LABEL_EDITED) == true
        useCacheProxy = when {
            savedInstanceState?.containsKey(KEY_USE_PROXY) == true ->
                savedInstanceState.getBoolean(KEY_USE_PROXY)
            // Dev build default: on. Production: off (and the row is
            // hidden anyway, see buildCacheProxyRow).
            me.phie.tawc.BuildConfig.DEBUG -> true
            else -> false
        }

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

        setContentView(scaffold.root)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(KEY_LABEL_EDITED, labelEdited)
        // Guard the late-init lookup the same way [revalidate] does —
        // saving state can in principle fire before the form is built
        // if a future onCreate path bails early.
        if (::labelField.isInitialized) {
            outState.putString(KEY_LABEL_TEXT, labelField.text.toString())
        }
        selectedMethod?.let { outState.putString(KEY_METHOD, it) }
        selectedDistro?.let { outState.putString(KEY_DISTRO, it) }
        useCacheProxy?.let { outState.putBoolean(KEY_USE_PROXY, it) }
    }

    private fun buildFormSection(pad: Int, savedLabelText: String?): LinearLayout {
        val s = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        // List the distros that match the host's primary ABI. Empty
        // list means no Distro supports this device; render an
        // explanatory line rather than a dead radio group, and let
        // the service-level gate refuse the install if the user taps
        // anyway.
        val available = DistroRegistry.availableForHost()
        val initialDistro = (selectedDistro ?: available.firstOrNull()?.key)
        selectedDistro = initialDistro

        s.addView(buildDistroPicker(available, pad), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        s.addView(buildInstallDirField(available, savedLabelText), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        // Method picker. Defaults to tawcroot (the recommended method);
        // saved instance state overrides for rotation.
        s.addView(buildMethodPicker(), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))

        // Dev-only "Use cache proxy" checkbox. Hidden in release builds
        // — production must never even ask the user about a localhost
        // proxy URL, since it'd never be reachable from a packaged APK.
        if (me.phie.tawc.BuildConfig.DEBUG) {
            s.addView(buildCacheProxyRow(), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))
        }

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
     * unless saved state nudges otherwise. Hidden when only one distro
     * matches (the first-class case before Manjaro) so we don't render
     * a single-choice radio group.
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
     * Dev-only "Use cache proxy" checkbox. Drives [useCacheProxy],
     * which gates whether [beginInstall] passes a `mirrorProxy` URL to
     * the service. See `notes/cache-proxy.md`.
     */
    private fun buildCacheProxyRow(): CheckBox {
        cacheProxyCheckbox = CheckBox(this).apply {
            text = "Use local cache proxy"
            isChecked = useCacheProxy ?: true
            setOnCheckedChangeListener { _, checked -> useCacheProxy = checked }
        }
        return cacheProxyCheckbox
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
            // We don't have a panel anymore; surface as a quick
            // toast-style status on the form. Service-level gate would
            // also refuse, but a fail-fast at the form level avoids the
            // service start.
            android.widget.Toast.makeText(
                this,
                "root (su) not available — pick proot or tawcroot, or grant Magisk root.",
                android.widget.Toast.LENGTH_LONG,
            ).show()
            return
        }
        val targetId = resolvedId ?: return  // button disabled when null

        val distroKey = selectedDistro
        val labelText = labelField.text.toString().trim().takeIf { it.isNotEmpty() }
        // Dev-time cache proxy URL: when the (debug-only) checkbox is
        // on, use the standard local proxy URL; else null. Service-side
        // gates this on BuildConfig.DEBUG so a release APK ignores any
        // stray value anyway.
        val mirrorProxyUrl = if (useCacheProxy == true) DEFAULT_PROXY_URL else null

        InstallationService.startInstall(this, targetId, methodKey, distroKey, labelText, mirrorProxyUrl)
        startActivity(LogScreenActivity.intentFor(this, "install:$targetId"))
        finish()
    }

    companion object {
        /** URL the "Use cache proxy" checkbox sets. Debug-only. */
        private const val DEFAULT_PROXY_URL = "http://127.0.0.1:8080/proxy/"
        private const val KEY_METHOD = "tawc.install.method"
        private const val KEY_DISTRO = "tawc.install.distro"
        private const val KEY_LABEL_EDITED = "tawc.install.labelEdited"
        private const val KEY_LABEL_TEXT = "tawc.install.labelText"
        private const val KEY_USE_PROXY = "tawc.install.useCacheProxy"
    }
}
