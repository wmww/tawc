package me.phie.tawc

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.AllFilesAccess
import android.view.Gravity
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Spinner
import me.phie.tawc.install.EnabledGraphicsBackends
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.ManageBindsActivity
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.buildAndoCommitRow
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.licenses.LicensesActivity
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp

/**
 * App settings screen. Reachable from the home screen's ⋮ menu and a
 * drawer row's ⋮. The first card holds per-install settings for
 * [EXTRA_ID] (else the open distro, [OpenDistro]); the rest are global. Each section is its own card with a bold title at the top
 * followed by the section's controls. Add a new section by building a
 * card via [buildSectionCard] and adding it to `scaffold.content`.
 *
 * Picks are saved immediately on selection — no Save button to
 * accidentally forget. Graphics-driver picks land in [Settings] and
 * are read by [me.phie.tawc.install.RootfsEnv] on the **next** rootfs
 * spawn, so already-running clients keep their existing env until
 * restart. Render-time toggles (e.g. [Settings.tintBuffersByType])
 * are also pushed straight into the compositor via [NativeBridge] so
 * the live frame reflects the change immediately.
 */
class SettingsActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }

    /** Serializes ando toggle commits so rapid taps land in click order. */
    private val andoCommitExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()

    /** Holds the open-distro card; refilled in [onResume] so a distro
     *  switch or uninstall while we were in the back stack re-renders. */
    private lateinit var distroSlot: FrameLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen(getString(R.string.title_settings))
        val pad = (16 * resources.displayMetrics.density).toInt()
        // Scroll so the last card isn't squeezed on short screens.
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        distroSlot = FrameLayout(this)
        column.addView(distroSlot, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))
        column.addView(buildScaleCard(), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))
        // The magenta SHM tint ships in release on purpose: it's a
        // supported diagnostic for GPU-fallback issues on user devices,
        // not a debug-build-only tool. Only the default differs per
        // build type (BuildConfig.TINT_BUFFERS_BY_TYPE_DEFAULT: on in
        // debug, off in release).
        column.addView(
            buildSectionCard(getString(R.string.settings_graphics_driver), buildGraphicsSettings()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )
        column.addView(
            buildSectionCard(getString(R.string.settings_compatibility), buildCompatibilitySettings()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )
        column.addView(
            buildSectionCard(getString(R.string.settings_about), buildAboutSettings()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )

        scaffold.content.addView(
            ScrollView(this).apply { addView(column, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)) },
            LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT),
        )
        setContentView(scaffold.root)
    }

    override fun onResume() {
        super.onResume()
        distroSlot.removeAllViews()
        // A drawer row's ⋮ names its install; otherwise the open one.
        val inst = intent.getStringExtra(EXTRA_ID)
            ?.takeIf { Installation.isValidId(it) }
            ?.let { store.load(it) }
            ?: OpenDistro.resolve(store)
        if (inst == null) {
            distroSlot.visibility = android.view.View.GONE
        } else {
            distroSlot.visibility = android.view.View.VISIBLE
            distroSlot.addView(buildDistroCard(inst))
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // Queued commits still run; this only lets the worker exit.
        andoCommitExecutor.shutdown()
    }

    /**
     * Settings stored on the open install: ando (all methods) and
     * storage binds (tawcroot, when this build declares all-files
     * access). Both gated to READY/FAILED so an edit can't race the
     * service's own metadata writes; FAILED stays editable since a bad
     * bind is one way a slot fails.
     */
    private fun buildDistroCard(inst: Installation): android.view.View {
        val pad = (12 * resources.displayMetrics.density).toInt()
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = DistroRegistry.displayLabel(inst)
        // Distro line only when the label doesn't already say it, as
        // on the home screen.
        val displayName = DistroRegistry.forInstallation(inst)?.displayName
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"
        if (title != displayName) {
            body.addView(TextView(this).apply {
                text = displayName
                textSize = 14f
                alpha = 0.7f
            })
        }
        val editable = inst.state == Installation.State.READY || inst.state == Installation.State.FAILED
        if (!editable) {
            body.addView(TextView(this).apply {
                text = getString(R.string.settings_distro_unavailable)
                textSize = 14f
                setPadding(0, pad / 2, 0, 0)
            })
        } else {
            body.addView(
                buildAndoCommitRow(this, store, inst, andoCommitExecutor),
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = pad / 2 },
            )
            if (inst.method == TawcrootMethod.KEY && AllFilesAccess.declared(this)) {
                body.addView(
                    tonalButton(getString(R.string.distro_info_manage_binds)) {
                        startActivity(ManageBindsActivity.intentForInstall(this, inst.id))
                    },
                    LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { topMargin = pad / 2 },
                )
            }
        }
        return buildSectionCard(title, body)
    }

    private fun buildSectionCard(title: String, body: android.view.View): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val card = tawcCard()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }
        column.addView(
            TextView(this).apply {
                text = title
                textSize = 16f
                setTypeface(typeface, Typeface.BOLD)
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
        column.addView(body, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        card.addView(column)
        return card
    }

    private fun buildGraphicsBackendGroup(): RadioGroup {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val group = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
        val current = Settings.graphicsBackend
        for (backend in EnabledGraphicsBackends.enabled) {
            group.addView(
                RadioButton(this).apply {
                    // ordinal+1 — RadioGroup uses 0 to mean "nothing checked"
                    // in onCheckedChange callbacks, and View.NO_ID is -1, so
                    // any positive int that round-trips back to the enum is fine.
                    id = backend.ordinal + 1
                    text = backend.displayName
                    textSize = 15f
                    isChecked = backend == current
                    setPadding(0, cardPad / 2, 0, cardPad / 2)
                },
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
            )
        }
        group.setOnCheckedChangeListener { _, checkedId ->
            val picked = EnabledGraphicsBackends.enabled.firstOrNull { it.ordinal + 1 == checkedId }
            if (picked != null) Settings.graphicsBackend = picked
        }
        return group
    }

    private fun buildGraphicsSettings(): android.view.View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(buildGraphicsBackendGroup(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(buildTintBuffersCheckbox(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
    }

    private fun buildTintBuffersCheckbox(): CheckBox {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        return CheckBox(this).apply {
            text = getString(R.string.settings_tint_buffers_by_type)
            textSize = 15f
            isChecked = Settings.tintBuffersByType
            setPadding(0, cardPad / 2, 0, cardPad / 2)
            setOnCheckedChangeListener { _, checked ->
                Settings.tintBuffersByType = checked
                NativeBridge.nativeSetTintBuffersByType(checked)
            }
        }
    }

    private fun buildCompatibilitySettings(): android.view.View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(buildXwaylandCheckbox(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(buildGtk3BrokenMenusWorkaroundCheckbox(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
    }

    private fun buildXwaylandCheckbox(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        val checkbox = CheckBox(this).apply {
            text = getString(R.string.settings_xwayland)
            textSize = 15f
            isChecked = Settings.xwayland
            setPadding(0, cardPad / 2, 0, cardPad / 2)
            setOnCheckedChangeListener { _, checked ->
                Settings.xwayland = checked
                NativeBridge.nativeSetXwaylandEnabled(checked)
            }
        }
        column.addView(checkbox, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return column
    }

    private fun buildGtk3BrokenMenusWorkaroundCheckbox(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        val checkbox = CheckBox(this).apply {
            text = getString(R.string.settings_gtk3_broken_menus_workaround)
            textSize = 15f
            isChecked = Settings.gtk3BrokenMenusWorkaround
            setPadding(0, cardPad / 2, 0, cardPad / 4)
            setOnCheckedChangeListener { _, checked ->
                Settings.gtk3BrokenMenusWorkaround = checked
                NativeBridge.nativeSetGtk3BrokenMenusWorkaround(checked)
            }
        }
        val detail = TextView(this).apply {
            text = getString(R.string.settings_gtk3_broken_menus_workaround_detail)
            textSize = 13f
            setPadding(cardPad / 2, 0, 0, cardPad / 2)
        }
        column.addView(checkbox, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        column.addView(detail, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return column
    }

    /**
     * Application ID and version, plus the way into the licenses index.
     * The identity line is selectable because the README asks bug
     * reporters to quote their app version — make that copyable rather
     * than something to squint at and retype.
     */
    private fun buildAboutSettings(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(
            TextView(this).apply {
                text = getString(
                    R.string.settings_about_detail,
                    BuildConfig.APPLICATION_ID,
                    BuildConfig.VERSION_NAME,
                )
                textSize = 14f
                setTextIsSelectable(true)
                setPadding(0, cardPad / 2, 0, cardPad)
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
        column.addView(
            tonalButton(getString(R.string.settings_about_licenses)) {
                startActivity(Intent(this, LicensesActivity::class.java))
            },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
        return column
    }

    private fun buildScaleCard(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val card = tawcCard()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
            clipToPadding = false
            addView(buildTerminalScaleSlider(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(buildOutputScaleSlider(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(buildRefreshRateRow(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        card.addView(column)
        return card
    }

    private fun buildOutputScaleSlider(): android.view.View = buildScaleSlider(
        R.string.settings_gui_scale,
        Settings.MIN_OUTPUT_SCALE, Settings.MAX_OUTPUT_SCALE, Settings.OUTPUT_SCALE_STEP,
        Settings.outputScale, Settings::snapOutputScale, Settings::formatOutputScale,
    ) { scale ->
        Settings.outputScale = scale
        NativeBridge.nativeSetOutputScale(scale)
    }

    /** Applied by the terminal pane on resume. */
    private fun buildTerminalScaleSlider(): android.view.View = buildScaleSlider(
        R.string.settings_terminal_scale,
        Settings.MIN_TERMINAL_SCALE, Settings.MAX_TERMINAL_SCALE, Settings.TERMINAL_SCALE_STEP,
        Settings.terminalScale, Settings::snapTerminalScale, Settings::formatTerminalScale,
    ) { Settings.terminalScale = it }

    /**
     * Cap on the panel's refresh rate.
     *
     * The list is exactly what this display reports — no rate the panel
     * would refuse — and the top entry means "follow the panel" rather
     * than freezing today's maximum, so the setting keeps working when the
     * device (or the panel's current mode) changes. Changing it pushes the
     * new rate to a running compositor straight away; this Activity's own
     * window preference is applied by `CompositorActivity` when it resumes.
     */
    private fun buildRefreshRateRow(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val title = TextView(this).apply {
            text = getString(R.string.settings_refresh_rate)
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
        }
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(title, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
        val usable = RefreshRate.usableMhz(displayRefreshRatesMhz(this))
        if (usable.isEmpty()) return row

        // Index 0 is "follow the panel"; the rest are the concrete rates
        // below its maximum, descending.
        val caps = listOf(RefreshRate.MAX_MHZ) + usable.reversed().drop(1)
        fun labelFor(capMhz: Int): String = if (capMhz == RefreshRate.MAX_MHZ) {
            getString(R.string.settings_refresh_rate_max, RefreshRate.formatMhz(usable.last()))
        } else {
            RefreshRate.formatMhz(capMhz)
        }
        val spinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@SettingsActivity,
                android.R.layout.simple_spinner_item,
                caps.map { labelFor(it) },
            ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            contentDescription = getString(R.string.settings_refresh_rate)
            setSelection(caps.indexOf(Settings.refreshRateCapMhz).coerceAtLeast(0), false)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                    val picked = caps[position]
                    if (picked == Settings.refreshRateCapMhz) return
                    Settings.refreshRateCapMhz = picked
                    RefreshRate.effectiveMhz(usable, picked)
                        ?.let { NativeBridge.nativeSetOutputRefreshRate(it) }
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        row.addView(spinner, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, cardPad / 2, 0, cardPad / 2)
            addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        return column
    }

    private fun buildScaleSlider(
        titleRes: Int,
        min: Float,
        maxScale: Float,
        step: Float,
        current: Float,
        snap: (Float) -> Float,
        format: (Float) -> String,
        onChange: (Float) -> Unit,
    ): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val steps = ((maxScale - min) / step + 0.5f).toInt()
        val title = TextView(this).apply {
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
            text = getString(titleRes, format(current))
        }
        val slider = SeekBar(this).apply {
            max = steps
            progress = ((current - min) / step + 0.5f).toInt()
            setPadding(cardPad, cardPad / 2, cardPad, cardPad / 2)
        }
        slider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
                val scale = snap(min + progress * step)
                title.text = getString(titleRes, format(scale))
                if (fromUser) onChange(scale)
            }

            override fun onStartTrackingTouch(seekBar: SeekBar) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar) = Unit
        })
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            clipToPadding = false
            addView(title, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(slider, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
    }

    companion object {
        /** Install whose card leads the screen; default the open one. */
        const val EXTRA_ID = "id"
    }
}
