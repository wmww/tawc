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
import me.phie.tawc.ExternalAccessMode
import me.phie.tawc.R

/**
 * Radio group over [ExternalAccessMode], for Settings ("External apps").
 * Each row is the mode's name over a one-line description.
 */
fun Context.externalAccessGroup(
    current: ExternalAccessMode,
    onPick: (ExternalAccessMode) -> Unit,
): RadioGroup {
    val rowPad = (6 * resources.displayMetrics.density).toInt()
    val group = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
    for (mode in ExternalAccessMode.entries) {
        val (name, detail) = when (mode) {
            ExternalAccessMode.DISABLED ->
                R.string.external_access_disabled to R.string.external_access_disabled_detail
            ExternalAccessMode.ALLOWLIST ->
                R.string.external_access_allowlist to R.string.external_access_allowlist_detail
            ExternalAccessMode.ANY ->
                R.string.external_access_any to R.string.external_access_any_detail
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
        ExternalAccessMode.entries.firstOrNull { it.ordinal + 1 == checkedId }?.let(onPick)
    }
    return group
}
