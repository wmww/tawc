package me.phie.tawc.compositor

import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.view.View
import android.view.inputmethod.EditorInfo
import androidx.core.net.toUri
import java.lang.ref.WeakReference

object NativeBridge {
    private const val TAG = "tawc"
    private val mainHandler = Handler(Looper.getMainLooper())

    /** Weak ref to the view for keyboard show/hide. Set by CompositorActivity.
     *  Phase 6 will replace this with a per-Activity lookup via CompositorService. */
    private var inputViewRef: WeakReference<View>? = null

    /** Whether the compositor most recently asked for the keyboard to be
     *  shown (vs. hidden). Sticky so that an activity which registers its
     *  view *after* the Wayland client enabled text-input — e.g. when the
     *  client maps + enables text-input faster than Android brings up the
     *  Activity — still gets the keyboard request replayed once the view
     *  is available. Without this the request silently no-ops and the
     *  view never gains focus, so IMM never calls `onCreateInputConnection`
     *  and no `TawcInputConnection` is ever bound. */
    @Volatile private var pendingKeyboardShown: Boolean = false

    var inputView: View?
        get() = inputViewRef?.get()
        set(value) {
            inputViewRef = value?.let { WeakReference(it) }
            // Replay the last keyboard-show on register so a late-arriving
            // Activity catches up with the compositor's current state.
            if (value != null && pendingKeyboardShown) {
                mainHandler.post {
                    val v = inputView ?: return@post
                    if (pendingKeyboardShown) {
                        v.requestFocus()
                        imeOutput.showSoftInput(v)
                    }
                }
            }
        }

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
     *  the dev `enable-test-input` broker action swaps in a
     *  [RecordingImeOutput] so input integration tests don't race the
     *  system IME's reaction to `updateSelection`. See [ImeOutput] kdoc. */
    @Volatile var imeOutput: ImeOutput = RealImeOutput

    /** `(EditorInfo.inputType, extraImeOptionsFlags)` for the focused
     *  Wayland text-input field. Driven by the Wayland client via
     *  text-input-v3's `set_content_type`, pushed up by
     *  [onContentTypeChanged], read by `onCreateInputConnection` on the
     *  next IC build. Stored as a single `@Volatile` reference so concurrent
     *  reads always see a consistent pair — `restartInput` makes the typical
     *  read happen-after the write, but other paths (system focus changes,
     *  configuration changes) also rebuild the IC and don't share that
     *  ordering. */
    @Volatile var imeEditorInfo: Pair<Int, Int> =
        EditorInfo.TYPE_CLASS_TEXT to 0
        private set

    /** Application context, captured by CompositorService.onCreate so the
     *  reverse-JNI `spawnActivity` callback can `startActivity(...)` even
     *  when no Activity is currently in the foreground. */
    private var appContext: Context? = null

    /** Weak ref to the running CompositorService for finishActivity lookups. */
    private var serviceRef: WeakReference<CompositorService>? = null
    private val fullscreenByActivity = mutableMapOf<String, Boolean>()
    private val pendingFinishActivities = mutableSetOf<String>()

    fun attachService(service: CompositorService) {
        appContext = service.applicationContext
        serviceRef = WeakReference(service)
        ClipboardBridge.attach(service.applicationContext)
    }

    fun detachService() {
        ClipboardBridge.detach()
        appContext = null
        serviceRef = null
    }

    /**
     * Look up the live [CompositorService], if any. Public-but-internal
     * for the dev broker action handlers (see
     * [me.phie.tawc.dev.InputActions]) — no production code should reach
     * for this; use [attachService]/[detachService] instead.
     */
    fun serviceRefForDev(): CompositorService? = serviceRef?.get()

    fun consumePendingFinishActivity(activityId: String): Boolean =
        synchronized(pendingFinishActivities) {
            pendingFinishActivities.remove(activityId)
        }

    init {
        System.loadLibrary("compositor")
    }

    // --- Compositor lifecycle: called from CompositorService ---

    /** Start the Rust compositor thread. Idempotent — second call is a no-op.
     *  The first real output size comes from [nativeRegisterActivitySurface]. */
    external fun nativeStartCompositor(
        outputScale: Float,
        gtk3BrokenMenusWorkaround: Boolean,
    )

    /** Stop the Rust compositor thread. Called when the Service is destroyed. */
    external fun nativeStopCompositor()

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

    // --- Text input: Android InputConnection → Compositor ---
    //
    // These are the JNI primitives the production [TawcInputConnection]
    // calls into the Rust compositor with. Tests never call them
    // directly — there is intentionally no broker action that reaches
    // these — because that would bypass the IC's state machine
    // (`computeReplaceDeltas`, the Editable mirror, the
    // `composingRegionIsPreedit` short-circuit) and let wayland-side
    // fallback behaviour hide IC bugs. See [me.phie.tawc.dev.InputActions].

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

