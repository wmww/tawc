package me.phie.tawc.compositor

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.PointerIcon
import android.view.Surface
import android.view.inputmethod.EditorInfo
import androidx.core.net.toUri
import me.phie.tawc.terminal.TerminalPane
import java.lang.ref.WeakReference

object NativeBridge {
    private const val TAG = "tawc"
    private val mainHandler = Handler(Looper.getMainLooper())

    /** Sticky keyboard visibility by Activity. A show can arrive before the
     *  corresponding CompositorActivity has registered with the Service; the
     *  Service replays only that Activity's pending show on registration. */
    private val pendingKeyboardShownByActivity = mutableMapOf<String, Boolean>()

    /** The currently active TawcInputConnection. Set by TawcInputConnection's
     *  init, cleared by closeConnection. The Wayland-to-Android Editable sync
     *  pushes through this; the dev IC actions (`ic-commit-text`, …) also
     *  reuse it so multi-step IME flows (compose → finish → commit) share
     *  Editable state. */
    private var activeICRef: WeakReference<TawcInputConnection>? = null

    var activeInputConnection: TawcInputConnection?
        get() = activeICRef?.get()
        set(value) { activeICRef = value?.let { WeakReference(it) } }

    /** Outbound calls to the system IME (showSoftInput, updateSelection,
     *  …) go through this. Default is the production [RealImeOutput];
     *  the dev `test-init` broker action swaps in a `RecordingImeOutput`
     *  so input integration tests don't race the system IME's reaction
     *  to `updateSelection`. See [ImeOutput] kdoc. */
    @Volatile var imeOutput: ImeOutput = RealImeOutput

    /** `(EditorInfo.inputType, extraImeOptionsFlags)` by Activity for the focused
     *  Wayland text-input field. Driven by the Wayland client via
     *  text-input-v3's `set_content_type`, pushed up by
     *  [onContentTypeChanged], read by `onCreateInputConnection` on the
     *  next IC build. Guarded by a small lock because reverse-JNI writes and
     *  Activity-side IC creation can happen on different threads. */
    private val imeEditorInfoByActivity = mutableMapOf<String, Pair<Int, Int>>()

    /** Application context, captured by CompositorService.onCreate so the
     *  reverse-JNI `spawnActivity` callback can `startActivity(...)` even
     *  when no Activity is currently in the foreground. */
    private var appContext: Context? = null

    /** Weak ref to the running CompositorService for finishActivity lookups. */
    private var serviceRef: WeakReference<CompositorService>? = null
    private val fullscreenByActivity = mutableMapOf<String, Boolean>()
    private val pendingFinishActivities = mutableSetOf<String>()

    /** Process start ([CompositorService.ensureActivation]): reverse-JNI
     *  needs a context before any service exists. */
    fun attachContext(context: Context) {
        appContext = context.applicationContext
    }

    fun attachService(service: CompositorService) {
        appContext = service.applicationContext
        serviceRef = WeakReference(service)
        ClipboardBridge.attach(service.applicationContext)
    }

    fun detachService() {
        ClipboardBridge.detach()
        serviceRef = null
        synchronized(pendingKeyboardShownByActivity) {
            pendingKeyboardShownByActivity.clear()
        }
        synchronized(imeEditorInfoByActivity) {
            imeEditorInfoByActivity.clear()
        }
    }

    /**
     * Look up the live [CompositorService], if any. Public-but-internal
     * for the dev broker action handlers (see `me.phie.tawc.dev.InputActions`,
     * debug builds only) — no production code should reach
     * for this; use [attachService]/[detachService] instead.
     */
    fun serviceRefForDev(): CompositorService? = serviceRef?.get()

    fun consumePendingFinishActivity(activityId: String): Boolean =
        synchronized(pendingFinishActivities) {
            pendingFinishActivities.remove(activityId)
        }

