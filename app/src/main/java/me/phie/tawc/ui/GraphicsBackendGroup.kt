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
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.R
import me.phie.tawc.install.EnabledGraphicsBackends

/**
 * Radio group over [EnabledGraphicsBackends.enabled], shared by
 * Settings (the global pick) and the `.desktop` editor (a per-entry
 * override). Each row is the backend's name over a one-line
 * description.
 */
fun Context.graphicsBackendGroup(
    current: GraphicsBackend,
    onPick: (GraphicsBackend) -> Unit,
): RadioGroup {
    val rowPad = (6 * resources.displayMetrics.density).toInt()
    val group = RadioGroup(this).apply { orientation = RadioGroup.VERTICAL }
    for (backend in EnabledGraphicsBackends.enabled) {
        val detail = when (backend) {
            GraphicsBackend.LIBHYBRIS -> R.string.graphics_backend_libhybris_detail
            GraphicsBackend.LIBHYBRIS_ZINK -> R.string.graphics_backend_libhybris_zink_detail
            GraphicsBackend.GFXSTREAM -> R.string.graphics_backend_gfxstream_detail
            GraphicsBackend.CPU -> R.string.graphics_backend_cpu_detail
        }
        group.addView(
            RadioButton(this).apply {
                // Ids are ordinal+1 — RadioGroup reports 0 for "nothing
                // checked" and View.NO_ID is -1.
                id = backend.ordinal + 1
                text = SpannableStringBuilder()
                    .append(backend.displayName)
                    .append('\n')
                    .inSpans(RelativeSizeSpan(0.85f)) { append(getString(detail)) }
                textSize = 15f
                isChecked = backend == current
                setPadding(0, rowPad, 0, rowPad)
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
    }
    group.setOnCheckedChangeListener { _, checkedId ->
        EnabledGraphicsBackends.enabled.firstOrNull { it.ordinal + 1 == checkedId }?.let(onPick)
    }
    return group
}
