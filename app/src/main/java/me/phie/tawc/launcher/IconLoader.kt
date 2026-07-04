package me.phie.tawc.launcher

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.widget.ImageView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Async PNG icon loader for launcher rows.
 *
 * Backed by an in-memory `path → Bitmap` cache so the same icon shown on
 * different filter states (or the same row re-rendered after a filter
 * keystroke) doesn't decode the file twice. Decoding goes through
 * `BitmapFactory` with `inSampleSize` chosen to land at roughly the
 * target display size — a 256 × 256 PNG would otherwise cost ~256 KiB
 * of heap each, and a screen of ~50 of them adds up.
 *
 * Concurrency: each `load()` call sets `ImageView.tag` to the requested
 * path and re-checks it before applying the bitmap. So if the same
 * `ImageView` gets recycled with a different path mid-flight (rapid
 * filter typing), the stale completion is dropped.
 */
class IconLoader(
    private val scope: CoroutineScope,
    /** Target on-screen size in pixels. The decoded bitmap is no smaller
     *  than this, no more than 2× larger. */
    private val sizePx: Int,
) {
    private val cache = HashMap<String, Bitmap>()

    fun load(path: String, target: ImageView) {
        if (path.isEmpty()) {
            target.setImageDrawable(null)
            target.tag = null
            return
        }
        cache[path]?.let {
            target.setImageBitmap(it)
            target.tag = path
            return
        }
        target.setImageDrawable(null)
        target.tag = path
        scope.launch {
            val bmp = withContext(Dispatchers.IO) { decode(path, sizePx) }
            if (bmp == null || target.tag != path) return@launch
            cache[path] = bmp
            target.setImageBitmap(bmp)
        }
    }

    companion object {
        /**
         * Decode [path] into a [Bitmap] no smaller than [targetPx] in its
         * shorter dimension. Returns null on any error (bad PNG, missing
         * file, decoder doesn't recognise the format, e.g. SVG handed
         * through by mistake). Internal: [EntryShortcuts] reuses the
         * bounded decode for pin icons.
         */
        internal fun decode(path: String, targetPx: Int): Bitmap? {
            val f = File(path)
            if (!f.isFile) return null
            return runCatching {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(path, bounds)
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@runCatching null
                var sample = 1
                val shorter = minOf(bounds.outWidth, bounds.outHeight)
                while (shorter / (sample * 2) >= targetPx) sample *= 2
                val opts = BitmapFactory.Options().apply { inSampleSize = sample }
                BitmapFactory.decodeFile(path, opts)
            }.getOrNull()
        }
    }
}