    /** Query compositor state (logs COMPOSITOR_STATE line to logcat). */
    external fun nativeQueryState()

    /** Toggle the renderer's per-buffer-type tint (today: magenta SHM
     *  wash). Read live by every frame, so the change is visible on the
     *  next paint. */
    external fun nativeSetTintBuffersByType(enabled: Boolean)

    /** Update the compositor output scale live. The compositor propagates
     *  this through wl_output, fractional-scale, and xdg configure events. */
    external fun nativeSetOutputScale(scale: Float)

    /** Toggle the contained GTK3 broken menubar workaround. */
    external fun nativeSetGtk3BrokenMenusWorkaround(enabled: Boolean)

    /** Forward Android ClipboardManager text changes into the compositor. */
    external fun nativeOnAndroidClipboardText(text: String)

    /**
     * Scan a rootfs for installed `.desktop` apps. Returns a JSON array
     * string: `[{id, name, comment, exec, terminal}, …]` (sorted by name,
     * de-duplicated by id, NoDisplay/Hidden filtered out). Empty `[]` if
     * the rootfs has no apps or doesn't exist. The work is pure file I/O
     * with no compositor-state interaction, so this is safe to call from
     * any thread (LauncherActivity dispatches it on Dispatchers.IO).
     */
    external fun nativeLauncherScan(rootfs: String): String

    // --- Reverse JNI: Compositor → Android (called from compositor thread) ---

    /** Called from native when a Wayland client enables text input. */
    @JvmStatic
    fun onShowKeyboard() {
        Log.d(TAG, "onShowKeyboard (from compositor)")
        pendingKeyboardShown = true
        mainHandler.post {
            val view = inputView ?: return@post
            view.requestFocus()
            imeOutput.showSoftInput(view)
        }
    }

    /** Called from native when a Wayland client disables text input. */
    @JvmStatic
    fun onHideKeyboard() {
        Log.d(TAG, "onHideKeyboard (from compositor)")
        pendingKeyboardShown = false
        mainHandler.post {
            val view = inputView ?: return@post
            imeOutput.hideSoftInput(view)
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
        Log.d(TAG, "spawnActivity from native: $activityId")
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
        Log.d(TAG, "finishActivity from native: $activityId")
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
                Log.d(TAG, "finishActivity($activityId): no live Activity yet")
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
        Log.d(TAG, "setActivityFullscreen from native: $activityId fullscreen=$fullscreen")
        synchronized(fullscreenByActivity) {
            fullscreenByActivity[activityId] = fullscreen
        }
        mainHandler.post {
            val service = serviceRef?.get() ?: return@post
            service.setWindowFullscreen(activityId, fullscreen)
            service.getActivity(activityId)?.setFullscreenFromCompositor(fullscreen)
        }
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
        Log.d(TAG, "updateWindowMetadata from native: $activityId title=$title appId=$appId desktopId=$desktopId icon=$iconPath")
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
    fun onContentTypeChanged(inputType: Int, imeFlags: Int) {
        Log.d(TAG, "onContentTypeChanged inputType=0x${Integer.toHexString(inputType)} imeFlags=0x${Integer.toHexString(imeFlags)}")
        imeEditorInfo = inputType to imeFlags
        mainHandler.post {
            val view = inputView ?: return@post
            imeOutput.restartInput(view)
        }
    }

    /**
     * Called from native after a Wayland client commits a `set_surrounding_text`
     * with the canonical text and selection. Replaces the active
     * TawcInputConnection's Editable contents and selection so Gboard's
     * queries (`getTextBeforeCursor`, `getExtractedText`, etc.) match the
     * editor's actual state.
     *
     * Without this, Gboard's text model drifts whenever the Wayland client
     * changes text outside the IME path (cursor moves on touch, autocomplete,
     * paste, undo) — which makes autocorrect, predictions and word boundaries
     * silently wrong.
     *
     * `selStart`/`selEnd` are UTF-16 code unit offsets within `text`.
     */
    @JvmStatic
    fun onUpdateEditableText(text: String, selStart: Int, selEnd: Int) {
        Log.d(TAG, "onUpdateEditableText (from compositor): \"$text\" sel=$selStart..$selEnd")
        mainHandler.post {
            val ic = activeInputConnection ?: return@post
            ic.updateFromCompositor(text, selStart, selEnd)
        }
    }

    /** Called from native when a Wayland/X11 client selection should become
     * Android's system clipboard text. */
    @JvmStatic
    fun onSetAndroidClipboardText(text: String) {
        Log.d(TAG, "onSetAndroidClipboardText (from compositor): ${text.length} chars")
        mainHandler.post {
            ClipboardBridge.setTextFromCompositor(text)
        }
    }
}
