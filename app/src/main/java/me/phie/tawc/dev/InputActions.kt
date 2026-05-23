package me.phie.tawc.dev

import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.inputmethod.CompletionInfo
import android.view.inputmethod.CorrectionInfo
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import me.phie.tawc.Settings
import me.phie.tawc.compositor.ClipboardBridge
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.compositor.RecordingImeOutput
import me.phie.tawc.compositor.TawcInputConnection

/**
 * Broker actions that drive compositor input from host tests, registered
 * from [me.phie.tawc.TawcApplication.onCreate] (debug builds only).
 *
 * # Rule: tests drive input through `TawcInputConnection`, never around it
 *
 * Every input action here calls a method on the active
 * [me.phie.tawc.compositor.TawcInputConnection] — the same Kotlin
 * surface the system IMM dispatches Gboard / OpenBoard / AOSP-latin
 * events through. There is intentionally **no broker action that calls
 * `NativeBridge.native*` directly**. The test path = the production
 * path: a "keyboard" sends commits / preedits / key events into the IC,
 * the IC mirrors the Editable, computes deltas, applies the
 * `composingRegionIsPreedit` short-circuit when it should, and forwards
 * to native. The wayland client (wayland-debug-app) is the other endpoint
 * — tests assert on what *it* sees on the wire.
 *
 * Why this matters: an earlier version of this file exposed bypass
 * actions (`inject-text`, `set-composing`, `key-event`, …) that called
 * native trampolines directly. They were originally added because the
 * system IME amplified test broadcasts in non-deterministic ways. Once
 * [test-init] swaps [NativeBridge.imeOutput] to a [RecordingImeOutput],
 * the IME is no longer in the loop, and the
 * justification disappears — the bypass became dead weight that hid
 * IC regressions behind a wayland-side fallback (text-input-v3 done
 * ordering produces the right *observable* even when IC's
 * `computeReplaceDeltas`, Editable mirror, or short-circuit are buggy,
 * because the protocol's preedit-replacement does the work). Driving
 * everything through IC closes that hole.
 *
 * Anything that needs to read compositor state without driving input
 * (e.g. [query-state] for `clients` / `toplevels` counts) is allowed —
 * those are observational. Anything that *changes* compositor input
 * state must come in through the IC.
 *
 * # Action surface
 *
 * IC drivers (mirror Gboard's [android.view.inputmethod.InputConnection]
 * surface):
 *
 * | Action | Args | Calls |
 * |--------|------|-------|
 * | `ic-commit-text` | `text` | `IC.commitText(text, 1)` |
 * | `ic-commit-completion` | `text` | `IC.commitCompletion(CompletionInfo(..., text))` |
 * | `ic-commit-correction` | `offset`, `old`, `new` | `IC.commitCorrection(CorrectionInfo(...))` |
 * | `ic-replace-text` | `start`, `end`, `text` | `IC.replaceText(start, end, text, 1, null)` |
 * | `ic-set-composing-text` | `text` | `IC.setComposingText(text, 1)` |
 * | `ic-set-composing-region` | `start`, `end` | `IC.setComposingRegion(start, end)` |
 * | `ic-finish-composing` | — | `IC.finishComposingText()` |
 * | `ic-set-selection` | `start`, `end` | `IC.setSelection(start, end)` |
 * | `ic-delete-surrounding-text` | `before`, `after` | `IC.deleteSurroundingText(before, after)` |
 * | `ic-send-key-event` | `keycode` | `IC.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode))` |
 * | `ic-send-modified-key-event` | `keycode`, `ctrl`, `alt`, `shift` | `IC.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode, metaState))` |
 * | `ic-finish-hidden-composing` | — | `RecordingImeOutput` stale hidden IC `finishComposingText()` |
 * | `inject-touch` | `kind=tap|tap-logical|tap-outside-popup|drag|multitouch` | Dispatch MotionEvents to the focused SurfaceView |
 *
 * Test-mode helpers:
 *
 * | Action | Calls |
 * |--------|-------|
 * | `input-ready` | succeeds only when the focused Activity has an active IC |
 * | `test-init` | enter in-memory test settings, enable test input, close current client windows |
 *
 * Observational:
 *
 * | Action | Calls |
 * |--------|-------|
 * | `query-state` | `NativeBridge.nativeQueryState()` (no main-loop hop, no focused activity required) |
 */
internal object InputActions {

    private const val TAG = "tawc"

