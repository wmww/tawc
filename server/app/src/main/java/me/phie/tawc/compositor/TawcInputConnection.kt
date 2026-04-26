package me.phie.tawc.compositor

import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.util.Log

/**
 * Custom InputConnection that bridges Android IME events to the Wayland
 * compositor via JNI.
 *
 * Extends BaseInputConnection with fullEditor=true, which maintains an
 * internal Editable buffer. We call super in all overridden methods so
 * this buffer stays in sync with what we send to the Wayland client.
 * This is important because Gboard queries the buffer (getTextBeforeCursor,
 * getTextAfterCursor, getExtractedText, etc.) to understand the editor
 * state for predictions, autocorrect, and cursor tracking.
 *
 * Without calling super, the buffer is always empty and Gboard operates
 * blind — it doesn't know what text is in the field or where the cursor
 * is, causing broken behavior after cursor movement.
 *
 * Note: BaseInputConnection with fullEditor=true does NOT call
 * sendCurrentText() (which would cause duplicate input), because
 * mFallbackMode is false when fullEditor is true.
 */
class TawcInputConnection(view: View) : BaseInputConnection(view, true) {

    companion object {
        private const val TAG = "tawc"
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val str = text?.toString() ?: return false
        Log.d(TAG, "InputConnection.commitText: \"$str\"")
        super.commitText(text, newCursorPosition)
        NativeBridge.nativeCommitText(str)
        return true
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val str = text?.toString() ?: ""
        Log.d(TAG, "InputConnection.setComposingText: \"$str\"")
        super.setComposingText(text, newCursorPosition)
        NativeBridge.nativeSetComposingText(str)
        return true
    }

    override fun finishComposingText(): Boolean {
        Log.d(TAG, "InputConnection.finishComposingText")
        super.finishComposingText()
        NativeBridge.nativeFinishComposingText()
        return true
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        Log.d(TAG, "InputConnection.deleteSurroundingText: before=$beforeLength after=$afterLength")
        super.deleteSurroundingText(beforeLength, afterLength)
        NativeBridge.nativeDeleteSurroundingText(beforeLength, afterLength)
        return true
    }

    override fun performEditorAction(actionCode: Int): Boolean {
        Log.d(TAG, "InputConnection.performEditorAction: actionCode=$actionCode")
        // IME action button (Go, Done, Search, etc.) — treat as Enter
        NativeBridge.nativeSendKeyEvent(KeyEvent.KEYCODE_ENTER)
        return true
    }

    override fun sendKeyEvent(event: KeyEvent): Boolean {
        // Only handle key-down events to avoid double-processing
        if (event.action != KeyEvent.ACTION_DOWN) return true

        Log.d(TAG, "InputConnection.sendKeyEvent: keyCode=${event.keyCode}")
        NativeBridge.nativeSendKeyEvent(event.keyCode)
        return true
    }
}