    fun imeEditorInfoForActivity(activityId: String): Pair<Int, Int> =
        synchronized(imeEditorInfoByActivity) {
            imeEditorInfoByActivity[activityId] ?: (EditorInfo.TYPE_CLASS_TEXT to 0)
        }

    fun replayPendingKeyboardForActivity(activityId: String, activity: CompositorActivity) {
        val shouldShow = synchronized(pendingKeyboardShownByActivity) {
            pendingKeyboardShownByActivity[activityId] == true
        }
        if (shouldShow) {
            activity.showKeyboardFromCompositor()
        }
    }

    fun clearActivityImeState(activityId: String) {
        synchronized(pendingKeyboardShownByActivity) {
            pendingKeyboardShownByActivity.remove(activityId)
        }
        synchronized(imeEditorInfoByActivity) {
            imeEditorInfoByActivity.remove(activityId)
        }
    }

    init {
        System.loadLibrary("compositor")
    }

    // --- Compositor lifecycle: called from CompositorService ---

    /** Start the Rust compositor thread. Returns true if this call spawned
     *  it, false if one was already running. Waits out a previous run that
     *  is still tearing down.
     *  `displayWidthPx`/`displayHeightPx` are the Android panel metrics; they
     *  seed a provisional `wl_output` mode so clients that connect before any
     *  Activity exists still see a display. The first real output size comes
     *  from [nativeRegisterActivitySurface]. */
    external fun nativeStartCompositor(
        outputScale: Float,
        displayWidthPx: Int,
        displayHeightPx: Int,
        xwayland: Boolean,
        gtk3BrokenMenusWorkaround: Boolean,
    ): Boolean

    /** Stop the Rust compositor thread and wait until it is gone. The
     *  thread then reports through [onCompositorStopped]. */
    external fun nativeStopCompositor()

    /** True while a compositor thread exists, including one tearing down. */
    external fun nativeIsCompositorRunning(): Boolean

    /** Bind the process-lifetime Wayland/X11 sockets and watch them for
     *  the first connection, which calls [onActivationRequested].
     *  Idempotent. Needs the `TAWC_*` env. */
    external fun nativeStartActivation(xwayland: Boolean)

    /** Debug builds (`compositor-hold`): keep a clientless compositor up. */
    external fun nativeSetCompositorHold(hold: Boolean)

    /** Reconcile the per-distro ando broker listeners to exactly the
     *  enabled set (run Android commands from rootfs guests — see
     *  notes/ando.md). `ids[i]` is an install id and `paths[i]` its
     *  host-side socket path; the arrays are parallel. Driven by
     *  [me.phie.tawc.AndoBrokers.refresh] at startup and on every ando
     *  install/uninstall/toggle. Idempotent. */
    external fun nativeSyncAndoBrokers(ids: Array<String>, paths: Array<String>)

    // --- Per-Activity surface lifecycle: called from CompositorActivity ---

    /** Register an Activity's `SurfaceView.Surface` with the compositor.
     *  The compositor takes ownership of an `ANativeWindow` derived from
     *  the Surface and creates an `EGLSurface` bound to it. */
    external fun nativeRegisterActivitySurface(activityId: String, surface: Surface, width: Int, height: Int)

    /** Notify the compositor that an Activity's Surface dimensions changed. */
    external fun nativeOnActivitySurfaceChanged(activityId: String, width: Int, height: Int)

    /** Notify the compositor that an Activity's Surface was destroyed.
     *  The host record is retained — the Activity may rebind on resume. */
    external fun nativeOnActivitySurfaceDestroyed(activityId: String)

    /** Notify the compositor that an Activity is being destroyed.
     *  In multi-window mode this drops the host and any toplevels assigned
     *  to it. For phase 0/1 (single Activity) it just removes the host. */
    external fun nativeOnActivityDestroyed(activityId: String)

    /** Forward a touch event from a specific Activity's SurfaceView to
     *  the compositor. The activityId tags the event so the compositor
     *  can route it to the right host's foreground toplevel. */
    external fun nativeOnTouchEvent(activityId: String, action: Int, pointerId: Int, x: Float, y: Float, eventTime: Long)

