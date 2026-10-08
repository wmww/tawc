package me.phie.tawc.compositor

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.util.TypedValue
import android.view.Gravity
import android.view.ContextThemeWrapper
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.launcher.IconLoader
import me.phie.tawc.launcher.Launch
import me.phie.tawc.launcher.LaunchState
import me.phie.tawc.ui.tonalButton

/**
 * The launch splash over a [CompositorActivity]'s SurfaceView: the entry's
 * icon and name while the program starts, its output log with a Close
 * button if it fails, gone once its window renders. A plain Android view,
 * so the compositor never draws it. See notes/launcher.md ("Launch splash").
 */
internal class LaunchSplash(
    private val activity: Activity,
    private val launch: Launch,
    private val close: () -> Unit,
) {
    val view: FrameLayout
    private val splash: LinearLayout
    private val icon: ImageView
    private val logPanel: LinearLayout
    private val status: TextView
    private val logScroll: ScrollView
    private val logText: TextView

    /** The overlay still covers the window (Back closes the task). */
    var active = true
        private set

    init {
        val density = activity.resources.displayMetrics.density
        fun dp(v: Int) = (v * density).toInt()

        icon = ImageView(activity).apply {
            setImageResource(if (launch.terminal) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback)
        }
        val name = TextView(activity).apply {
            text = launch.name
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            gravity = Gravity.CENTER
            setPadding(dp(24), dp(16), dp(24), 0)
        }
        splash = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            addView(icon, LinearLayout.LayoutParams(dp(ICON_DP), dp(ICON_DP)))
            addView(name, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }

        status = TextView(activity).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, 0, 0, dp(12))
        }
        logText = TextView(activity).apply {
            setTextColor(0xFFCCCCCC.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(activity).apply {
            addView(logText, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        // The compositor theme isn't Material; the button needs it.
        val closeButton = ContextThemeWrapper(activity, R.style.Theme_Tawc)
            .tonalButton(activity.getString(R.string.launch_splash_close)) { close() }
        logPanel = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
            visibility = View.GONE
            addView(status, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(closeButton, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                gravity = Gravity.END
                topMargin = dp(12)
            })
        }

        view = FrameLayout(activity).apply {
            setBackgroundColor(Color.BLACK)
            // Nothing under the splash takes input until the window shows.
            isClickable = true
            addView(splash, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            addView(logPanel, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
    }

    /** Follow the launch until the window shows or the task closes. */
    fun bind(scope: CoroutineScope) {
        val targetPx = (ICON_DP * activity.resources.displayMetrics.density).toInt()
        if (launch.iconPath.isNotEmpty()) scope.launch {
            withContext(Dispatchers.IO) { IconLoader.decode(launch.iconPath, targetPx) }
                ?.let { icon.setImageBitmap(it) }
        }
        scope.launch { launch.state.collect(::render) }
        scope.launch {
            launch.logVersion.collect {
                if (logPanel.visibility == View.VISIBLE) showLog()
                // Coalesce bursts of output into a few redraws a second.
                delay(LOG_REFRESH_MS)
            }
        }
    }

    private fun render(state: LaunchState) {
        val failure = when (state) {
            LaunchState.TimedOut -> activity.getString(R.string.launch_splash_no_window, launch.name)
            is LaunchState.Exited -> activity.getString(R.string.launch_splash_exited, launch.name, state.code)
            is LaunchState.Failed -> activity.getString(R.string.launch_splash_failed, launch.name, state.message)
            else -> null
        }
        when {
            failure != null -> {
                status.text = failure
                splash.visibility = View.GONE
                logPanel.visibility = View.VISIBLE
                showLog()
            }
            state == LaunchState.Shown -> hide()
            state == LaunchState.Quit -> {
                Toast.makeText(
                    activity,
                    activity.getString(R.string.launch_splash_quit, launch.name),
                    Toast.LENGTH_LONG,
                ).show()
                close()
            }
            state == LaunchState.Detached -> close()
        }
    }

    private fun showLog() {
        val atEnd = !logScroll.canScrollVertically(1)
        logText.text = launch.logText().ifEmpty { activity.getString(R.string.launch_splash_no_output) }
        if (atEnd) logScroll.post { logScroll.scrollTo(0, logText.height) }
    }

    private fun hide() {
        if (!active) return
        active = false
        view.animate().alpha(0f).setDuration(FADE_MS).withEndAction { view.visibility = View.GONE }
    }

    private companion object {
        const val ICON_DP = 96
        const val FADE_MS = 150L
        const val LOG_REFRESH_MS = 200L
    }
}