    fun registerAll() {
        ActionRegistry.register("ic-commit-text", IcCommitTextAction)
        ActionRegistry.register("ic-commit-completion", IcCommitCompletionAction)
        ActionRegistry.register("ic-commit-correction", IcCommitCorrectionAction)
        ActionRegistry.register("ic-replace-text", IcReplaceTextAction)
        ActionRegistry.register("ic-set-composing-text", IcSetComposingTextAction)
        ActionRegistry.register("ic-set-composing-region", IcSetComposingRegionAction)
        ActionRegistry.register("ic-finish-composing", IcFinishComposingAction)
        ActionRegistry.register("ic-set-selection", IcSetSelectionAction)
        ActionRegistry.register("ic-delete-surrounding-text", IcDeleteSurroundingTextAction)
        ActionRegistry.register("ic-send-key-event", IcSendKeyEventAction)
        ActionRegistry.register("ic-send-modified-key-event", IcSendModifiedKeyEventAction)
        ActionRegistry.register("ic-finish-hidden-composing", IcFinishHiddenComposingAction)
        ActionRegistry.register("inject-touch", InjectTouchAction)

        ActionRegistry.register("query-state", QueryStateAction)
        ActionRegistry.register("input-ready", InputReadyAction)
        ActionRegistry.register("clipboard-set-text", ClipboardSetTextAction)
        ActionRegistry.register("clipboard-get-text", ClipboardGetTextAction)
        ActionRegistry.register("test-init", TestInitAction)
    }

    // -- Helpers ------------------------------------------------------------

    private val mainHandler = Handler(Looper.getMainLooper())

