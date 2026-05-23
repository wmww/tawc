package me.phie.tawc.compositor

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ClipDescription
import android.content.Context
import android.os.SystemClock
import android.util.Log

/**
 * Process-local bridge between Android's real ClipboardManager and the
 * native Wayland selection state. Text-only by design for now.
 */
object ClipboardBridge {
    private const val TAG = "tawc"
    private const val MAX_TEXT_BYTES = 1024 * 1024

    private var clipboard: ClipboardManager? = null
    private var suppressText: String? = null
    private var suppressUntilMs: Long = 0

    private val listener = ClipboardManager.OnPrimaryClipChangedListener {
        val text = currentText() ?: return@OnPrimaryClipChangedListener

        val now = SystemClock.uptimeMillis()
        if (text == suppressText && now <= suppressUntilMs) {
            suppressText = null
            suppressUntilMs = 0
            return@OnPrimaryClipChangedListener
        }

        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: dropping Android clipboard text over ${MAX_TEXT_BYTES}B")
            return@OnPrimaryClipChangedListener
        }
        NativeBridge.nativeOnAndroidClipboardText(text)
    }

    fun attach(context: Context) {
        val ctx = context.applicationContext
        clipboard?.removePrimaryClipChangedListener(listener)
        clipboard = ctx.getSystemService(ClipboardManager::class.java)
        clipboard?.addPrimaryClipChangedListener(listener)
    }

    fun detach() {
        clipboard?.removePrimaryClipChangedListener(listener)
        clipboard = null
        suppressText = null
        suppressUntilMs = 0
    }

    fun setTextFromCompositor(text: String) {
        val cm = clipboard ?: return
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: refusing to write compositor text over ${MAX_TEXT_BYTES}B")
            return
        }
        suppressText = text
        suppressUntilMs = SystemClock.uptimeMillis() + 1000
        cm.setPrimaryClip(ClipData.newPlainText("tawc", text))
    }

    fun setTextFromDevAction(text: String) {
        val cm = clipboard ?: return
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: refusing to write dev text over ${MAX_TEXT_BYTES}B")
            return
        }
        cm.setPrimaryClip(ClipData.newPlainText("tawc-dev", text))
    }

    fun getTextForDevAction(): String {
        return currentText() ?: ""
    }

    fun syncCurrentTextToNative() {
        val text = currentText() ?: return
        if (text.toByteArray(Charsets.UTF_8).size > MAX_TEXT_BYTES) {
            Log.w(TAG, "ClipboardBridge: dropping current Android clipboard text over ${MAX_TEXT_BYTES}B")
            return
        }
        NativeBridge.nativeOnAndroidClipboardText(text)
    }

    private fun currentText(): String? {
        val clip = clipboard?.primaryClip ?: return null
        if (clip.itemCount <= 0) return null
        if (!clip.description.hasMimeType(ClipDescription.MIMETYPE_TEXT_PLAIN)) return null
        val text = clip.getItemAt(0).text ?: return null
        if (text.length > MAX_TEXT_BYTES) return null
        return text.toString()
    }
}
