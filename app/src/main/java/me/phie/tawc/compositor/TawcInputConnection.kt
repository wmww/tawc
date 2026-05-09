package me.phie.tawc.compositor

import android.text.Editable
import android.text.Selection
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.ExtractedText
import android.view.inputmethod.ExtractedTextRequest
import android.view.inputmethod.InputMethodManager
import android.util.Log

/**
 * Custom InputConnection that bridges Android IME events to the Wayland
 * compositor via JNI.
 *
 * # Editable mirror of the Wayland buffer
 *
 * The IME (Gboard, OpenBoard) reads from the Editable via
 * `getTextBeforeCursor`/`getExtractedText`. We keep it in sync with
 * the Wayland client's actual text via two channels:
 *
 * 1. **Outbound:** every IME method calls `super` first to update the
 *    Editable, then JNI to forward to the compositor.
 * 2. **Inbound:** when the Wayland client commits a `set_surrounding_text`,
 *    the compositor calls reverse-JNI [updateFromCompositor], which
 *    overwrites the Editable with the truth (handles autocomplete,
 *    paste, undo, etc.).
 *
 * # Composing-region delta propagation
 *
 * Android and Wayland model "composing text" differently:
 *  - Android: composing text is *bytes-in-document* with a span annotation.
 *    `setComposingRegion(s, e)` adds the annotation without changing
 *    content. The next `setComposingText("X")` or `commitText("X")`
 *    *replaces* the annotated range with the new text.
 *  - Wayland text-input-v3: preedit is a cursor-relative *overlay*, not
 *    document content. There is no equivalent of "annotate this committed
 *    range as composing".
 *
 * Bridging the two requires distinguishing where the Editable's composing
 * region came from:
 *  - Set by `setComposingText`: the range IS the Wayland preedit. The
 *    protocol's done-ordering already replaces the existing preedit on
 *    the next preedit/commit, so no `delete_surrounding_text` is needed.
 *  - Set by `setComposingRegion`: the range is committed text on the
 *    Wayland side. The next preedit/commit must `delete_surrounding_text`
 *    around the cursor to remove the marked bytes before inserting the
 *    new content — otherwise the original word stays and the new content
 *    gets appended (the "hellohello" duplicate-text bug).
 *
 * `composingRegionIsPreedit` carries that distinction. The deltas it
 * yields go through `nativeSetComposingText` / `nativeCommitText` and
 * become `delete_surrounding_text` on the wire.
 *
 * # Cursor synchronisation
 *
 * The wire-side `delete_surrounding_text` is "around the cursor" — but
 * which cursor? Wayland's, not the Editable's. Our IC computes deltas
 * against the Editable's cursor and the compositor applies them at the
 * Wayland buffer's cursor; the two are supposed to match (`updateFromCompositor`
 * mirrors them on every round-trip).
 *
 * They desync when the IME moves the Editable's cursor *without* going
 * through any IC method we forward: most importantly `setSelection`.
 * Wayland text-input-v3 has no "move the cursor" request, so we can't
 * push that change to the client; the Editable says cursor=N while the
 * client still has cursor=M. Computing deltas against N and applying
 * them at M slices the wrong bytes — the "extra h prepended on each
 * Enter" bug observed with OpenBoard's Enter handler, which moves the
 * Editable cursor back into the just-committed word's region before
 * doing a `commitText`.
 *
 * `lastSyncedCursor` tracks the cursor position the Wayland client most
 * recently told us about (via `set_surrounding_text` ⇒ `updateFromCompositor`).
 * When `computeReplaceDeltas` sees the Editable's current cursor differ
 * from this baseline, it refuses to propagate deltas — the safe fallback
 * is to skip the delete and let `super.commitText` update the Editable;
 * the next round-trip from the client reconciles the buffer.
 *
 * `lastSyncedCursor` does NOT catch the case where the Wayland buffer's
 * cursor has moved without our model knowing — for instance after we sent
 * a `commit_string("\n")` whose effect GTK doesn't include in the next
 * `set_surrounding_text` (it reports the previous line as context with
 * cursor at end-of-line, hiding the trailing newline). Editable cursor
 * and `lastSyncedCursor` both say 5; the actual wire cursor is at 6.
 * For OpenBoard's per-Enter "re-commit the previous word" pattern that
 * relies on this — `setComposingRegion(0, N)` + `commitText(word, 1)` +
 * `commitText("\n", 1)` — `commitText` short-circuits when the new text
 * equals the marked region: the buffer already contains those bytes, so
 * no wire `delete_surrounding_text` is needed (and would be wrong).
 *
 * `BaseInputConnection(view, true)` (`fullEditor=true`) means
 * `mFallbackMode=false`, so `sendCurrentText()` is a no-op — calling
 * `super` does NOT cause duplicate input via key events.
 */
