package me.phie.tawc.compositor

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log

/**
 * Process-local bridge between Android's real ClipboardManager and the
 * native Wayland selection state. Text-only by design for now.
 *
 * Since Android 10, clip-changed listeners only fire while this app holds
 * input focus, so copies made in other apps are invisible until the user
 * returns. [syncOnWindowFocusGained] re-reads the clipboard when a
 * compositor window regains focus to pick those up.
 */
object ClipboardBridge {
    private const val TAG = "tawc"
    private const val MAX_TEXT_BYTES = 1024 * 1024

    /** Clipboard reads can still be denied (null) for a short window
     *  after `onWindowFocusChanged(true)`; retry briefly before giving up. */
    private const val FOCUS_SYNC_ATTEMPTS = 3
    private const val FOCUS_SYNC_RETRY_MS = 250L

    private val handler = Handler(Looper.getMainLooper())
    private var clipboard: ClipboardManager? = null
    private var suppressText: String? = null
    private var suppressUntilMs: Long = 0

    /** Last text either side has seen, in either direction. Focus-gain
     *  syncs skip matching text so they never replace a live client-owned
     *  Wayland selection (which may offer richer MIME types) with a
     *  compositor-owned text-only copy of itself. */
    private var lastSyncedText: String? = null
    private var focusSyncAttemptsLeft = 0

    private val listener = ClipboardManager.OnPrimaryClipChangedListener {
        val text = currentText() ?: return@OnPrimaryClipChangedListener

        val now = SystemClock.uptimeMillis()
        if (text == suppressText && now <= suppressUntilMs) {
            suppressText = null
            suppressUntilMs = 0
            return@OnPrimaryClipChangedListener
        }

        pushTextToNative(text, "listener")
    }

    private val focusSyncRunnable = object : Runnable {
        override fun run() {
            focusSyncAttemptsLeft--
            val text = currentText()
            if (text == null) {
                if (focusSyncAttemptsLeft > 0) handler.postDelayed(this, FOCUS_SYNC_RETRY_MS)
                return
            }
            if (text == lastSyncedText) return
            pushTextToNative(text, "focus sync")
        }
    }

    fun attach(context: Context) {
        val ctx = context.applicationContext
        clipboard?.removePrimaryClipChangedListener(listener)
        clipboard = ctx.getSystemService(ClipboardManager::class.java)
        clipboard?.addPrimaryClipChangedListener(listener)
    }

    fun detach() {
        clipboard?.removePrimaryClipChangedListener(listener)
        handler.removeCallbacks(focusSyncRunnable)
        clipboard = null
        suppressText = null
        suppressUntilMs = 0
        lastSyncedText = null
        focusSyncAttemptsLeft = 0
    }

    fun setTextFromCompositor(text: String) {
        val cm = clipboard ?: return
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: refusing to write compositor text over ${MAX_TEXT_BYTES}B")
            return
        }
        suppressText = text
        suppressUntilMs = SystemClock.uptimeMillis() + 1000
        lastSyncedText = text
        cm.setPrimaryClip(ClipData.newPlainText("tawc", text))
    }

    /** [asHtml] mimics Firefox/Gecko web-content copies: an HTML clip whose
     *  description has no text/plain MIME but whose item carries the text. */
    fun setTextFromDevAction(text: String, asHtml: Boolean = false) {
        val cm = clipboard ?: return
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: refusing to write dev text over ${MAX_TEXT_BYTES}B")
            return
        }
        val clip = if (asHtml) {
            ClipData.newHtmlText("tawc-dev", text, "<span>$text</span>")
        } else {
            ClipData.newPlainText("tawc-dev", text)
        }
        cm.setPrimaryClip(clip)
    }

    fun getTextForDevAction(): String {
        return currentText() ?: ""
    }

    fun syncCurrentTextToNative() {
        val text = currentText() ?: return
        pushTextToNative(text, "startup sync")
    }

    /** Re-read the clipboard now that the app regained window focus,
     *  catching copies made in other apps while we were backgrounded. */
    fun syncOnWindowFocusGained() {
        if (clipboard == null) return
        handler.removeCallbacks(focusSyncRunnable)
        focusSyncAttemptsLeft = FOCUS_SYNC_ATTEMPTS
        focusSyncRunnable.run()
    }

    private fun pushTextToNative(text: String, why: String) {
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: dropping Android clipboard text over ${MAX_TEXT_BYTES}B ($why)")
            return
        }
        lastSyncedText = text
        NativeBridge.nativeOnAndroidClipboardText(text)
    }

    /** No ClipDescription MIME gate: Firefox/Gecko copies of web content are
     *  HTML clips that advertise only text/html yet carry the plain text in
     *  the item. Non-text clips (images, URIs) have a null item text. */
    private fun currentText(): String? {
        val clip = clipboard?.primaryClip ?: return null
        if (clip.itemCount <= 0) return null
        val text = clip.getItemAt(0).text ?: return null
        if (text.length > MAX_TEXT_BYTES) return null
        return text.toString()
    }
}
