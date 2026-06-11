package me.phie.tawc.terminal

import android.app.ActivityManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.termux.shared.termux.extrakeys.ExtraKeysConstants
import com.termux.shared.termux.extrakeys.ExtraKeysInfo
import com.termux.shared.termux.extrakeys.ExtraKeysView
import com.termux.shared.termux.extrakeys.SpecialButton
import com.termux.shared.termux.terminal.io.TerminalExtraKeys
import com.termux.terminal.TerminalEmulator
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient
import com.termux.view.TerminalView
import com.termux.view.TerminalViewClient
import me.phie.tawc.R
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.distro.DistroRegistry
import java.io.IOException

/**
 * Interactive shells into one installed rootfs, built on termux's
 * vendored terminal-emulator/terminal-view modules (Apache-2.0; see
 * settings.gradle.kts). The termux JNI forks the pty pair and execs
 * tawcroot as the pty child ([TawcrootMethod.ptyShellExec]), so the
 * in-rootfs bash gets a real controlling tty — readline, job control
 * and curses apps work, unlike the pipe-fed RunCommandOp path. No
 * compositor involvement: the Wayland env vars are set but nothing
 * waits for the socket, so the terminal works with the graphics stack
 * cold (launching a GUI app from it requires a compositor started via
 * Run/launcher).
 *
 * One terminal activity per distro, multiple shell sessions as tabs:
 * documentLaunchMode="intoExisting" plus a unique tawc://terminal/<id>
 * data URI reuse the activity instance and recents card per id (same
 * trick as CompositorActivity). The sessions and tab selection live in
 * [TerminalSessions] so reopening/recreation reattaches. A compact
 * [TerminalTabBar] replaces the scaffold toolbar; one [TerminalView]
 * shows the selected session via `attachSession` (termux-app's own
 * multi-session pattern — background sessions keep a stale pty size
 * until selected). Tab labels follow the session's xterm window title
 * (OSC 0/2; tawc's shipped bashrc defaults set a cwd-only title —
 * see ShellDefaults), falling back to a static "Terminal" while unset.
 *
 * tawcroot-only: chroot spawns via su and proot is dev-only, so the
 * home-screen Terminal button is gated on the tawcroot method.
 */
class TerminalActivity : AppCompatActivity(), TerminalViewClient, TerminalSessionClient {

    private lateinit var terminalView: TerminalView
    private lateinit var extraKeysView: ExtraKeysView
    private lateinit var tabBar: TerminalTabBar
    private lateinit var store: InstallationStore
    private lateinit var method: TawcrootMethod
    private var activeSession: TerminalSession? = null
    private var distroId: String = ""
    private var fontSizePx: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val id = intent?.getStringExtra(EXTRA_ID)
        if (id == null) {
            finish()
            return
        }
        distroId = id

        store = InstallationStore(this)
        val installation = store.load(distroId)
        val tawcroot = installation?.let {
            InstallationMethod.forKey(this, it.method) as? TawcrootMethod
        }
        if (installation == null || tawcroot == null) {
            Log.w(TAG, "no tawcroot installation for '$distroId'")
            finish()
            return
        }
        method = tawcroot
        // No toolbar shows it, but the recents card and accessibility
        // still name the screen by the activity title.
        title = DistroRegistry.displayLabel(installation)

