package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.graphics.Rect
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
import android.widget.PopupMenu
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.tonalIconButton
import me.phie.tawc.ui.verticalLp
import kotlin.math.max
import kotlin.math.min

/**
 * App launcher: type-to-filter list of installed `.desktop` apps for one
 * distro. The Rust compositor library does the actual scanning
 * ([NativeBridge.nativeLauncherScan]); Kotlin here just renders + filters
 * + dispatches launches.
 *
 * UX is intentionally minimal: one search field, one scrolling list,
 * Enter launches the top match, tap launches that row. Long-press opens
 * a per-entry action menu (Hide/Unhide, Add to home screen, Edit —
 * assembled in [entryActionsFor]). The ⋮ overflow beside the search field
 * holds the transient "Show hidden" toggle. Pinning, frecency,
 * window-list integration are deferred (see notes/launcher.md
 * "Future UX").
 *
 * Launches are fire-and-forget via [EntryLauncher], whose process-wide
 * scope outlives this Activity — closing the launcher does NOT kill the
 * program.
 *
 * Hidden entries ([Installation.hiddenDesktopIds]) are filtered here in
 * Kotlin, not in the Rust scanner — hide state is per-install app
 * metadata, and the scanner is shared with window icon/title resolution
 * which must keep seeing hidden apps (notes/launcher.md).
 */
class LauncherActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }

    private lateinit var searchField: EditText
    private lateinit var menuButton: View
    private lateinit var listColumn: LinearLayout
    private lateinit var emptyView: TextView

    /** Full app list (loaded once). Filtered subset is rebuilt on every keystroke. */
    private var allEntries: List<LauncherEntry> = emptyList()
    private var filteredEntries: List<LauncherEntry> = emptyList()

    /** Render hidden entries (dimmed, in sort position). Transient
     *  per-Activity state, deliberately not persisted. */
    private var showHidden = false

    /** UI scope for loading + search filtering. Cancelled on destroy. */
    private val uiScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Lazy because it needs `displayMetrics.density`, which is only safe
     *  to read after `onCreate` is on the way (Activity context is bound). */
    private val iconLoader by lazy { IconLoader(uiScope, iconSizePx) }
    private val iconSizePx by lazy { (ICON_SIZE_DP * resources.displayMetrics.density).toInt() }

    private var installationId: String = ""
    private var installation: Installation? = null

    /** One launch per Activity instance. A hardware Enter arrives both as
     *  a key event and as the IME editor action (~10ms apart), and finish()
     *  isn't instant, so without this guard one press launches twice. */
    private var launched = false

    /** Editor round-trip: RESULT_OK means the rootfs changed — rescan. */
    private val editEntry = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) loadApps()
    }
    private var popupWidthPx = 0
    private var popupHeightPx = 0

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
            hint = getString(R.string.hint_search)
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
        menuButton = tonalIconButton(
            R.drawable.ic_more_vert,
            getString(R.string.launcher_menu_description),
        ) { showOverflowMenu() }
        val searchRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        searchRow.addView(searchField, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        searchRow.addView(
            menuButton,
            LinearLayout.LayoutParams(tawcButtonSizePx(), tawcButtonSizePx()).also {
                it.marginStart = pad / 2
            },
        )
        content.addView(
            searchRow,
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )

        emptyView = TextView(this).apply {
            text = getString(R.string.launcher_loading_apps)
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
        ViewCompat.setOnApplyWindowInsetsListener(window.decorView) { _, insets ->
            sizePopupWindow(insets)
            insets
        }
        content.viewTreeObserver.addOnGlobalLayoutListener {
            sizePopupWindow(ViewCompat.getRootWindowInsets(window.decorView))
        }
        ViewCompat.requestApplyInsets(window.decorView)

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
        sizePopupWindow(null)
    }

    override fun onDestroy() {
        super.onDestroy()
        uiScope.cancel()
        // LAUNCH_SCOPE intentionally NOT cancelled — backing out of this
        // activity should not tear down a freshly launched application.
    }

    private fun sizePopupWindow(insets: WindowInsetsCompat?) {
        val display = resources.displayMetrics
        val density = display.density
        val width = min((display.widthPixels * 0.92f).toInt(), (720 * density).toInt())
        val naturalHeight = min((display.heightPixels * 0.78f).toInt(), (640 * density).toInt())
        val bars = insets?.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        val ime = insets?.getInsets(WindowInsetsCompat.Type.ime())
        val imeVisible = insets?.isVisible(WindowInsetsCompat.Type.ime()) == true
        val visibleFrame = Rect().also { window.decorView.getWindowVisibleDisplayFrame(it) }
        val keyboardGap = if (imeVisible) (16 * density).toInt() else 0
        val availableHeight = if (imeVisible && (ime?.bottom ?: 0) > 0) {
            val coveredBottom = max(bars?.bottom ?: 0, ime?.bottom ?: 0)
            display.heightPixels - (bars?.top ?: 0) - coveredBottom - keyboardGap
        } else if (imeVisible && visibleFrame.height() > 0) {
            visibleFrame.height() - keyboardGap
        } else {
            naturalHeight
        }.coerceAtLeast((240 * density).toInt())
        val height = min(naturalHeight, availableHeight)
        if (width == popupWidthPx && height == popupHeightPx) return
        popupWidthPx = width
        popupHeightPx = height
        window.setLayout(width, height)
    }

    private fun loadApps() {
        val inst = installation
        if (inst == null) {
            emptyView.text = getString(R.string.launcher_installation_not_found, installationId)
            return
        }
        if (inst.state == Installation.State.INSTALLING ||
            inst.state == Installation.State.UNINSTALLING) {
            emptyView.text = getString(R.string.launcher_installation_state_wait, inst.state.name.lowercase())
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
                emptyView.text = getString(R.string.launcher_no_launchable_apps)
            }
        }
    }

    /** Ids the user hid, from the current metadata record. */
    private fun hiddenIds(): Set<String> =
        installation?.hiddenDesktopIds?.toSet() ?: emptySet()

    /** Hidden entries that actually exist in this rootfs (stale ids don't count). */
    private fun hiddenCount(): Int {
        val hidden = hiddenIds()
        return allEntries.count { it.id in hidden }
    }

    /**
     * Re-filter [allEntries] against hide state + the search field.
     * Hidden entries are dropped unless [showHidden]. Query is a
     * substring match, case-insensitive, against name + id + comment.
     * Order: prefix matches on name first (so typing "fire" surfaces
     * Firefox above "WireFire"), then any other substring match, all
     * already pre-sorted by name from the Rust scanner.
     */
    private fun applyFilter() {
        val hidden = hiddenIds()
        val visible = if (showHidden) allEntries else allEntries.filter { it.id !in hidden }
        val q = searchField.text.toString().trim().lowercase()
        filteredEntries = if (q.isEmpty()) {
            visible
        } else {
            val prefix = ArrayList<LauncherEntry>()
            val other = ArrayList<LauncherEntry>()
            for (e in visible) {
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
            val q = searchField.text.toString().trim()
            emptyView.text = if (q.isEmpty()) {
                // Every entry is hidden (show-hidden off): keep the
                // no-apps message but say why the list is empty.
                getString(R.string.launcher_no_launchable_apps) + "\n" +
                    getString(R.string.launcher_hidden_count_hint, hiddenCount())
            } else {
                getString(R.string.launcher_no_matches)
            }
            emptyView.visibility = View.VISIBLE
            return
        }
        emptyView.visibility = if (allEntries.isEmpty()) View.VISIBLE else View.GONE
        val hidden = hiddenIds()
        val pad = (12 * resources.displayMetrics.density).toInt()
        val rowMargin = (6 * resources.displayMetrics.density).toInt()
        for (entry in filteredEntries) {
            val row = buildRow(entry, pad, dimmed = entry.id in hidden)
            listColumn.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = rowMargin))
        }
    }

    private fun showOverflowMenu() {
        PopupMenu(this, menuButton).apply {
            menu.add(getString(R.string.launcher_menu_show_hidden, hiddenCount())).apply {
                isCheckable = true
                isChecked = showHidden
                setOnMenuItemClickListener {
                    showHidden = !showHidden
                    applyFilter()
                    true
                }
            }
            if (canEditEntries()) {
                menu.add(getString(R.string.launcher_menu_add_entry)).setOnMenuItemClickListener {
                    openEditor(null)
                    true
                }
            }
            show()
        }
    }

    /**
     * Editor writes are plain app-uid file I/O into the rootfs — works
     * for tawcroot/proot but not chroot's root-owned rootfs
     * (notes/launcher.md "Access model"), so chroot installs get no
     * New/Edit entry points, consistent with the terminal gating.
     */
    private fun canEditEntries(): Boolean {
        val inst = installation ?: return false
        return inst.method != Installation.METHOD_CHROOT
    }

    private fun openEditor(entryPath: String?) {
        val inst = installation ?: return
        val i = Intent(this, DesktopFileEditorActivity::class.java)
            .putExtra(DesktopFileEditorActivity.EXTRA_ID, inst.id)
        if (entryPath != null) i.putExtra(DesktopFileEditorActivity.EXTRA_PATH, entryPath)
        editEntry.launch(i)
    }

    /**
     * One long-press menu item. Assembled per entry by [entryActionsFor]
     * so dependent features (Add to home screen, Edit) can append items
     * conditionally; disabled actions are not shown.
     */
    private data class EntryAction(
        val label: CharSequence,
        val enabled: Boolean = true,
        val run: () -> Unit,
    )

    private fun entryActionsFor(entry: LauncherEntry): List<EntryAction> {
        val hidden = entry.id in hiddenIds()
        return listOfNotNull(
            if (hidden) {
                EntryAction(getString(R.string.launcher_action_unhide)) { setEntryHidden(entry, false) }
            } else {
                EntryAction(getString(R.string.launcher_action_hide)) { setEntryHidden(entry, true) }
            },
            EntryAction(getString(R.string.launcher_action_add_home)) { pinEntry(entry) },
            // Only entries in the managed dir are editable — everything
            // else is package-owned (see DesktopEntryFile).
            EntryAction(getString(R.string.launcher_action_edit)) { openEditor(entry.path) }
                .takeIf {
                    canEditEntries() &&
                        DesktopEntryFile.isManaged(entry.path, store.rootfsDir(installationId))
                },
        )
    }

    private fun showEntryMenu(entry: LauncherEntry) {
        val actions = entryActionsFor(entry).filter { it.enabled }
        if (actions.isEmpty()) return
        AlertDialog.Builder(this)
            .setTitle(entry.name.ifEmpty { entry.id })
            .setItems(actions.map { it.label }.toTypedArray()) { _, which ->
                actions[which].run()
            }
            .show()
    }

    /**
     * Pin [entry] to the home screen ([EntryShortcuts]). Icon decode is
     * I/O, so build the request off the main thread; the system pin
     * sheet takes over from there.
     */
    private fun pinEntry(entry: LauncherEntry) {
        val inst = installation ?: return
        uiScope.launch {
            val result = withContext(Dispatchers.IO) {
                EntryShortcuts.requestPin(this@LauncherActivity, inst, entry)
            }
            val toast = when (result) {
                EntryShortcuts.PinResult.REQUESTED -> null
                EntryShortcuts.PinResult.UPDATED -> R.string.shortcut_pin_updated
                EntryShortcuts.PinResult.UNSUPPORTED -> R.string.shortcut_pin_unsupported
            }
            toast?.let { Toast.makeText(this@LauncherActivity, it, Toast.LENGTH_SHORT).show() }
        }
    }

    /**
     * Persist hide/unhide through the locked read-modify-write.
     * [InstallationStore.update] returns the record it wrote, which
     * becomes the new [installation] so the filter sees the fresh set;
     * null (lost race against uninstall) just leaves the list as-is —
     * the whole slot is going away.
     */
    private fun setEntryHidden(entry: LauncherEntry, hidden: Boolean) {
        val inst = installation ?: return
        store.update(inst.id) { it.withEntryHidden(entry.id, hidden) }
            ?.let { installation = it }
        applyFilter()
    }

    private fun buildRow(entry: LauncherEntry, pad: Int, dimmed: Boolean = false): View {
        val card = tawcCard().apply {
            isClickable = true
            isFocusable = true
            isLongClickable = true
            if (dimmed) alpha = 0.5f
            setOnClickListener { launchEntry(entry) }
            setOnLongClickListener { showEntryMenu(entry); true }
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
     * Fire-and-forget launch via [EntryLauncher]; failures surface from
     * there ([LaunchErrorActivity]). We finish the Activity right away —
     * the launch scope is process-wide so the coroutine keeps running.
     */
    private fun launchEntry(entry: LauncherEntry) {
        if (launched) return
        val inst = installation ?: return
        launched = true
        EntryLauncher.launch(applicationContext, inst, entry)
        finish()
    }

    companion object {
        const val EXTRA_ID = "id"

        /** Square icon edge in dp. ~48 is the standard list-row icon size
         *  per Material guidelines; bumped to 56 here because chroot apps
         *  rarely have icon sets that look crisp at the smaller size and
         *  the extra display surface helps with brand recognition. */
        private const val ICON_SIZE_DP = 56f
    }
}
