package me.phie.tawc.terminal

import android.util.Log
import com.termux.terminal.TerminalEmulator
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient

/**
 * Process-wide registry of live terminal sessions, one per installation
 * id. Sessions outlive [TerminalActivity] — recreation (uncaught config
 * changes, system pressure) and re-opening from the home screen reattach
 * to the running shell instead of spawning a second one. (Back merely
 * backgrounds the task, keeping the activity itself alive.) Sessions
 * die with the app process; there is
 * deliberately no foreground service keeping shells alive, so a
 * backgrounded app's shell can be reaped with the process.
 */
internal object TerminalSessions {
    private val sessions = HashMap<String, TerminalSession>()

    @Synchronized
    fun get(id: String): TerminalSession? = sessions[id]

    @Synchronized
    fun put(id: String, session: TerminalSession) {
        sessions[id] = session
    }

    /** Remove [session] if it is still the registered one for [id] —
     * guards against a finished session clobbering its replacement. */
    @Synchronized
    fun remove(id: String, session: TerminalSession) {
        if (sessions[id] === session) sessions.remove(id)
    }
}

/**
 * Client swapped in by [TerminalActivity.onDestroy] so a retained
 * session doesn't keep the destroyed activity (and its view tree)
 * reachable until the next reattach. The pty reader threads keep
 * draining output into the transcript regardless of client. If the
 * shell exits while detached, drop the registry entry here — the
 * activity's own [TerminalActivity.onSessionFinished] is gone.
 */
internal class DetachedTerminalClient(private val id: String) : TerminalSessionClient {
    override fun onTextChanged(changedSession: TerminalSession) {}
    override fun onTitleChanged(changedSession: TerminalSession) {}
    override fun onSessionFinished(finishedSession: TerminalSession) {
        TerminalSessions.remove(id, finishedSession)
    }
    override fun onCopyTextToClipboard(session: TerminalSession, text: String?) {}
    override fun onPasteTextFromClipboard(session: TerminalSession?) {}
    override fun onBell(session: TerminalSession) {}
    override fun onColorsChanged(session: TerminalSession) {}
    override fun onTerminalCursorStateChange(state: Boolean) {}
    override fun setTerminalShellPid(session: TerminalSession, pid: Int) {}
    override fun getTerminalCursorStyle(): Int? = TerminalEmulator.DEFAULT_TERMINAL_CURSOR_STYLE
    override fun logError(tag: String?, message: String?) { Log.e(tag ?: TAG, message ?: "") }
    override fun logWarn(tag: String?, message: String?) { Log.w(tag ?: TAG, message ?: "") }
    override fun logInfo(tag: String?, message: String?) {}
    override fun logDebug(tag: String?, message: String?) {}
    override fun logVerbose(tag: String?, message: String?) {}
    override fun logStackTraceWithMessage(tag: String?, message: String?, e: Exception?) {
        Log.e(tag ?: TAG, message ?: "", e)
    }
    override fun logStackTrace(tag: String?, e: Exception?) { Log.e(tag ?: TAG, "", e) }

    private companion object {
        const val TAG = "tawc-terminal"
    }
}
