package me.phie.tawc.install

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.os.IBinder
import android.text.method.ScrollingMovementMethod
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import me.phie.tawc.R

/**
 * Shared "operation in progress" UI. Builds a status line + accent-
 * tinted progress bar + scrolling log + Cancel button, manages a
 * binding to [InstallationService] for the lifetime of [activity], and
 * updates the views off the service's progress and log flows.
 *
 * Owners attach [view] into their layout, call [bindToService] from
 * `onStart` and [unbind] from `onStop`. Local log lines (e.g. "[ui]
 * starting install") can be pushed in via [appendLog].
 *
 * The cancel button visibility tracks the current [InstallStage]: it's
 * shown for any in-progress stage and hidden on `IDLE`/`DONE`/`FAILED`
 * (no active job to cancel). Owners set [onCancelClicked] to a handler
 * that decides whether to confirm and which service method to invoke
 * (cancel-install-then-uninstall vs. cancel-uninstall-leaves-failed).
 * The handler can read [boundService] to call directly into the
 * service it's already wired to.
 */
class OperationLogPanel(private val activity: Activity) {

    val view: LinearLayout
    private val statusText: TextView
    private val progressBar: ProgressBar
    private val logText: TextView
    private val logScroll: ScrollView
    private val cancelButton: MaterialButton

    private var service: InstallationService? = null
    private var collectScope: CoroutineScope? = null

    /**
     * Set by the owning activity to handle a Cancel tap. Install path
     * confirms via dialog and then triggers the cancel-and-uninstall
     * service call; the uninstall path calls cancel directly without
     * a confirm step (user may be quickly aborting to avoid losing
     * data — see notes/installation.md).
     */
    var onCancelClicked: (() -> Unit)? = null

    /**
     * The bound service, or `null` until the binder connects. Owners
     * use this from [onCancelClicked] so the Cancel handler can call
     * straight into the service that's driving this panel.
     */
    val boundService: InstallationService? get() = service

    init {
        val pad = (16 * activity.resources.displayMetrics.density).toInt()
        val accent = activity.getColor(R.color.tawc_accent)

        view = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        statusText = TextView(activity).apply {
            text = ""
            // Status reads as a heading: bold + the surface's default
            // text color (so it neither competes with the accent
            // primary action nor disappears into placeholder grey).
            // The DONE / FAILED stages swap to success-green / danger-
            // red in [startCollecting] for terminal contrast.
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

        // Cancel sits below the log so it doesn't compete with the
        // primary status row at the top, and so the user reads the
        // operation's current state before reaching the abort. Subdued
        // styling (text button, neutral on-surface color) so it doesn't
        // visually compete with the operation's status line — a
        // background recovery action, not the primary path. Hidden until
        // a stage event tells us a job is actually running.
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

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val b = binder as? InstallationService.LocalBinder ?: return
            service = b.service
            startCollecting()
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
        }
    }

    fun bindToService() {
        activity.bindService(
            Intent(activity, InstallationService::class.java),
            connection,
            Context.BIND_AUTO_CREATE,
        )
    }

    fun unbind() {
        try { activity.unbindService(connection) } catch (_: IllegalArgumentException) { /* not bound */ }
        collectScope?.cancel()
        collectScope = null
        service = null
    }

    fun setStatus(text: String) {
        statusText.text = text
    }

    /**
     * Drop every line currently rendered in the on-screen log AND the
     * service-side replay buffer. Owners call this from their
     * `beginInstall` / `beginUninstall` just before invoking the
     * service so:
     *   1. the previous operation's lines vanish from this panel's
     *      already-collected TextView; and
     *   2. any items still buffered in the SharedFlow's replay cache
     *      can't race in *after* the wipe (`logText.text = ""` runs on
     *      Main, but a queued `collect` callback on Main can dispatch
     *      between the wipe and the user's next action).
     * `resetReplayCache` only affects *future* subscribers, but
     * combined with the local wipe it closes the bind→collect→wipe
     * window cleanly.
     */
    fun clearLog() {
        service?.resetLogReplay()
        logText.text = ""
    }

    fun appendLog(line: String) {
        // Cap on-screen log to keep memory bounded; full history is in logcat.
        val cur = logText.text
        if (cur.length > 80_000) {
            logText.text = cur.subSequence(40_000, cur.length).toString()
        }
        logText.append(line)
        logText.append("\n")
        logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    private fun startCollecting() {
        collectScope?.cancel()
        val s = service ?: return
        val cs = CoroutineScope(Dispatchers.Main)
        collectScope = cs

        val danger = activity.getColor(R.color.tawc_danger)
        val success = activity.getColor(R.color.tawc_success)
        val defaultTextColor = MaterialColors.getColor(
            statusText, com.google.android.material.R.attr.colorOnSurface,
        )

        cs.launch {
            s.progress.collectLatest { p ->
                statusText.text = p.message
                statusText.setTextColor(when (p.stage) {
                    InstallStage.FAILED -> danger
                    InstallStage.DONE -> success
                    else -> defaultTextColor
                })
                if (p.percent != null) {
                    progressBar.isIndeterminate = false
                    progressBar.progress = p.percent
                } else {
                    progressBar.isIndeterminate = true
                }
                progressBar.visibility = when (p.stage) {
                    InstallStage.IDLE, InstallStage.DONE, InstallStage.FAILED -> View.GONE
                    else -> View.VISIBLE
                }
                cancelButton.visibility = when (p.stage) {
                    InstallStage.IDLE, InstallStage.DONE, InstallStage.FAILED -> View.GONE
                    else -> View.VISIBLE
                }
            }
        }

        cs.launch {
            s.log.collect { line -> appendLog(line) }
        }
    }

    private fun lp(w: Int, h: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(w, h).also { it.bottomMargin = bottomMargin }
}
