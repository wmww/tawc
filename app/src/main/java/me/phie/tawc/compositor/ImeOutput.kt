package me.phie.tawc.compositor

import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager

/**
 * Outbound calls into [InputMethodManager] from the compositor and from
 * [TawcInputConnection]. Extracted so tests can swap a recording impl in
 * via the broker `test-init` action — that replaces the production
 * [RealImeOutput] (which wakes up Gboard / OpenBoard / AOSP-latin / …) with
 * one that never touches the system IME.
 *
 * Why this matters for tests: `updateSelection(-1, -1)` is the IMM signal
 * "the editor cleared its composing region", and most IMEs react to it
 * defensively by calling `IC.finishComposingText`. That fires
 * `nativeFinishComposingText`, which on the wayland side commits whatever
 * preedit the test just set — racing the next test step. Driving tests
 * through a recording impl removes the system IME from the loop entirely.
 *
 * See also: `notes/text-input.md` and `notes/testing.md` for the failure
 * mode this interface guards against.
 */
interface ImeOutput {
    /** Notify the IME of new selection / composing region. */
    fun updateSelection(view: View, selStart: Int, selEnd: Int, composingStart: Int, composingEnd: Int)

    /** Show the soft keyboard for [view]. */
    fun showSoftInput(view: View)

    /** Hide the soft keyboard attached to [view]. */
    fun hideSoftInput(view: View)

    /** Tell the IME to discard any cached IC and call back into
     *  `View.onCreateInputConnection` for a fresh one. */
    fun restartInput(view: View)
}

/**
 * Production [ImeOutput]. Looks up the system [InputMethodManager] from
 * the view's context at every call (cheap — `getSystemService` is cached
 * per context) and forwards directly. Identical behavior to the original
 * inline calls.
 */
object RealImeOutput : ImeOutput {
    override fun updateSelection(view: View, selStart: Int, selEnd: Int, composingStart: Int, composingEnd: Int) {
        view.context.getSystemService(InputMethodManager::class.java)
            ?.updateSelection(view, selStart, selEnd, composingStart, composingEnd)
    }

    override fun showSoftInput(view: View) {
        view.context.getSystemService(InputMethodManager::class.java)
            ?.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
    }

    override fun hideSoftInput(view: View) {
        view.context.getSystemService(InputMethodManager::class.java)
            ?.hideSoftInputFromWindow(view.windowToken, 0)
    }

    override fun restartInput(view: View) {
        view.context.getSystemService(InputMethodManager::class.java)
            ?.restartInput(view)
    }
}

/**
 * Test [ImeOutput]. Records every call without ever hitting the real
 * [InputMethodManager], so the system IME never sees `updateSelection`
 * (and therefore never fires defensive `finishComposingText` reactions
 * mid-test). Installed by the broker `test-init` action; process death
 * restores [RealImeOutput].
 *
 * Records are append-only and exposed for assertions, but the primary
 * value of this impl is what it *prevents* (IME reactivity), not what it
 * captures.
 */
class RecordingImeOutput : ImeOutput {
    sealed class Call {
        data class UpdateSelection(val selStart: Int, val selEnd: Int, val composingStart: Int, val composingEnd: Int) : Call()
        data object ShowSoftInput : Call()
        data object HideSoftInput : Call()
        data object RestartInput : Call()
    }

    private val _calls = mutableListOf<Call>()
    private var testInputConnection: TawcInputConnection? = null
    private var hiddenInputConnection: TawcInputConnection? = null
    val calls: List<Call> get() = synchronized(_calls) { _calls.toList() }

    private fun bindTestInputConnection(view: View) {
        hiddenInputConnection = testInputConnection ?: hiddenInputConnection
        testInputConnection = view.onCreateInputConnection(EditorInfo()) as? TawcInputConnection
    }

    internal fun clearTestInputConnection() {
        testInputConnection = null
        hiddenInputConnection = null
    }

    internal fun finishHiddenComposingTextForDev(): Boolean {
        val ic = hiddenInputConnection ?: return false
        hiddenInputConnection = null
        val ok = ic.finishComposingText()
        return ok
    }

    override fun updateSelection(view: View, selStart: Int, selEnd: Int, composingStart: Int, composingEnd: Int) {
        synchronized(_calls) { _calls += Call.UpdateSelection(selStart, selEnd, composingStart, composingEnd) }
    }

    override fun showSoftInput(view: View) {
        synchronized(_calls) { _calls += Call.ShowSoftInput }
        bindTestInputConnection(view)
    }

    override fun hideSoftInput(view: View) {
        synchronized(_calls) { _calls += Call.HideSoftInput }
        if (hiddenInputConnection == null) {
            hiddenInputConnection = testInputConnection
        }
        testInputConnection = null
        val active = NativeBridge.activeInputConnection
        if (active === hiddenInputConnection || active?.targetsView(view) == true) {
            NativeBridge.activeInputConnection = null
        }
    }

    override fun restartInput(view: View) {
        synchronized(_calls) { _calls += Call.RestartInput }
        bindTestInputConnection(view)
    }
}
