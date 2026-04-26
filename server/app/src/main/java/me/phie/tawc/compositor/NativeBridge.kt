package me.phie.tawc.compositor

import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.view.View
import android.view.inputmethod.InputMethodManager
import java.lang.ref.WeakReference

object NativeBridge {
    private const val TAG = "tawc"
    private val mainHandler = Handler(Looper.getMainLooper())

    /** Weak ref to the view for keyboard show/hide. Set by CompositorActivity. */
    private var inputViewRef: WeakReference<View>? = null

    var inputView: View?
        get() = inputViewRef?.get()
        set(value) { inputViewRef = value?.let { WeakReference(it) } }

    init {
        System.loadLibrary("compositor")
    }

    /** Start the compositor render loop. Called once with the initial surface. */
    external fun nativeOnSurfaceCreated(surface: Surface)

    /** Notify the compositor that the surface dimensions changed. */
    external fun nativeOnSurfaceChanged(width: Int, height: Int)

    /** Notify the compositor that the surface is being destroyed. */
    external fun nativeOnSurfaceDestroyed()

    /** Forward a touch event to the compositor. */
    external fun nativeOnTouchEvent(action: Int, pointerId: Int, x: Float, y: Float, eventTime: Long)

    // --- Text input: Android InputConnection → Compositor ---

    /** commitText from InputConnection */
    external fun nativeCommitText(text: String)

    /** setComposingText from InputConnection */
    external fun nativeSetComposingText(text: String)

    /** finishComposingText from InputConnection */
    external fun nativeFinishComposingText()

    /** deleteSurroundingText from InputConnection */
    external fun nativeDeleteSurroundingText(before: Int, after: Int)

    /** sendKeyEvent mapped to keycode */
    external fun nativeSendKeyEvent(keycode: Int)

    /** Query compositor state (logs COMPOSITOR_STATE line to logcat). */
    external fun nativeQueryState()

    // --- Reverse JNI: Compositor → Android (called from compositor thread) ---

    /** Called from native when a Wayland client enables text input. */
    @JvmStatic
    fun onShowKeyboard() {
        Log.d(TAG, "onShowKeyboard (from compositor)")
        mainHandler.post {
            val view = inputView ?: return@post
            view.requestFocus()
            val imm = view.context.getSystemService(InputMethodManager::class.java)
            imm?.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    /** Called from native when a Wayland client disables text input. */
    @JvmStatic
    fun onHideKeyboard() {
        Log.d(TAG, "onHideKeyboard (from compositor)")
        mainHandler.post {
            val view = inputView ?: return@post
            val imm = view.context.getSystemService(InputMethodManager::class.java)
            imm?.hideSoftInputFromWindow(view.windowToken, 0)
        }
    }

    /**
     * Called from native when a Wayland client reports cursor/selection changes
     * caused by non-IME actions (user touch, arrow keys, etc.).
     * Notifies Gboard so it can reset its composing state and text model.
     */
    @JvmStatic
    fun onUpdateSelection(selStart: Int, selEnd: Int, composingStart: Int, composingEnd: Int) {
        Log.d(TAG, "onUpdateSelection (from compositor): sel=$selStart..$selEnd composing=$composingStart..$composingEnd")
        mainHandler.post {
            val view = inputView ?: return@post
            val imm = view.context.getSystemService(InputMethodManager::class.java)
            imm?.updateSelection(view, selStart, selEnd, composingStart, composingEnd)
        }
    }
}
