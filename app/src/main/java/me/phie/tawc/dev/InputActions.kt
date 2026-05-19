package me.phie.tawc.dev

import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.compositor.RealImeOutput
import me.phie.tawc.compositor.RecordingImeOutput

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
 * to native. The wayland client (gtk4-debug-app) is the other endpoint
 * — tests assert on what *it* sees on the wire.
 *
 * Why this matters: an earlier version of this file exposed bypass
 * actions (`inject-text`, `set-composing`, `key-event`, …) that called
 * native trampolines directly. They were originally added because the
 * system IME amplified test broadcasts in non-deterministic ways. Once
 * [enable-test-input] / [disable-test-input] swap [NativeBridge.imeOutput]
 * to a [RecordingImeOutput], the IME is no longer in the loop, and the
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
 * | `ic-set-composing-text` | `text` | `IC.setComposingText(text, 1)` |
 * | `ic-set-composing-region` | `start`, `end` | `IC.setComposingRegion(start, end)` |
 * | `ic-finish-composing` | — | `IC.finishComposingText()` |
 * | `ic-set-selection` | `start`, `end` | `IC.setSelection(start, end)` |
 * | `ic-delete-surrounding-text` | `before`, `after` | `IC.deleteSurroundingText(before, after)` |
 * | `ic-send-key-event` | `keycode` | `IC.sendKeyEvent(KeyEvent(ACTION_DOWN, keycode))` |
 * | `inject-touch` | `kind=tap|drag|multitouch` | Dispatch normalized MotionEvents to the focused SurfaceView |
 *
 * Test-mode helpers:
 *
 * | Action | Calls |
 * |--------|-------|
 * | `enable-test-input` | swap [NativeBridge.imeOutput] to a fresh [RecordingImeOutput] |
 * | `disable-test-input` | restore [RealImeOutput] |
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
        ActionRegistry.register("ic-set-composing-text", IcSetComposingTextAction)
        ActionRegistry.register("ic-set-composing-region", IcSetComposingRegionAction)
        ActionRegistry.register("ic-finish-composing", IcFinishComposingAction)
        ActionRegistry.register("ic-set-selection", IcSetSelectionAction)
        ActionRegistry.register("ic-delete-surrounding-text", IcDeleteSurroundingTextAction)
        ActionRegistry.register("ic-send-key-event", IcSendKeyEventAction)
        ActionRegistry.register("inject-touch", InjectTouchAction)

        ActionRegistry.register("query-state", QueryStateAction)
        ActionRegistry.register("enable-test-input", EnableTestInputAction)
        ActionRegistry.register("disable-test-input", DisableTestInputAction)
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

    private fun argInt(args: Map<String, String>, key: String, default: Int? = null): Int? {
        val raw = args[key] ?: return default
        return raw.toIntOrNull() ?: error("'$key' must be an integer (got '$raw')")
    }

    // -- IC drivers ---------------------------------------------------------

    private object IcCommitTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("ic-commit-text: --arg text=... required")
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-commit-text \"$text\" (ic=${ic != null})")
                ic?.commitText(text, 1)
            }
        }
    }

    private object IcSetComposingTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val text = args["text"] ?: return ctx.fail("ic-set-composing-text: --arg text=... required")
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-set-composing-text \"$text\" (ic=${ic != null})")
                ic?.setComposingText(text, 1)
            }
        }
    }

    private object IcSetComposingRegionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val start = argInt(args, "start") ?: return ctx.fail("ic-set-composing-region: --arg start=... required")
            val end = argInt(args, "end") ?: return ctx.fail("ic-set-composing-region: --arg end=... required")
            if (start < 0 || end < 0) return ctx.fail("ic-set-composing-region: start/end must be >= 0")
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-set-composing-region $start..$end (ic=${ic != null})")
                ic?.setComposingRegion(start, end)
            }
        }
    }

    private object IcFinishComposingAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-finish-composing (ic=${ic != null})")
                ic?.finishComposingText()
            }
        }
    }

    private object IcSetSelectionAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val start = argInt(args, "start") ?: return ctx.fail("ic-set-selection: --arg start=... required")
            val end = argInt(args, "end") ?: return ctx.fail("ic-set-selection: --arg end=... required")
            if (start < 0 || end < 0) return ctx.fail("ic-set-selection: start/end must be >= 0")
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-set-selection $start..$end (ic=${ic != null})")
                ic?.setSelection(start, end)
            }
        }
    }

    private object IcDeleteSurroundingTextAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val before = argInt(args, "before", 0)!!
            val after = argInt(args, "after", 0)!!
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-delete-surrounding-text $before/$after (ic=${ic != null})")
                ic?.deleteSurroundingText(before, after)
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
            return withFocusedActivity(ctx) { _ ->
                val ic = NativeBridge.activeInputConnection
                Log.d(TAG, "InputAction ic-send-key-event $keycode (ic=${ic != null})")
                ic?.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keycode))
            }
        }
    }

    /**
     * Inject deterministic touch sequences into the focused SurfaceView.
     * Coordinates are normalized inside CompositorActivity, so host tests
     * do not depend on a particular device resolution.
     */
    private object InjectTouchAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            val kind = args["kind"] ?: return ctx.fail("inject-touch: --arg kind=tap|drag|multitouch required")
            if (kind !in setOf("tap", "drag", "multitouch")) {
                return ctx.fail("inject-touch: unknown kind '$kind'")
            }
            var injectError: String? = null
            val status = withFocusedActivity(ctx) { activity ->
                Log.d(TAG, "InputAction inject-touch $kind")
                injectError = activity.injectTouchSequenceForDev(kind)
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

    /**
     * `enable-test-input` — swap [NativeBridge.imeOutput] to a fresh
     * [RecordingImeOutput]. Subsequent `updateSelection`, `showSoftInput`,
     * etc. calls are recorded instead of going to the real
     * [android.view.inputmethod.InputMethodManager], so the system IME
     * (Gboard / OpenBoard / AOSP-latin) is removed from the loop entirely.
     * Idempotent — calling twice in a row replaces the recorder with a
     * fresh one. Process death resets to production by construction.
     *
     * Note: this doesn't bypass anything in our state machine — it stubs
     * out the *third-party* IME at the boundary. The IC still runs in
     * full; only its outbound calls into the system IMM go to a no-op
     * sink. See `notes/text-input.md` for why this is necessary for
     * deterministic testing.
     */
    private object EnableTestInputAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            NativeBridge.imeOutput = RecordingImeOutput()
            Log.i(TAG, "InputAction enable-test-input: swapped ImeOutput to RecordingImeOutput")
            return 0
        }
    }

    /**
     * `disable-test-input` — restore the production [RealImeOutput].
     * Tests should call this in teardown so a later non-test launch on
     * the same process doesn't quietly run with a recorder in place.
     */
    private object DisableTestInputAction : BrokerAction {
        override fun run(args: Map<String, String>, ctx: ActionContext): Int {
            NativeBridge.imeOutput = RealImeOutput
            Log.i(TAG, "InputAction disable-test-input: restored ImeOutput to RealImeOutput")
            return 0
        }
    }

    private fun ActionContext.fail(msg: String): Int {
        err(msg)
        return 2
    }
}
