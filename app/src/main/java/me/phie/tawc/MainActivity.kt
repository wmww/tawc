package me.phie.tawc

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.Menu
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.floatingactionbutton.FloatingActionButton
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.install.showRunCommandDialog
import me.phie.tawc.launcher.LauncherActivity
import me.phie.tawc.ops.LogScreenActivity
import me.phie.tawc.terminal.TerminalActivity
import me.phie.tawc.tasks.TaskManagerActivity
import me.phie.tawc.ui.DrawerScreen
import me.phie.tawc.ui.buildDrawerScreen
import me.phie.tawc.ui.fabLp
import me.phie.tawc.ui.tawcFab
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp

/**
 * Home screen. Shows the one open distro ([OpenDistro]) — label,
 * state, and a search stub into [LauncherActivity] — with a Terminal
 * FAB. The drawer switches the open distro and starts a new install;
 * ⋮ holds Distro info, Run command, Task manager and Settings. The compositor starts lazily when a user
 * launches a rootfs command, so a broken graphics backend doesn't keep
 * the home screen or Settings from opening.
 */
class MainActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private val gap by lazy { (8 * resources.displayMetrics.density).toInt() }
    private val pad by lazy { (16 * resources.displayMetrics.density).toInt() }

    private lateinit var screen: DrawerScreen
    private lateinit var listContainer: LinearLayout
    private lateinit var installButton: MaterialButton
    private lateinit var fab: FloatingActionButton

    /** Current open distro, as last rendered; menu/FAB actions read it. */
    private var open: Installation? = null

    /** Drawer item id → install id, rebuilt on every [refresh]. */
    private val drawerIds = mutableMapOf<Int, String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestNotificationPermissionIfNeeded()

        screen = buildDrawerScreen(getString(R.string.app_name))
        val content = screen.scaffold.content

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        content.addView(listContainer, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = gap))

        // Empty state only; with an install, the drawer carries this.
        installButton = tonalButton(getString(R.string.action_install_new_distro)) { openInstall() }
        installButton.backgroundTintList = ColorStateList.valueOf(getColor(R.color.tawc_accent))
        content.addView(installButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))

        fab = tawcFab(R.drawable.ic_terminal, getString(R.string.action_terminal)) {
            open?.let { openTerminal(it) }
        }
        screen.body.addView(fab, fabLp())

        buildOverflowMenu()
        screen.nav.addHeaderView(buildDrawerHeader())
        screen.nav.setNavigationItemSelectedListener { item ->
            val id = drawerIds[item.itemId]
            if (id != null) {
                OpenDistro.set(id)
                refresh()
            } else if (item.itemId == DRAWER_INSTALL) {
                openInstall()
            }
            screen.drawer.closeDrawer(screen.nav)
            true
        }

        setContentView(screen.drawer)
    }

    override fun onResume() {
        super.onResume()
        refresh()
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

    private fun refresh() {
        val installations = store.list()
        val inst = OpenDistro.resolve(installations)
        open = inst

        listContainer.removeAllViews()
        if (inst == null) {
            listContainer.addView(TextView(this).apply {
                text = getString(R.string.home_empty_no_distros)
                textSize = 16f
                alpha = 0.75f
                gravity = Gravity.CENTER_HORIZONTAL
            }, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = gap))
        } else {
            listContainer.addView(buildDistroView(inst), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = gap))
        }
        installButton.visibility = if (inst == null) View.VISIBLE else View.GONE

        // Terminal needs a runnable rootfs and the tawcroot spawn path
        // (chroot needs su, proot is dev-only — see TerminalActivity).
        val terminalOk = inst != null && inst.state == Installation.State.READY &&
            inst.method == TawcrootMethod.KEY
        fab.visibility = if (terminalOk) View.VISIBLE else View.GONE
        screen.scaffold.toolbar.menu.findItem(MENU_INFO)?.isVisible = inst != null
        screen.scaffold.toolbar.menu.findItem(MENU_RUN)?.isVisible =
            inst?.state == Installation.State.READY

        rebuildDrawerMenu(installations, inst)
    }

    private fun buildOverflowMenu() {
        val toolbar = screen.scaffold.toolbar
        toolbar.menu.add(Menu.NONE, MENU_INFO, 0, R.string.title_distro_info)
        toolbar.menu.add(Menu.NONE, MENU_RUN, 0, R.string.action_run_command)
        toolbar.menu.add(Menu.NONE, MENU_TASKS, 1, R.string.title_task_manager)
        toolbar.menu.add(Menu.NONE, MENU_SETTINGS, 2, R.string.title_settings)
        toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                MENU_INFO -> open?.let {
                    startActivity(
                        Intent(this, DistroInfoActivity::class.java)
                            .putExtra(DistroInfoActivity.EXTRA_ID, it.id)
                    )
                }
                MENU_RUN -> open?.let { showRunCommandDialog(it) }
                MENU_TASKS -> startActivity(Intent(this, TaskManagerActivity::class.java))
                MENU_SETTINGS -> startActivity(Intent(this, SettingsActivity::class.java))
                else -> return@setOnMenuItemClickListener false
            }
            true
        }
    }

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
            val label = DistroRegistry.displayLabel(it)
            val title = stateLine(it.state)?.let { s -> getString(R.string.home_drawer_item_state, label, s) }
                ?: label
            menu.add(DRAWER_GROUP_DISTROS, itemId, i, title).apply {
                // Also lines the labels up with Install's + icon.
                setIcon(R.drawable.ic_linux_logo)
                isCheckable = true
                isChecked = it.id == inst?.id
            }
        }
        menu.setGroupCheckable(DRAWER_GROUP_DISTROS, true, true)
        // A separate group gets NavigationView's divider above it.
        menu.add(DRAWER_GROUP_ACTIONS, DRAWER_INSTALL, installations.size, R.string.action_install_new_distro)
            .setIcon(R.drawable.ic_add)
    }

    /** The open distro's title lines, state and search stub, laid
     *  straight on the page (no card: there is only ever one). */
    private fun buildDistroView(inst: Installation): View {
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = DistroRegistry.displayLabel(inst)
        column.addView(TextView(this).apply {
            text = title
            textSize = 18f
        })
        // Distro line under the title — dropped when the title already
        // equals the display name (legacy records without a label).
        val displayName = DistroRegistry.forInstallation(inst)?.displayName
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"
        if (title != displayName) {
            column.addView(TextView(this).apply {
                text = displayName
                textSize = 14f
                alpha = 0.7f
            })
        }

        // Non-READY state marker, in red so a stuck/failed install pops.
        // Running ops have a live log; FAILED's text is on Distro info
        // (no completed-runs history, notes/log-screen.md).
        stateLine(inst.state)?.let { state ->
            val op = when (inst.state) {
                Installation.State.INSTALLING -> "install"
                Installation.State.UNINSTALLING -> "uninstall"
                else -> null
            }
            column.addView(TextView(this).apply {
                textSize = 14f
                setTextColor(getColor(R.color.tawc_danger))
                if (op == null) {
                    text = state
                } else {
                    text = getString(R.string.home_state_view_log, state)
                    setBackgroundResource(selectableBackground())
                    setOnClickListener {
                        startActivity(LogScreenActivity.intentFor(this@MainActivity, "$op:${inst.id}"))
                    }
                }
            }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }

        // Search-apps stub. It looks like a search field but never
        // holds focus — tapping it forwards into LauncherActivity,
        // which is the real search UI. Hidden on FAILED/CORRUPT (no
        // usable launcher) and disabled while installing/uninstalling
        // so it returns once ready.
        val topMargin = (8 * resources.displayMetrics.density).toInt()
        val searchBox = EditText(this).apply {
            hint = getString(R.string.hint_search_apps)
            isSingleLine = true
            isFocusable = false
            isClickable = true
            setTextColor(getColor(R.color.tawc_on_tonal))
            isEnabled = inst.state == Installation.State.READY
            setOnClickListener {
                val i = Intent(this@MainActivity, LauncherActivity::class.java)
                    .putExtra(LauncherActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }
        searchBox.visibility = if (
            inst.state != Installation.State.FAILED &&
            inst.state != Installation.State.CORRUPT
        ) View.VISIBLE else View.GONE
        column.addView(
            searchBox,
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = topMargin },
        )

        return column
    }

    private fun openTerminal(inst: Installation) {
        val i = Intent(this, TerminalActivity::class.java)
            .putExtra(TerminalActivity.EXTRA_ID, inst.id)
            // Unique per-distro document URI — see the manifest
            // comment on TerminalActivity.
            .setData(Uri.parse("tawc://terminal/${inst.id}"))
        startActivity(i)
    }

    private fun openInstall() {
        startActivity(Intent(this, InstallActivity::class.java))
    }

    private fun selectableBackground(): Int {
        val value = TypedValue()
        theme.resolveAttribute(android.R.attr.selectableItemBackground, value, true)
        return value.resourceId
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

    private companion object {
        const val REQUEST_NOTIFICATIONS = 1

        const val MENU_INFO = 4
        const val MENU_RUN = 1
        const val MENU_TASKS = 2
        const val MENU_SETTINGS = 3

        const val DRAWER_GROUP_DISTROS = 1
        const val DRAWER_GROUP_ACTIONS = 2
        const val DRAWER_INSTALL = 1
        const val DRAWER_FIRST_DISTRO = 100
    }
}