class TawcInputConnection(private val targetView: View) : BaseInputConnection(targetView, true) {

    /**
     * Tracks the Editable's current composing region's *origin*:
     *  - `true`: set by [setComposingText] — the range is the Wayland
     *    preedit overlay. No delete needed on the next replace.
     *  - `false`: set by [setComposingRegion] (or no region at all) — if
     *    a region exists it's committed text on the Wayland side. The
     *    next replace must `delete_surrounding_text` first.
     */
    private var composingRegionIsPreedit: Boolean = false

    /**
     * Last cursor position pushed to us by the Wayland client via
     * [updateFromCompositor]. The Editable's cursor is supposed to mirror
     * this; if it doesn't, the IME has moved it under us via [setSelection]
     * (or some other path that bypasses our IC overrides). In that state
     * the wire-side cursor and the Editable cursor diverge and any deltas
     * we compute against the Editable would slice the wrong bytes — see
     * the class-level "Cursor synchronisation" docs.
     */
    private var lastSyncedCursor: Int = 0

    init {
        // Cache as the active IC so reverse-JNI updates and broadcast tests
        // can find it. Also makes broadcasts share Editable state, which is
        // critical for multi-step IME flows (compose-then-commit, etc.).
        NativeBridge.activeInputConnection = this
    }

    override fun closeConnection() {
        if (NativeBridge.activeInputConnection === this) {
            NativeBridge.activeInputConnection = null
        }
        super.closeConnection()
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val str = text?.toString() ?: return false
        // OpenBoard's per-Enter handler — after the user types <word><space>
        // <backspace><enter> — fires `setComposingRegion(0, len(word))` then
        // `commitText(word, 1)` then `commitText("\n", 1)` for every later
        // Enter, treating the previous word as composing and "re-committing"
        // it. The bytes are unchanged, but the IC's normal path would emit
        // backspaces + commit_string and re-render the same word, with the
        // backspaces sometimes slicing past the marked region into bytes
        // GTK didn't include in surrounding text — the user-visible "extra h
        // prepended on each Enter". When the commit text equals the bytes
        // already there, skip the wire round-trip entirely; the buffer is
        // already correct. Real tap-to-retype with different text falls
        // through to the normal path.
        if (!composingRegionIsPreedit) {
            val ed = editable
            if (ed != null) {
                val composingStart = BaseInputConnection.getComposingSpanStart(ed)
                val composingEnd = BaseInputConnection.getComposingSpanEnd(ed)
                if (composingStart >= 0 && composingEnd > composingStart &&
                    str == ed.subSequence(composingStart, composingEnd).toString()) {
                    Log.d(TAG, "InputConnection.commitText: \"$str\" no-op (matches composing region)")
                    super.commitText(text, newCursorPosition)
                    composingRegionIsPreedit = false
                    return true
                }
            }
        }
        val (before, after) = computeReplaceDeltas()
        Log.d(TAG, "InputConnection.commitText: \"$str\" cursorPos=$newCursorPosition delete=$before/$after")
        super.commitText(text, newCursorPosition)
        // Pass the delete deltas (if any) through to the compositor so the
        // wire delete + commit happens atomically inside one done() — the
        // client otherwise fires set_surrounding_text(cause=other) between
        // the two and our preedit-clearing logic undoes the IME's commit.
        // (Standalone deleteSurroundingText takes the [emitKeys] path
        // instead — see its override below.)
        NativeBridge.nativeCommitText(str, before, after)
        composingRegionIsPreedit = false
        return true
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val str = text?.toString() ?: ""
        val (before, after) = computeReplaceDeltas()
        Log.d(TAG, "InputConnection.setComposingText: \"$str\" cursorPos=$newCursorPosition delete=$before/$after")
        super.setComposingText(text, newCursorPosition)
        NativeBridge.nativeSetComposingText(str, before, after)
        // The Editable's composing region is now whatever super created — and
        // it IS our Wayland preedit (the bytes only exist as overlay there).
        composingRegionIsPreedit = true
        return true
    }

