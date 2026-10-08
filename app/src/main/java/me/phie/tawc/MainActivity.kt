package me.phie.tawc

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import android.view.ContextThemeWrapper
import android.view.Gravity
import android.view.KeyEvent
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.floatingactionbutton.FloatingActionButton
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.DistroInfoView
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.install.showRunCommandDialog
import me.phie.tawc.remote.RemoteAccessActivity
import me.phie.tawc.session.SessionWake
import me.phie.tawc.session.toggleKeepAwake
import me.phie.tawc.tasks.TaskManagerActivity
import me.phie.tawc.terminal.TerminalPane
import me.phie.tawc.terminal.TerminalSessions
import me.phie.tawc.terminal.TerminalTabBar
import me.phie.tawc.ui.DrawerScreen
import me.phie.tawc.ui.buildDrawerScreen
import me.phie.tawc.ui.fabLp
import me.phie.tawc.ui.paneTopRowHeightPx
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.tawcFab
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp

/**
 * Home screen: one pane for the open distro ([OpenDistro]) —
 *
 * - intro with nothing installed,
 * - distro info ([DistroInfoView]) while it isn't READY,
 * - once it is, its [DistroHome]: a tab bar with the apps tab first,
 *   then terminal tabs, and a FAB on the apps tab opening a new one.
 *
 * Each pane supplies its own top row (≡, title or tabs, ⋮);
 * the drawer switches the open distro and starts a new install. The ⋮
 * popup is assembled here from per-distro, tab and app items. The
 * compositor starts lazily when a user launches a rootfs command, so a
 * broken graphics backend doesn't keep the home screen from opening.
 * See notes/android.md "Home screen".
 */
class MainActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private val pad by lazy { (16 * resources.displayMetrics.density).toInt() }

    private lateinit var screen: DrawerScreen
    private lateinit var fab: FloatingActionButton

    /** Open distro as last rendered; menu/FAB actions read it. */
    private var open: Installation? = null

    private var pane: Pane? = null

    /** Drawer item id → install id, rebuilt on every [refresh]. */
    private val drawerIds = mutableMapOf<Int, String>()

    /** A terminal launch ([commandIntent]) waiting for its distro's home. */
    private var queuedCommand: Pair<String, TerminalPane.CommandTab>? = null

    /** Tab to reselect when the next home is built (recreation). */
    private var restoreTab: Int? = null

    private var defaultLightBars = true

    private val editEntry = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) (pane as? Pane.Home)?.home?.apps?.rescan()
    }

    private sealed interface Pane {
        val installId: String?
        val view: View

        class Intro(override val view: View) : Pane {
            override val installId: String? = null
        }

        class Info(override val installId: String, override val view: View, val info: DistroInfoView) : Pane

        class Home(val home: DistroHome) : Pane {
            override val installId: String get() = home.installId
            override val view: View get() = home.view
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestNotificationPermissionIfNeeded()
        // A new home task: any earlier self-removal is over, whether or
        // not onTaskRemoved saw it.
        TerminalSessions.selfRemoving = false

        // Registered before the drawer's own callback so an open drawer
        // (added later, so consulted first) still closes on Back.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                val home = (pane as? Pane.Home)?.home
                if (home != null && home.selectedIndex == TerminalTabBar.APPS && home.apps.clearSearch()) {
                    return
                } else if (open != null) {
                    // Shells and tabs stay as they are.
                    moveTaskToBack(true)
                } else {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            }
        })

        screen = buildDrawerScreen()
        screen.drawer.addDrawerListener(object : DrawerLayout.SimpleDrawerListener() {
            // Terminal counts change without a refresh (a shell got
            // input or exited); re-read them as the drawer starts opening.
            override fun onDrawerStateChanged(newState: Int) {
                if (newState != DrawerLayout.STATE_IDLE && !screen.drawer.isDrawerOpen(screen.nav)) {
                    rebuildDrawerMenu(store.list(), open)
                }
            }

            override fun onDrawerOpened(drawerView: View) {
                getSystemService(InputMethodManager::class.java)
                    ?.hideSoftInputFromWindow(screen.drawer.windowToken, 0)
            }
        })
        defaultLightBars = WindowCompat.getInsetsController(window, window.decorView).isAppearanceLightStatusBars

        fab = tawcFab(R.drawable.ic_terminal, getString(R.string.action_new_terminal)) {
            (pane as? Pane.Home)?.home?.openTerminal()
        }
        fab.visibility = View.GONE
        screen.body.addView(fab, fabLp())

        screen.nav.addHeaderView(buildDrawerHeader())
        screen.nav.setNavigationItemSelectedListener { item ->
            val id = drawerIds[item.itemId]
            if (id != null) {
                if (id != open?.id) {
                    OpenDistro.set(id)
                    refresh()
                }
            } else if (item.itemId == DRAWER_INSTALL) {
                openInstall()
            }
            screen.drawer.closeDrawer(screen.nav)
            true
        }

        setContentView(screen.drawer)

        // A command launch (EntryLauncher) arrives as extras. Consumed
        // so same-process recreation doesn't respawn it — but
        // removeExtra can't reach the system's stored copy of the
        // task's base intent, which is redelivered pristine when the
        // task is reopened after process death. savedInstanceState
        // survives process death, so its presence means "restore, don't
        // re-run the command".
        if (savedInstanceState == null) {
            consumeCommand(intent)
        } else {
            restoreTab = savedInstanceState.getInt(STATE_TAB, TerminalTabBar.APPS)
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        (pane as? Pane.Home)?.let { outState.putInt(STATE_TAB, it.home.selectedIndex) }
    }

    /** Typing with nothing focused goes into the apps tab's search. */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val home = (pane as? Pane.Home)?.home
        if (home != null && home.selectedIndex == TerminalTabBar.APPS && home.apps.onUnhandledKey(event)) return true
        return super.onKeyDown(keyCode, event)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (consumeCommand(intent)) refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    override fun onPause() {
        super.onPause()
        (pane as? Pane.Info)?.info?.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        // Shells outlive the activity: only a recents swipe
        // (SessionService.onTaskRemoved) or the notification's Exit
        // closes them.
        tearDown()
    }

    // On API 33+ foreground-service notifications (install progress, the
    // running compositor) are suppressed unless POST_NOTIFICATIONS is granted.
    // Best-effort: we don't act on the result, the install still runs either way.
    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val perm = Manifest.permission.POST_NOTIFICATIONS
        if (checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED) return
        ActivityCompat.requestPermissions(this, arrayOf(perm), REQUEST_NOTIFICATIONS)
    }

    // ---- panes -------------------------------------------------------------

    private enum class Kind { INTRO, INFO, HOME }

    /** Re-read installs and show the right pane; same pane → just resume it. */
    private fun refresh() {
        val installations = store.list()
        val inst = OpenDistro.resolve(installations)
        open = inst

        val command = queuedCommand?.takeIf { (id, _) -> id == inst?.id }
        queuedCommand = null

        val kind = when {
            inst == null -> Kind.INTRO
            inst.state != Installation.State.READY -> Kind.INFO
            else -> Kind.HOME
        }
        val current = pane
        if (current != null && current.installId == inst?.id && kindOf(current) == kind) {
            when (current) {
                is Pane.Info -> current.info.render(inst!!)
                is Pane.Home -> current.home.onResume()
                is Pane.Intro -> Unit
            }
        } else {
            tearDown()
            val next = buildPane(kind, inst)
            pane = next
            screen.body.addView(next.view, 0, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            // The removed pane's focused view doesn't take the IME with it.
            getSystemService(InputMethodManager::class.java)
                ?.hideSoftInputFromWindow(screen.drawer.windowToken, 0)
            if (next is Pane.Home) restoreTab?.let { next.home.selectTerminal(it) }
        }
        restoreTab = null
        (pane as? Pane.Home)?.home?.let { home -> command?.let { home.openTerminal(it.second) } }
        onSelectionChanged()
        rebuildDrawerMenu(installations, inst)
    }

    private fun kindOf(p: Pane): Kind = when (p) {
        is Pane.Intro -> Kind.INTRO
        is Pane.Info -> Kind.INFO
        is Pane.Home -> Kind.HOME
    }

    private fun buildPane(kind: Kind, inst: Installation?): Pane =
        when (kind) {
            Kind.INTRO -> Pane.Intro(buildIntro())
            Kind.INFO -> buildInfo(inst!!)
            Kind.HOME -> Pane.Home(
                DistroHome(this, inst!!, terminalMethod(inst), object : DistroHome.Host {
                    override fun openDrawer() = screen.openDrawer()
                    override fun showMenu(anchor: View) = showOverflowMenu(anchor)
                    override fun openEditor(intent: Intent) = editEntry.launch(intent)
                    override fun onGridScrolled(down: Boolean) {
                        if (!fabWanted) return
                        if (down) fab.hide() else fab.show()
                    }
                    override fun onSelectionChanged() = this@MainActivity.onSelectionChanged()
                    override fun onLastSelectedTerminalClosed() {
                        if (isFinishing) return
                        TerminalSessions.selfRemoving = true
                        finishAndRemoveTask()
                    }
                }),
            )
        }

    private fun tearDown() {
        val current = pane ?: return
        pane = null
        when (current) {
            is Pane.Home -> current.home.destroy()
            is Pane.Info -> current.info.stop()
            is Pane.Intro -> Unit
        }
        screen.body.removeView(current.view)
    }

    /** The install's method if it can host the terminal: READY and
     *  tawcroot (chroot spawns via su, proot is dev-only). */
    private fun terminalMethod(inst: Installation?): TawcrootMethod? {
        if (inst == null || inst.state != Installation.State.READY || inst.method != TawcrootMethod.KEY) return null
        return InstallationMethod.forKey(this, inst.method) as? TawcrootMethod
    }

    private fun onSelectionChanged() {
        styleBars()
        updateFab()
    }

    /**
     * The home's tab bar continues into the status band; the nav band
     * is black under a terminal, else the window's own. Light bar icons
     * on whichever is dark.
     */
    private fun styleBars() {
        val home = (pane as? Pane.Home)?.home
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        val terminal = home != null && home.selectedIndex != TerminalTabBar.APPS
        screen.root.background = home?.let {
            SystemBands(screen.root, it.barColor, if (terminal) Color.BLACK else getColor(R.color.tawc_window_bg))
        }
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = home?.barIsDark != true && defaultLightBars && !night
            isAppearanceLightNavigationBars = !terminal && defaultLightBars && !night
        }
    }

    // ---- FAB -----------------------------------------------------------------

    /** On the apps tab of a home that can open terminals. */
    private var fabWanted = false

    private fun updateFab() {
        val home = (pane as? Pane.Home)?.home
        fabWanted = home != null && home.canOpenTerminal && home.selectedIndex == TerminalTabBar.APPS
        if (fabWanted) fab.show() else fab.visibility = View.GONE
    }

    // ---- ⋮ menu --------------------------------------------------------------

    /** The home ⋮: tab, per-distro, then app items. */
    private fun showOverflowMenu(anchor: View) {
        val popup = PopupMenu(ContextThemeWrapper(this, R.style.ThemeOverlay_Tawc_Surfaces), anchor)
        val menu = popup.menu
        val home = (pane as? Pane.Home)?.home
        if (home != null) {
            if (home.selectedIndex == TerminalTabBar.APPS) {
                home.apps.addMenuItems(menu, ORDER_PANE)
            } else {
                menu.item(ORDER_PANE, R.string.action_close_all_terminals) { home.closeAll() }
                if (SessionWake.available.value) {
                    menu.item(ORDER_PANE, R.string.action_keep_awake) { toggleKeepAwake() }
                        .setCheckable(true).isChecked = SessionWake.held.value
                }
            }
        }
        val inst = open
        if (inst != null) {
            addDistroItems(menu, inst)
        } else {
            menu.item(ORDER_SETTINGS, R.string.title_settings) {
                startActivity(Intent(this, SettingsActivity::class.java))
            }
        }
        menu.item(ORDER_APP, R.string.title_task_manager) {
            startActivity(Intent(this, TaskManagerActivity::class.java))
        }
        popup.show()
    }

    /** Settings (with [inst]'s card), Run…, Remote access and Distro info for [inst]
     *  (home ⋮ and drawer row ⋮). */
    private fun addDistroItems(menu: Menu, inst: Installation) {
        menu.item(ORDER_SETTINGS, R.string.title_settings) {
            startActivity(Intent(this, SettingsActivity::class.java).putExtra(SettingsActivity.EXTRA_ID, inst.id))
        }
        if (inst.state == Installation.State.READY) {
            menu.item(ORDER_DISTRO, R.string.action_run_command) { showRunCommandDialog(inst) }
        }
        // Same gate as the terminal: the agent spawns through tawcroot.
        if (terminalMethod(inst) != null) {
            menu.item(ORDER_DISTRO, R.string.action_remote_access) {
                startActivity(
                    Intent(this, RemoteAccessActivity::class.java).putExtra(RemoteAccessActivity.EXTRA_ID, inst.id)
                )
            }
        }
        menu.item(ORDER_INFO, R.string.title_distro_info) {
            startActivity(
                Intent(this, DistroInfoActivity::class.java).putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
            )
        }
    }

    private fun Menu.item(order: Int, title: Int, onClick: () -> Unit): MenuItem =
        add(Menu.NONE, Menu.NONE, order, title).setOnMenuItemClickListener { onClick(); true }

    // ---- intro / info panes ------------------------------------------------

    /** `[≡] <title> [⋮]`, the same height as the terminal's tab row. */
    private fun plainTopRow(title: CharSequence): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(pad / 4, 0, pad / 4, 0)
        }
        val button = tawcButtonSizePx()
        row.addView(
            plainIconButton(R.drawable.ic_menu, getString(R.string.action_open_drawer)) { screen.openDrawer() },
            LinearLayout.LayoutParams(button, button),
        )
        row.addView(TextView(this).apply {
            text = title
            textSize = 20f
            isSingleLine = true
            setPadding(pad / 2, 0, pad / 2, 0)
        }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        lateinit var menu: View
        menu = plainIconButton(R.drawable.ic_more_vert, getString(R.string.home_menu_description), iconSizeDp = 21) {
            showOverflowMenu(menu)
        }
        row.addView(menu, LinearLayout.LayoutParams(button, button))
        return row
    }

    private fun paneColumn(title: CharSequence): Pair<LinearLayout, LinearLayout> {
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(plainTopRow(title), LinearLayout.LayoutParams(MATCH_PARENT, paneTopRowHeightPx()))
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        column.addView(content, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        return column to content
    }

    private fun buildIntro(): View {
        val (column, content) = paneColumn(getString(R.string.app_name))
        content.gravity = Gravity.CENTER
        val logo = (96 * resources.displayMetrics.density).toInt()
        content.addView(ImageView(this).apply { setImageResource(R.drawable.ic_tawc_logo) },
            LinearLayout.LayoutParams(logo, logo).also { it.bottomMargin = pad })
        content.addView(TextView(this).apply {
            text = getString(R.string.home_intro_blurb)
            textSize = 16f
            alpha = 0.75f
            gravity = Gravity.CENTER_HORIZONTAL
        }, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad * 3 / 2))
        val install = tonalButton(getString(R.string.title_install_distro)) { openInstall() }
        install.backgroundTintList = ColorStateList.valueOf(getColor(R.color.tawc_accent))
        content.addView(install, verticalLp(WRAP_CONTENT, WRAP_CONTENT))
        return column
    }

    private fun buildInfo(inst: Installation): Pane.Info {
        val (column, content) = paneColumn(DistroRegistry.displayLabel(inst))
        val info = DistroInfoView(this)
        content.addView(info.view, LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        info.render(inst)
        return Pane.Info(inst.id, column, info)
    }

    // ---- drawer --------------------------------------------------------------

    private fun buildDrawerHeader(): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(pad, pad * 3 / 2, pad, pad)
        }
        // NavigationView leaves a header's status-bar inset to the header.
        ViewCompat.setOnApplyWindowInsetsListener(row) { v, insets ->
            val top = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()).top
            v.setPadding(pad, top + pad * 3 / 2, pad, pad)
            insets
        }
        val iconSize = (40 * resources.displayMetrics.density).toInt()
        row.addView(ImageView(this).apply {
            setImageResource(R.drawable.ic_tawc_logo)
        }, LinearLayout.LayoutParams(iconSize, iconSize).also { it.marginEnd = pad / 2 })
        row.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            setTextAppearance(R.style.TextAppearance_Tawc_HomeTitle)
        })
        return row
    }

    private fun rebuildDrawerMenu(installations: List<Installation>, inst: Installation?) {
        val menu = screen.nav.menu
        menu.clear()
        drawerIds.clear()
        installations.forEachIndexed { i, it ->
            val itemId = DRAWER_FIRST_DISTRO + i
            drawerIds[itemId] = it.id
            var title = DistroRegistry.displayLabel(it)
            stateLine(it.state)?.let { s -> title = getString(R.string.home_drawer_item_state, title, s) }
            // Shells left running in another distro stay findable.
            val shells = TerminalSessions.list(it.id).size
            if (shells > 0) {
                val count = resources.getQuantityString(R.plurals.session_terminals, shells, shells)
                title = getString(R.string.home_drawer_item_state, title, count)
            }
            menu.add(DRAWER_GROUP_DISTROS, itemId, i, title).apply {
                // Also lines the labels up with Install's + icon.
                setIcon(R.drawable.ic_linux_logo)
                isCheckable = true
                isChecked = it.id == inst?.id
                actionView = drawerRowMenuButton(it)
            }
        }
        menu.setGroupCheckable(DRAWER_GROUP_DISTROS, true, true)
        // Same group: NavigationView draws a divider between groups.
        // Added after setGroupCheckable, so it isn't checkable.
        menu.add(DRAWER_GROUP_DISTROS, DRAWER_INSTALL, installations.size, R.string.action_install_new_distro)
            .setIcon(R.drawable.ic_add)
            // NavigationView recycles row views and only ever replaces
            // an action view, never clears one: without an explicit
            // empty one, this row keeps the ⋮ of the distro row it was
            // when the list shrinks.
            .setActionView(View(this).apply { layoutParams = ViewGroup.LayoutParams(0, 0) })
    }

    /** Trailing ⋮ on a drawer row: [inst]'s items without switching to it. */
    private fun drawerRowMenuButton(inst: Installation): View {
        lateinit var button: View
        button = plainIconButton(R.drawable.ic_more_vert, getString(R.string.home_menu_description), iconSizeDp = 21) {
            val popup = PopupMenu(ContextThemeWrapper(this, R.style.ThemeOverlay_Tawc_Surfaces), button)
            addDistroItems(popup.menu, inst)
            popup.setOnMenuItemClickListener { screen.drawer.closeDrawer(screen.nav); false }
            popup.show()
        }
        val size = tawcButtonSizePx()
        button.layoutParams = LinearLayout.LayoutParams(size, size)
        return button
    }

    private fun openInstall() {
        startActivity(Intent(this, InstallActivity::class.java))
    }

    /** State marker; null for READY (no marker). */
    private fun stateLine(state: Installation.State): String? =
        when (state) {
            Installation.State.READY -> null
            Installation.State.INSTALLING -> getString(R.string.install_state_installing)
            Installation.State.UNINSTALLING -> getString(R.string.install_state_uninstalling)
            Installation.State.FAILED -> getString(R.string.install_state_failed)
            Installation.State.CORRUPT -> getString(R.string.install_state_corrupt)
        }

    // ---- intents ---------------------------------------------------------------

    /**
     * Take a terminal launch ([commandIntent]) off [intent]: open that
     * distro and queue the new tab for the next [refresh]. The extras
     * are removed so a retained intent can't respawn it. Returns
     * whether there was one.
     */
    private fun consumeCommand(intent: Intent?): Boolean {
        val id = intent?.getStringExtra(EXTRA_DISTRO) ?: return false
        // Other apps can start this exported activity; only the
        // non-exported alias may carry a command.
        if (intent.component?.className != COMMAND_ALIAS) return false
        val exec = intent.getStringExtra(EXTRA_COMMAND)
        val label = intent.getStringExtra(EXTRA_LABEL)
        val graphics = GraphicsBackend.fromKeyOrNull(intent.getStringExtra(EXTRA_GRAPHICS))
        intent.removeExtra(EXTRA_COMMAND)
        intent.removeExtra(EXTRA_LABEL)
        intent.removeExtra(EXTRA_GRAPHICS)
        intent.removeExtra(EXTRA_DISTRO)
        if (!Installation.isValidId(id)) return false
        OpenDistro.set(id)
        queuedCommand = id to TerminalPane.CommandTab(exec, label, graphics)
        return true
    }

    // ---- debug broker hooks ------------------------------------------------

    /**
     * Debug broker `home-tab`: show [installId]'s (or the open
     * distro's) home on [tab] — [TerminalTabBar.APPS], a terminal tab
     * index, or [DEV_NEW_TAB]. False when that tab doesn't exist.
     */
    internal fun showTabForDev(tab: Int, installId: String?): Boolean {
        if (installId != null && installId != open?.id) OpenDistro.set(installId)
        refresh()
        val home = (pane as? Pane.Home)?.home ?: return tab == TerminalTabBar.APPS
        return when (tab) {
            TerminalTabBar.APPS -> { home.selectApps(); true }
            DEV_NEW_TAB -> home.canOpenTerminal.also { if (it) home.openTerminal() }
            else -> home.selectTerminal(tab)
        }
    }

    /** Debug broker `terminal-state`: selected tab of [installId]'s
     *  home, null when it isn't showing. */
    internal fun selectedTabForDev(installId: String): Int? =
        (pane as? Pane.Home)?.home?.takeIf { it.installId == installId }?.selectedIndex

    companion object {
        /** Non-exported manifest alias command launches must target
         *  ([commandIntent]). */
        private const val COMMAND_ALIAS = "me.phie.tawc.CommandLaunch"

        /** Open a new terminal tab for [installId] running [exec] (null:
         *  a plain shell), named [label]. */
        fun commandIntent(
            context: Context,
            installId: String,
            exec: String?,
            label: String?,
            graphics: GraphicsBackend? = null,
        ): Intent =
            Intent().setClassName(context, COMMAND_ALIAS)
                .putExtra(EXTRA_DISTRO, installId)
                .putExtra(EXTRA_COMMAND, exec)
                .putExtra(EXTRA_LABEL, label)
                .putExtra(EXTRA_GRAPHICS, graphics?.key)

        /** Install id of a terminal launch. */
        const val EXTRA_DISTRO = "distro"

        /** Shell fragment to run in the new terminal tab (a launcher
         *  entry's Exec line; same trust level as EntryLauncher's own
         *  concatenation); absent for a plain shell. */
        const val EXTRA_COMMAND = "command"

        /** Tab label for an [EXTRA_COMMAND] session (the entry name). */
        const val EXTRA_LABEL = "label"

        /** [GraphicsBackend.key] for an [EXTRA_COMMAND] session (a
         *  launcher entry's override); absent = the global setting. */
        const val EXTRA_GRAPHICS = "graphics"

        /** [showTabForDev]: open a new terminal tab. */
        internal const val DEV_NEW_TAB = -2

        private const val STATE_TAB = "home_tab"

        private const val REQUEST_NOTIFICATIONS = 1

        // ⋮ order: pane items, Settings, Run…, Task manager, Distro info.
        private const val ORDER_PANE = 0
        private const val ORDER_SETTINGS = 1
        private const val ORDER_DISTRO = 2
        private const val ORDER_APP = 3
        private const val ORDER_INFO = 4

        private const val DRAWER_GROUP_DISTROS = 1
        private const val DRAWER_INSTALL = 1
        private const val DRAWER_FIRST_DISTRO = 100
    }
}

/**
 * Background for the home root's system-bar padding: [top] (status
 * band) over [bottom] (nav band, side bands, and anything the content
 * leaves uncovered).
 */
private class SystemBands(private val root: View, private val top: Int, private val bottom: Int) : Drawable() {
    private val paint = Paint()

    override fun draw(canvas: Canvas) {
        canvas.drawColor(bottom)
        paint.color = top
        canvas.drawRect(
            bounds.left.toFloat(), bounds.top.toFloat(),
            bounds.right.toFloat(), (bounds.top + root.paddingTop).toFloat(), paint,
        )
    }

    override fun setAlpha(alpha: Int) {}

    override fun setColorFilter(colorFilter: ColorFilter?) {}

    @Deprecated("Deprecated in Java")
    override fun getOpacity(): Int = PixelFormat.OPAQUE
}
