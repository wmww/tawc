package me.phie.tawc.compositor

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.SystemClock
import android.os.IBinder
import android.util.Log
import android.view.InputDevice
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

        initialized = true
    }

    override fun onDestroy() {
        if (initialized) {
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

    /**
     * Debug-broker hook used by integration tests that need deterministic
     * multi-touch. Events are dispatched through the SurfaceView, so this
     * still exercises the Activity's MotionEvent decoding before JNI.
     */
    fun injectTouchSequenceForDev(kind: String): String? {
        val width = surfaceView.width.toFloat()
        val height = surfaceView.height.toFloat()
        if (width <= 0f || height <= 0f) {
            return "surfaceView has no size yet (${surfaceView.width}x${surfaceView.height})"
        }

        val downTime = SystemClock.uptimeMillis()
        var eventTime = downTime

        fun point(xFrac: Float, yFrac: Float): Pair<Float, Float> =
            (xFrac * width) to (yFrac * height)

        fun send(
            actionMasked: Int,
            actionIndex: Int,
            ids: IntArray,
            points: Array<Pair<Float, Float>>,
        ) {
            eventTime += 16
            val props = Array(ids.size) { i ->
                MotionEvent.PointerProperties().apply {
                    id = ids[i]
                    toolType = MotionEvent.TOOL_TYPE_FINGER
                }
            }
            val coords = Array(ids.size) { i ->
                MotionEvent.PointerCoords().apply {
                    x = points[i].first
                    y = points[i].second
                    pressure = 1f
                    size = 0.08f
                }
            }
            val action = if (
                actionMasked == MotionEvent.ACTION_POINTER_DOWN ||
                actionMasked == MotionEvent.ACTION_POINTER_UP
            ) {
                actionMasked or (actionIndex shl MotionEvent.ACTION_POINTER_INDEX_SHIFT)
            } else {
                actionMasked
            }
            val event = MotionEvent.obtain(
                downTime,
                eventTime,
                action,
                ids.size,
                props,
                coords,
                0,
                0,
                1f,
                1f,
                0,
                0,
                InputDevice.SOURCE_TOUCHSCREEN,
                0,
            )
            try {
                surfaceView.dispatchTouchEvent(event)
            } finally {
                event.recycle()
            }
        }

        fun lerp(a: Float, b: Float, i: Int, steps: Int): Float =
            a + (b - a) * (i.toFloat() / steps.toFloat())

        when (kind) {
            "tap" -> {
                val p = point(0.30f, 0.35f)
                send(MotionEvent.ACTION_DOWN, 0, intArrayOf(0), arrayOf(p))
                send(MotionEvent.ACTION_UP, 0, intArrayOf(0), arrayOf(p))
            }
            "drag" -> {
                val ids = intArrayOf(0)
                send(MotionEvent.ACTION_DOWN, 0, ids, arrayOf(point(0.25f, 0.35f)))
                for (i in 1..6) {
                    send(
                        MotionEvent.ACTION_MOVE,
                        0,
                        ids,
                        arrayOf(point(lerp(0.25f, 0.70f, i, 6), lerp(0.35f, 0.60f, i, 6))),
                    )
                }
                send(MotionEvent.ACTION_UP, 0, ids, arrayOf(point(0.70f, 0.60f)))
            }
            "multitouch" -> {
                send(MotionEvent.ACTION_DOWN, 0, intArrayOf(0), arrayOf(point(0.25f, 0.35f)))
                send(
                    MotionEvent.ACTION_POINTER_DOWN,
                    1,
                    intArrayOf(0, 1),
                    arrayOf(point(0.25f, 0.35f), point(0.75f, 0.35f)),
                )
                for (i in 1..6) {
                    send(
                        MotionEvent.ACTION_MOVE,
                        0,
                        intArrayOf(0, 1),
                        arrayOf(
                            point(lerp(0.25f, 0.35f, i, 6), lerp(0.35f, 0.55f, i, 6)),
                            point(lerp(0.75f, 0.65f, i, 6), lerp(0.35f, 0.55f, i, 6)),
                        ),
                    )
                }
                send(
                    MotionEvent.ACTION_POINTER_UP,
                    1,
                    intArrayOf(0, 1),
                    arrayOf(point(0.35f, 0.55f), point(0.65f, 0.55f)),
                )
                send(MotionEvent.ACTION_UP, 0, intArrayOf(0), arrayOf(point(0.35f, 0.55f)))
            }
            else -> return "unknown touch sequence '$kind'"
        }

        return null
    }

    companion object {
        private const val TAG = "tawc"
    }
}
