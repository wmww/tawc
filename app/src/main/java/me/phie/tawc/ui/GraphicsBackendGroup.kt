package me.phie.tawc.ui

import android.content.Context
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.install.EnabledGraphicsBackends

/**
 * Radio group over [EnabledGraphicsBackends.enabled], shared by
 * Settings (the global pick) and the `.desktop` editor (a per-entry
 * override).
 */
fun Context.graphicsBackendGroup(
    current: GraphicsBackend,
    onPick: (GraphicsBackend) -> Unit,
): RadioGroup {
    val rowPad = (6 * resources.displayMetrics.density).toInt()
    val group = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
    fun add(id: Int, label: String, checked: Boolean) = group.addView(
        RadioButton(this).apply {
            this.id = id
            text = label
            textSize = 15f
            isChecked = checked
            setPadding(0, rowPad, 0, rowPad)
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    // Ids are ordinal+1 — RadioGroup reports 0 for "nothing checked"
    // and View.NO_ID is -1.
    for (backend in EnabledGraphicsBackends.enabled) {
        add(backend.ordinal + 1, backend.displayName, backend == current)
    }
    group.setOnCheckedChangeListener { _, checkedId ->
        EnabledGraphicsBackends.enabled.firstOrNull { it.ordinal + 1 == checkedId }?.let(onPick)
    }
    return group
}