    /** Forward a real pointer (mouse) event from an Activity's SurfaceView.
     *  Mouse-source MotionEvents never go down [nativeOnTouchEvent]; see
     *  notes/input.md for the source split.
     *
     *  `kind` is 0 = motion, 1 = button, 2 = axis. `button` is a single
     *  `MotionEvent.BUTTON_*` bit (mapped to evdev `BTN_*` natively),
     *  `vscroll`/`hscroll` are raw `AXIS_VSCROLL`/`AXIS_HSCROLL` detents
     *  (native applies the Wayland sign and units), `fromTouchpad` picks
     *  `wl_pointer.axis_source`, and `stop` marks the end of a touchpad
     *  scroll gesture. */
    external fun nativeOnPointerEvent(
        activityId: String,
        kind: Int,
        x: Float,
        y: Float,
        button: Int,
        pressed: Boolean,
        vscroll: Float,
        hscroll: Float,
        fromTouchpad: Boolean,
        stop: Boolean,
        eventTime: Long,
    )

    /** Tell the compositor whether any mouse-class InputDevice is attached.
     *  Drives one of the two reasons the seat advertises `wl_pointer`.
     *  Pushed by [MouseWatcher]. */
    external fun nativeOnMouseAttachedChanged(attached: Boolean)

    /** Forward an Activity window-focus change. The compositor uses this
     *  to track `foreground_host`; phase 7 will use the same hook to
     *  send `Activated`/`Suspended` configures and pause frame callbacks. */
    external fun nativeOnActivityFocusChanged(activityId: String, hasFocus: Boolean)

    /** Notify the compositor that Android externally changed an Activity's
     *  fullscreen state. Most fullscreen transitions originate in native
     *  xdg-shell handling and come back through [setActivityFullscreen]. */
    external fun nativeOnActivityFullscreenChanged(activityId: String, fullscreen: Boolean)

    /** Let the compositor consume Android Back using Wayland window state. */
    external fun nativeOnBackPressed(activityId: String)

    /** Forward a hardware key event from the focused compositor view. Returns
     *  false when the key is intentionally unmapped so Android can continue
     *  normal system handling (Back, volume, media keys, etc.). */
    external fun nativeOnHardwareKeyEvent(
        activityId: String,
        keycode: Int,
        pressed: Boolean,
        repeatCount: Int,
    ): Boolean

    // --- Text input: Android InputConnection → Compositor ---
    //
    // These are the JNI primitives the production [TawcInputConnection]
    // calls into the Rust compositor with. Tests never call them
    // directly — there is intentionally no broker action that reaches
    // these — because that would bypass the IC's state machine
    // (`computeReplaceDeltas`, the Editable mirror, the
    // `composingRegionIsPreedit` short-circuit) and let wayland-side
    // fallback behaviour hide IC bugs. See `me.phie.tawc.dev.InputActions`.

    /** commitText from InputConnection. `deleteBefore`/`deleteAfter` are
     *  UTF-16 code-unit counts around the cursor that should be removed
     *  from the Wayland client's committed buffer first — non-zero only
     *  when the IME is replacing a region established by
     *  setComposingRegion (still committed text on Wayland), zero
     *  otherwise. The compositor sends this as a single atomic
     *  `delete_surrounding_text` + `commit_string` + `done` so the client
     *  doesn't fire `set_surrounding_text(cause=other)` between the two
     *  and trip our preedit-clearing logic. (Standalone
     *  deleteSurroundingText goes through [nativeSendKeyEvent] from the
     *  IC instead — see TawcInputConnection.)
     *  Production-only — only called from [TawcInputConnection.commitText]. */
    external fun nativeCommitText(text: String, deleteBefore: Int, deleteAfter: Int)

    /** setComposingText from InputConnection. Same delete semantics as
     *  [nativeCommitText]. Production-only — only called from
     *  [TawcInputConnection.setComposingText]. */
    external fun nativeSetComposingText(text: String, deleteBefore: Int, deleteAfter: Int)

