package me.phie.tawc.launcher

import android.content.Context
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.verticalLp
import kotlin.math.min

/**
 * App launcher: type-to-filter list of installed `.desktop` apps for one
 * distro. The Rust compositor library does the actual scanning
 * ([NativeBridge.nativeLauncherScan]); Kotlin here just renders + filters
 * + dispatches launches.
 *
 * UX is intentionally minimal: one search field, one scrolling list,
 * Enter launches the top match, tap launches that row. Pinning,
 * frecency, window-list integration are deferred (see notes/launcher.md
 * "Future UX").
 *
 * Launches are fire-and-forget on the process-scoped [LAUNCH_SCOPE]
 * (Dispatchers.IO). `InstallationMethod.runInside` blocks until the
 * launched program exits, so the coroutine pins one IO thread for
 * the program's lifetime — which is fine, since the program needs
 * the JVM alive anyway (Wayland socket lives in CompositorService,
 * proot/tawcroot tracee tree gets torn down on JVM death). Closing
 * this Activity does NOT kill the program: the scope outlives the
 * Activity via JVM lifetime.
 */
class LauncherActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }

    private lateinit var searchField: EditText
    private lateinit var listColumn: LinearLayout
    private lateinit var emptyView: TextView

    /** Full app list (loaded once). Filtered subset is rebuilt on every keystroke. */
    private var allEntries: List<LauncherEntry> = emptyList()
    private var filteredEntries: List<LauncherEntry> = emptyList()

    /** UI scope for loading + search filtering. Cancelled on destroy. */
    private val uiScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Lazy because it needs `displayMetrics.density`, which is only safe
     *  to read after `onCreate` is on the way (Activity context is bound). */
    private val iconLoader by lazy { IconLoader(uiScope, iconSizePx) }
    private val iconSizePx by lazy { (ICON_SIZE_DP * resources.displayMetrics.density).toInt() }

    private var installationId: String = ""
    private var installation: Installation? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installationId = intent?.getStringExtra(EXTRA_ID) ?: ""

        installation = store.load(installationId)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad / 2)
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
        }

        searchField = EditText(this).apply {
            hint = "Search"
            textSize = 24f
            isSingleLine = true
            imeOptions = EditorInfo.IME_ACTION_GO
            // Keep the search box quietly focused on entry so the
            // soft keyboard pops up automatically — same shape as
            // `apropos` / a desktop run dialog.
            isFocusableInTouchMode = true
            requestFocus()
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) = applyFilter()
            })
            setOnEditorActionListener { _, actionId, event ->
                val isEnter = actionId == EditorInfo.IME_ACTION_GO ||
                    actionId == EditorInfo.IME_ACTION_DONE ||
                    (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
                if (isEnter) { launchTop(); true } else false
            }
        }
        content.addView(
            searchField,
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )

        emptyView = TextView(this).apply {
            text = "Loading apps…"
            textSize = 14f
            alpha = 0.7f
        }
        content.addView(
            emptyView,
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
        )

        listColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, pad / 2)
        }
        val scroll = ScrollView(this).apply {
            setFillViewport(true)
            clipToPadding = false
            isVerticalFadingEdgeEnabled = true
            setFadingEdgeLength(pad)
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(listColumn)
        }
        content.addView(scroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        setContentView(content)

        // Force the keyboard up: simply requesting focus isn't enough on
        // some Android versions when the IME hasn't been shown yet in the
        // current task. Post so the layout is attached first.
        searchField.post {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            imm?.showSoftInput(searchField, InputMethodManager.SHOW_IMPLICIT)
        }

        loadApps()
    }

    override fun onStart() {
        super.onStart()
        sizePopupWindow()
    }

    override fun onDestroy() {
        super.onDestroy()
        uiScope.cancel()
        // LAUNCH_SCOPE intentionally NOT cancelled — backing out of this
        // activity should not tear down a freshly launched application.
    }

    private fun sizePopupWindow() {
        val display = resources.displayMetrics
        val density = display.density
        val width = min((display.widthPixels * 0.92f).toInt(), (720 * density).toInt())
        val height = min((display.heightPixels * 0.78f).toInt(), (640 * density).toInt())
        window.setLayout(width, height)
    }

    private fun loadApps() {
        val inst = installation
        if (inst == null) {
            emptyView.text = "Installation '$installationId' not found."
            return
        }
        if (inst.state == Installation.State.INSTALLING ||
            inst.state == Installation.State.UNINSTALLING) {
            emptyView.text = "Installation is ${inst.state.name.lowercase()} — wait for it to finish."
            return
        }
        val rootfs = store.rootfsDir(inst.id).absolutePath
        uiScope.launch {
            val json = withContext(Dispatchers.IO) {
                runCatching { NativeBridge.nativeLauncherScan(rootfs) }.getOrNull()
            }
            allEntries = LauncherEntry.parseList(json)
            applyFilter()
            if (allEntries.isEmpty()) {
                emptyView.text = "No launchable apps found in this rootfs.\n" +
                    "Install GUI packages from a shell first."
            }
        }
    }

    /**
     * Re-filter [allEntries] against the search field. Substring match,
     * case-insensitive, against name + id + comment. Order: prefix
     * matches on name first (so typing "fire" surfaces Firefox above
     * "WireFire"), then any other substring match, all already pre-sorted
     * by name from the Rust scanner.
     */
    private fun applyFilter() {
        val q = searchField.text.toString().trim().lowercase()
        filteredEntries = if (q.isEmpty()) {
            allEntries
        } else {
            val prefix = ArrayList<LauncherEntry>()
            val other = ArrayList<LauncherEntry>()
            for (e in allEntries) {
                val n = e.name.lowercase()
                if (n.startsWith(q)) prefix.add(e)
                else if (n.contains(q) || e.id.lowercase().contains(q) ||
                    e.comment.lowercase().contains(q)) other.add(e)
            }
            prefix + other
        }
        renderList()
    }

    private fun renderList() {
        listColumn.removeAllViews()
        if (filteredEntries.isEmpty() && allEntries.isNotEmpty()) {
            emptyView.text = "No matches."
            emptyView.visibility = View.VISIBLE
            return
        }
        emptyView.visibility = if (allEntries.isEmpty()) View.VISIBLE else View.GONE
        val pad = (12 * resources.displayMetrics.density).toInt()
        val rowMargin = (6 * resources.displayMetrics.density).toInt()
        for (entry in filteredEntries) {
            listColumn.addView(buildRow(entry, pad), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = rowMargin))
        }
    }

    private fun buildRow(entry: LauncherEntry, pad: Int): View {
        val card = tawcCard().apply {
            isClickable = true
            isFocusable = true
            setOnClickListener { launchEntry(entry) }
        }
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        val icon = ImageView(this).apply {
            scaleType = ImageView.ScaleType.FIT_CENTER
            // Pre-set the layout size so rows without an icon don't
            // shift their text leftwards. ImageView default is
            // WRAP_CONTENT which collapses to 0 when the drawable is
            // null.
            adjustViewBounds = false
        }
        iconLoader.load(entry.iconPath, icon)
        row.addView(
            icon,
            LinearLayout.LayoutParams(iconSizePx, iconSizePx).also {
                it.marginEnd = pad
            },
        )

        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(TextView(this).apply {
            text = entry.name.ifEmpty { entry.id }
            textSize = 16f
        })
        if (entry.comment.isNotEmpty()) {
            column.addView(TextView(this).apply {
                text = entry.comment
                textSize = 13f
                alpha = 0.7f
            })
        }
        row.addView(column, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))

        card.addView(row)
        return card
    }

    private fun launchTop() {
        val top = filteredEntries.firstOrNull() ?: return
        launchEntry(top)
    }

    /**
     * Fire-and-forget launch via [InstallationMethod.runInside]. We
     * finish the Activity right away; [LAUNCH_SCOPE] is process-scoped
     * so the coroutine keeps running.
     *
     * Stdio is redirected to /dev/null so a chatty program can't fill
     * the pipe back to the JVM (which we never read).
     *
     * No `setsid -f` detach: under proot's `--kill-on-exit` the
     * detached child gets SIGKILLed when the launcher bash exits, so
     * the app dies before it ever opens a Wayland window. Letting
     * runInside block for the program's whole lifetime is the
     * correct behaviour anyway — the program needs the JVM alive for
     * the compositor's Wayland socket, so there's nothing to gain
     * from detaching.
     */
    private fun launchEntry(entry: LauncherEntry) {
        val inst = installation ?: return
        val method = InstallationMethod.forKey(this, inst.method) ?: return
        val rootfs = store.rootfsDir(inst.id).absolutePath
        val cmd = "${entry.exec} </dev/null >/dev/null 2>&1"
        LAUNCH_SCOPE.launch {
            runCatching { method.runInside(rootfs, cmd) }
                .onFailure { android.util.Log.w(TAG, "launch ${entry.id}: $it") }
        }
        finish()
    }

    companion object {
        const val EXTRA_ID = "id"
        private const val TAG = "tawc-launcher"

        /** Square icon edge in dp. ~48 is the standard list-row icon size
         *  per Material guidelines; bumped to 56 here because chroot apps
         *  rarely have icon sets that look crisp at the smaller size and
         *  the extra display surface helps with brand recognition. */
        private const val ICON_SIZE_DP = 56f

        /**
         * Process-wide scope for fire-and-forget launches. Outlives
         * individual [LauncherActivity] instances so closing the
         * launcher doesn't tear down the program the user just
         * started. [SupervisorJob] keeps one failed launch from
         * cancelling sibling launches.
         */
        private val LAUNCH_SCOPE =
            CoroutineScope(SupervisorJob() + Dispatchers.IO)
    }
}
