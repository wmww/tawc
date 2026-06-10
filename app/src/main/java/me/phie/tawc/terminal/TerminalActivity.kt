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
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.buildChildScreen
import java.io.IOException

/**
 * Interactive shell into one installed rootfs, built on termux's
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
 * One terminal per distro: documentLaunchMode="intoExisting" plus a
 * unique tawc://terminal/<id> data URI reuse the activity instance and
 * recents card per id (same trick as CompositorActivity), and the
 * session itself lives in [TerminalSessions] so reopening reattaches.
 *
 * tawcroot-only: chroot spawns via su and proot is dev-only, so the
 * home-screen Terminal button is gated on the tawcroot method.
 */
class TerminalActivity : AppCompatActivity(), TerminalViewClient, TerminalSessionClient {

    private lateinit var terminalView: TerminalView
    private lateinit var extraKeysView: ExtraKeysView
    private var session: TerminalSession? = null
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

        val store = InstallationStore(this)
        val installation = store.load(distroId)
        val method = installation?.let {
            InstallationMethod.forKey(this, it.method) as? TawcrootMethod
        }
        if (installation == null || method == null) {
            Log.w(TAG, "no tawcroot installation for '$distroId'")
            finish()
            return
        }

        val scaffold = buildChildScreen(DistroRegistry.displayLabel(installation))
        // Leaving the terminal (up arrow or system back) backgrounds the
        // task instead of finishing it: the shell keeps running, the
        // recents card stays, and — because the activity instance stays
        // alive with the task — swiping the card later still reaches
        // onDestroy, which kills the shell. finish() here would leave a
        // card whose swipe the app never sees (no onTaskRemoved service;
        // see TerminalSessions).
        scaffold.toolbar.setNavigationOnClickListener { moveTaskToBack(true) }
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                moveTaskToBack(true)
            }
        })
        // Replace the scaffold's system-bar-only inset padding with one
        // that includes the IME, so the keyboard resizes the terminal
        // and the prompt stays visible above it.
        ViewCompat.setOnApplyWindowInsetsListener(scaffold.root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or
                    WindowInsetsCompat.Type.displayCutout() or
                    WindowInsetsCompat.Type.ime()
            )
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }

        fontSizePx = (DEFAULT_FONT_SIZE_DP * resources.displayMetrics.density).toInt()
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
        scaffold.content.setPadding(0, 0, 0, 0)
        scaffold.content.addView(
            terminalView,
            LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f),
        )

        // Termux's extra-keys row (ESC/arrows/CTRL/...) between the
        // terminal and the IME. Same default layout and per-row height
        // as termux; held CTRL/ALT/SHIFT/FN state is consumed via the
        // read*Key() client callbacks below.
        val extraKeysInfo = ExtraKeysInfo(
            EXTRA_KEYS_CONFIG, EXTRA_KEYS_STYLE, ExtraKeysConstants.CONTROL_CHARS_ALIASES,
        )
        val rowHeightPx = EXTRA_KEYS_ROW_HEIGHT_DP * resources.displayMetrics.density
        extraKeysView = ExtraKeysView(this, null).apply {
            setExtraKeysViewClient(TerminalExtraKeys(terminalView))
            setBackgroundColor(Color.BLACK)
        }
        scaffold.content.addView(
            extraKeysView,
            LinearLayout.LayoutParams(
                MATCH_PARENT,
                (rowHeightPx * extraKeysInfo.matrix.size + 0.5f).toInt(),
            ),
        )
        extraKeysView.reload(extraKeysInfo, rowHeightPx)
        setContentView(scaffold.root)

        val existing = TerminalSessions.get(distroId)
        val s = if (existing != null && existing.isRunning) {
            existing
        } else {
            // ptyShellExec fails closed (IOException) on a bad external
            // bind — revoked all-files access, missing host dir
            // (notes/external-binds.md). Surface the message instead of
            // crashing; the user fixes it under Manage binds / settings.
            val exec = try {
                method.ptyShellExec(store.rootfsDir(distroId).absolutePath)
            } catch (e: IOException) {
                Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
                finish()
                return
            }
            TerminalSession(
                exec.argv[0],
                exec.cwd,
                exec.argv.toTypedArray(),
                exec.hostEnv.toTypedArray(),
                TRANSCRIPT_ROWS,
                this,
            ).also { TerminalSessions.put(distroId, it) }
        }
        session = s
        s.updateTerminalSessionClient(this)
        terminalView.attachSession(s)

        terminalView.requestFocus()
    }

    override fun onResume() {
        super.onResume()
        if (::terminalView.isInitialized) terminalView.onScreenUpdated()
    }

    override fun onDestroy() {
        super.onDestroy()
        val s = session ?: return
        // The session outlives this activity in [TerminalSessions]; drop
        // its reference to us so the destroyed activity (and view tree)
        // is collectable. Reopening swaps the live client back in.
        s.updateTerminalSessionClient(DetachedTerminalClient(distroId))
        // Distinguish "task swiped away in recents" from recreation
        // (config change, system pressure): a swipe removes the recents
        // card before destroying us; recreation isn't finishing. With
        // the card gone nothing can reattach, so kill the shell like
        // closing a desktop terminal window. Back never finishes us
        // (see onCreate), so a live card always has a live activity to
        // receive this.
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val taskInRecents = am.appTasks.any { it.taskInfo.taskId == taskId }
        if (isFinishing && !taskInRecents) {
            TerminalSessions.remove(distroId, s)
            s.finishIfRunning()
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
        // Enter on a dead session closes the terminal (matches termux).
        if (keyCode == KeyEvent.KEYCODE_ENTER && !session.isRunning) {
            finishAndRemoveTask()
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

    override fun onTextChanged(changedSession: TerminalSession) {
        terminalView.onScreenUpdated()
    }

    override fun onTitleChanged(changedSession: TerminalSession) {}

    override fun onSessionFinished(finishedSession: TerminalSession) {
        TerminalSessions.remove(distroId, finishedSession)
        // The shell exited (user typed `exit`, or the rootfs side
        // died). Closing the screen and dropping the recents card
        // mirrors a desktop terminal window; a leftover card would
        // just respawn a fresh shell when tapped.
        if (!isFinishing) finishAndRemoveTask()
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