    /** finishComposingText from InputConnection. Production-only — only
     *  called from [TawcInputConnection.finishComposingText]. */
    external fun nativeFinishComposingText()

    /** sendKeyEvent mapped to keycode. Production-only — only called
     *  from [TawcInputConnection.sendKeyEvent] (and [TawcInputConnection.deleteSurroundingText],
     *  which translates the delta into Backspace / Forward-Delete key events). */
    external fun nativeSendKeyEvent(keycode: Int)

    /** One half of a real key press/release pair. Used by
     *  [TawcInputConnection.sendKeyEvent] for modified shortcuts where
     *  the modifier has to stay down while the main key is delivered. */
    external fun nativeSendKeyState(keycode: Int, pressed: Boolean)

    /** Query compositor state on the compositor event loop. */
    external fun nativeQueryState(): String?

    /** Toggle the renderer's per-buffer-type tint (today: magenta SHM
     *  wash). Read live by every frame, so the change is visible on the
     *  next paint. */
    external fun nativeSetTintBuffersByType(enabled: Boolean)

    /** Update the compositor output scale live. The compositor propagates
     *  this through wl_output, fractional-scale, and xdg configure events. */
    external fun nativeSetOutputScale(scale: Float)

    /** Update the refresh rate advertised in `wl_output.mode` (mHz).
     *  Clients that pace themselves by the output — WebRender, games,
     *  `wl_surface.frame` callbacks — render at this rate, so it must
     *  match what the Activity asks Android for (`preferredRefreshRate`). */
    external fun nativeSetOutputRefreshRate(mhz: Int)

    /** Start/stop the compositor-owned Xwayland server live. */
    external fun nativeSetXwaylandEnabled(enabled: Boolean)

    /** Toggle the contained GTK3 broken menubar workaround. */
    external fun nativeSetGtk3BrokenMenusWorkaround(enabled: Boolean)

    /** Test hook: ask every attached Wayland/XWayland client window to close. */
    external fun nativeCloseAllClientsForTest(): Int

    /** Announce that Android's clipboard holds a text clip, without reading
     *  it. [timestampMs] is the ClipDescription timestamp (0 if the OEM
     *  build doesn't stamp clips); [ownWrite] marks TAWC's own
     *  Wayland→Android mirror writes. Content is fetched lazily at paste
     *  time via [fetchClipboardText]. */
    external fun nativeOnAndroidClipAvailable(timestampMs: Long, ownWrite: Boolean)

    /** Structured debug counters for clipboard integration tests. */
    external fun nativeClipboardDebugState(): String?

    /**
     * Scan a rootfs for installed `.desktop` apps. Returns a JSON array
     * string: `[{id, name, comment, exec, terminal}, …]` (sorted by name,
     * de-duplicated by id, NoDisplay/Hidden filtered out). Empty `[]` if
     * the rootfs has no apps or doesn't exist. The work is pure file I/O
     * with no compositor-state interaction, so this is safe to call from
     * any thread (AppsPane dispatches it on Dispatchers.IO).
     */
    external fun nativeLauncherScan(rootfs: String): String

    // --- Remote access (me.phie.tawc.remote.RemoteSession; notes/remote-access.md) ---

    /** Start the agent from a JSON request; null when started, else why
     *  not (one already runs, bad relay URL, …). */
    external fun nativeRemoteStart(request: String): String?

    /** Stop and join the agent, hanging up every login. Idempotent. */
    external fun nativeRemoteStop()

    /** Agent status JSON; `{"state":"stopped"}` if nothing ever ran. */
    external fun nativeRemoteStatus(): String

    // --- Reverse JNI: Compositor → Android (called from compositor thread) ---

