package me.phie.tawc

import android.graphics.Typeface
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.install.EnabledGraphicsBackends
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.verticalLp

/**
 * App settings screen. Reachable from the home screen tonal "Settings"
 * button. Today: a single section, the graphics-driver picker. New
 * sections should follow the same pattern (header `TextView` + a card
 * with the controls).
 *
 * Picks are saved immediately on selection — no Save button to
 * accidentally forget. The new value lands in [Settings] and is read
 * by [me.phie.tawc.install.RootfsEnv] on the **next** rootfs spawn,
 * so already-running clients keep their existing env until restart.
 */
class SettingsActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen("Settings")
        val pad = (16 * resources.displayMetrics.density).toInt()
        val cardPad = (12 * resources.displayMetrics.density).toInt()

        scaffold.content.addView(
            TextView(this).apply {
                text = "Graphics driver"
                textSize = 16f
                setTypeface(typeface, Typeface.BOLD)
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
        )

        val card = tawcCard()
        val cardColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }
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
                    // Vertical breathing room between options without a
                    // separator line — keeps the card visually a single
                    // surface like the rest of the app's cards.
                    setPadding(0, cardPad / 2, 0, cardPad / 2)
                },
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
            )
        }
        group.setOnCheckedChangeListener { _, checkedId ->
            val picked = EnabledGraphicsBackends.enabled.firstOrNull { it.ordinal + 1 == checkedId }
            if (picked != null) Settings.graphicsBackend = picked
        }
        cardColumn.addView(group, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        card.addView(cardColumn)
        scaffold.content.addView(card, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        setContentView(scaffold.root)
    }
}