        // Leaving the terminal (system back/gesture) backgrounds the
        // task instead of finishing it: the shells keep running, the
        // recents card stays, and — because the activity instance stays
        // alive with the task — swiping the card later still reaches
        // onDestroy, which kills the shells. finish() here would leave a
        // card whose swipe the app never sees (no onTaskRemoved service;
        // see TerminalSessions).
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                moveTaskToBack(true)
            }
        })

        val density = resources.displayMetrics.density
        // Black root so the inset padding bands (status/nav bar areas)
        // match the always-black terminal surface.
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }
        // IME-inclusive insets so the keyboard resizes the terminal and
        // the prompt stays visible above it.
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or
                    WindowInsetsCompat.Type.displayCutout() or
                    WindowInsetsCompat.Type.ime()
            )
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }

        tabBar = TerminalTabBar(this).apply {
            onTabSelected = { selectTab(it) }
            onTabCloseClicked = { closeTab(it) }
            onNewTabClicked = { openNewTab() }
        }
        root.addView(
            tabBar,
            LinearLayout.LayoutParams(MATCH_PARENT, (TAB_BAR_HEIGHT_DP * density).toInt()),
        )

        fontSizePx = (DEFAULT_FONT_SIZE_DP * density).toInt()
        terminalView = TerminalView(this, null).apply {
            setTerminalViewClient(this@TerminalActivity)
            setTextSize(fontSizePx)
            setBackgroundColor(Color.BLACK)
            keepScreenOn = true
            // Key events only reach the view when it can hold focus —
            // termux sets this in XML (activity_termux.xml); the view
            // itself doesn't.
            isFocusableInTouchMode = true
        }
        root.addView(terminalView, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        // Termux's extra-keys row (ESC/arrows/CTRL/...) between the
        // terminal and the IME. Same default layout and per-row height
        // as termux; held CTRL/ALT/SHIFT/FN state is consumed via the
        // read*Key() client callbacks below. Targets the view, not a
        // session, so tab switches need no extra-keys work.
        val extraKeysInfo = ExtraKeysInfo(
            EXTRA_KEYS_CONFIG, EXTRA_KEYS_STYLE, ExtraKeysConstants.CONTROL_CHARS_ALIASES,
        )
        val rowHeightPx = EXTRA_KEYS_ROW_HEIGHT_DP * density
        extraKeysView = ExtraKeysView(this, null).apply {
            setExtraKeysViewClient(TerminalExtraKeys(terminalView))
            setBackgroundColor(Color.BLACK)
        }
        root.addView(
            extraKeysView,
            LinearLayout.LayoutParams(
                MATCH_PARENT,
                (rowHeightPx * extraKeysInfo.matrix.size + 0.5f).toInt(),
            ),
        )
        extraKeysView.reload(extraKeysInfo, rowHeightPx)
        setContentView(root)

        var sessions = TerminalSessions.list(distroId)
        if (sessions.isEmpty()) {
            // Zero tabs = nothing to show; only this initial spawn
            // failure finishes the activity (cf. openNewTab).
            val s = spawnSession()
            if (s == null) {
                finish()
                return
            }
            TerminalSessions.add(distroId, s)
            sessions = listOf(s)
        }
        for ((i, s) in sessions.withIndex()) {
            s.updateTerminalSessionClient(this)
            tabBar.addTab(labelFor(s, i))
        }
        selectTab(TerminalSessions.selected(distroId))

        terminalView.requestFocus()
    }

    override fun onResume() {
        super.onResume()
        if (::terminalView.isInitialized) terminalView.onScreenUpdated()
    }

    override fun onDestroy() {
        super.onDestroy()
        // Sessions outlive this activity in [TerminalSessions]; drop
        // their references to us so the destroyed activity (and view
        // tree) is collectable. Reopening swaps the live client back in.
        for (s in TerminalSessions.list(distroId)) {
            s.updateTerminalSessionClient(DetachedTerminalClient(distroId))
        }
        // Distinguish "task swiped away in recents" from recreation
        // (config change, system pressure): a swipe removes the recents
        // card before destroying us; recreation isn't finishing. With
        // the card gone nothing can reattach, so kill every shell like
        // closing a desktop terminal window. Back never finishes us
        // (see onCreate), so a live card always has a live activity to
        // receive this.
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val taskInRecents = am.appTasks.any { it.taskInfo.taskId == taskId }
        if (isFinishing && !taskInRecents) {
            for (s in TerminalSessions.removeAll(distroId)) s.finishIfRunning()
        }
    }

    // ---- tabs ------------------------------------------------------------

    /**
     * Spawn a fresh shell session, or toast and return null on failure:
     * ptyShellExec fails closed (IOException) on a bad external bind —
     * revoked all-files access, missing host dir
     * (notes/external-binds.md). The user fixes it under Manage binds /
     * settings.
     */
    private fun spawnSession(): TerminalSession? {
        val exec = try {
            method.ptyShellExec(store.rootfsDir(distroId).absolutePath)
        } catch (e: IOException) {
            Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
            return null
        }
        return TerminalSession(
            exec.argv[0],
            exec.cwd,
            exec.argv.toTypedArray(),
            exec.hostEnv.toTypedArray(),
            TRANSCRIPT_ROWS,
            this,
        )
    }

    private fun labelFor(session: TerminalSession, index: Int): CharSequence {
        val title = session.title?.takeUnless { it.isBlank() }
            ?: return getString(R.string.terminal_tab_fallback)
        // The shipped bashrc defaults title tabs with the cwd
        // (ShellDefaults), so every fresh tab would read `~` — number
        // those by tab position instead.
        return if (title == "~") getString(R.string.terminal_tab_home, index + 1) else title
    }

    /** Reapply every tab's label (index-derived labels shift on close). */
    private fun relabelTabs() {
        TerminalSessions.list(distroId).forEachIndexed { i, s -> tabBar.setLabel(i, labelFor(s, i)) }
    }

    /** Attach the session at [index] (registry order == bar order). */
    private fun selectTab(index: Int) {
        val session = TerminalSessions.list(distroId).getOrNull(index) ?: return
        TerminalSessions.setSelected(distroId, index)
        activeSession = session
        tabBar.setSelected(index)
        // attachSession resets emulator/top-row state and updateSize()s,
        // (re)initializing or SIGWINCHing the pty for the current view.
        terminalView.attachSession(session)
        terminalView.onScreenUpdated()
    }

    private fun openNewTab() {
        val session = spawnSession() ?: return // toast shown; existing tabs stay up
        TerminalSessions.add(distroId, session)
        val index = TerminalSessions.list(distroId).size - 1
        tabBar.addTab(labelFor(session, index))
        selectTab(index)
    }

    private fun closeTab(index: Int) {
        val session = TerminalSessions.list(distroId).getOrNull(index) ?: return
        if (session.isRunning) {
            // The exit lands back in onSessionFinished, which removes
            // the tab.
            session.finishIfRunning()
        } else {
            // Already-dead shell (race window before onSessionFinished,
            // or a never-started session): drop the tab directly.
            removeFinishedSession(session)
        }
    }

    /** Drop [session]'s tab; pick the neighbor or finish on last-tab. */
    private fun removeFinishedSession(session: TerminalSession) {
        val index = TerminalSessions.list(distroId).indexOfFirst { it === session }
        if (index < 0) return
        TerminalSessions.remove(distroId, session)
        tabBar.removeTab(index)
        if (TerminalSessions.list(distroId).isEmpty()) {
            // Last shell gone (user typed `exit`, closed the tab, or the
            // rootfs side died). Closing the screen and dropping the
            // recents card mirrors a desktop terminal window; a leftover
            // card would just respawn a fresh shell when tapped.
            if (!isFinishing) finishAndRemoveTask()
            return
        }
        relabelTabs()
        if (session === activeSession) {
            selectTab(TerminalSessions.selected(distroId))
        } else {
            // Indices shifted under the unchanged selection; restyle.
            tabBar.setSelected(TerminalSessions.selected(distroId))
        }
    }

    private fun showSoftKeyboard() {
        terminalView.requestFocus()
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(terminalView, 0)
    }

    private fun changeFontSize(increase: Boolean) {
        val density = resources.displayMetrics.density
        val step = (FONT_SIZE_STEP_DP * density).toInt().coerceAtLeast(1)
        val min = (MIN_FONT_SIZE_DP * density).toInt()
        val max = (MAX_FONT_SIZE_DP * density).toInt()
        fontSizePx = (fontSizePx + if (increase) step else -step).coerceIn(min, max)
        terminalView.setTextSize(fontSizePx)
    }

    // ---- TerminalViewClient --------------------------------------------

    override fun onScale(scale: Float): Float {
        if (scale < 0.9f || scale > 1.1f) {
            changeFontSize(increase = scale > 1f)
            return 1.0f
        }
        return scale
    }

    override fun onSingleTapUp(e: MotionEvent) {
        showSoftKeyboard()
    }

    override fun shouldBackButtonBeMappedToEscape(): Boolean = false

    // TYPE_NULL input: makes IMEs send discrete key events instead of
    // composing/autocorrecting — same reason the Run dialog uses
    // VISIBLE_PASSWORD. Composing IMEs misbehave against a terminal.
    override fun shouldEnforceCharBasedInput(): Boolean = true

    override fun shouldUseCtrlSpaceWorkaround(): Boolean = false

    override fun isTerminalViewSelected(): Boolean = true

    override fun copyModeChanged(copyMode: Boolean) {}

    override fun onKeyDown(keyCode: Int, e: KeyEvent, session: TerminalSession): Boolean {
        // Enter on a dead session closes its tab (only reachable in the
        // race window before onSessionFinished lands).
        if (keyCode == KeyEvent.KEYCODE_ENTER && !session.isRunning) {
            removeFinishedSession(session)
            return true
        }
        return false
    }

    // TerminalViewClient.onKeyUp shares Activity.onKeyUp's signature,
    // so this one override serves both callers: TerminalView consults
    // it for app-handled keys (none), and the framework dispatches
    // unhandled key-ups here. Returning a bare `false` would shadow
    // Activity.onKeyUp and swallow the back button.
    override fun onKeyUp(keyCode: Int, e: KeyEvent): Boolean = super.onKeyUp(keyCode, e)

    override fun onLongPress(event: MotionEvent): Boolean = false

    // Held/locked virtual modifiers from the extra-keys row (matches
    // termux's TermuxTerminalViewClient.readExtraKeysSpecialButton).
    private fun readSpecialButton(button: SpecialButton): Boolean =
        extraKeysView.readSpecialButton(button, true) == true

    override fun readControlKey(): Boolean = readSpecialButton(SpecialButton.CTRL)

    override fun readAltKey(): Boolean = readSpecialButton(SpecialButton.ALT)

    override fun readShiftKey(): Boolean = readSpecialButton(SpecialButton.SHIFT)

    override fun readFnKey(): Boolean = readSpecialButton(SpecialButton.FN)

    override fun onCodePoint(codePoint: Int, ctrlDown: Boolean, session: TerminalSession): Boolean = false

    override fun onEmulatorSet() {}

    // ---- TerminalSessionClient -----------------------------------------
    // The activity is the sole client for all live sessions; each
    // callback carries the changed session, so display callbacks act
    // only for the selected tab while background sessions keep
    // accumulating transcript via their pty reader threads.

    override fun onTextChanged(changedSession: TerminalSession) {
        if (changedSession === activeSession) terminalView.onScreenUpdated()
    }

    override fun onTitleChanged(changedSession: TerminalSession) {
        val index = TerminalSessions.list(distroId).indexOfFirst { it === changedSession }
        if (index >= 0) tabBar.setLabel(index, labelFor(changedSession, index))
    }

    override fun onSessionFinished(finishedSession: TerminalSession) {
        removeFinishedSession(finishedSession)
    }

    override fun onCopyTextToClipboard(session: TerminalSession, text: String?) {
        if (text.isNullOrEmpty()) return
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("", text))
    }

    override fun onPasteTextFromClipboard(session: TerminalSession?) {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val item = clipboard.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0) ?: return
        val text = item.coerceToText(this).toString()
        if (text.isNotEmpty()) terminalView.currentSession?.emulator?.paste(text)
    }

    override fun onBell(session: TerminalSession) {}

    override fun onColorsChanged(session: TerminalSession) {}

    override fun onTerminalCursorStateChange(state: Boolean) {}

    override fun setTerminalShellPid(session: TerminalSession, pid: Int) {}

    override fun getTerminalCursorStyle(): Int? = TerminalEmulator.DEFAULT_TERMINAL_CURSOR_STYLE

    // ---- logging (both client interfaces route logs through us) --------

    override fun logError(tag: String?, message: String?) {
        Log.e(tag ?: TAG, message ?: "")
    }

    override fun logWarn(tag: String?, message: String?) {
        Log.w(tag ?: TAG, message ?: "")
    }

    override fun logInfo(tag: String?, message: String?) {
        Log.i(tag ?: TAG, message ?: "")
    }

    override fun logDebug(tag: String?, message: String?) {}

    override fun logVerbose(tag: String?, message: String?) {}

    override fun logStackTraceWithMessage(tag: String?, message: String?, e: Exception?) {
        Log.e(tag ?: TAG, message ?: "", e)
    }

    override fun logStackTrace(tag: String?, e: Exception?) {
        Log.e(tag ?: TAG, "", e)
    }

    companion object {
        const val EXTRA_ID = "id"
        private const val TAG = "tawc-terminal"
        private const val TRANSCRIPT_ROWS = 4000
        private const val TAB_BAR_HEIGHT_DP = 40
        private const val DEFAULT_FONT_SIZE_DP = 13f
        // Termux's default extra-keys config and per-row height
        // (TermuxPropertyConstants.DEFAULT_IVALUE_EXTRA_KEYS and the
        // 37.5dp terminal_toolbar_view_pager in activity_termux.xml).
        private const val EXTRA_KEYS_CONFIG =
            "[['ESC','/',{key: '-', popup: '|'},'HOME','UP','END','PGUP'], " +
                "['TAB','CTRL','ALT','LEFT','DOWN','RIGHT','PGDN']]"
        private const val EXTRA_KEYS_STYLE = "default"
        private const val EXTRA_KEYS_ROW_HEIGHT_DP = 37.5f
        private const val FONT_SIZE_STEP_DP = 1f
        private const val MIN_FONT_SIZE_DP = 7f
        private const val MAX_FONT_SIZE_DP = 36f
    }
}