    /** Called from native when a Wayland client enables text input. */
    @JvmStatic
    fun onShowKeyboard(activityId: String) {
        synchronized(pendingKeyboardShownByActivity) {
            pendingKeyboardShownByActivity[activityId] = true
        }
        mainHandler.post {
            serviceRef?.get()
                ?.getActivity(activityId)
                ?.showKeyboardFromCompositor()
        }
    }

    /** Called from native when a Wayland client disables text input. */
    @JvmStatic
    fun onHideKeyboard(activityId: String) {
        synchronized(pendingKeyboardShownByActivity) {
            pendingKeyboardShownByActivity[activityId] = false
        }
        mainHandler.post {
            serviceRef?.get()
                ?.getActivity(activityId)
                ?.hideKeyboardFromCompositor()
        }
    }

    /**
     * Called from native (compositor policy) to spawn a new
     * [CompositorActivity] for a freshly-assigned host. The activityId is
     * encoded into the Intent's data URI so Android's documentLaunchMode
     * treats each id as its own task.
     *
     * Phase 5 wires this up; for phase 0-4 the policy stays in
     * single-Activity mode and never calls it.
     */
    @JvmStatic
    fun spawnActivity(activityId: String) {
        mainHandler.post {
            val ctx = appContext ?: run {
                Log.e(TAG, "spawnActivity($activityId): no appContext yet")
                return@post
            }
            val intent = Intent(ctx, CompositorActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                data = "tawc://activity/$activityId".toUri()
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                        Intent.FLAG_ACTIVITY_NEW_DOCUMENT or
                        Intent.FLAG_ACTIVITY_MULTIPLE_TASK
            }
            ctx.startActivity(intent)
        }
    }

    /**
     * Called from native to finish (and remove from recents) the
     * [CompositorActivity] for an activityId. No-op if the Activity has
     * already been destroyed.
     */
    @JvmStatic
    fun finishActivity(activityId: String) {
        mainHandler.post {
            val service = serviceRef?.get()
            if (service == null) {
                synchronized(pendingFinishActivities) {
                    pendingFinishActivities.add(activityId)
                }
                return@post
            }
            service.removeWindow(activityId)
            val activity = service.getActivity(activityId)
            if (activity == null) {
                synchronized(pendingFinishActivities) {
                    pendingFinishActivities.add(activityId)
                }
                return@post
            }
            synchronized(pendingFinishActivities) {
                pendingFinishActivities.remove(activityId)
            }
            activity.finishAndRemoveTask()
        }
    }

    fun fullscreenForActivity(activityId: String): Boolean =
        synchronized(fullscreenByActivity) { fullscreenByActivity[activityId] == true }

    @JvmStatic
    fun setActivityFullscreen(activityId: String, fullscreen: Boolean) {
        synchronized(fullscreenByActivity) {
            fullscreenByActivity[activityId] = fullscreen
        }
        mainHandler.post {
            val service = serviceRef?.get() ?: return@post
            service.setWindowFullscreen(activityId, fullscreen)
            service.getActivity(activityId)?.setFullscreenFromCompositor(fullscreen)
        }
    }

    /**
     * Called from native when a client sets a named cursor shape
     * (`wp_cursor_shape_v1`, or a `wl_pointer.set_cursor` we could not turn
     * into a bitmap). [shape] is a CSS/cursor-shape-v1 name; the empty string
     * hides the cursor.
     *
     * Android draws the pointer sprite itself when a real mouse is attached,
     * so TAWC maps the request onto the SurfaceView's PointerIcon rather than
     * rendering a second cursor into the Wayland scene. `View.setPointerIcon`
     * must run on the UI thread.
     */
    @JvmStatic
    fun setPointerIcon(activityId: String, shape: String) {
        val type = pointerIconTypeFor(shape)
        mainHandler.post {
            val activity = serviceRef?.get()?.getActivity(activityId) ?: return@post
            activity.setPointerIconFromCompositor(PointerIcon.getSystemIcon(activity, type))
        }
    }