    override fun setComposingRegion(start: Int, end: Int): Boolean {
        Log.d(TAG, "InputConnection.setComposingRegion: $start..$end")
        super.setComposingRegion(start, end)
        // The newly-marked range is committed text on the Wayland side
        // (setComposingRegion doesn't insert anything). Next replace must
        // delete those bytes before inserting the new content.
        composingRegionIsPreedit = false
        return true
    }

    override fun finishComposingText(): Boolean {
        Log.d(TAG, "InputConnection.finishComposingText")
        super.finishComposingText()
        NativeBridge.nativeFinishComposingText()
        composingRegionIsPreedit = false
        return true
    }

    /**
     * Convert a (before, after) UTF-16 unit pair into the count of
     * Backspace / Forward-Delete key presses that produce the same
     * deletion in the focused Wayland client. We treat each user-perceived
     * character (codepoint, with surrogate pairs counted once) as one key
     * press — that's what every Wayland client interprets a Backspace to
     * mean, regardless of whether it tracks an editable buffer for IME
     * purposes.
     *
     * When the Editable mirror is empty (the Wayland client never pushed
     * surrounding text — terminals, anything that enables text-input-v3
     * just for the soft keyboard but holds no editable buffer), the unit
     * counts pass straight through. That's exact for ASCII, which is the
     * only case where an empty mirror coexists with a non-zero unit count.
     */
    private fun unitsToKeyCounts(beforeUnits: Int, afterUnits: Int): Pair<Int, Int> {
        if (beforeUnits == 0 && afterUnits == 0) return Pair(0, 0)
        val ed = editable
        if (ed == null || ed.isEmpty()) return Pair(beforeUnits, afterUnits)
        val cursor = Selection.getSelectionStart(ed).coerceIn(0, ed.length)
        val beforeStart = (cursor - beforeUnits).coerceAtLeast(0)
        val afterEnd = (cursor + afterUnits).coerceAtMost(ed.length)
        return Pair(
            Character.codePointCount(ed, beforeStart, cursor),
            Character.codePointCount(ed, cursor, afterEnd),
        )
    }

    /** Send `before` Backspace + `after` Forward-Delete key events. */
    private fun emitKeys(before: Int, after: Int) {
        repeat(before) { NativeBridge.nativeSendKeyEvent(KeyEvent.KEYCODE_DEL) }
        repeat(after) { NativeBridge.nativeSendKeyEvent(KeyEvent.KEYCODE_FORWARD_DEL) }
    }

    /**
     * If the Editable has a composing region that DOES NOT correspond to
     * the Wayland preedit (i.e. set by [setComposingRegion] — committed
     * text on the Wayland side), return the (before, after) UTF-16 unit
     * deltas around the cursor that span that region. Caller passes them
     * to [NativeBridge.nativeCommitText] / [NativeBridge.nativeSetComposingText];
     * the compositor turns them into a `delete_surrounding_text` event
     * emitted in the same atomic done() as the commit/preedit.
     *
     * Returns (0, 0) when:
     *  - the region IS the Wayland preedit (the protocol's done-ordering
     *    already replaces existing preedit on the next replace), or
     *  - there is no composing region, or
     *  - the cursor sits outside the region (Wayland's
     *    `delete_surrounding_text` is "around the cursor", so we can't
     *    represent a range that doesn't contain it; in practice IMEs
     *    always pick regions containing the cursor).
     */
    private fun computeReplaceDeltas(): Pair<Int, Int> {
        if (composingRegionIsPreedit) return Pair(0, 0)
        val ed = editable ?: return Pair(0, 0)
        val start = BaseInputConnection.getComposingSpanStart(ed)
        val end = BaseInputConnection.getComposingSpanEnd(ed)
        if (start < 0 || end < 0 || start >= end) return Pair(0, 0)
        val cursor = Selection.getSelectionStart(ed).coerceAtLeast(0)
        if (cursor < start || cursor > end) return Pair(0, 0)
        // The wire delete is relative to the Wayland client's cursor, not
        // the Editable's. They match as long as nothing has moved the
        // Editable cursor under us since the last round-trip. If they
        // diverge (the IME called setSelection or similar), our deltas
        // would slice the wrong bytes — refuse to propagate them and let
        // the round-trip reconcile.
        if (cursor != lastSyncedCursor) return Pair(0, 0)
        return Pair(cursor - start, end - cursor)
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        Log.d(TAG, "InputConnection.deleteSurroundingText: before=$beforeLength after=$afterLength")
        // Snapshot key counts BEFORE super collapses the deleted span.
        val (beforeKeys, afterKeys) = unitsToKeyCounts(beforeLength, afterLength)
        super.deleteSurroundingText(beforeLength, afterLength)
        emitKeys(beforeKeys, afterKeys)
        return true
    }