    /**
     * Run [block] on the main thread and wait for it. We block the broker
     * thread because actions are one-shot — the host expects a single
     * exit code and the action shouldn't return before the work has
     * actually run. The wait has a 5s safety timeout to avoid hanging
     * the broker on a frozen main loop.
     */
    private fun onMainBlocking(block: () -> Unit): Boolean {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            block()
            return true
        }
        val latch = CountDownLatch(1)
        mainHandler.post {
            try { block() } finally { latch.countDown() }
        }
        return latch.await(5, TimeUnit.SECONDS)
    }

    /**
     * Resolve the currently-focused [me.phie.tawc.compositor.CompositorActivity]
     * via [NativeBridge.serviceRefForDev] and pass it to [block]. Returns
     * 0 on success, 1 with an error line on the host's stderr if no
     * focused activity exists. The error is loud by design — silent skip
     * masks test setup mistakes (no app launched, or app crashed before
     * the action fired). Setting up a focused activity is the test's
     * responsibility.
     */
    private fun withFocusedActivity(ctx: ActionContext, block: (me.phie.tawc.compositor.CompositorActivity) -> Unit): Int {
        val service = NativeBridge.serviceRefForDev() ?: run {
            ctx.err("no CompositorService running (cold start the app first)")
            return 1
        }
        var ok = false
        val ran = onMainBlocking {
            val activity = service.focusedActivity()
            if (activity == null) {
                ctx.err("no focused CompositorActivity (no client window has focus)")
            } else {
                block(activity)
                ok = true
            }
        }
        if (!ran) {
            ctx.err("main loop did not run action within 5s")
            return 1
        }
        return if (ok) 0 else 1
    }

    private fun clearRecordingImeOutput() {
        (NativeBridge.imeOutput as? RecordingImeOutput)?.clearTestInputConnection()
    }

    private fun withActiveInputConnection(
        ctx: ActionContext,
        action: String,
        block: (TawcInputConnection) -> Boolean,
    ): Int {
        var missing = false
        var rejected = false
        val status = withFocusedActivity(ctx) {
            val ic = it.focusedInputConnectionForDev()
            if (ic == null) {
                ctx.err("no active TawcInputConnection for focused activity")
                missing = true
            } else {
                rejected = !block(ic)
                if (rejected) {
                    ctx.err("$action returned false")
                }
            }
        }
        if (status != 0) return status
        return if (missing || rejected) 1 else 0
    }

    private fun argInt(args: Map<String, String>, key: String, default: Int? = null): Int? {
        val raw = args[key] ?: return default
        return raw.toIntOrNull() ?: error("'$key' must be an integer (got '$raw')")
    }

    private fun argBool(args: Map<String, String>, key: String): Boolean =
        when (args[key]?.lowercase()) {
            null, "", "0", "false", "no" -> false
            "1", "true", "yes" -> true
            else -> error("'$key' must be a boolean")
        }

    // -- IC drivers ---------------------------------------------------------

    private object IcCommitTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("ic-commit-text: --arg text=... required")
            return withActiveInputConnection(ctx, "ic-commit-text") { ic ->
                Log.d(TAG, "InputAction ic-commit-text \"$text\"")
                ic.commitText(text, 1)
            }
        }
    }

    private object IcCommitCompletionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("ic-commit-completion: --arg text=... required")
            return withActiveInputConnection(ctx, "ic-commit-completion") { ic ->
                Log.d(TAG, "InputAction ic-commit-completion \"$text\"")
                ic.commitCompletion(CompletionInfo(0, 0, text))
            }
        }
    }

    private object IcCommitCorrectionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val offset = argInt(args, "offset") ?: return ctx.fail("ic-commit-correction: --arg offset=... required")
            val oldText = args["old"] ?: return ctx.fail("ic-commit-correction: --arg old=... required")
            val newText = args["new"] ?: return ctx.fail("ic-commit-correction: --arg new=... required")
            if (offset < 0) return ctx.fail("ic-commit-correction: offset must be >= 0")
            return withActiveInputConnection(ctx, "ic-commit-correction") { ic ->
                Log.d(TAG, "InputAction ic-commit-correction $offset \"$oldText\" -> \"$newText\"")
                ic.commitCorrection(CorrectionInfo(offset, oldText, newText))
            }
        }
    }

    private object IcReplaceTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val start = argInt(args, "start") ?: return ctx.fail("ic-replace-text: --arg start=... required")
            val end = argInt(args, "end") ?: return ctx.fail("ic-replace-text: --arg end=... required")
            val text = args["text"] ?: return ctx.fail("ic-replace-text: --arg text=... required")
            if (start < 0 || end < 0) return ctx.fail("ic-replace-text: start/end must be >= 0")
            return withActiveInputConnection(ctx, "ic-replace-text") { ic ->
                Log.d(TAG, "InputAction ic-replace-text $start..$end \"$text\"")
                ic.replaceText(start, end, text, 1, null)
            }
        }
    }

    private object IcSetComposingTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("ic-set-composing-text: --arg text=... required")
            return withActiveInputConnection(ctx, "ic-set-composing-text") { ic ->
                Log.d(TAG, "InputAction ic-set-composing-text \"$text\"")
                ic.setComposingText(text, 1)
            }
        }
    }

    private object IcSetComposingRegionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val start = argInt(args, "start") ?: return ctx.fail("ic-set-composing-region: --arg start=... required")
            val end = argInt(args, "end") ?: return ctx.fail("ic-set-composing-region: --arg end=... required")
            if (start < 0 || end < 0) return ctx.fail("ic-set-composing-region: start/end must be >= 0")
            return withActiveInputConnection(ctx, "ic-set-composing-region") { ic ->
                Log.d(TAG, "InputAction ic-set-composing-region $start..$end")
                ic.setComposingRegion(start, end)
            }
        }
    }

    private object IcFinishComposingAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            return withActiveInputConnection(ctx, "ic-finish-composing") { ic ->
                Log.d(TAG, "InputAction ic-finish-composing")
                ic.finishComposingText()
            }
        }
    }

    private object IcSetSelectionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val start = argInt(args, "start") ?: return ctx.fail("ic-set-selection: --arg start=... required")
            val end = argInt(args, "end") ?: return ctx.fail("ic-set-selection: --arg end=... required")
            if (start < 0 || end < 0) return ctx.fail("ic-set-selection: start/end must be >= 0")
            return withActiveInputConnection(ctx, "ic-set-selection") { ic ->
                Log.d(TAG, "InputAction ic-set-selection $start..$end")
                ic.setSelection(start, end)
            }
        }
    }

    private object IcDeleteSurroundingTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val before = argInt(args, "before", 0)!!
            val after = argInt(args, "after", 0)!!
            return withActiveInputConnection(ctx, "ic-delete-surrounding-text") { ic ->
                Log.d(TAG, "InputAction ic-delete-surrounding-text $before/$after")
                ic.deleteSurroundingText(before, after)
            }
        }
    }

    /**
     * Drive [TawcInputConnection.sendKeyEvent] with an `ACTION_DOWN`
     * [KeyEvent]. The IC drops everything that isn't `ACTION_DOWN`, and
     * tests only ever care about key-down (which is what produces the
     * `wl_keyboard` press the client reacts to). For Backspace tests
     * prefer [IcDeleteSurroundingTextAction] — the IC translates that
     * into the same Backspace key event but also keeps the Editable
     * mirror in step.
     */
    private object IcSendKeyEventAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val keycode = argInt(args, "keycode") ?: return ctx.fail("ic-send-key-event: --arg keycode=... required")
            if (keycode < 0) return ctx.fail("ic-send-key-event: keycode must be >= 0")
            return withActiveInputConnection(ctx, "ic-send-key-event") { ic ->
                Log.d(TAG, "InputAction ic-send-key-event $keycode")
                ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keycode))
            }
        }
    }

    private object IcSendModifiedKeyEventAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val keycode = argInt(args, "keycode")
                ?: return ctx.fail("ic-send-modified-key-event: --arg keycode=... required")
            if (keycode < 0) return ctx.fail("ic-send-modified-key-event: keycode must be >= 0")
            var metaState = 0
            if (argBool(args, "ctrl")) metaState = metaState or KeyEvent.META_CTRL_ON
            if (argBool(args, "alt")) metaState = metaState or KeyEvent.META_ALT_ON
            if (argBool(args, "shift")) metaState = metaState or KeyEvent.META_SHIFT_ON
            return withActiveInputConnection(ctx, "ic-send-modified-key-event") { ic ->
                Log.d(TAG, "InputAction ic-send-modified-key-event $keycode meta=$metaState")
                ic.sendKeyEvent(KeyEvent(0, 0, KeyEvent.ACTION_DOWN, keycode, 0, metaState))
            }
        }
    }

    private object IcFinishHiddenComposingAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            var ok = false
            val ran = onMainBlocking {
                val recorder = NativeBridge.imeOutput as? RecordingImeOutput
                ok = recorder?.finishHiddenComposingTextForDev() == true
            }
            if (!ran) return ctx.fail("main loop did not run ic-finish-hidden-composing within 5s")
            return if (ok) 0 else ctx.fail("ic-finish-hidden-composing: no hidden test InputConnection")
        }
    }

    /**
     * Inject deterministic touch sequences into the focused SurfaceView.
     * Coordinates are normalized inside CompositorActivity, so host tests
     * do not depend on a particular device resolution.
     */
    private object InjectTouchAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val kind = args["kind"] ?: return ctx.fail("inject-touch: --arg kind=tap|tap-logical|tap-outside-popup|tap-menu-a|tap-menu-b|drag|multitouch required")
            if (kind !in setOf("tap", "tap-logical", "tap-outside-popup", "tap-menu-a", "tap-menu-b", "drag", "multitouch")) {
                return ctx.fail("inject-touch: unknown kind '$kind'")
            }
            val x = args["x"]?.toFloatOrNull()
            val y = args["y"]?.toFloatOrNull()
            if ((args["x"] != null && x == null) || (args["y"] != null && y == null)) {
                return ctx.fail("inject-touch: x/y must be numbers")
            }
            var injectError: String? = null
            val status = withFocusedActivity(ctx) { activity ->
                Log.d(TAG, "InputAction inject-touch $kind")
                injectError = activity.injectTouchSequenceForDev(kind, x, y)
            }
            if (status != 0) return status
            return injectError?.let { ctx.fail("inject-touch: $it") } ?: 0
        }
    }

    // -- Observational + test-mode helpers ----------------------------------

    /**
     * `query-state` — log a `COMPOSITOR_STATE` line via the compositor's
     * own log channel. Pure native call, no main-loop hop required.
     * Lives on the service rather than an activity so tests can poll it
     * before any client window has focus (e.g. right after compositor
     * boot, before the first GTK app has come up). Observational only —
     * doesn't change input state.
     */
    private object QueryStateAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            NativeBridge.nativeQueryState()
            return 0
        }
    }

    private object InputReadyAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            return withActiveInputConnection(ctx, "input-ready") {
                ctx.out("ready")
                true
            }
        }
    }

    private object ClipboardSetTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("clipboard-set-text: --arg text=... required")
            ClipboardBridge.setTextFromDevAction(text)
            return 0
        }
    }

    private object ClipboardGetTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            ctx.out(ClipboardBridge.getTextForDevAction())
            return 0
        }
    }

    /**
     * `test-init` — fast per-test reset. Nothing here writes
     * SharedPreferences; app process death restores normal persisted
     * settings and the production ImeOutput.
     */
    private object TestInitAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val ran = onMainBlocking {
                Settings.enterTestMode()
                NativeBridge.nativeSetTintBuffersByType(Settings.tintBuffersByType)
                NativeBridge.nativeSetOutputScale(Settings.outputScale)
                NativeBridge.nativeSetGtk3BrokenMenusWorkaround(Settings.gtk3BrokenMenusWorkaround)
                clearRecordingImeOutput()
                NativeBridge.activeInputConnection = null
                NativeBridge.imeOutput = RecordingImeOutput()
            }
            if (!ran) return ctx.fail("main loop did not run test-init within 5s")
            val closed = NativeBridge.nativeCloseAllClientsForTest()
            if (closed < 0) return ctx.fail("compositor did not process close-all-clients request")
            ctx.out("closed=$closed")
            Log.i(TAG, "InputAction test-init: reset in-memory settings and requested close for $closed client windows")
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
