package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.os.SystemClock
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.Menu
import android.view.View
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.doAfterTextChanged
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.searchPill
import me.phie.tawc.ui.verticalLp

/**
 * The home screen's apps tab: an alphabetical icon grid of installed
 * `.desktop` apps for one distro, plus the app's built-in entries
 * ([LauncherEntry.Builtin]). The Rust compositor library does the
 * actual scanning and name sort ([LauncherEntry.scan]); Kotlin here
 * just renders + filters + dispatches launches.
 *
 * Layout is Android-launcher-like: an always-visible search bar
 * ("Search <distro>"), then a grid of icons with single-line names (no
 * descriptions). The bar filters the grid; Enter launches the top
 * match, ✕ or Back clears it, and typing on a hardware keyboard with
 * nothing focused types into it. Tap launches; long-press opens a
 * per-entry action menu (Hide/Unhide, Add to home screen, Edit —
 * assembled in [entryActionsFor]). The home ⋮ gets this pane's items
 * from [addMenuItems] (Show hidden, Add entry…). Frecency and
 * window-list integration are deferred (see notes/launcher.md
 * "Future UX").
 *
 * Launches are fire-and-forget via [EntryLauncher], whose process-wide
 * scope outlives the pane. The list is rescanned on every show and
 * resume, so packages installed from the terminal show up.
 *
 * Hidden entries ([Installation.hiddenDesktopIds]) are filtered here in
 * Kotlin, not in the Rust scanner — hide state is per-install app
 * metadata, and the scanner is shared with window icon/title resolution
 * which must keep seeing hidden apps (notes/launcher.md).
 */
