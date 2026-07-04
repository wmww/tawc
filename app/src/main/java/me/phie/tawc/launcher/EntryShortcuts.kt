package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import me.phie.tawc.R
import me.phie.tawc.install.Installation

/**
 * Pinned home-screen shortcuts for launcher entries.
 *
 * The shortcut intent carries only `(installId, desktopId)` plus a label
 * for error dialogs — never the Exec string. [ShortcutLaunchActivity]
 * re-resolves the entry at tap time, so pins stay current when the
 * `.desktop` file changes and the system's shortcut store never holds an
 * executable command.
 */
object EntryShortcuts {

    /** Stable id so re-pinning the same entry updates the existing pin
     *  instead of creating a duplicate. */
    fun shortcutId(installId: String, desktopId: String) = "$installId/$desktopId"

    /**
     * Inverse of [shortcutId], for bulk operations on the system's pin
     * list (e.g. disabling a distro's pins on uninstall). Install ids
     * cannot contain `/` ([Installation.isValidId]), desktop ids could,
     * so split at the first one. Null for a malformed id.
     */
    fun splitShortcutId(id: String): Pair<String, String>? {
        val slash = id.indexOf('/')
        if (slash <= 0 || slash == id.length - 1) return null
        return id.substring(0, slash) to id.substring(slash + 1)
    }

    enum class PinResult {
        /** System pin sheet shown; the launcher takes it from here. */
        REQUESTED,
        /** Id was already pinned — refreshed label/icon in place. */
        UPDATED,
        /** Launcher has no pinned-shortcut support (rare: custom launchers). */
        UNSUPPORTED,
    }

    /**
     * Ask the system launcher to pin [entry] on the home screen; the
     * system sheet handles placement/confirmation. An already-pinned id
     * is updated in place instead — re-requesting would duplicate the
     * workspace icon on launchers that don't dedupe (Pixel launcher
     * does not). Decodes the entry icon, so call off the main thread.
     */
    fun requestPin(context: Context, inst: Installation, entry: LauncherEntry): PinResult {
        if (!ShortcutManagerCompat.isRequestPinShortcutSupported(context)) {
            return PinResult.UNSUPPORTED
        }
        val label = entry.name.ifEmpty { entry.id }
        // Shortcut intents require an action; the extras are the payload.
        val intent = Intent(context, ShortcutLaunchActivity::class.java)
            .setAction(Intent.ACTION_VIEW)
            .putExtra(ShortcutLaunchActivity.EXTRA_INSTALL_ID, inst.id)
            .putExtra(ShortcutLaunchActivity.EXTRA_DESKTOP_ID, entry.id)
            .putExtra(ShortcutLaunchActivity.EXTRA_LABEL, label)
        val info = ShortcutInfoCompat.Builder(context, shortcutId(inst.id, entry.id))
            .setShortLabel(label)
            .setIcon(pinIcon(context, entry.iconPath))
            .setIntent(intent)
            .build()
        val pinned = ShortcutManagerCompat
            .getShortcuts(context, ShortcutManagerCompat.FLAG_MATCH_PINNED)
            .any { it.id == info.id }
        return if (pinned) {
            ShortcutManagerCompat.updateShortcuts(context, listOf(info))
            PinResult.UPDATED
        } else {
            ShortcutManagerCompat.requestPinShortcut(context, info, null)
            PinResult.REQUESTED
        }
    }

    private fun pinIcon(context: Context, iconPath: String): IconCompat {
        val bmp = pinBitmap(iconPath)
        // No usable entry icon → the tawc app icon, so the pin is still
        // recognizable.
        return if (bmp != null) IconCompat.createWithAdaptiveBitmap(bmp)
        else IconCompat.createWithResource(context, R.mipmap.ic_launcher)
    }

    /**
     * The entry icon centered on a neutral square for
     * [IconCompat.createWithAdaptiveBitmap] (masks correctly on every
     * launcher shape), or null when the icon is missing/undecodable.
     */
    private fun pinBitmap(iconPath: String): Bitmap? {
        if (iconPath.isEmpty()) return null
        val src = IconLoader.decode(iconPath, PIN_BITMAP_PX * 2 / 3) ?: return null
        val fit = pinIconFit(PIN_BITMAP_PX, src.width, src.height) ?: return null
        val out = Bitmap.createBitmap(PIN_BITMAP_PX, PIN_BITMAP_PX, Bitmap.Config.ARGB_8888)
        out.eraseColor(PIN_BACKGROUND)
        Canvas(out).drawBitmap(src, null, Rect(fit[0], fit[1], fit[2], fit[3]), Paint(Paint.FILTER_BITMAP_FLAG))
        return out
    }

    /**
     * Destination rect `[l, t, r, b]` fitting a srcW×srcH icon into a
     * canvasPx² adaptive bitmap: centered, aspect preserved, longer edge
     * scaled to 2/3 of the canvas — launchers mask adaptive icons to
     * roughly the middle two-thirds ("safe zone"), so anything bigger
     * risks clipping. Null for degenerate sizes (caller falls back to
     * the app icon).
     */
    internal fun pinIconFit(canvasPx: Int, srcW: Int, srcH: Int): IntArray? {
        if (canvasPx <= 0 || srcW <= 0 || srcH <= 0) return null
        val content = canvasPx * 2 / 3
        val w = if (srcW >= srcH) content else content * srcW / srcH
        val h = if (srcW >= srcH) content * srcH / srcW else content
        if (w <= 0 || h <= 0) return null
        val l = (canvasPx - w) / 2
        val t = (canvasPx - h) / 2
        return intArrayOf(l, t, l + w, t + h)
    }

    /** Adaptive-bitmap edge: 108 dp at xxhdpi. */
    private const val PIN_BITMAP_PX = 324

    /** Neutral light grey behind transparent icon regions. */
    private const val PIN_BACKGROUND = 0xFFF2F2F2.toInt()
}
