package me.phie.tawc.compositor

import android.app.Activity
import android.app.ActivityManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.IBinder
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import android.widget.FrameLayout
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.Settings
import java.io.File

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
    private lateinit var rootView: FrameLayout
    private lateinit var surfaceView: SurfaceView
    /** Set in onCreate from intent.data. Always non-null at runtime —
     *  the only path that creates this Activity is the spawnActivity
     *  reverse-JNI call, which always sets a `tawc://activity/<id>` URI. */
    private lateinit var activityId: String
    /** False until onCreate finished its full setup — guards onDestroy
     *  cleanup against the early-return path when intent.data is missing. */
    private var initialized = false
    private var compositorFullscreen = false
    private val metadataScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val taskIconCache = HashMap<String, Bitmap?>()
    private var taskMetadataVersion = 0

    private var compositorService: CompositorService? = null
    private var backCallback: Any? = null

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            compositorService = (binder as CompositorService.LocalBinder).getService()
            compositorService?.registerActivity(activityId, this@CompositorActivity)
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
        if (NativeBridge.consumePendingFinishActivity(activityId)) {
            finishAndRemoveTask()
            return
        }

        // Ensure and bind the CompositorService. The Service owns the
        // compositor thread (and runs xkb-data extraction) and survives
        // this Activity's lifetime.
        val serviceIntent = Intent(this, CompositorService::class.java)
        CompositorService.ensureRunning(this)
        bindService(serviceIntent, serviceConnection, Context.BIND_AUTO_CREATE)

        surfaceView = TawcSurfaceView(this, activityId)
        rootView = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surfaceView, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
                val bars = surfaceInsets(insets)
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                insets
            }
        }
        setContentView(rootView)
        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        surfaceView.requestFocus()
        surfaceView.setOnTouchListener { _, event -> dispatchTouchToCompositor(event) }
        applyCompositorFullscreen(NativeBridge.fullscreenForActivity(activityId))
        registerBackCallback()

        initialized = true
    }

    override fun onDestroy() {
        if (initialized) {
            unregisterBackCallback()
            metadataScope.cancel()
            if (NativeBridge.activeInputConnection?.targetsView(surfaceView) == true) {
                NativeBridge.activeInputConnection = null
            }
            NativeBridge.clearActivityImeState(activityId)
            NativeBridge.nativeOnActivityDestroyed(activityId)
            compositorService?.unregisterActivity(activityId)
            compositorService?.removeWindow(activityId)
            try {
                unbindService(serviceConnection)
            } catch (e: IllegalArgumentException) {
                // Service was never successfully bound — safe to ignore.
            }
        }
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    @Deprecated("Deprecated in Android; kept for pre-OnBackInvoked dispatch.")
    override fun onBackPressed() {
        if (initialized) {
            NativeBridge.nativeOnBackPressed(activityId)
        } else {
            super.onBackPressed()
        }
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
        if (hasFocus) {
            surfaceView.requestFocus()
            applyCompositorFullscreen(compositorFullscreen)
        }
        compositorService?.setWindowFocused(activityId, hasFocus)
        NativeBridge.nativeOnActivityFocusChanged(activityId, hasFocus)
        if (hasFocus) {
            NativeBridge.replayPendingKeyboardForActivity(activityId, this)
        }
    }

    internal fun focusedInputConnectionForDev(): TawcInputConnection? {
        val ic = NativeBridge.activeInputConnection ?: return null
        return ic.takeIf { it.targetsView(surfaceView) }
    }

    internal fun activityIdForDev(): String = activityId

    internal fun updateEditableTextFromCompositor(text: String, selStart: Int, selEnd: Int) {
        val ic = NativeBridge.activeInputConnection ?: return
        if (ic.targetsView(surfaceView)) {
            ic.updateFromCompositor(text, selStart, selEnd)
        }
    }

    internal fun showKeyboardFromCompositor() {
        if (!initialized) return
        surfaceView.requestFocus()
        if (!hasWindowFocus()) return
        NativeBridge.imeOutput.showSoftInput(surfaceView)
    }

    internal fun hideKeyboardFromCompositor() {
        if (!initialized) return
        NativeBridge.imeOutput.hideSoftInput(surfaceView)
    }

    internal fun restartInputFromCompositor() {
        if (!initialized) return
        NativeBridge.imeOutput.restartInput(surfaceView)
    }

    internal fun dispatchHardwareKeyForDev(
        keycode: Int,
        pressed: Boolean,
        repeatCount: Int = 0,
    ): Boolean {
        val action = if (pressed) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP
        val now = SystemClock.uptimeMillis()
        val event = KeyEvent(now, now, action, keycode, repeatCount.coerceAtLeast(0), 0)
        return dispatchKeyEvent(event)
    }

    fun setFullscreenFromCompositor(fullscreen: Boolean) {
        if (!initialized) {
            compositorFullscreen = fullscreen
            return
        }
        applyCompositorFullscreen(fullscreen)
    }

    fun setTaskMetadata(window: OpenWindow) {
        if (!initialized || window.activityId != activityId) return
        val label = window.title.ifBlank {
            window.desktopName.ifBlank {
                window.appId.ifBlank { getString(me.phie.tawc.R.string.app_name) }
            }
        }
        val iconPath = window.iconPath
        val version = ++taskMetadataVersion
        metadataScope.launch {
            val icon = if (iconPath.isBlank()) {
                null
            } else if (taskIconCache.containsKey(iconPath)) {
                taskIconCache[iconPath]
            } else {
                withContext(Dispatchers.IO) { decodeTaskIcon(iconPath, taskIconSizePx()) }
                    .also { taskIconCache[iconPath] = it }
            }
            if (!initialized || version != taskMetadataVersion || isFinishing || isDestroyed) {
                return@launch
            }
            @Suppress("DEPRECATION")
            setTaskDescription(ActivityManager.TaskDescription(label, icon))
        }
    }

    private fun applyCompositorFullscreen(fullscreen: Boolean) {
        compositorFullscreen = fullscreen
        if (fullscreen) {
            window.addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)
        }

        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        if (fullscreen) {
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars())
        }
        if (::rootView.isInitialized) ViewCompat.requestApplyInsets(rootView)
    }

    private fun surfaceInsets(insets: WindowInsetsCompat): Insets {
        val system = if (compositorFullscreen) {
            Insets.NONE
        } else {
            insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        }
        val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
        return Insets.of(
            maxOf(system.left, ime.left),
            maxOf(system.top, ime.top),
            maxOf(system.right, ime.right),
            maxOf(system.bottom, ime.bottom),
        )
    }

    private fun taskIconSizePx(): Int = (TASK_ICON_SIZE_DP * resources.displayMetrics.density).toInt()

    private fun registerBackCallback() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        registerBackCallbackApi33()
    }

    private fun unregisterBackCallback() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        unregisterBackCallbackApi33()
    }

    private fun registerBackCallbackApi33() {
        val callback = OnBackInvokedCallback {
            if (initialized) NativeBridge.nativeOnBackPressed(activityId)
        }
        backCallback = callback
        onBackInvokedDispatcher.registerOnBackInvokedCallback(
            OnBackInvokedDispatcher.PRIORITY_DEFAULT,
            callback,
        )
    }

    private fun unregisterBackCallbackApi33() {
        val callback = backCallback as? OnBackInvokedCallback ?: return
        onBackInvokedDispatcher.unregisterOnBackInvokedCallback(callback)
        backCallback = null
    }

    /**
     * Custom SurfaceView that provides our InputConnection to the IME.
     * This makes the view act as a text input target for Gboard.
     */
    private class TawcSurfaceView(
        context: Context,
        private val activityId: String,
    ) : SurfaceView(context) {
        override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
            if (dispatchHardwareKeyToCompositor(event)) {
                return true
            }
            return super.onKeyDown(keyCode, event)
        }

        override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
            if (dispatchHardwareKeyToCompositor(event)) {
                return true
            }
            return super.onKeyUp(keyCode, event)
        }

        override fun onCheckIsTextEditor(): Boolean = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            val (inputType, extraFlags) = NativeBridge.imeEditorInfoForActivity(activityId)
            outAttrs.inputType = inputType
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
                EditorInfo.IME_ACTION_NONE or extraFlags
            return TawcInputConnection(this)
        }

        private fun dispatchHardwareKeyToCompositor(event: KeyEvent): Boolean {
            val pressed = when (event.action) {
                KeyEvent.ACTION_DOWN -> true
                KeyEvent.ACTION_UP -> false
                else -> return false
            }
            return NativeBridge.nativeOnHardwareKeyEvent(
                activityId,
                event.keyCode,
                pressed,
                event.repeatCount,
            )
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
    fun injectTouchSequenceForDev(kind: String, logicalX: Float? = null, logicalY: Float? = null): String? {
        val width = surfaceView.width.toFloat()
        val height = surfaceView.height.toFloat()
        if (width <= 0f || height <= 0f) {
            return "surfaceView has no size yet (${surfaceView.width}x${surfaceView.height})"
        }

        val downTime = SystemClock.uptimeMillis()
        var eventTime = downTime

        fun point(xFrac: Float, yFrac: Float): Pair<Float, Float> =
            (xFrac * width) to (yFrac * height)

        fun logicalPoint(): Pair<Float, Float> {
            val x = logicalX ?: return point(0.30f, 0.35f)
            val y = logicalY ?: return point(0.30f, 0.35f)
            return (x * Settings.outputScale) to (y * Settings.outputScale)
        }

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
            "tap-logical" -> {
                if (logicalX == null || logicalY == null) {
                    return "tap-logical requires x and y"
                }
                val p = logicalPoint()
                send(MotionEvent.ACTION_DOWN, 0, intArrayOf(0), arrayOf(p))
                send(MotionEvent.ACTION_UP, 0, intArrayOf(0), arrayOf(p))
            }
            "tap-outside-popup" -> {
                val p = point(0.80f, 0.80f)
                send(MotionEvent.ACTION_DOWN, 0, intArrayOf(0), arrayOf(p))
                send(MotionEvent.ACTION_UP, 0, intArrayOf(0), arrayOf(p))
            }
            "tap-menu-a" -> {
                val p = point(0.30f, 0.10f)
                send(MotionEvent.ACTION_DOWN, 0, intArrayOf(0), arrayOf(p))
                send(MotionEvent.ACTION_UP, 0, intArrayOf(0), arrayOf(p))
            }
            "tap-menu-b" -> {
                val p = point(0.65f, 0.10f)
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
        private const val TASK_ICON_SIZE_DP = 96

        private fun decodeTaskIcon(path: String, targetPx: Int): Bitmap? {
            val f = File(path)
            if (!f.isFile) return null
            return runCatching {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(path, bounds)
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@runCatching null
                var sample = 1
                val shorter = minOf(bounds.outWidth, bounds.outHeight)
                while (shorter / (sample * 2) >= targetPx) sample *= 2
                BitmapFactory.decodeFile(
                    path,
                    BitmapFactory.Options().apply { inSampleSize = sample },
                )
            }.getOrNull()
        }
    }
}