    override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
        // Convert code points to UTF-16 code units using our Editable, then
        // delegate.
        val ed = editable ?: return super.deleteSurroundingTextInCodePoints(beforeLength, afterLength)
        val cursor = Selection.getSelectionStart(ed).coerceAtLeast(0)
        val before16 = utf16FromCodePoints(ed, cursor, -beforeLength)
        val after16 = utf16FromCodePoints(ed, cursor, afterLength)
        return deleteSurroundingText(before16, after16)
    }

    override fun performEditorAction(actionCode: Int): Boolean {
        Log.d(TAG, "InputConnection.performEditorAction: actionCode=$actionCode")
        // IME action button (Go, Done, Search, etc.) — treat as Enter
        NativeBridge.nativeSendKeyEvent(KeyEvent.KEYCODE_ENTER)
        return true
    }

    override fun sendKeyEvent(event: KeyEvent): Boolean {
        // Only handle key-down events to avoid double-processing
        if (event.action != KeyEvent.ACTION_DOWN) return true

        Log.d(TAG, "InputConnection.sendKeyEvent: keyCode=${event.keyCode}")
        NativeBridge.nativeSendKeyEvent(event.keyCode)
        return true
    }

    override fun getExtractedText(request: ExtractedTextRequest?, flags: Int): ExtractedText? {
        return super.getExtractedText(request, flags)
    }

    /**
     * Replace our Editable's contents and selection with the Wayland
     * client's authoritative state. Called from native via
     * [NativeBridge.onUpdateEditableText] after the compositor processes
     * a `set_surrounding_text` + `commit`.
     *
     * Always drops any composing span on the Editable: the client's
     * surrounding text excludes preedit, and the compositor has its own
     * preedit lifecycle on the Wayland side. Keeping a stale span here
     * would mislead a later `super.setComposingText` into replacing the
     * wrong range.
     *
     * Also calls `InputMethodManager.updateSelection` so the IME (which
     * keeps its own snapshot) stays in lockstep.
     */
    fun updateFromCompositor(text: String, selStart: Int, selEnd: Int) {
        val ed = editable ?: return
        val newSelStart = selStart.coerceIn(0, text.length)
        val newSelEnd = selEnd.coerceIn(0, text.length)
        val curText = ed.toString()
        val curSelStart = Selection.getSelectionStart(ed)
        val curSelEnd = Selection.getSelectionEnd(ed)

        BaseInputConnection.removeComposingSpans(ed)
        // Mirror clears the Editable's composing region; reset our flag.
        composingRegionIsPreedit = false
        // Record the Wayland cursor so we can detect later setSelection
        // calls that diverge our Editable from the client's view.
        lastSyncedCursor = newSelStart

        if (curText != text) {
            ed.replace(0, ed.length, text)
        }
        if (curSelStart != newSelStart || curSelEnd != newSelEnd) {
            Selection.setSelection(ed, newSelStart, newSelEnd)
        }

        val imm = targetView.context.getSystemService(InputMethodManager::class.java)
        // Composing region is cleared; -1, -1 tells the IME there's no preedit.
        imm?.updateSelection(targetView, newSelStart, newSelEnd, -1, -1)
    }

    private fun utf16FromCodePoints(ed: Editable, cursor: Int, codePointDelta: Int): Int {
        if (codePointDelta == 0) return 0
        val s = ed.toString()
        var idx = cursor
        if (codePointDelta > 0) {
            var remaining = codePointDelta
            while (remaining > 0 && idx < s.length) {
                val cp = s.codePointAt(idx)
                idx += Character.charCount(cp)
                remaining--
            }
            return idx - cursor
        } else {
            var remaining = -codePointDelta
            while (remaining > 0 && idx > 0) {
                val cp = s.codePointBefore(idx)
                idx -= Character.charCount(cp)
                remaining--
            }
            return cursor - idx
        }
    }

    companion object {
        private const val TAG = "tawc"
    }
}
