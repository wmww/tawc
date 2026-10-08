package me.phie.tawc

import android.content.Intent
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import com.termux.terminal.TerminalSession
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.install.Installation
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.launcher.AppsPane
import me.phie.tawc.terminal.TerminalPane
import me.phie.tawc.terminal.TerminalSessions
import me.phie.tawc.terminal.TerminalTabBar
import me.phie.tawc.ui.paneTopRowHeightPx

/**
 * A READY distro's home: the [TerminalTabBar] over either the apps tab
 * ([AppsPane]) or the terminal ([TerminalPane], tawcroot only — other
 * methods get just the title). Owns which tab is selected (in memory only; the
 * activity carries it across recreation) and the terminal tab policy
 * (notes/terminal.md "Lifecycle"):
 *
 * - a new tab is always selected; selecting a terminal pops the IME,
 *   selecting apps drops it unless the search field has focus;
 * - a terminal going away (exit or ×) while selected selects its right
 *   neighbour, else its left; the last one closes the app
 *   ([Host.onLastSelectedTerminalClosed]); while apps or another tab is
 *   selected the selection stays.
 */
internal class DistroHome(
    private val activity: AppCompatActivity,
    inst: Installation,
    method: TawcrootMethod?,
    private val host: Host,
) : TerminalPane.Host {

    interface Host {
        fun openDrawer()
        fun showMenu(anchor: View)
        /** Start the `.desktop` editor for result. */
        fun openEditor(intent: Intent)
        /** Grid scrolled; hide the FAB going down, show it going up. */
        fun onGridScrolled(down: Boolean)
        /** The selected tab changed (FAB, bar styling). */
        fun onSelectionChanged()
        /** The selected terminal went away and no other was left. */
        fun onLastSelectedTerminalClosed()
    }

    val installId = inst.id

    private val bar = TerminalTabBar(activity, DistroRegistry.displayLabel(inst))
    val apps: AppsPane
    private val terminal: TerminalPane?

    /** Shown session, null for the apps tab. */
    private var selected: TerminalSession? = null

    /** Selected terminal tab index, or [TerminalTabBar.APPS]. */
    val selectedIndex: Int
        get() = selected?.let { s -> sessions().indexOfFirst { it === s } } ?: TerminalTabBar.APPS

    val canOpenTerminal: Boolean get() = terminal != null

    /** The tab bar's current fill, for the status band above it. */
    val barColor: Int get() = bar.barColor

    /** The bar is in its dark palette (terminal tabs exist). */
    val barIsDark: Boolean get() = bar.hasTabs

    val view: LinearLayout

    init {
        view = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        bar.apply {
            onAppsSelected = { selectApps() }
            onTabSelected = { selectTerminal(it) }
            onTabCloseClicked = { closeTab(it) }
            onNewTabClicked = { openTerminal() }
            onDrawerClicked = { host.openDrawer() }
            onMenuClicked = { host.showMenu(it) }
        }
        view.addView(bar, LinearLayout.LayoutParams(MATCH_PARENT, activity.paneTopRowHeightPx()))

        val body = FrameLayout(activity)
        apps = AppsPane(activity, inst, object : AppsPane.Host {
            override fun openEditor(intent: Intent) = host.openEditor(intent)
            override fun onGridScrolled(down: Boolean) = host.onGridScrolled(down)
        })
        body.addView(apps.view, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        terminal = method?.let { TerminalPane(activity, inst.id, it, this) }
        terminal?.let {
            it.view.visibility = View.GONE
            body.addView(it.view, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            val running = sessions()
            it.adopt(running)
            running.forEachIndexed { i, s -> bar.addTab(it.labelFor(s, i)) }
        }
        view.addView(body, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        bar.setSelected(TerminalTabBar.APPS)
    }

    private fun sessions(): List<TerminalSession> = TerminalSessions.list(installId)

    fun onResume() {
        apps.onResume()
        terminal?.onResume()
    }

    /** Stop driving the views; shells keep running detached. */
    fun destroy() {
        terminal?.detach()
        apps.destroy()
    }

    // ---- selection ---------------------------------------------------------

    fun selectApps() {
        selected = null
        terminal?.hide()
        bar.setSelected(TerminalTabBar.APPS)
        apps.view.visibility = View.VISIBLE
        if (!apps.searchHasFocus) {
            // Before hiding the terminal, so its focus doesn't fall to
            // the search field.
            apps.unfocus()
            activity.getSystemService(InputMethodManager::class.java)
                ?.hideSoftInputFromWindow(view.windowToken, 0)
        }
        terminal?.view?.visibility = View.GONE
        host.onSelectionChanged()
    }

    /** Show terminal tab [index] and pop the IME; false if there is none. */
    fun selectTerminal(index: Int): Boolean {
        val pane = terminal ?: return false
        val session = sessions().getOrNull(index) ?: return false
        selected = session
        bar.setSelected(index)
        pane.view.visibility = View.VISIBLE
        apps.view.visibility = View.GONE
        pane.show(session)
        host.onSelectionChanged()
        return true
    }

    /** New tab running [command] (null: a plain shell), selected. */
    fun openTerminal(command: TerminalPane.CommandTab? = null) {
        val pane = terminal ?: return
        val session = pane.spawn(command) ?: return // toast shown
        TerminalSessions.add(installId, session)
        val index = sessions().size - 1
        bar.addTab(pane.labelFor(session, index))
        selectTerminal(index)
    }

    /** Hang up every terminal tab and select apps. */
    fun closeAll() {
        val closed = TerminalSessions.removeAll(installId)
        // Dropped from the registry first, so their exits find no tab.
        for (i in closed.indices.reversed()) bar.removeTab(i)
        TerminalSessions.hangUp(closed)
        selectApps()
    }

    private fun closeTab(index: Int) {
        val session = sessions().getOrNull(index) ?: return
        if (session.isRunning && session.pid > 0) {
            // The exit lands back in onSessionExited, which removes the
            // tab.
            session.finishIfRunning()
        } else {
            // Already-dead shell (race window before onSessionFinished,
            // or a never-started session): drop the tab directly.
            onSessionExited(session)
        }
    }

    // ---- TerminalPane.Host -------------------------------------------------

    override fun onSessionExited(session: TerminalSession) {
        val index = sessions().indexOfFirst { it === session }
        if (index < 0) return
        TerminalSessions.remove(installId, session)
        bar.removeTab(index)
        onTitlesChanged()
        if (session !== selected) {
            // Indices shifted under the unchanged selection; restyle
            // (the last tab going drops the bar's dark palette).
            bar.setSelected(selectedIndex)
            host.onSelectionChanged()
            return
        }
        selected = null
        val left = sessions()
        if (left.isEmpty()) {
            // Like closing the last desktop terminal window.
            host.onLastSelectedTerminalClosed()
        } else {
            selectTerminal(minOf(index, left.size - 1))
        }
    }

    /** Reapply every tab's label (index-derived labels shift on close). */
    override fun onTitlesChanged() {
        val pane = terminal ?: return
        sessions().forEachIndexed { i, s -> bar.setLabel(i, pane.labelFor(s, i)) }
    }
}
