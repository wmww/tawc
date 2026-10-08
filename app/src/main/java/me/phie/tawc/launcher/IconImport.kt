package me.phie.tawc.launcher

import android.content.Context
import android.graphics.Bitmap
import android.graphics.ImageDecoder
import android.net.Uri
import android.provider.OpenableColumns
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException

/**
 * Turns an image the user picked (the editor's Load button) into a PNG
 * for the [LauncherStore]'s `icons/` dir. Rasters are scaled down to
 * fit [RASTER_PX] and re-encoded; SVGs are rasterized once at that
 * size by the icon cache's renderer ([NativeBridge.nativeRasterizeIcon]).
 * Storing only PNG keeps "`iconPath` is a decodable PNG" with no cache.
 */
internal object IconImport {

    /** Name prefix of the old editor's rootfs imports, which
     *  [LauncherMigration] moves into the store. */
    const val PREFIX = "tawc-"

    /** Imports are scaled to fit this square, never up. */
    const val RASTER_PX = 256

    /** Same cap as `icon_cache::MAX_SOURCE_BYTES`: a bigger SVG would
     *  never render. */
    const val MAX_SVG_BYTES = 1024 * 1024

    /** An import: PNG [bytes] and the [slug] its file is named after. */
    class Result(val bytes: ByteArray, val slug: String)

    /** Slug for a document named [displayName], extension dropped;
     *  `icon` when nothing slugifiable is left. */
    fun slugFor(displayName: String?): String {
        val base = displayName.orEmpty().let { n ->
            val dot = n.lastIndexOf('.')
            if (dot > 0) n.substring(0, dot) else n
        }
        return Installation.slugifyLabel(base) ?: "icon"
    }

    /**
     * File name for [bytes]: `<slug>.png`, then `<slug>-2.png`, … The
     * first candidate holding identical bytes is reused; otherwise the
     * first free one wins. [existing] returns a candidate's bytes, or
     * null if absent.
     */
    fun chooseName(slug: String, bytes: ByteArray, existing: (name: String) -> ByteArray?): String {
        var n = 1
        while (true) {
            val name = if (n == 1) "$slug.png" else "$slug-$n.png"
            val same = existing(name) ?: return name
            if (same.contentEquals(bytes)) return name
            n++
        }
    }

    /** Is this document an SVG, going by its MIME type or name? */
    fun isSvg(mime: String?, displayName: String?): Boolean =
        mime == "image/svg+xml" || displayName.orEmpty().endsWith(".svg", ignoreCase = true)

    /**
     * Read [uri] as a PNG. [scratch] is a private dir for the SVG
     * render. Blocking I/O; call on Dispatchers.IO. Throws
     * [IOException] on anything that didn't produce an icon.
     */
    fun import(context: Context, uri: Uri, scratch: File): Result {
        val resolver = context.contentResolver
        val displayName = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
        val bytes = if (isSvg(resolver.getType(uri), displayName)) {
            rasterizeSvg(readSvg(context, uri), scratch)
        } else {
            encodePng(decodeScaled(context, uri))
        }
        return Result(bytes, slugFor(displayName))
    }

    private fun readSvg(context: Context, uri: Uri): ByteArray {
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("can't open")
        val bytes = input.use { it.readNBytesCompat(MAX_SVG_BYTES + 1) }
        if (bytes.size > MAX_SVG_BYTES) throw IOException("SVG over 1 MiB")
        return bytes
    }

    private fun rasterizeSvg(svg: ByteArray, scratch: File): ByteArray {
        scratch.mkdirs()
        val src = File.createTempFile("import", ".svg", scratch)
        val dst = File(scratch, src.name.removeSuffix(".svg") + ".png")
        try {
            src.writeBytes(svg)
            if (!NativeBridge.nativeRasterizeIcon(src.path, dst.path, RASTER_PX)) throw IOException("can't render SVG")
            return dst.readBytes()
        } finally {
            src.delete()
            dst.delete()
        }
    }

    private fun decodeScaled(context: Context, uri: Uri): Bitmap = try {
        val source = ImageDecoder.createSource(context.contentResolver, uri)
        ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
            val (w, h) = fitWithin(info.size.width, info.size.height, RASTER_PX)
            decoder.setTargetSize(w, h)
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
        }
    } catch (e: Exception) {
        // ImageDecoder throws DecodeException (an IOException) but also
        // IllegalArgumentException etc. on odd inputs.
        throw e as? IOException ?: IOException(e.message, e)
    }

    private fun encodePng(bitmap: Bitmap): ByteArray {
        val out = ByteArrayOutputStream()
        if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)) throw IOException("PNG encode failed")
        return out.toByteArray()
    }

    /** [w]×[h] scaled to fit a [max] square, aspect kept, never up. */
    fun fitWithin(w: Int, h: Int, max: Int): Pair<Int, Int> {
        if (w <= max && h <= max) return w to h
        return if (w >= h) max to maxOf(1, h * max / w) else maxOf(1, w * max / h) to max
    }

    /** Up to [limit] bytes; `InputStream.readNBytes` is API 33. */
    private fun java.io.InputStream.readNBytesCompat(limit: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(8192)
        while (out.size() < limit) {
            val n = read(buf, 0, minOf(buf.size, limit - out.size()))
            if (n < 0) break
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }
}
