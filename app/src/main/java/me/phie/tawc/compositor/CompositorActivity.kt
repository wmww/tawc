package me.phie.tawc.compositor

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection

/**
 * Hosts the Rust Wayland compositor on a SurfaceView. All interaction
 * with the native compositor (surface lifecycle, touch, IME) lives in
 * this package.
 *
 * The Activity binds to [CompositorService] (which owns the compositor
 * thread + Wayland socket) and forwards its `SurfaceView` lifecycle and
 * input events to native, tagged with its `activityId`. The id comes
 * from `intent.data?.lastPathSegment` of a `tawc://activity/<id>` URI;
 * the only path that launches this Activity is the `spawnActivity`
 * reverse-JNI call from the compositor's policy.
 */
class CompositorActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    /** Set in onCreate from intent.data. Always non-null at runtime —
     *  the only path that creates this Activity is the spawnActivity
     *  reverse-JNI call, which always sets a `tawc://activity/<id>` URI. */
    private lateinit var activityId: String
    /** False until onCreate finished its full setup — guards onDestroy
     *  cleanup against the early-return path when intent.data is missing. */
    private var initialized = false

    private var compositorService: CompositorService? = null

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            compositorService = (binder as CompositorService.LocalBinder).getService()
            compositorService?.registerActivity(activityId, this@CompositorActivity)
            Log.d(TAG, "Bound to CompositorService for $activityId")
        }
        override fun onServiceDisconnected(name: ComponentName) {
            compositorService = null
        }
    }

    /**
     * BroadcastReceiver for injecting text input from tests.
     * Usage: adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"
     * Usage: adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67
     *
     * Events go through TawcInputConnection (via InputMethodManager) so they
     * exercise the same code path as real Gboard input, including the
     * BaseInputConnection Editable updates and all TawcInputConnection logic.
     *
     * QUERY_STATE lives on CompositorService instead — it has to work
     * even when no Activity is in the foreground.
     */
    /**
     * BroadcastReceiver for injecting test input.
     *
     * **These broadcasts bypass [TawcInputConnection] and call native
     * directly.** The reason: the system IME (OpenBoard, Gboard, etc.) is
     * also bound to our SurfaceView's InputConnection and reacts to every
     * Editable change with its own `setComposingRegion`/`setComposingText`
     * calls (e.g. marking the just-typed word as composing for autocorrect).
     * That makes integration tests non-deterministic — a test broadcast
     * that says "type X at cursor" gets amplified by the IME into "replace
     * the whole word with X". Bypassing the InputConnection here means
     * tests drive the compositor's text-input pipeline directly without
     * any third-party IME in the loop.
     *
     * Real IME input still goes through [TawcInputConnection] — the system
     * IMM picks the IC returned by `onCreateInputConnection`, and that
     * path applies all the composing-region translation, Editable mirror,
     * etc. that real Gboard usage needs.
     *
     * Test broadcasts mirror the [TawcInputConnection] surface but accept
     * explicit `deleteBefore`/`deleteAfter` integers (UTF-16 code unit
     * counts around the cursor). These let tests simulate Gboard's
     * "replace the composing region" semantics without an IME present.
     */
    private val testInputReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            // With multi-window every CompositorActivity registers a
            // receiver. Only the one currently in the foreground should
            // act on the broadcast — otherwise N Activities each
            // commit the same text and the test sees N copies.
            if (!hasWindowFocus()) {
                Log.d(TAG, "${intent.action}: not focused, ignoring (activityId=$activityId)")
                return
            }
            when (intent.action) {
                "me.phie.tawc.TEXT_INPUT" -> {
                    val text = intent.getStringExtra("text") ?: return
                    val before = intent.getIntExtra("deleteBefore", 0)
                    val after = intent.getIntExtra("deleteAfter", 0)
                    Log.d(TAG, "TestInput: commitText \"$text\" delete=$before/$after")
                    if (text == "\n") {
                        NativeBridge.nativeSendKeyEvent(android.view.KeyEvent.KEYCODE_ENTER)
                    } else {
                        NativeBridge.nativeCommitText(text, before, after)
                    }
                }
                "me.phie.tawc.SET_COMPOSING_TEXT" -> {
                    val text = intent.getStringExtra("text") ?: return
                    val before = intent.getIntExtra("deleteBefore", 0)
                    val after = intent.getIntExtra("deleteAfter", 0)
                    Log.d(TAG, "TestInput: setComposingText \"$text\" delete=$before/$after")
                    NativeBridge.nativeSetComposingText(text, before, after)
                }
                "me.phie.tawc.FINISH_COMPOSING_TEXT" -> {
                    Log.d(TAG, "TestInput: finishComposingText")
                    NativeBridge.nativeFinishComposingText()
                }
                "me.phie.tawc.DELETE_SURROUNDING_TEXT" -> {
                    val before = intent.getIntExtra("before", 0)
                    val after = intent.getIntExtra("after", 0)
                    Log.d(TAG, "TestInput: deleteSurroundingText $before/$after")
                    // Backspace / Forward-Delete keys — same wire as the IC path.
                    // The bypass exists to skip the local Editable mirror, not to
                    // exercise a separate Wayland code path.
                    repeat(before) { NativeBridge.nativeSendKeyEvent(android.view.KeyEvent.KEYCODE_DEL) }
                    repeat(after) { NativeBridge.nativeSendKeyEvent(android.view.KeyEvent.KEYCODE_FORWARD_DEL) }
                }
                "me.phie.tawc.KEY_EVENT" -> {
                    val keycode = intent.getIntExtra("keycode", -1)
                    if (keycode >= 0) {
                        Log.d(TAG, "TestInput: sendKeyEvent $keycode")
                        NativeBridge.nativeSendKeyEvent(keycode)
                    }
                }

                // ---- IC-driven test broadcasts ----
                // The above broadcasts bypass TawcInputConnection to avoid
                // IME amplification. These ones do the opposite: they call
                // the active TawcInputConnection's IME methods directly so
                // tests can exercise the IC's own logic (composing-region
                // delta computation, Editable mirror, etc.) — i.e. the
                // path real Gboard takes. Use these only for tests that
                // specifically need IC behaviour; they may be racy with the
                // system IME's reactions.
                "me.phie.tawc.IC_COMMIT_TEXT" -> {
                    val text = intent.getStringExtra("text") ?: return
                    val ic = NativeBridge.activeInputConnection
                    Log.d(TAG, "TestInput[IC]: commitText \"$text\" (ic=${ic != null})")
                    ic?.commitText(text, 1)
                }
                "me.phie.tawc.IC_SET_COMPOSING_TEXT" -> {
                    val text = intent.getStringExtra("text") ?: return
                    val ic = NativeBridge.activeInputConnection
                    Log.d(TAG, "TestInput[IC]: setComposingText \"$text\" (ic=${ic != null})")
                    ic?.setComposingText(text, 1)
                }
                "me.phie.tawc.IC_SET_COMPOSING_REGION" -> {
                    val start = intent.getIntExtra("start", -1)
                    val end = intent.getIntExtra("end", -1)
                    if (start < 0 || end < 0) return
                    val ic = NativeBridge.activeInputConnection
                    Log.d(TAG, "TestInput[IC]: setComposingRegion $start..$end (ic=${ic != null})")
                    ic?.setComposingRegion(start, end)
                }
                "me.phie.tawc.IC_FINISH_COMPOSING" -> {
                    val ic = NativeBridge.activeInputConnection
                    Log.d(TAG, "TestInput[IC]: finishComposingText (ic=${ic != null})")
                    ic?.finishComposingText()
                }
                "me.phie.tawc.IC_SET_SELECTION" -> {
                    val start = intent.getIntExtra("start", -1)
                    val end = intent.getIntExtra("end", -1)
                    if (start < 0 || end < 0) return
                    val ic = NativeBridge.activeInputConnection
                    Log.d(TAG, "TestInput[IC]: setSelection $start..$end (ic=${ic != null})")
                    ic?.setSelection(start, end)
                }
            }
        }
    }

    @Suppress("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // The only legitimate launch path is the spawnActivity reverse-JNI
        // call, which always sets a `tawc://activity/<uuid>` data URI.
        // Anything else (system relaunches an old Intent without data,
        // a stray `am start` from the user) is an orphaned task; finish
        // immediately so the recents card disappears.
        val id = intent?.data?.lastPathSegment
        if (id.isNullOrEmpty()) {
            Log.w(TAG, "CompositorActivity launched without tawc:// activityId — finishing")
            finishAndRemoveTask()
            return
        }
        activityId = id
        Log.d(TAG, "CompositorActivity onCreate activityId=$activityId")

        // Start and bind the CompositorService. The Service owns the
        // compositor thread (and runs xkb-data extraction) and survives
        // this Activity's lifetime.
        val serviceIntent = Intent(this, CompositorService::class.java)
        startForegroundService(serviceIntent)
        bindService(serviceIntent, serviceConnection, Context.BIND_AUTO_CREATE)

        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        )
        window.addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)

        surfaceView = TawcSurfaceView(this)
        setContentView(surfaceView)
        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        surfaceView.setOnTouchListener { _, event -> dispatchTouchToCompositor(event) }
        NativeBridge.inputView = surfaceView

        // Register test input receiver
        val filter = IntentFilter().apply {
            addAction("me.phie.tawc.TEXT_INPUT")
            addAction("me.phie.tawc.SET_COMPOSING_TEXT")
            addAction("me.phie.tawc.FINISH_COMPOSING_TEXT")
            addAction("me.phie.tawc.DELETE_SURROUNDING_TEXT")
            addAction("me.phie.tawc.KEY_EVENT")
            addAction("me.phie.tawc.IC_COMMIT_TEXT")
            addAction("me.phie.tawc.IC_SET_COMPOSING_TEXT")
            addAction("me.phie.tawc.IC_SET_COMPOSING_REGION")
            addAction("me.phie.tawc.IC_FINISH_COMPOSING")
            addAction("me.phie.tawc.IC_SET_SELECTION")
        }
        @Suppress("UnspecifiedRegisterReceiverFlag")
        registerReceiver(testInputReceiver, filter, RECEIVER_EXPORTED)
        initialized = true
    }

    override fun onDestroy() {
        if (initialized) {
            unregisterReceiver(testInputReceiver)
            NativeBridge.nativeOnActivityDestroyed(activityId)
            compositorService?.unregisterActivity(activityId)
            try {
                unbindService(serviceConnection)
            } catch (e: IllegalArgumentException) {
                // Service was never successfully bound — safe to ignore.
            }
        }
        super.onDestroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (!initialized) return
        // Use the SurfaceFrame for the registration size — the holder
        // already knows the buffer geometry. The compositor falls back
        // to ANativeWindow_get{Width,Height} if these come in as 0.
        val frame = holder.surfaceFrame
        NativeBridge.nativeRegisterActivitySurface(
            activityId, holder.surface, frame.width(), frame.height()
        )
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (!initialized) return
        NativeBridge.nativeOnActivitySurfaceChanged(activityId, width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (!initialized) return
        NativeBridge.nativeOnActivitySurfaceDestroyed(activityId)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!initialized) return
        NativeBridge.nativeOnActivityFocusChanged(activityId, hasFocus)
    }

    /**
     * Custom SurfaceView that provides our InputConnection to the IME.
     * This makes the view act as a text input target for Gboard.
     */
    private class TawcSurfaceView(context: Context) : SurfaceView(context) {
        override fun onCheckIsTextEditor(): Boolean = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            val (inputType, extraFlags) = NativeBridge.imeEditorInfo
            outAttrs.inputType = inputType
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
                EditorInfo.IME_ACTION_NONE or extraFlags
            return TawcInputConnection(this)
        }
    }

    private fun dispatchTouchToCompositor(event: MotionEvent): Boolean {
        val actionMasked = event.actionMasked
        when (actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val idx = event.actionIndex
                NativeBridge.nativeOnTouchEvent(
                    activityId, actionMasked, event.getPointerId(idx),
                    event.getX(idx), event.getY(idx), event.eventTime
                )
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    NativeBridge.nativeOnTouchEvent(
                        activityId, actionMasked, event.getPointerId(i),
                        event.getX(i), event.getY(i), event.eventTime
                    )
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val idx = event.actionIndex
                NativeBridge.nativeOnTouchEvent(
                    activityId, actionMasked, event.getPointerId(idx),
                    event.getX(idx), event.getY(idx), event.eventTime
                )
            }
            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) {
                    NativeBridge.nativeOnTouchEvent(
                        activityId, MotionEvent.ACTION_UP, event.getPointerId(i),
                        event.getX(i), event.getY(i), event.eventTime
                    )
                }
            }
        }
        return true
    }

    companion object {
        private const val TAG = "tawc"
    }
}
