package me.phie.tawc.home

import android.app.ActivityManager
import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.launcher.EntryLauncher
import me.phie.tawc.launcher.IconLoader
import me.phie.tawc.launcher.LauncherEntry
import me.phie.tawc.ui.paneTopRowHeightPx
import me.phie.tawc.ui.tawcCard

/**
 * Home screen's right-hand column on wide screens: what the open distro
 * is doing (Active distro) and which apps are one tap away (Quick
 * launch).
 *
 * Additive by design — a narrow screen builds none of it and keeps the
 * single-pane layout. Everything shown is state the app already has:
 * the memory and uptime numbers come from the kernel, which is also
 * what `free` and `uptime` report *inside* a rootfs, since a rootfs
 * shares this kernel with the app.
 */
internal class DashboardColumn(
    private val activity: AppCompatActivity,
    private val host: Host,
) {

    interface Host {
        /** Reach the full app list (the "View all" affordance). */
        fun openApps()

        /** Show the distro's info screen (a tap on the distro card). */
        fun openDistroInfo(installId: String)
    }

    private val density = activity.resources.displayMetrics.density
    private val pad = (16 * density).toInt()
    private val store = InstallationStore(activity)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val iconLoader = IconLoader(scope, (ICON_DP * density).toInt())

    private val handler = Handler(Looper.getMainLooper())
    private val distroIcon = ImageView(activity)
    private val distroName = TextView(activity)
    private val distroSubtitle = TextView(activity)
    private val distroState = chip()
    private val memoryValue = TextView(activity)
    private val uptimeValue = TextView(activity)
    private val memoryBar = LinearProgressIndicator(activity)
    private val quickRows = LinearLayout(activity)
    private val quickEmpty = TextView(activity)
    private val quickCard = activity.tawcCard()

    val view: LinearLayout = LinearLayout(activity).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(pad, 0, pad, pad)
    }

    /** Open distro as last bound; the quick-launch rows and the stats read it. */
    private var installation: Installation? = null

    /** Last scan result; each render replaces it wholesale. */
    private var entries: List<LauncherEntry> = emptyList()

    private val statsTick = object : Runnable {
        override fun run() {
            renderStats()
            handler.postDelayed(this, STATS_TICK_MS)
        }
    }

    init {
        distroIcon.setImageResource(R.drawable.ic_linux_logo)
        distroIcon.setColorFilter(activity.getColor(R.color.tawc_accent))
        distroName.textSize = 18f
        distroSubtitle.textSize = 13f
        distroSubtitle.setTextColor(secondaryTextColor())
        distroSubtitle.isSingleLine = true
        distroSubtitle.ellipsize = TextUtils.TruncateAt.END
        quickEmpty.text = activity.getString(R.string.home_dashboard_no_apps)
        quickEmpty.textSize = 13f
        quickEmpty.setTextColor(secondaryTextColor())
        quickRows.orientation = LinearLayout.VERTICAL
        memoryBar.isIndeterminate = false

        view.addView(View(activity), LinearLayout.LayoutParams(MATCH_PARENT, activity.paneTopRowHeightPx()))
        view.addView(buildDistroCard(), LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        buildQuickCard()
        view.addView(quickCard, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = pad })

        // Stats tick only while the column is on screen; a detached view
        // has nobody to update.
        view.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) {
                handler.postDelayed(statsTick, STATS_TICK_MS)
            }

            override fun onViewDetachedFromWindow(v: View) {
                handler.removeCallbacks(statsTick)
            }
        })
    }

    /** Re-read the open distro. Rescans the app list only when it changed. */
    fun bind(inst: Installation) {
        val changed = inst.id != installation?.id
        installation = inst
        distroName.text = DistroRegistry.displayLabel(inst)
        distroSubtitle.text = listOfNotNull(
            DistroRegistry.forInstallation(inst)?.linuxArch,
            inst.method,
        ).joinToString(" · ")
        distroState.text = stateLabel(inst.state)
        renderStats()
        if (changed) scanApps() else renderQuickLaunch()
    }

    /** Coming back to the screen: the rootfs may have gained apps. */
    fun onResume() {
        installation?.let { renderStats() }
        if (installation != null) scanApps()
    }

    fun destroy() {
        handler.removeCallbacks(statsTick)
        scope.cancel()
    }

    // ---- cards ---------------------------------------------------------------

    private fun buildDistroCard(): View {
        val card = activity.tawcCard()
        card.isClickable = true
        card.setOnClickListener {
            installation?.let { host.openDistroInfo(it.id) }
        }
        card.contentDescription = activity.getString(R.string.home_dashboard_active_distro)

        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        content.addView(label(activity.getString(R.string.home_dashboard_active_distro)))

        val title = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        title.addView(distroIcon, LinearLayout.LayoutParams((32 * density).toInt(), (32 * density).toInt()))
        title.addView(distroName, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).also { it.marginStart = pad / 2 })
        title.addView(distroState, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        content.addView(title, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = pad / 2 })
        content.addView(distroSubtitle, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        val stats = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        stats.addView(stat(activity.getString(R.string.home_dashboard_memory), memoryValue), weightLp())
        stats.addView(stat(activity.getString(R.string.home_dashboard_uptime), uptimeValue), weightLp())
        content.addView(stats, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = pad })
        content.addView(memoryBar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        card.addView(content)
        return card
    }

    private fun buildQuickCard() {
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad / 2)
        }
        val header = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(label(activity.getString(R.string.home_dashboard_quick_launch)))
        header.addView(View(activity), LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        header.addView(TextView(activity).apply {
            text = activity.getString(R.string.home_dashboard_view_all)
            textSize = 13f
            setTextColor(activity.getColor(R.color.tawc_accent))
            setPadding(pad / 2, pad / 4, 0, pad / 4)
            isClickable = true
            setOnClickListener { host.openApps() }
        })
        content.addView(header)
        content.addView(quickRows, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        content.addView(quickEmpty, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        quickCard.addView(content)
    }

    // ---- quick launch --------------------------------------------------------

    private fun scanApps() {
        val inst = installation ?: return
        val rootfs = store.rootfsDir(inst.id).absolutePath
        val hidden = inst.hiddenDesktopIds.toSet()
        scope.launch {
            val scanned = withContext(Dispatchers.IO) { LauncherEntry.scan(rootfs) }
            entries = scanned
                .filter { it.id !in hidden }
                .sortedBy { it.name.lowercase() }
            renderQuickLaunch()
        }
    }

    private fun renderQuickLaunch() {
        quickRows.removeAllViews()
        val shown = entries.take(QUICK_LAUNCH_MAX)
        quickEmpty.visibility = if (shown.isEmpty()) View.VISIBLE else View.GONE
        quickCard.visibility = View.VISIBLE
        for (entry in shown) quickRows.addView(entryRow(entry))
    }

    private fun entryRow(entry: LauncherEntry): View {
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, pad / 2, 0, pad / 2)
            isClickable = true
            setOnClickListener {
                installation?.let { EntryLauncher.launch(activity, it, entry) }
            }
        }
        val icon = ImageView(activity)
        row.addView(icon, LinearLayout.LayoutParams((ICON_DP * density).toInt(), (ICON_DP * density).toInt()))
        iconLoader.load(entry.iconPath, icon, R.drawable.ic_app_fallback)

        val column = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        column.addView(TextView(activity).apply {
            text = entry.name
            textSize = 15f
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
        })
        column.addView(TextView(activity).apply {
            text = entry.comment
            textSize = 12f
            setTextColor(secondaryTextColor())
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
        })
        row.addView(column, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).also { it.marginStart = pad / 2 })
        row.contentDescription = entry.name
        return row
    }

    // ---- stats ---------------------------------------------------------------

    private fun renderStats() {
        val manager = activity.getSystemService(ActivityManager::class.java) ?: return
        val info = ActivityManager.MemoryInfo().also { manager.getMemoryInfo(it) }
        val used = (info.totalMem - info.availMem).coerceAtLeast(0)
        memoryValue.text = DashboardStats.formatBytes(used)
        memoryBar.progress = if (info.totalMem > 0) ((used * 100) / info.totalMem).toInt() else 0
        uptimeValue.text = DashboardStats.formatUptime(SystemClock.elapsedRealtime())
    }

    // ---- small builders ------------------------------------------------------

    private fun label(title: String): TextView = TextView(activity).apply {
        this.text = title.uppercase()
        textSize = 12f
        letterSpacing = 0.08f
        setTextColor(secondaryTextColor())
    }

    private fun chip(): TextView = TextView(activity).apply {
        textSize = 12f
        setTextColor(activity.getColor(R.color.tawc_on_tonal))
        background = GradientDrawable().apply {
            cornerRadius = 999f
            setColor(activity.getColor(R.color.tawc_accent_container))
        }
        setPadding(pad / 2, pad / 4, pad / 2, pad / 4)
    }

    private fun stat(name: String, value: TextView): LinearLayout =
        LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(label(name))
            value.textSize = 20f
            addView(value)
        }

    private fun weightLp(): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)

    private fun stateLabel(state: Installation.State): String = when (state) {
        Installation.State.READY -> activity.getString(R.string.install_state_ready)
        Installation.State.INSTALLING -> activity.getString(R.string.install_state_installing)
        Installation.State.UNINSTALLING -> activity.getString(R.string.install_state_uninstalling)
        Installation.State.FAILED -> activity.getString(R.string.install_state_failed)
        Installation.State.CORRUPT -> activity.getString(R.string.install_state_corrupt)
    }

    /** `?android:textColorSecondary`, i.e. what the platform dims labels with. */
    private fun secondaryTextColor(): Int {
        val value = TypedValue()
        activity.theme.resolveAttribute(android.R.attr.textColorSecondary, value, true)
        return if (value.resourceId != 0) activity.getColor(value.resourceId) else value.data
    }

    private companion object {
        const val ICON_DP = 28
        const val QUICK_LAUNCH_MAX = 4
        const val STATS_TICK_MS = 30_000L
    }
}

/**
 * Number formatting for the dashboard. Pure, so the shapes the card
 * shows are unit-tested instead of eyeballed on a device.
 */
internal object DashboardStats {

    private const val GIB = 1L shl 30
    private const val MIB = 1L shl 20

    /** "2.4 GB" / "512 MB" — one decimal above a gigabyte, none below. */
    fun formatBytes(bytes: Long): String = when {
        bytes >= GIB -> String.format(Locale.US, "%.1f GB", bytes.toDouble() / GIB)
        else -> "${bytes / MIB} MB"
    }

    /** "2h 14m" / "3d 4h" / "14m". */
    fun formatUptime(millis: Long): String {
        val minutes = millis / 60_000
        val hours = minutes / 60
        val days = hours / 24
        return when {
            days > 0 -> "${days}d ${hours % 24}h"
            hours > 0 -> "${hours}h ${minutes % 60}m"
            else -> "${minutes}m"
        }
    }
}
