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
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.Display
import android.view.Surface
import android.os.IBinder
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PointerIcon
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
import me.phie.tawc.RefreshRate
import me.phie.tawc.Settings
import me.phie.tawc.displayRefreshRatesMhz
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

    /** Last mouse button mask seen from a mouse-source MotionEvent. Android
     *  reports a bitmask rather than per-button actions, so press/release
     *  events come from diffing this. */
    private var mouseButtonState = 0

    /** Ends a touchpad scroll gesture with an `axis_stop` frame. */
    private val scrollStopRunnable = Runnable {
        NativeBridge.nativeOnPointerEvent(
            activityId, POINTER_KIND_AXIS, 0f, 0f,
            0, false, 0f, 0f, true, true, SystemClock.uptimeMillis(),
        )
    }
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

        surfaceView = TawcSurfaceView(this)
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
            surfaceView.removeCallbacks(scrollStopRunnable)
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
        // The surface exists now, so the frame-rate request can land too.
        applyRefreshRatePreference()
    }

    /**
     * Ask Android for the configured refresh rate.
     *
     * A high-refresh panel only helps when the window asks for it: without
     * a preference the system is free to sit at its idle rate (60 Hz on
     * this tablet) even though the panel can do 120, and
     * `setFrameRate(FIXED_SOURCE)` is what holds the rate instead of
     * letting it drop while nothing animates. The compositor advertises the
     * same number to clients (`nativeSetOutputRefreshRate` in
     * `CompositorService`), so both halves come from one `Settings` read.
     */
    private fun applyRefreshRatePreference() {
        val effective = RefreshRate.effectiveMhz(
            displayRefreshRatesMhz(this),
            Settings.refreshRateCapMhz,
        ) ?: return
        val hz = effective / 1000f
        val attributes = window.attributes
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            attributes.preferredRefreshRate = hz
        } else {
            // API 29 has no per-window rate preference; name the mode.
            getSystemService(DisplayManager::class.java)
                ?.getDisplay(Display.DEFAULT_DISPLAY)
                ?.supportedModes
                ?.minByOrNull { kotlin.math.abs(it.refreshRate - hz) }
                ?.let { attributes.preferredDisplayModeId = it.modeId }
        }
        window.attributes = attributes
        // Advertising happens from here, not from CompositorService: since
        // Android 12 a non-visual Context gets `null` from
        // `DisplayManager.getDisplay()`, so the service cannot read the
        // panel's modes at all (it silently produced an empty list and the
        // output stayed at the 60 Hz default). The window may briefly
        // advertise the default before this runs; clients handle the
        // `wl_output.mode` update that follows.
        // Persist it too: the compositor's *first* advertised mode must be
        // right, and only this Activity can read the panel (see the note in
        // CompositorService). A client that samples once at startup — as
        // Firefox does for its refresh-driver target — otherwise keeps
        // pacing at the 60 Hz default even after the mode is corrected.
        Settings.outputRefreshMhz = effective
        NativeBridge.nativeSetOutputRefreshRate(effective)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // The holder's Surface can already be gone — `onResume` runs
            // after a torn-down window too — and `setFrameRate` throws on a
            // released Surface, which crashed the Activity on resume and
            // took the whole window (and the app's task) with it. `isValid`
            // is a race against the surface thread, so the catch stays.
            val surface = surfaceView.holder.surface
            if (surface?.isValid == true) {
                try {
                    surface.setFrameRate(hz, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                } catch (e: IllegalStateException) {
                    Log.w(TAG, "surface released before setFrameRate($hz)", e)
                } catch (e: IllegalArgumentException) {
                    Log.w(TAG, "setFrameRate($hz) rejected by this display", e)
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Settings may have changed the cap while this Activity was paused.
        applyRefreshRatePreference()
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
            // Copies made in other apps don't fire the clip-changed
            // listener while we're backgrounded (Android 10+); catch up.
            ClipboardBridge.syncOnWindowFocusGained()
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

    /**
     * Dev-broker Back dispatch: same entry the system OnBackInvoked
     * callback (API 33+) and legacy [onBackPressed] route into.
     */
    internal fun dispatchBackForDev(): Boolean {
        if (!initialized) return false
        NativeBridge.nativeOnBackPressed(activityId)
        return true
    }

    internal fun activityIdForDev(): String = activityId

    internal fun updateEditableTextFromCompositor(
        text: String,
        selStart: Int,
        selEnd: Int,
        authoritative: Boolean,
    ) {
        val ic = NativeBridge.activeInputConnection ?: return
        if (ic.targetsView(surfaceView)) {
            ic.updateFromCompositor(text, selStart, selEnd, authoritative)
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
    private inner class TawcSurfaceView(context: Context) : SurfaceView(context) {
        override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
            if (swallowMouseButtonKey(event)) {
                return true
            }
            if (dispatchHardwareKeyToCompositor(event)) {
                return true
            }
            return super.onKeyDown(keyCode, event)
        }

        override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
            if (swallowMouseButtonKey(event)) {
                return true
            }
            if (dispatchHardwareKeyToCompositor(event)) {
                return true
            }
            return super.onKeyUp(keyCode, event)
        }

        /** Android raises `KEYCODE_BACK`/`KEYCODE_FORWARD` alongside the
         *  mouse side buttons. Those already went out as `BTN_SIDE`/
         *  `BTN_EXTRA`, so consume the key here — otherwise a mouse Back
         *  click would also run the Android Back policy (dismiss popup /
         *  leave fullscreen / send Escape). */
        private fun swallowMouseButtonKey(event: KeyEvent): Boolean =
            event.isFromSource(InputDevice.SOURCE_MOUSE) &&
                (event.keyCode == KeyEvent.KEYCODE_BACK ||
                    event.keyCode == KeyEvent.KEYCODE_FORWARD)

        /** Wheel (`ACTION_SCROLL`) and mouse button press/release actions
         *  arrive here rather than through the touch listener. */
        override fun onGenericMotionEvent(event: MotionEvent): Boolean {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE) &&
                dispatchPointerToCompositor(event)
            ) {
                return true
            }
            return super.onGenericMotionEvent(event)
        }

        /** Android routes hover actions here first and only falls through to
         *  [onGenericMotionEvent] if this returns false, so hook it
         *  explicitly rather than relying on the fall-through. */
        override fun onHoverEvent(event: MotionEvent): Boolean {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE) &&
                dispatchPointerToCompositor(event)
            ) {
                return true
            }
            return super.onHoverEvent(event)
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
        // Source split. A mouse click reaches the touch listener as
        // ACTION_DOWN/MOVE/UP, so without this a single click would deliver
        // both a wl_touch.down and a wl_pointer.button and clients would
        // double-handle it. Stylus deliberately stays on touch — a real
        // zwp_tablet_v2 path is out of scope. Anything else (rotary
        // encoders, gamepads) is not ours.
        if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
            return dispatchPointerToCompositor(event)
        }
        if (!event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN) &&
            !event.isFromSource(InputDevice.SOURCE_STYLUS)
        ) {
            return false
        }
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

    /** Apply a cursor the compositor derived from the client's cursor
     *  request. Android draws the sprite; TAWC never renders one itself. */
    fun setPointerIconFromCompositor(icon: PointerIcon) {
        if (!initialized) return
        surfaceView.pointerIcon = icon
    }

    /**
     * Translate one mouse-source [MotionEvent] into `wl_pointer` events.
     *
     * Motion and button state both arrive on the same events: moves with a
     * button held come through the touch listener as ACTION_MOVE, moves
     * without one come through [TawcSurfaceView.onHoverEvent]. Android
     * reports buttons as a bitmask rather than per-button actions, so
     * presses and releases come from diffing that mask —
     * ACTION_BUTTON_PRESS/RELEASE are ignored because the diff already
     * covers them.
     *
     * ACTION_HOVER_EXIT is deliberately dropped, not mapped to
     * `wl_pointer.leave`: Android synthesizes one before every mouse
     * ACTION_DOWN (and a HOVER_ENTER after the matching UP), and wrapping
     * every click in leave/enter closes GTK menus and breaks drags. Real
     * leaves come from the compositor on focus loss / surface destroy.
     */
    private fun dispatchPointerToCompositor(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                sendPointerMotion(event)
                // A mouse ACTION_DOWN unambiguously means a button went
                // down, but not every producer fills in the mask: Android's
                // own `input mouse motionevent DOWN` injector leaves
                // buttonState at 0. Real hardware always sets it; assume the
                // primary button when it doesn't, so a click is never
                // silently dropped.
                syncMouseButtons(
                    event,
                    event.buttonState.takeIf { it != 0 } ?: MotionEvent.BUTTON_PRIMARY,
                )
            }
            MotionEvent.ACTION_MOVE,
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_HOVER_ENTER,
            MotionEvent.ACTION_HOVER_MOVE -> {
                sendPointerMotion(event)
                syncMouseButtons(event, event.buttonState)
            }
            MotionEvent.ACTION_HOVER_EXIT -> Unit
            MotionEvent.ACTION_CANCEL -> syncMouseButtons(event, 0)
            MotionEvent.ACTION_SCROLL -> {
                sendPointerMotion(event)
                sendPointerScroll(event)
            }
            else -> return false
        }
        return true
    }

    private fun sendPointerMotion(event: MotionEvent) {
        NativeBridge.nativeOnPointerEvent(
            activityId, POINTER_KIND_MOTION, event.x, event.y,
            0, false, 0f, 0f, false, false, event.eventTime,
        )
    }

    /** Emit one button event per bit that changed since the last event. */
    private fun syncMouseButtons(event: MotionEvent, rawMask: Int) {
        val mask = rawMask and MOUSE_BUTTON_MASK
        var changed = mask xor mouseButtonState
        if (changed == 0) return
        mouseButtonState = mask
        while (changed != 0) {
            val bit = changed and -changed
            changed = changed and bit.inv()
            NativeBridge.nativeOnPointerEvent(
                activityId, POINTER_KIND_BUTTON, event.x, event.y,
                bit, (mask and bit) != 0, 0f, 0f, false, false, event.eventTime,
            )
        }
    }

    private fun sendPointerScroll(event: MotionEvent) {
        val vscroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
        val hscroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
        if (vscroll == 0f && hscroll == 0f) return
        // A touchpad scroll is a finger gesture, not wheel detents. GTK only
        // settles its kinetic scrolling on the axis_stop that ends one, and
        // Android has no gesture-end event, so time it out.
        val fromTouchpad = event.device?.supportsSource(InputDevice.SOURCE_TOUCHPAD) == true
        NativeBridge.nativeOnPointerEvent(
            activityId, POINTER_KIND_AXIS, event.x, event.y,
            0, false, vscroll, hscroll, fromTouchpad, false, event.eventTime,
        )
        surfaceView.removeCallbacks(scrollStopRunnable)
        if (fromTouchpad) {
            surfaceView.postDelayed(scrollStopRunnable, SCROLL_STOP_DELAY_MS)
        }
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

    /**
     * Debug-broker mouse injection (`inject-pointer`). Builds real
     * `SOURCE_MOUSE` MotionEvents and dispatches them through the
     * SurfaceView, so the Activity's own decoding — the source split, the
     * button-mask diff, the hover rules — is what the test exercises.
     *
     * `kind`:
     *  - `move` — one hover move
     *  - `button` — Android's full click sequence for `button` (default
     *    `primary`): HOVER_EXIT, DOWN, BUTTON_PRESS, BUTTON_RELEASE, UP,
     *    HOVER_ENTER. The hover events are part of the shape on purpose:
     *    they are what a click must *not* turn into a leave/enter pair.
     *  - `scroll` / `hscroll` — one ACTION_SCROLL of `amount` detents
     *  - `hover-exit` — a bare HOVER_EXIT
     *
     * Coordinates are Wayland logical, like `inject-touch`'s `tap-logical`.
     */
    fun injectPointerSequenceForDev(
        kind: String,
        logicalX: Float? = null,
        logicalY: Float? = null,
        button: String? = null,
        amount: Float = 1f,
    ): String? {
        val width = surfaceView.width.toFloat()
        val height = surfaceView.height.toFloat()
        if (width <= 0f || height <= 0f) {
            return "surfaceView has no size yet (${surfaceView.width}x${surfaceView.height})"
        }

        val x = logicalX?.let { it * Settings.outputScale } ?: (0.30f * width)
        val y = logicalY?.let { it * Settings.outputScale } ?: (0.35f * height)

        val buttonBit = when (button ?: "primary") {
            "primary" -> MotionEvent.BUTTON_PRIMARY
            "secondary" -> MotionEvent.BUTTON_SECONDARY
            "tertiary" -> MotionEvent.BUTTON_TERTIARY
            "back" -> MotionEvent.BUTTON_BACK
            "forward" -> MotionEvent.BUTTON_FORWARD
            else -> return "unknown button '$button'"
        }

        val downTime = SystemClock.uptimeMillis()
        var eventTime = downTime

        fun send(action: Int, buttonState: Int, vscroll: Float, hscroll: Float) {
            eventTime += 16
            val props = arrayOf(
                MotionEvent.PointerProperties().apply {
                    id = 0
                    toolType = MotionEvent.TOOL_TYPE_MOUSE
                },
            )
            val coords = arrayOf(
                MotionEvent.PointerCoords().apply {
                    this.x = x
                    this.y = y
                    if (vscroll != 0f) setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll)
                    if (hscroll != 0f) setAxisValue(MotionEvent.AXIS_HSCROLL, hscroll)
                },
            )
            val event = MotionEvent.obtain(
                downTime, eventTime, action, 1, props, coords,
                0, buttonState, 1f, 1f, 0, 0,
                InputDevice.SOURCE_MOUSE, 0,
            )
            try {
                // Android splits pointer dispatch the same way: DOWN/MOVE/UP
                // go to the touch path, everything else (hover, scroll,
                // button press/release) to generic motion.
                when (action) {
                    MotionEvent.ACTION_DOWN,
                    MotionEvent.ACTION_MOVE,
                    MotionEvent.ACTION_UP,
                    MotionEvent.ACTION_CANCEL -> surfaceView.dispatchTouchEvent(event)
                    else -> surfaceView.dispatchGenericMotionEvent(event)
                }
            } finally {
                event.recycle()
            }
        }

        when (kind) {
            "move" -> send(MotionEvent.ACTION_HOVER_MOVE, 0, 0f, 0f)
            "hover-exit" -> send(MotionEvent.ACTION_HOVER_EXIT, 0, 0f, 0f)
            "button" -> {
                send(MotionEvent.ACTION_HOVER_EXIT, 0, 0f, 0f)
                send(MotionEvent.ACTION_DOWN, buttonBit, 0f, 0f)
                send(MotionEvent.ACTION_BUTTON_PRESS, buttonBit, 0f, 0f)
                send(MotionEvent.ACTION_BUTTON_RELEASE, 0, 0f, 0f)
                send(MotionEvent.ACTION_UP, 0, 0f, 0f)
                send(MotionEvent.ACTION_HOVER_ENTER, 0, 0f, 0f)
            }
            "scroll" -> send(MotionEvent.ACTION_SCROLL, 0, amount, 0f)
            "hscroll" -> send(MotionEvent.ACTION_SCROLL, 0, 0f, amount)
            else -> return "unknown pointer sequence '$kind'"
        }

        return null
    }

    companion object {
        private const val TAG = "tawc"
        private const val TASK_ICON_SIZE_DP = 96

        // NativeBridge.nativeOnPointerEvent `kind` values.
        private const val POINTER_KIND_MOTION = 0
        private const val POINTER_KIND_BUTTON = 1
        private const val POINTER_KIND_AXIS = 2

        /** The five mouse buttons the compositor maps to evdev `BTN_*`. */
        private const val MOUSE_BUTTON_MASK =
            MotionEvent.BUTTON_PRIMARY or MotionEvent.BUTTON_SECONDARY or
                MotionEvent.BUTTON_TERTIARY or MotionEvent.BUTTON_BACK or
                MotionEvent.BUTTON_FORWARD

        /** Idle gap after which a touchpad scroll gesture is considered over. */
        private const val SCROLL_STOP_DELAY_MS = 120L

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
