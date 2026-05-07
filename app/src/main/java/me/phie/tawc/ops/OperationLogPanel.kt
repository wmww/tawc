package me.phie.tawc.ops

import android.app.Activity
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.text.SpannableString
import android.text.Spanned
import android.text.method.ScrollingMovementMethod
import android.text.style.LeadingMarginSpan
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import me.phie.tawc.R

/**
 * Reusable "operation in progress" UI: bold status line + accent-tinted
 * progress bar + scrolling log + subdued borderless Cancel button.
 *
 * Owners attach [view] into their layout, then call [bind] with an
 * [Operation] when one is available and [unbind] when leaving the
 * screen. The panel collects from [Operation.progress] / [Operation.log]
 * and updates the views; cancel taps invoke [onCancelClicked] (the
 * owner usually wraps with a confirm dialog if [Operation.cancelConfirmation]
 * says to).
 *
 * Lifecycle note: when an Operation terminates and is unregistered, the
 * owner calls [unbind]. The TextView/status views are *not* cleared;
 * the panel just stops collecting. This matches the design choice that
 * a still-open viewer keeps its last-rendered state frozen rather than
 * blanking out.
 */
class OperationLogPanel(private val activity: Activity) {

    val view: LinearLayout
    private val statusText: TextView
    private val progressBar: ProgressBar
    private val logText: TextView
    private val logScroll: ScrollView
    private val cancelButton: MaterialButton

    private var collectScope: CoroutineScope? = null

    /** The currently bound op, or `null`. Owners may read this from [onCancelClicked]. */
    var boundOperation: Operation? = null
        private set

    /**
     * Tap handler for the Cancel button. The default just calls
     * [Operation.cancel] on [boundOperation]; owners can override to
     * wrap with a confirm dialog (driven by [Operation.cancelConfirmation]).
     */
    var onCancelClicked: (() -> Unit)? = null

    init {
        val pad = (16 * activity.resources.displayMetrics.density).toInt()
        val accent = activity.getColor(R.color.tawc_accent)

        view = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        statusText = TextView(activity).apply {
            text = ""
            setTypeface(typeface, Typeface.BOLD)
        }
        view.addView(statusText, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))

        progressBar = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply {
            isIndeterminate = true
            indeterminateTintList = ColorStateList.valueOf(accent)
            progressTintList = ColorStateList.valueOf(accent)
        }
        view.addView(progressBar, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        logScroll = ScrollView(activity)
        logText = TextView(activity).apply {
            typeface = Typeface.MONOSPACE
            textSize = 11f
            setTextIsSelectable(true)
            movementMethod = ScrollingMovementMethod.getInstance()
        }
        logScroll.addView(logText)
        view.addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        cancelButton = MaterialButton(
            activity, null, com.google.android.material.R.attr.borderlessButtonStyle,
        ).apply {
            text = "Cancel"
            setTextColor(
                MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
            setOnClickListener { onCancelClicked?.invoke() }
        }
        cancelButton.visibility = View.GONE
        view.addView(cancelButton, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = 0).apply {
            topMargin = pad / 2
        })
    }

    /**
     * Bind the panel to [op]. Cancels any previous binding's collectors
     * but **does not** clear the local TextView — by design, since the
     * intended UX is "show the previous frozen state until the new op
     * fills the panel." The service's per-op `_log.resetReplayCache()`
     * keeps the new op's replay buffer from leaking the previous run.
     */
    fun bind(op: Operation) {
        unbind()
        boundOperation = op
        val cs = CoroutineScope(Dispatchers.Main)
        collectScope = cs

        val danger = activity.getColor(R.color.tawc_danger)
        val success = activity.getColor(R.color.tawc_success)
        val defaultTextColor = MaterialColors.getColor(
            statusText, com.google.android.material.R.attr.colorOnSurface,
        )

        cs.launch {
            op.progress.collectLatest { p ->
                statusText.text = p.message
                statusText.setTextColor(when (p.stage) {
                    OperationStage.FAILED -> danger
                    OperationStage.DONE -> success
                    else -> defaultTextColor
                })
                if (p.percent != null) {
                    progressBar.isIndeterminate = false
                    progressBar.progress = p.percent
                } else {
                    progressBar.isIndeterminate = true
                }
                val terminal = p.stage.isTerminal || p.stage == OperationStage.IDLE
                progressBar.visibility = if (terminal) View.GONE else View.VISIBLE
                cancelButton.visibility = if (terminal) View.GONE else View.VISIBLE
            }
        }

        cs.launch {
            op.log.collect { line -> appendLog(line) }
        }
    }

    /** Stop collecting. Leaves the views as-is so the owner can show the frozen final state. */
    fun unbind() {
        collectScope?.cancel()
        collectScope = null
        boundOperation = null
    }

    fun appendLog(line: String) {
        // Cap on-screen log to keep memory bounded; full history is in logcat.
        val cur = logText.text
        if (cur.length > 80_000) {
            logText.text = cur.subSequence(40_000, cur.length).toString()
        }
        val withNewline = "$line\n"
        val span = SpannableString(withNewline)
        span.setSpan(
            LeadingMarginSpan.Standard(0, hangingIndentPx),
            0, withNewline.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
        )
        logText.append(span)
        logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    private val hangingIndentPx: Int =
        (16 * activity.resources.displayMetrics.density).toInt()

    private fun lp(w: Int, h: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(w, h).also { it.bottomMargin = bottomMargin }
}
