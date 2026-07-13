package me.phie.tawc

import android.graphics.Typeface
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.EnabledGraphicsBackends
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.verticalLp

/**
 * App settings screen. Reachable from the home screen tonal "Settings"
 * button. Each section is its own card with a bold title at the top
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen(getString(R.string.title_settings))
        val pad = (16 * resources.displayMetrics.density).toInt()

        scaffold.content.addView(
            buildSectionCard(getString(R.string.settings_graphics_driver), buildGraphicsBackendGroup()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )
        // "Debug rendering" ships in release on purpose: the magenta
        // SHM tint is a supported diagnostic for GPU-fallback issues
        // on user devices, not a debug-build-only tool. Only the
        // default differs per build type
        // (BuildConfig.TINT_BUFFERS_BY_TYPE_DEFAULT: on in debug, off
        // in release).
        scaffold.content.addView(
            buildSectionCard(getString(R.string.settings_debug_rendering), buildTintBuffersCheckbox()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )
        scaffold.content.addView(
            buildSectionCard(getString(R.string.settings_compatibility), buildCompatibilitySettings()),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )
        scaffold.content.addView(
            buildOutputScaleCard(),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )

        setContentView(scaffold.root)
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

    private fun buildOutputScaleCard(): android.view.View {
        val cardPad = (12 * resources.displayMetrics.density).toInt()
        val min = Settings.MIN_OUTPUT_SCALE
        val step = Settings.OUTPUT_SCALE_STEP
        val steps = ((Settings.MAX_OUTPUT_SCALE - min) / step).toInt()
        val title = TextView(this).apply {
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
            text = getString(R.string.settings_display_scale, Settings.formatOutputScale(Settings.outputScale))
        }
        val slider = SeekBar(this).apply {
            max = steps
            progress = ((Settings.outputScale - min) / step).toInt()
            setPadding(cardPad, cardPad / 2, cardPad, cardPad / 2)
        }
        slider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
                val scale = Settings.snapOutputScale(min + progress * step)
                title.text = getString(R.string.settings_display_scale, Settings.formatOutputScale(scale))
                if (fromUser) {
                    Settings.outputScale = scale
                    NativeBridge.nativeSetOutputScale(scale)
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar) = Unit
        })
        val card = tawcCard()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
            clipToPadding = false
            addView(title, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(slider, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        card.addView(column)
        return card
    }
}
