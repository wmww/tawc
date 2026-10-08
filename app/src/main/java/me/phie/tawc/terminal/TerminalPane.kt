package me.phie.tawc.terminal

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.util.Log
import android.util.TypedValue
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewTreeObserver
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.res.ResourcesCompat
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
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.Settings
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import java.io.File
import java.io.IOException
import java.lang.ref.WeakReference

/**
 * The home screen's terminal surface: interactive shells into one
 * installed rootfs, built on termux's vendored
 * terminal-emulator/terminal-view modules (Apache-2.0; see
 * settings.gradle.kts). The termux JNI forks the pty pair and execs
 * tawcroot as the pty child ([TawcrootMethod.ptyShellExec]), so the
 * in-rootfs shell gets a real controlling tty — readline, job control
 * and curses apps work, unlike the pipe-fed RunCommandOp path. No
 * compositor involvement: the Wayland env vars are set but nothing
 * waits for the socket, so the terminal works with the graphics stack
 * cold.
 *
 * One [TerminalView] shows whichever session [show] was last given
 * (termux-app's own multi-session pattern — background sessions keep a
 * stale pty size until shown). Tabs, selection and the close policy
 * belong to [DistroHome]; this class spawns sessions, is their client
 * while attached, and reports exits and title changes to [host].
 *
 * tawcroot-only: chroot spawns via su and proot is dev-only.
 */