    /**
     * Called from native with a client-drawn cursor image, packed
     * `ARGB_8888` row-major. Used by legacy `wl_pointer.set_cursor` clients —
     * Xwayland is the main one.
     */
    @JvmStatic
    fun setPointerIconBitmap(
        activityId: String,
        pixels: IntArray,
        width: Int,
        height: Int,
        hotspotX: Int,
        hotspotY: Int,
    ) {
        if (width <= 0 || height <= 0 || pixels.size < width * height) {
            Log.w(TAG, "setPointerIconBitmap: bad ${width}x$height for ${pixels.size} pixels")
            return
        }
        val bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
        mainHandler.post {
            val activity = serviceRef?.get()?.getActivity(activityId) ?: return@post
            val icon = runCatching {
                PointerIcon.create(bitmap, hotspotX.toFloat(), hotspotY.toFloat())
            }.getOrNull()
            if (icon == null) {
                Log.w(TAG, "setPointerIconBitmap: PointerIcon.create rejected the bitmap")
                return@post
            }
            activity.setPointerIconFromCompositor(icon)
        }
    }

    /**
     * `wp_cursor_shape_v1` / CSS cursor name to the closest Android
     * `PointerIcon` system type. Android has no sprite for several shapes
     * (`cell`, `dnd-*`, the single-direction resizes), so those collapse onto
     * the nearest one that exists.
     */
    private fun pointerIconTypeFor(shape: String): Int = when (shape) {
        "" -> PointerIcon.TYPE_NULL
        "default" -> PointerIcon.TYPE_ARROW
        "context-menu" -> PointerIcon.TYPE_CONTEXT_MENU
        "help" -> PointerIcon.TYPE_HELP
        "pointer" -> PointerIcon.TYPE_HAND
        "progress", "wait" -> PointerIcon.TYPE_WAIT
        "cell" -> PointerIcon.TYPE_CELL
        "crosshair" -> PointerIcon.TYPE_CROSSHAIR
        "text" -> PointerIcon.TYPE_TEXT
        "vertical-text" -> PointerIcon.TYPE_VERTICAL_TEXT
        "alias" -> PointerIcon.TYPE_ALIAS
        "copy" -> PointerIcon.TYPE_COPY
        "move" -> PointerIcon.TYPE_ALL_SCROLL
        "no-drop" -> PointerIcon.TYPE_NO_DROP
        "not-allowed" -> PointerIcon.TYPE_NO_DROP
        "grab" -> PointerIcon.TYPE_GRAB
        "grabbing" -> PointerIcon.TYPE_GRABBING
        "e-resize", "w-resize", "ew-resize", "col-resize" ->
            PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW
        "n-resize", "s-resize", "ns-resize", "row-resize" ->
            PointerIcon.TYPE_VERTICAL_DOUBLE_ARROW
        "ne-resize", "sw-resize", "nesw-resize" ->
            PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW
        "nw-resize", "se-resize", "nwse-resize" ->
            PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW
        "all-scroll" -> PointerIcon.TYPE_ALL_SCROLL
        "zoom-in" -> PointerIcon.TYPE_ZOOM_IN
        "zoom-out" -> PointerIcon.TYPE_ZOOM_OUT
        else -> PointerIcon.TYPE_ARROW
    }

    @JvmStatic
    fun updateWindowMetadata(
        activityId: String,
        title: String,
        appId: String,
        desktopId: String,
        desktopName: String,
        iconPath: String,
    ) {
        mainHandler.post {
            serviceRef?.get()?.updateWindowMetadata(
                activityId = activityId,
                title = title,
                appId = appId,
                desktopId = desktopId,
                desktopName = desktopName,
                iconPath = iconPath,
            )
        }
    }

    /** Called from the native socket holder: a client is waiting to connect
     *  and no compositor is running. */
    @JvmStatic
    fun onActivationRequested() {
        val ctx = appContext ?: return
        CompositorService.ensureRunning(ctx)
    }

