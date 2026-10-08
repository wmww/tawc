package me.phie.tawc.ui

import android.content.Context
import android.text.SpannableStringBuilder
import android.text.style.RelativeSizeSpan
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import androidx.core.text.inSpans
import me.phie.tawc.PointerEmulation
import me.phie.tawc.R

/**
 * Radio group over [PointerEmulation], shared by Settings (the global
 * pick) and the `.desktop` editor (a per-entry override). Each row is
 * the mode's name over a one-line description.
 */
fun Context.pointerEmulationGroup(
    current: PointerEmulation,
    onPick: (PointerEmulation) -> Unit,
): RadioGroup {
    val rowPad = (6 * resources.displayMetrics.density).toInt()
    val group = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
    for (mode in PointerEmulation.entries) {
        val (name, detail) = when (mode) {
            PointerEmulation.NONE -> R.string.pointer_emulation_none to R.string.pointer_emulation_none_detail
            PointerEmulation.HOVER -> R.string.pointer_emulation_hover to R.string.pointer_emulation_hover_detail
            PointerEmulation.FULL -> R.string.pointer_emulation_full to R.string.pointer_emulation_full_detail
        }
        group.addView(
            RadioButton(this).apply {
                // Ids are ordinal+1 — RadioGroup reports 0 for "nothing
                // checked" and View.NO_ID is -1.
                id = mode.ordinal + 1
                text = SpannableStringBuilder()
                    .append(getString(name))
                    .append('\n')
                    .inSpans(RelativeSizeSpan(0.85f)) { append(getString(detail)) }
                textSize = 15f
                isChecked = mode == current
                setPadding(0, rowPad, 0, rowPad)
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
    }
    group.setOnCheckedChangeListener { _, checkedId ->
        PointerEmulation.entries.firstOrNull { it.ordinal + 1 == checkedId }?.let(onPick)
    }
    return group
}
