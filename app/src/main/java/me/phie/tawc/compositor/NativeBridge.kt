package me.phie.tawc.compositor

import android.content.Context
import android.content.Intent
import android.net.Uri
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

    /** Weak ref to the view for keyboard show/hide. Set by CompositorActivity.
     *  Phase 6 will replace this with a per-Activity lookup via CompositorService. */
    private var inputViewRef: WeakReference<View>? = null

    var inputView: View?
        get() = inputViewRef?.get()
        set(value) { inputViewRef = value?.let { WeakReference(it) } }

    /** The currently active TawcInputConnection. Set by TawcInputConnection's
     *  init, cleared by closeConnection. The Wayland-to-Android Editable sync
     *  pushes through this; broadcast tests also reuse it so multi-step IME
     *  flows (compose → finish → commit) share Editable state. */
    private var activeICRef: WeakReference<TawcInputConnection>? = null

    var activeInputConnection: TawcInputConnection?
        get() = activeICRef?.get()
        set(value) { activeICRef = value?.let { WeakReference(it) } }

    /** Application context, captured by CompositorService.onCreate so the
     *  reverse-JNI `spawnActivity` callback can `startActivity(...)` even
     *  when no Activity is currently in the foreground. */
    private var appContext: Context? = null

    /** Weak ref to the running CompositorService for finishActivity lookups. */
    private var serviceRef: WeakReference<CompositorService>? = null

    fun attachService(service: CompositorService) {
        appContext = service.applicationContext
        serviceRef = WeakReference(service)
    }

    fun detachService() {
        appContext = null
        serviceRef = null
    }

    init {
        System.loadLibrary("compositor")
    }

    // --- Compositor lifecycle: called from CompositorService ---

    /** Start the Rust compositor thread. Idempotent — second call is a no-op.
     *
     *  `width`/`height` are the device's display size in physical pixels,
     *  used as the initial `output_logical_size` so `xdg_toplevel.configure`
     *  events sent before any CompositorActivity has registered carry a
     *  real size instead of 0x0 — a Wayland client that commits a buffer
     *  between the initial configure and the host registering would
     *  otherwise be told to resize after the fact, which Vulkan/vkcube
     *  doesn't recover from. */
    external fun nativeStartCompositor(width: Int, height: Int)

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

    // --- Text input: Android InputConnection → Compositor ---

    /** commitText from InputConnection. `deleteBefore`/`deleteAfter` are
     *  UTF-16 code-unit counts around the cursor that should be removed
     *  from the Wayland client's committed buffer first — non-zero only
     *  when the IME is replacing a region established by
     *  setComposingRegion (still committed text on Wayland), zero
     *  otherwise. */
    external fun nativeCommitText(text: String, deleteBefore: Int, deleteAfter: Int)

    /** setComposingText from InputConnection. Same delete semantics as
     *  [nativeCommitText]. */
    external fun nativeSetComposingText(text: String, deleteBefore: Int, deleteAfter: Int)

    /** finishComposingText from InputConnection */
    external fun nativeFinishComposingText()

    /** deleteSurroundingText from InputConnection */
    external fun nativeDeleteSurroundingText(before: Int, after: Int)

    /** sendKeyEvent mapped to keycode */
    external fun nativeSendKeyEvent(keycode: Int)

    /** Query compositor state (logs COMPOSITOR_STATE line to logcat). */
    external fun nativeQueryState()

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
                data = Uri.parse("tawc://activity/$activityId")
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
            val activity = serviceRef?.get()?.getActivity(activityId)
            if (activity == null) {
                Log.d(TAG, "finishActivity($activityId): no live Activity")
                return@post
            }
            activity.finishAndRemoveTask()
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
}