internal class TerminalPane(
    private val activity: AppCompatActivity,
    private val distroId: String,
    private val method: TawcrootMethod,
    private val host: Host,
) : TerminalViewClient, TerminalSessionClient {

    interface Host {
        /** [session]'s shell exited. */
        fun onSessionExited(session: TerminalSession)
        /** Some session's title changed (settled); relabel tabs. */
        fun onTitlesChanged()
    }

    private val store = InstallationStore(activity)
    private val density = activity.resources.displayMetrics.density
    private val terminalView: TerminalView
    private val extraKeysView: ExtraKeysView
    // Volatile: [focusedTty] reads them off the main thread.
    @Volatile private var activeSession: TerminalSession? = null
    private var fontSizePx = fontSizePx()
    @Volatile private var detached = false

    /** Black column: terminal, extra keys. */
    val view: LinearLayout

    init {
        view = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }

        terminalView = TerminalView(activity, null).apply {
            setTerminalViewClient(this@TerminalPane)
            setTextSize(fontSizePx)
            // Bundled so glyph widths don't depend on the OEM's
            // "monospace" (a non-mono one gets per-glyph stretched).
            ResourcesCompat.getFont(activity, R.font.hack_regular)?.let { setTypeface(it) }
            setBackgroundColor(Color.BLACK)
            // Key events only reach the view when it can hold focus —
            // termux sets this in XML (activity_termux.xml); the view
            // itself doesn't.
            isFocusableInTouchMode = true
            // Only applies while the view is visible, i.e. a terminal
            // tab is selected.
            keepScreenOn = true
        }
        view.addView(terminalView, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        // Termux's extra-keys row (ESC/arrows/CTRL/...) between the
        // terminal and the IME. Same default layout and per-row height
        // as termux; held CTRL/ALT/SHIFT/FN state is consumed via the
        // read*Key() client callbacks below. Targets the view, not a
        // session, so tab switches need no extra-keys work.
        val extraKeysInfo = ExtraKeysInfo(
            EXTRA_KEYS_CONFIG, EXTRA_KEYS_STYLE, ExtraKeysConstants.CONTROL_CHARS_ALIASES,
        )
        val rowHeightPx = EXTRA_KEYS_ROW_HEIGHT_DP * density
        extraKeysView = ExtraKeysView(activity, null).apply {
            setExtraKeysViewClient(TerminalExtraKeys(terminalView))
            setBackgroundColor(Color.BLACK)
        }
        val extraKeysHeightPx = (rowHeightPx * extraKeysInfo.matrix.size + 0.5f).toInt()
        view.addView(extraKeysView, LinearLayout.LayoutParams(MATCH_PARENT, extraKeysHeightPx))
        extraKeysView.reload(extraKeysInfo, rowHeightPx)
        current = WeakReference(this)
    }

    /** Become the client of [sessions] (already-running tabs). */
    fun adopt(sessions: List<TerminalSession>) {
        for (s in sessions) s.updateTerminalSessionClient(this)
    }

    /**
     * Stop driving [distroId]'s sessions. They keep running under a
     * [DetachedTerminalClient].
     */
    fun detach() {
        if (detached) return
        detached = true
        if (current?.get() === this) current = null
        view.removeCallbacks(relabel)
        for (s in TerminalSessions.list(distroId)) {
            s.updateTerminalSessionClient(DetachedTerminalClient(distroId))
        }
        activeSession = null
    }

    fun onResume() {
        // Terminal scale may have changed in settings.
        val size = fontSizePx()
        if (size != fontSizePx) {
            fontSizePx = size
            terminalView.setTextSize(size)
        }
        screenUpdated()
    }

    /** Show [session] and pop the keyboard. */
    fun show(session: TerminalSession) {
        activeSession = session
        // attachSession resets emulator/top-row state and updateSize()s,
        // (re)initializing or SIGWINCHing the pty for the current view.
        terminalView.attachSession(session)
        terminalView.onScreenUpdated()
        showSoftKeyboard()
    }

    /** No tab shown (the apps tab is selected). */
    fun hide() {
        activeSession = null
    }

    private fun showSoftKeyboard() {
        terminalView.requestFocus()
        val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (terminalView.hasWindowFocus()) {
            // Post: a just-shown view isn't laid out yet.
            terminalView.post { imm.showSoftInput(terminalView, 0) }
            return
        }
        // Cold start or onNewIntent: the IME ignores a window without
        // focus, so wait for it.
        val observer = terminalView.viewTreeObserver
        observer.addOnWindowFocusChangeListener(object : ViewTreeObserver.OnWindowFocusChangeListener {
            override fun onWindowFocusChanged(hasFocus: Boolean) {
                if (!hasFocus) return
                terminalView.viewTreeObserver.removeOnWindowFocusChangeListener(this)
                if (activeSession != null && terminalView.hasFocus()) imm.showSoftInput(terminalView, 0)
            }
        })
    }

    /**
     * Spawn a fresh shell session, or toast and return null on failure:
     * ptyShellExec fails closed (IOException) on a bad external bind —
     * revoked all-files access, missing host dir
     * (notes/external-binds.md). The user fixes it under Manage binds /
     * settings.
     *
     * [CommandTab.exec] (a launcher entry's Exec line) runs wrapped in
     * the hold-open trailer so its output survives exit until a
     * keypress; [CommandTab.label] names the tab until an OSC title
     * arrives ([labelFor]).
     */
    fun spawn(command: CommandTab?): TerminalSession? {
        // GUI programs typed into the shell start the compositor by
        // connecting; its sockets must be listening first.
        CompositorService.ensureActivation(activity)
        val exec = try {
            method.ptyShellExec(
                store.rootfsDir(distroId).absolutePath,
                graphics = command?.graphics,
                command = command?.exec?.let { "$it$HOLD_OPEN_TRAILER" },
            )
        } catch (e: IOException) {
            Toast.makeText(activity, e.message, Toast.LENGTH_LONG).show()
            return null
        }
        return TerminalSession(
            exec.argv[0],
            exec.cwd,
            exec.argv.toTypedArray(),
            exec.hostEnv.toTypedArray(),
            TRANSCRIPT_ROWS,
            this,
        ).also { it.mSessionName = command?.label }
    }

    fun labelFor(session: TerminalSession, index: Int): CharSequence {
        val title = session.title?.takeUnless { it.isBlank() }
        // The shipped bashrc defaults title tabs with the cwd
        // (ShellDefaults), so every fresh tab would read `~` — number
        // those by tab position instead, and use the same numbering
        // while no title is set yet. Command tabs carry the launcher
        // entry's name in mSessionName.
        return if (title == null || title == "~") {
            session.mSessionName?.takeUnless { it.isBlank() }
                ?: activity.getString(R.string.terminal_tab_home, index + 1)
        } else {
            title
        }
    }

    /** sp so it follows the system font size; see [Settings.terminalScale]. */
    private fun fontSizePx(): Int {
        val sp = TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_SP, DEFAULT_FONT_SIZE_SP, activity.resources.displayMetrics,
        )
        return (sp * Settings.terminalScale).toInt().coerceAtLeast(1)
    }

    // ---- TerminalViewClient --------------------------------------------

    // Pinch-zoom disabled: size comes from the terminal scale setting.
    override fun onScale(scale: Float): Float = 1.0f

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

    // System keys (Back, volume) and bare modifiers write nothing.
    override fun onKeyDown(keyCode: Int, e: KeyEvent, session: TerminalSession): Boolean {
        if (keyCode == KeyEvent.KEYCODE_ENTER && !session.isRunning) {
            // Enter on a dead tab closes it (only reachable in the
            // race window before onSessionFinished lands).
            host.onSessionExited(session)
            return true
        }
        if (!e.isSystem && !KeyEvent.isModifierKey(keyCode)) snapToBottom()
        return false
    }

    override fun onKeyUp(keyCode: Int, e: KeyEvent): Boolean = false

    override fun onLongPress(event: MotionEvent): Boolean = false

    // Held/locked virtual modifiers from the extra-keys row (matches
    // termux's TermuxTerminalViewClient.readExtraKeysSpecialButton).
    private fun readSpecialButton(button: SpecialButton): Boolean =
        extraKeysView.readSpecialButton(button, true) == true

    override fun readControlKey(): Boolean = readSpecialButton(SpecialButton.CTRL)

    override fun readAltKey(): Boolean = readSpecialButton(SpecialButton.ALT)

    override fun readShiftKey(): Boolean = readSpecialButton(SpecialButton.SHIFT)

    override fun readFnKey(): Boolean = readSpecialButton(SpecialButton.FN)

    override fun onCodePoint(codePoint: Int, ctrlDown: Boolean, session: TerminalSession): Boolean {
        snapToBottom()
        return false
    }

    override fun onEmulatorSet() {}

    // ---- TerminalSessionClient -----------------------------------------
    // The pane is the sole client for its distro's live sessions; each
    // callback carries the changed session, so display callbacks act
    // only for the shown tab while background sessions keep
    // accumulating transcript via their pty reader threads.

    override fun onTextChanged(changedSession: TerminalSession) {
        if (changedSession !== activeSession) return
        screenUpdated()
    }

    /**
     * onScreenUpdated() that holds the viewport on the same lines when
     * scrolled back, instead of termux's unconditional snap to bottom.
     */
    private fun screenUpdated() {
        val emulator = terminalView.mEmulator
        val topRow = terminalView.topRow
        // Pinned to the bottom, or upstream already shifts (selection).
        if (emulator == null || topRow == 0 || terminalView.isSelectingText) {
            terminalView.onScreenUpdated()
            return
        }
        val shift = emulator.scrollCounter // cleared by onScreenUpdated
        terminalView.onScreenUpdated(true)
        // Once the transcript ring drops the held lines, sit at the oldest.
        terminalView.topRow = maxOf(-emulator.screen.activeTranscriptRows, topRow - shift)
        terminalView.invalidate()
    }

    /** Input while scrolled back jumps to the live screen, like xterm. */
    private fun snapToBottom() {
        if (terminalView.topRow == 0) return
        terminalView.topRow = 0
        terminalView.invalidate()
    }

    // A prompt can set the title twice in a row (a distro
    // PROMPT_COMMAND's `root@localhost:~`, then ShellDefaults' `~`),
    // and the two may land in separate output chunks; relabelling on
    // each would flash the long one. Settle first.
    override fun onTitleChanged(changedSession: TerminalSession) {
        view.removeCallbacks(relabel)
        view.postDelayed(relabel, TITLE_SETTLE_MS)
    }

    private val relabel = Runnable { if (!detached) host.onTitlesChanged() }

    override fun onSessionFinished(finishedSession: TerminalSession) {
        if (finishedSession === activeSession) activeSession = null
        host.onSessionExited(finishedSession)
    }

    override fun onCopyTextToClipboard(session: TerminalSession, text: String?) {
        if (text.isNullOrEmpty()) return
        val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("", text))
    }

    override fun onPasteTextFromClipboard(session: TerminalSession?) {
        val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val item = clipboard.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0) ?: return
        val text = item.coerceToText(activity).toString()
        if (text.isEmpty()) return
        snapToBottom()
        terminalView.currentSession?.emulator?.paste(text)
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

    /** What a new tab runs: [exec] (a launcher entry's Exec line), or
     *  a plain shell when null; [label] names it; [graphics] is the
     *  entry's override (null = the global setting). */
    data class CommandTab(val exec: String?, val label: String?, val graphics: GraphicsBackend? = null)

    /** [Companion.focusedTty] for this pane. */
    private fun focusedTty(): Int {
        if (detached || !terminalView.hasWindowFocus()) return 0
        val pid = activeSession?.pid?.takeIf { it > 0 } ?: return 0
        return try {
            ttyOf(File("/proc/$pid/stat").readText()) ?: 0
        } catch (_: IOException) {
            0
        }
    }

    companion object {
        /** The attached pane, for [focusedTty]. */
        @Volatile private var current: WeakReference<TerminalPane>? = null

        /**
         * `tty_nr` of the shell the user is looking at: the attached
         * pane's selected tab, while its window has Android focus. 0 for
         * none. Any thread — the compositor gates `wl-paste` on it
         * (notes/clipboard.md).
         */
        fun focusedTty(): Int = current?.get()?.focusedTty() ?: 0

        /**
         * Appended to every command tab before spawn: the session would
         * exit (and [onSessionFinished] drop the tab) the moment the
         * command finishes, vanishing its output. Holding in `read`
         * keeps the shell alive until a keypress, then the normal
         * tab-removal flow runs — no session-lifecycle changes.
         */
        const val HOLD_OPEN_TRAILER =
            "; __c=$?; printf '\\n[exited %d — press any key]\\n' \"\$__c\"; read -rsn1"

        const val TAG = "tawc-terminal"
        const val TITLE_SETTLE_MS = 150L
        const val TRANSCRIPT_ROWS = 4000
        const val DEFAULT_FONT_SIZE_SP = 13f
        // Termux's default extra-keys config and per-row height
        // (TermuxPropertyConstants.DEFAULT_IVALUE_EXTRA_KEYS and the
        // 37.5dp terminal_toolbar_view_pager in activity_termux.xml).
        const val EXTRA_KEYS_CONFIG =
            "[['ESC','/',{key: '-', popup: '|'},'HOME','UP','END','PGUP'], " +
                "['TAB','CTRL','ALT','LEFT','DOWN','RIGHT','PGDN']]"
        const val EXTRA_KEYS_STYLE = "default"
        const val EXTRA_KEYS_ROW_HEIGHT_DP = 37.5f
    }
}

/** Controlling tty (`tty_nr`, 0 for none) from a `/proc/<pid>/stat`
 *  line; the `(comm)` may itself hold spaces and parentheses. */
internal fun ttyOf(stat: String): Int? {
    val close = stat.lastIndexOf(')')
    if (close < 0) return null
    return stat.substring(close + 1).trim().split(' ').getOrNull(4)?.toIntOrNull()
}