    /** Called from the remote agent's thread for each event. Must not
     *  call the remote natives synchronously ([nativeRemoteStop] joins
     *  that thread). */
    @JvmStatic
    fun onRemoteEvent(json: String) {
        me.phie.tawc.remote.RemoteSession.onNativeEvent(json)
    }

    /** Called from the compositor thread as its very last act. */
    @JvmStatic
    fun onCompositorStopped() {
        mainHandler.post {
            serviceRef?.get()?.onCompositorStopped()
        }
    }

    @JvmStatic
    fun onToplevelCountChanged(count: Int) {
        mainHandler.post {
            serviceRef?.get()?.updateToplevelCount(count)
        }
    }

    /**
     * Called from native when the focused Wayland text-input instance's
     * `(content_hint, content_purpose)` resolves to a new Android
     * `(EditorInfo.inputType, imeOptions-flags)`. Caches the values and
     * asks the IME to rebuild its connection so the next
     * `onCreateInputConnection` carries the new EditorInfo (URL bar →
     * URL keyboard, etc.). Compositor dedupes; we don't repeat the dedupe
     * here.
     */
    @JvmStatic
    fun onContentTypeChanged(activityId: String, inputType: Int, imeFlags: Int) {
        synchronized(imeEditorInfoByActivity) {
            imeEditorInfoByActivity[activityId] = inputType to imeFlags
        }
        mainHandler.post {
            serviceRef?.get()
                ?.getActivity(activityId)
                ?.restartInputFromCompositor()
        }
    }

    /**
     * Called from native after the focused Wayland client commits a
     * `set_surrounding_text` with the canonical text and selection. Replaces
     * the TawcInputConnection attached to [activityId], if that Activity still
     * owns the active IC, so Gboard's queries (`getTextBeforeCursor`,
     * `getExtractedText`, etc.) match the editor's actual state.
     *
     * Without this, Gboard's text model drifts whenever the Wayland client
     * changes text outside the IME path (cursor moves on touch, autocomplete,
     * paste, undo) — which makes autocorrect, predictions and word boundaries
     * silently wrong.
     *
     * `selStart`/`selEnd` are UTF-16 code unit offsets within `text`.
     * `authoritative` reports (cause=other, or the enable-cycle report of
     * a freshly enabled field) must be applied even mid-composition;
     * non-authoritative (cause=input_method) reports may be the echo of
     * our own preedit and must not cancel an in-progress composition.
     */
    @JvmStatic
    fun onUpdateEditableText(activityId: String, text: String, selStart: Int, selEnd: Int, authoritative: Boolean) {
        mainHandler.post {
            serviceRef?.get()
                ?.getActivity(activityId)
                ?.updateEditableTextFromCompositor(text, selStart, selEnd, authoritative)
        }
    }

    /** Called from native when a Wayland/X11 client selection should become
     * Android's system clipboard text. */
    @JvmStatic
    fun onSetAndroidClipboardText(text: String) {
        mainHandler.post {
            ClipboardBridge.setTextFromCompositor(text)
        }
    }

    /** Called from native, on the event loop, to gate data-control
     *  clipboard reads (`wl-paste`): the `tty_nr` of the terminal tab the
     *  user is looking at, 0 for none. */
    @JvmStatic
    fun focusedTerminalTty(): Int = TerminalPane.focusedTty()

    /** Called from a native clipboard-fetch thread when a client pastes the
     *  compositor-owned Android selection. Runs the real clipboard read —
     *  deliberately not posted to the main thread, the fetch thread blocks
     *  on the result. Null when the clip is unreadable (TAWC not focused),
     *  not text, or over the size cap. Never throws: some OEM builds throw
     *  SecurityException instead of returning null for unfocused reads, and
     *  an exception left pending on the fetch thread would kill the process
     *  on detach. */
    @JvmStatic
    fun fetchClipboardText(): String? = try {
        ClipboardBridge.getTextForPaste()
    } catch (e: Exception) {
        Log.w(TAG, "fetchClipboardText: clipboard read failed", e)
        null
    }
}