internal class AppsPane(
    private val activity: AppCompatActivity,
    private var installation: Installation,
    private val host: Host,
) {

    interface Host {
        /** Start the `.desktop` editor for result; RESULT_OK → [rescan]. */
        fun openEditor(intent: Intent)
        /** Grid scrolled; hide the FAB going down, show it going up. */
        fun onGridScrolled(down: Boolean)
    }

    private val store = InstallationStore(activity)
    private val density = activity.resources.displayMetrics.density
    private val pad = (16 * density).toInt()

    private val searchField: EditText
    private val clearButton: View
    private val grid: RecyclerView
    private val gridLayout: GridLayoutManager
    private val adapter = EntryAdapter()
    private val emptyView: TextView

    /** Full app list (from the last scan). Filtered subset is rebuilt on every keystroke. */
    private var allEntries: List<LauncherEntry> = emptyList()
    private var filteredEntries: List<LauncherEntry> = emptyList()

    /** Render hidden entries (dimmed, in sort position). Transient
     *  per-pane state, deliberately not persisted. */
    private var showHidden = false

    /** The first scan is done (until then the empty view says loading). */
    private var loaded = false

    /** UI scope for loading + search filtering. Cancelled on [destroy]. */
    private val uiScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private val iconSizePx = (ICON_SIZE_DP * density).toInt()

    /** `?attr/colorControlNormal`, for the bare Add entry glyph. */
    private val controlTint: ColorStateList? = TypedValue().let {
        activity.theme.resolveAttribute(androidx.appcompat.R.attr.colorControlNormal, it, true)
        if (it.resourceId != 0) activity.getColorStateList(it.resourceId) else ColorStateList.valueOf(it.data)
    }
    private val iconLoader = IconLoader(uiScope, iconSizePx)

    /** A hardware Enter arrives both as a key event and as the IME
     *  editor action (~10ms apart); without a debounce one press
     *  launches twice. */
    private var lastLaunchMs = 0L

    val view: LinearLayout

    val searchHasFocus: Boolean get() = searchField.hasFocus()

    init {
        view = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            // Soaks up the window's initial focus, which would otherwise
            // land in the search field: it takes focus only on a tap or
            // hardware typing ([onUnhandledKey]).
            isFocusableInTouchMode = true
            // Out of touch mode (key input) the highlight would grey the
            // whole page.
            defaultFocusHighlightEnabled = false
        }

        clearButton = activity.plainIconButton(R.drawable.ic_close, activity.getString(R.string.action_clear_search)) {
            clearSearch()
        }.apply { visibility = View.INVISIBLE }
        searchField = EditText(activity).apply {
            hint = activity.getString(R.string.hint_search)
            textSize = 16f
            isSingleLine = true
            background = null
            imeOptions = EditorInfo.IME_ACTION_GO
            isFocusableInTouchMode = true
            doAfterTextChanged {
                clearButton.visibility = if (it.isNullOrEmpty()) View.INVISIBLE else View.VISIBLE
                applyFilter()
                grid.scrollToPosition(0)
            }
            setOnEditorActionListener { _, actionId, event ->
                val isEnter = actionId == EditorInfo.IME_ACTION_GO ||
                    actionId == EditorInfo.IME_ACTION_DONE ||
                    (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
                if (isEnter) { launchTop(); true } else false
            }
        }
        view.addView(
            activity.searchPill(searchField, clearButton),
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also {
                it.setMargins(pad, pad * 3 / 4, pad, pad / 2)
            },
        )

        emptyView = TextView(activity).apply {
            text = activity.getString(R.string.launcher_loading_apps)
            textSize = 14f
            alpha = 0.7f
            setPadding(pad, pad / 2, pad, 0)
        }
        view.addView(emptyView, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))

        gridLayout = GridLayoutManager(activity, MIN_COLUMNS)
        grid = EdgeFadeRecyclerView(activity).apply {
            layoutManager = gridLayout
            adapter = this@AppsPane.adapter
            // Room past the last row, so it can scroll clear of the FAB.
            setPadding(pad / 2, pad / 4, pad / 2, (BOTTOM_CLEARANCE_DP * density).toInt())
            clipToPadding = false
            isVerticalFadingEdgeEnabled = true
            setFadingEdgeLength(pad)
            overScrollMode = View.OVER_SCROLL_NEVER
            addOnScrollListener(object : RecyclerView.OnScrollListener() {
                override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) {
                    if (dy != 0) host.onGridScrolled(dy > 0)
                }
            })
            // Columns follow the width (rotation, split screen).
            addOnLayoutChangeListener { v, _, _, _, _, _, _, _, _ ->
                val columns = maxOf(MIN_COLUMNS, v.width / (CELL_MIN_WIDTH_DP * density).toInt())
                if (columns != gridLayout.spanCount) v.post { gridLayout.spanCount = columns }
            }
        }
        view.addView(grid, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        rescan()
    }

    fun onResume() {
        // Hide state may have changed elsewhere (another pane instance).
        store.load(installation.id)?.let { installation = it }
        rescan()
    }

    fun destroy() {
        uiScope.cancel()
    }

    /**
     * Clear the query, drop the IME and give up focus. Returns whether
     * there was a query (Back clears it before leaving).
     */
    fun clearSearch(): Boolean {
        val had = searchField.text.isNotEmpty()
        val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.hideSoftInputFromWindow(searchField.windowToken, 0)
        view.requestFocus()
        searchField.text.clear()
        return had
    }

    /** Take focus off the search field (the apps tab was just shown). */
    fun unfocus() {
        view.requestFocus()
    }

    /**
     * A key nothing focused consumed (hardware keyboard): a printable
     * character goes into the search field, launcher-style.
     */
    fun onUnhandledKey(event: KeyEvent): Boolean {
        if (searchField.hasFocus() || event.isCtrlPressed || event.isAltPressed || event.isMetaPressed) return false
        val c = event.unicodeChar
        if (c == 0 || Character.isISOControl(c) || Character.isWhitespace(c)) return false
        searchField.requestFocus()
        searchField.append(String(Character.toChars(c)))
        val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(searchField, InputMethodManager.SHOW_IMPLICIT)
        return true
    }

    /** This pane's group of the home ⋮ menu. */
    fun addMenuItems(menu: Menu, order: Int) {
        val hidden = hiddenCount()
        if (hidden > 0) {
            menu.add(Menu.NONE, Menu.NONE, order, activity.getString(R.string.launcher_menu_show_hidden, hidden))
                .apply {
                isCheckable = true
                isChecked = showHidden
                setOnMenuItemClickListener {
                    showHidden = !showHidden
                    applyFilter()
                    true
                }
            }
        }
        if (canEditEntries()) {
            menu.add(Menu.NONE, Menu.NONE, order, R.string.launcher_menu_add_entry).setOnMenuItemClickListener {
                openEditor(null)
                true
            }
        }
    }

    fun rescan() {
        val inst = installation
        val rootfs = store.rootfsDir(inst.id).absolutePath
        uiScope.launch {
            allEntries = withContext(Dispatchers.IO) {
                iconLoader.dropStale()
                LauncherEntry.list(activity, inst, rootfs)
            }
            loaded = true
            applyFilter()
        }
    }

    /** Ids the user hid, from the current metadata record. */
    private fun hiddenIds(): Set<String> = installation.hiddenDesktopIds.toSet()

    /** Hidden entries that actually exist in this rootfs (stale ids don't count). */
    private fun hiddenCount(): Int {
        val hidden = hiddenIds()
        return allEntries.count { it.id in hidden }
    }

    /** Re-filter [allEntries] against hide state + the search field
     *  ([LauncherEntry.filter]) and re-render. */
    private fun applyFilter() {
        filteredEntries = LauncherEntry.filter(
            allEntries, hiddenIds(), showHidden, searchField.text.toString(),
        )
        renderList()
    }

    private fun renderList() {
        adapter.submit(filteredEntries, hiddenIds())
        if (!loaded) return
        if (filteredEntries.isNotEmpty()) {
            emptyView.visibility = View.GONE
            return
        }
        val hidden = hiddenCount()
        emptyView.text = when {
            searchField.text.isNotBlank() -> activity.getString(R.string.launcher_no_matches)
            // Every entry is hidden (show-hidden off): say why the grid
            // is empty.
            hidden > 0 -> activity.getString(R.string.launcher_no_launchable_apps) + "\n" +
                activity.getString(R.string.launcher_hidden_count_hint, hidden)
            else -> activity.getString(R.string.launcher_no_launchable_apps)
        }
        emptyView.visibility = View.VISIBLE
    }

    /**
     * Editor writes are plain app-uid file I/O into the rootfs — works
     * for tawcroot/proot but not chroot's root-owned rootfs
     * (notes/launcher.md "Access model"), so chroot installs get no
     * New/Edit entry points, consistent with the terminal gating.
     */
    private fun canEditEntries(): Boolean = installation.method != Installation.METHOD_CHROOT

    /** The editor for [entry], or a new entry when null. */
    private fun openEditor(entry: LauncherEntry?) {
        val i = Intent(activity, DesktopFileEditorActivity::class.java)
            .putExtra(DesktopFileEditorActivity.EXTRA_ID, installation.id)
        if (entry != null) {
            i.putExtra(DesktopFileEditorActivity.EXTRA_PATH, entry.path)
            i.putExtra(DesktopFileEditorActivity.EXTRA_SHADOWS, entry.shadows)
        }
        host.openEditor(i)
    }

    /**
     * One long-press menu item. Assembled per entry by [entryActionsFor];
     * conditional items are dropped there (`takeIf`/`listOfNotNull`).
     */
    private data class EntryAction(
        val label: CharSequence,
        val run: () -> Unit,
    )

    private fun entryActionsFor(entry: LauncherEntry): List<EntryAction> {
        val hidden = entry.id in hiddenIds()
        val builtin = entry.builtin
        return listOfNotNull(
            if (hidden) {
                EntryAction(activity.getString(R.string.launcher_action_unhide)) { setEntryHidden(entry, false) }
            } else {
                EntryAction(activity.getString(R.string.launcher_action_hide)) { setEntryHidden(entry, true) }
            },
            EntryAction(activity.getString(R.string.launcher_action_add_home)) { pinEntry(entry) }
                .takeIf { builtin == null || builtin.opensTerminal },
            // Any scanned entry; editing a packaged one saves an
            // override copy in the managed dir (see DesktopEntryFile).
            EntryAction(activity.getString(R.string.launcher_action_edit)) { openEditor(entry) }
                .takeIf { builtin == null && canEditEntries() },
        )
    }

    private fun showEntryMenu(entry: LauncherEntry) {
        val actions = entryActionsFor(entry)
        if (actions.isEmpty()) return
        AlertDialog.Builder(activity)
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
        val inst = installation
        uiScope.launch {
            val result = withContext(Dispatchers.IO) {
                EntryShortcuts.requestPin(activity, inst, entry)
            }
            val toast = when (result) {
                EntryShortcuts.PinResult.REQUESTED -> null
                EntryShortcuts.PinResult.UPDATED -> R.string.shortcut_pin_updated
                EntryShortcuts.PinResult.UNSUPPORTED -> R.string.shortcut_pin_unsupported
            }
            toast?.let { Toast.makeText(activity, it, Toast.LENGTH_SHORT).show() }
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
        store.update(installation.id) { it.withEntryHidden(entry.id, hidden) }
            ?.let { installation = it }
        applyFilter()
    }

    /**
     * With clipToPadding off, View still draws fading edges at the
     * padding lines (mid-row at the bottom); the offsets move them to
     * the view's real edges.
     */
    private class EdgeFadeRecyclerView(context: Context) : RecyclerView(context) {
        override fun isPaddingOffsetRequired() = !clipToPadding
        override fun getTopPaddingOffset() = -paddingTop
        override fun getBottomPaddingOffset() = paddingBottom
    }

    private class Cell(val root: LinearLayout, val icon: ImageView, val label: TextView) :
        RecyclerView.ViewHolder(root)

    /** Grid cells: icon over a one-line, end-ellipsized name. */
    private inner class EntryAdapter : RecyclerView.Adapter<Cell>() {
        private var entries: List<LauncherEntry> = emptyList()
        private var hidden: Set<String> = emptySet()

        fun submit(entries: List<LauncherEntry>, hidden: Set<String>) {
            this.entries = entries
            this.hidden = hidden
            notifyDataSetChanged()
        }

        override fun getItemCount() = entries.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Cell {
            val cellPad = (8 * density).toInt()
            val root = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                setPadding(cellPad, cellPad, cellPad, cellPad)
                isClickable = true
                isFocusable = true
                isLongClickable = true
                val ripple = TypedValue()
                activity.theme.resolveAttribute(android.R.attr.selectableItemBackground, ripple, true)
                setBackgroundResource(ripple.resourceId)
                layoutParams = RecyclerView.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
            }
            val icon = ImageView(activity).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
            root.addView(icon, LinearLayout.LayoutParams(iconSizePx, iconSizePx).also { it.bottomMargin = cellPad * 3 / 4 })
            val label = TextView(activity).apply {
                textSize = 12f
                isSingleLine = true
                ellipsize = TextUtils.TruncateAt.END
                gravity = Gravity.CENTER_HORIZONTAL
            }
            root.addView(label, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            return Cell(root, icon, label)
        }

        override fun onBindViewHolder(cell: Cell, position: Int) {
            val entry = entries[position]
            val name = entry.name.ifEmpty { entry.id }
            cell.label.text = name
            cell.root.contentDescription = name
            cell.root.alpha = if (entry.id in hidden) 0.5f else 1f
            cell.root.setOnClickListener { launchEntry(entry) }
            cell.root.setOnLongClickListener { showEntryMenu(entry); true }
            val builtin = entry.builtin
            cell.icon.background = null
            cell.icon.imageTintList = null
            cell.icon.setPadding(0, 0, 0, 0)
            if (builtin != null) {
                cell.icon.tag = null
                cell.icon.setImageResource(builtin.iconRes)
                if (builtin.opensTerminal) {
                    // White on a round black tile, like a terminal entry.
                    cell.icon.setBackgroundResource(R.drawable.builtin_icon_bg)
                    cell.icon.imageTintList = ColorStateList.valueOf(Color.WHITE)
                    val inset = iconSizePx / 4
                    cell.icon.setPadding(inset, inset, inset, inset)
                } else {
                    // The vector's own theme tint was cleared above.
                    cell.icon.imageTintList = controlTint
                    val inset = iconSizePx / 8
                    cell.icon.setPadding(inset, inset, inset, inset)
                }
                return
            }
            iconLoader.load(
                entry.iconPath,
                cell.icon,
                if (entry.terminal) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback,
            )
        }
    }

    private fun launchTop() {
        val top = filteredEntries.firstOrNull() ?: return
        launchEntry(top)
    }

    /**
     * Fire-and-forget launch via [EntryLauncher]; failures surface from
     * there ([LaunchErrorActivity]). Add entry opens the editor here
     * instead, for its result. Search is cleared and the IME dropped:
     * the app's window (or the terminal) comes forward, and this pane
     * is what the user returns to.
     */
    private fun launchEntry(entry: LauncherEntry) {
        val now = SystemClock.uptimeMillis()
        if (now - lastLaunchMs < LAUNCH_DEBOUNCE_MS) return
        lastLaunchMs = now
        clearSearch()
        if (entry.builtin == LauncherEntry.Builtin.ADD_ENTRY) {
            openEditor(null)
        } else {
            EntryLauncher.launch(activity.applicationContext, installation, entry)
        }
    }

    internal companion object {
        /** Square icon edge in dp, about a phone launcher's. */
        const val ICON_SIZE_DP = 52f

        /** Columns: as many [CELL_MIN_WIDTH_DP] cells as fit, at least [MIN_COLUMNS]. */
        const val CELL_MIN_WIDTH_DP = 88
        const val MIN_COLUMNS = 3

        /** Grid bottom padding: FAB (56) + its margins (16 + 16). */
        private const val BOTTOM_CLEARANCE_DP = 88

        private const val LAUNCH_DEBOUNCE_MS = 500L
    }
}
